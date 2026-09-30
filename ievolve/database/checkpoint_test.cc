#include "ievolve/database/checkpoint.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "gtest/gtest.h"
#include "ievolve/database/artifact_store.h"

namespace ievolve {
namespace {
namespace fs = std::filesystem;

class CheckpointTest : public testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = fs::temp_directory_path() /
            ("ievolve-checkpoint-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
    fs::create_directory(root_);
    path_ = root_ / "checkpoint";
  }
  void TearDown() override {
    std::error_code error;
    fs::remove_all(root_, error);
  }
  static Program MakeProgram(std::string id) {
    Program program;
    program.id = std::move(id);
    program.code = "print('你好')\n";
    program.timestamp = 1234.5;
    program.metrics = {{"combined_score", 0.75}};
    program.metadata = {{"nested", {{"value", "µ"}}}};
    program.prompts = Metrics{{"evolve", "prompt\ntext"}};
    program.embedding = {0.1, 0.2};

    return program;
  }
  static std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
  static void Write(const fs::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary);
    output << value;
    output.close();
    ASSERT_TRUE(output.good());
  }
  static Metrics ReadJson(const fs::path& path) { return Metrics::parse(Read(path)); }
  fs::path Generation() const {
    std::string name = Read(path_ / "CURRENT");
    if (!name.empty() && name.back() == '\n') name.pop_back();

    return path_ / "snapshots" / name;
  }
  fs::path ProgramFile() const {
    const auto generation = Generation();
    const auto envelope = ReadJson(generation / "metadata.json");

    return generation / "programs" / envelope.at("program_files").at(0).get<std::string>();
  }
  CheckpointData Sample() const {
    CheckpointData data;
    data.metadata = {{"epoch", 7}, {"label", "测试"}};
    data.programs = {MakeProgram("../a/b:你好"), MakeProgram("second")};

    return data;
  }
  fs::path root_;
  fs::path path_;
};

TEST_F(CheckpointTest, RoundTripsFieldsAndInsertionOrderWithArbitraryIds) {
  const auto input = Sample();

  ASSERT_TRUE(Checkpoint::Save(path_, input).ok());

  auto loaded = Checkpoint::Load(path_);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  EXPECT_FALSE(loaded->legacy);
  EXPECT_EQ(loaded->metadata, input.metadata);
  ASSERT_EQ(loaded->programs.size(), 2u);
  for (std::size_t i = 0; i < input.programs.size(); ++i) {
    EXPECT_EQ(*loaded->programs[i].ToJson(), *input.programs[i].ToJson());
  }
  EXPECT_FALSE(fs::exists(root_ / "a"));
}

TEST_F(CheckpointTest, RepeatedSaveExcludesRemovedProgramsAndRetainsOldGeneration) {
  auto data = Sample();
  ASSERT_TRUE(Checkpoint::Save(path_, data).ok());
  const auto first = Generation();

  data.programs.erase(data.programs.begin());
  data.metadata["epoch"] = 8;

  ASSERT_TRUE(Checkpoint::Save(path_, data).ok());

  EXPECT_NE(Generation(), first);
  EXPECT_TRUE(fs::is_regular_file(first / "metadata.json"));

  auto loaded = Checkpoint::Load(path_);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  ASSERT_EQ(loaded->programs.size(), 1u);
  EXPECT_EQ(loaded->programs[0].id, "second");
  EXPECT_EQ(loaded->metadata["epoch"], 8);
}

TEST_F(CheckpointTest, InvalidInputPreservesCommittedGeneration) {
  ASSERT_TRUE(Checkpoint::Save(path_, Sample()).ok());
  const auto current = Read(path_ / "CURRENT");

  auto data = Sample();
  data.programs.push_back(data.programs.front());
  EXPECT_EQ(Checkpoint::Save(path_, data).code(), absl::StatusCode::kInvalidArgument);

  data = Sample();
  data.metadata["bad"] = std::numeric_limits<double>::infinity();
  EXPECT_EQ(Checkpoint::Save(path_, data).code(), absl::StatusCode::kInvalidArgument);

  data = Sample();
  data.metadata = Metrics::array();
  EXPECT_EQ(Checkpoint::Save(path_, data).code(), absl::StatusCode::kInvalidArgument);

  data = Sample();
  data.programs[0].generation = -1;
  EXPECT_EQ(Checkpoint::Save(path_, data).code(), absl::StatusCode::kInvalidArgument);

  EXPECT_EQ(Read(path_ / "CURRENT"), current);
  EXPECT_TRUE(Checkpoint::Load(path_).ok());
}

TEST_F(CheckpointTest, MissingRootAndMissingMetadataAreDistinct) {
  EXPECT_EQ(Checkpoint::Load(path_).status().code(), absl::StatusCode::kNotFound);

  fs::create_directory(path_);
  EXPECT_EQ(Checkpoint::Load(path_).status().code(), absl::StatusCode::kDataLoss);
}

TEST_F(CheckpointTest, RejectsCorruptOrMissingProgramWithoutPartialResult) {
  ASSERT_TRUE(Checkpoint::Save(path_, Sample()).ok());
  const auto program_file = ProgramFile();

  Write(program_file, "{\"id\":");
  EXPECT_EQ(Checkpoint::Load(path_).status().code(), absl::StatusCode::kDataLoss);

  fs::remove(program_file);
  EXPECT_EQ(Checkpoint::Load(path_).status().code(), absl::StatusCode::kDataLoss);
}

TEST_F(CheckpointTest, RejectsUnsupportedEnvelopeAndUnsafeFileReferences) {
  ASSERT_TRUE(Checkpoint::Save(path_, Sample()).ok());
  const auto metadata_file = Generation() / "metadata.json";
  const auto original = ReadJson(metadata_file);

  auto envelope = original;
  envelope["checkpoint_version"] = 42;
  Write(metadata_file, envelope.dump());
  EXPECT_EQ(Checkpoint::Load(path_).status().code(), absl::StatusCode::kFailedPrecondition);

  envelope = original;
  envelope["program_files"][0] = "../../external.json";
  Write(metadata_file, envelope.dump());
  EXPECT_FALSE(Checkpoint::Load(path_).ok());

  envelope = original;
  envelope["program_files"][1] = envelope["program_files"][0];
  Write(metadata_file, envelope.dump());
  EXPECT_FALSE(Checkpoint::Load(path_).ok());

  envelope = original;
  envelope["state"] = Metrics::array();
  Write(metadata_file, envelope.dump());
  EXPECT_FALSE(Checkpoint::Load(path_).ok());
}

TEST_F(CheckpointTest, RejectsUnsafeCurrentAndSymlinkEntries) {
  ASSERT_TRUE(Checkpoint::Save(path_, Sample()).ok());
  const auto current = Read(path_ / "CURRENT");

  Write(path_ / "CURRENT", "../outside\n");
  EXPECT_FALSE(Checkpoint::Load(path_).ok());

  Write(path_ / "CURRENT", current);
  const auto program = ProgramFile();
  fs::rename(program, root_ / "outside.json");
  fs::create_symlink(root_ / "outside.json", program);
  EXPECT_FALSE(Checkpoint::Load(path_).ok());

  fs::remove(path_ / "CURRENT");
  Write(root_ / "pointer", current);
  fs::create_symlink(root_ / "pointer", path_ / "CURRENT");
  EXPECT_FALSE(Checkpoint::Load(path_).ok());
  EXPECT_FALSE(Checkpoint::Save(path_, Sample()).ok());
  EXPECT_EQ(Read(root_ / "pointer"), current);
}

TEST_F(CheckpointTest, LegacyImportSortsFilenamesAndRejectsCorruption) {
  fs::create_directories(path_ / "programs");
  Write(path_ / "metadata.json", "{\"islands\":[[\"a\"],[\"z\"]]}");
  Write(path_ / "programs" / "z.json", MakeProgram("z").ToJson()->dump());
  Write(path_ / "programs" / "a.json", MakeProgram("a").ToJson()->dump());

  auto loaded = Checkpoint::Load(path_);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  EXPECT_TRUE(loaded->legacy);
  ASSERT_EQ(loaded->programs.size(), 2u);
  EXPECT_EQ(loaded->programs[0].id, "a");
  EXPECT_EQ(loaded->programs[1].id, "z");

  Write(path_ / "programs" / "z.json", "broken");
  EXPECT_EQ(Checkpoint::Load(path_).status().code(), absl::StatusCode::kDataLoss);
}

TEST_F(CheckpointTest, ImportsEmptyPythonCheckpointWithoutProgramsDirectory) {
  fs::create_directory(path_);
  Write(path_ / "metadata.json", "{\"islands\":[[],[]],\"archive\":[]}");

  auto loaded = Checkpoint::Load(path_);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  EXPECT_TRUE(loaded->legacy);
  EXPECT_TRUE(loaded->programs.empty());
}

TEST_F(CheckpointTest, RejectsSymlinkRootWithTrailingSeparator) {
  ASSERT_TRUE(Checkpoint::Save(path_, Sample()).ok());
  const auto alias = root_ / "alias";
  fs::create_directory_symlink(path_, alias);

  EXPECT_FALSE(Checkpoint::Load(alias / "").ok());
  EXPECT_FALSE(Checkpoint::Save(alias / "", Sample()).ok());
}

TEST_F(CheckpointTest, ArtifactFailurePreservesCurrentAndUnrelatedFiles) {
  ASSERT_TRUE(Checkpoint::Save(path_, Sample()).ok());
  const auto current = Read(path_ / "CURRENT");
  fs::create_directory(path_ / "snapshots" / "unrelated");
  Write(path_ / "snapshots" / "unrelated" / "keep", "user data");

  auto data = Sample();
  data.programs[1].artifacts_json = "broken";

  EXPECT_FALSE(Checkpoint::Save(path_, data).ok());

  EXPECT_EQ(Read(path_ / "CURRENT"), current);
  EXPECT_EQ(Read(path_ / "snapshots" / "unrelated" / "keep"), "user data");
  EXPECT_EQ(std::distance(fs::directory_iterator(path_ / "snapshots"), fs::directory_iterator()), 2);
  EXPECT_TRUE(Checkpoint::Load(path_).ok());
}

TEST_F(CheckpointTest, RejectsDuplicateIdsAndDuplicateJsonKeysOnLoad) {
  ASSERT_TRUE(Checkpoint::Save(path_, Sample()).ok());
  const auto generation = Generation();
  auto envelope = ReadJson(generation / "metadata.json");
  const auto second = generation / "programs" / envelope["program_files"][1].get<std::string>();

  Write(second, Read(ProgramFile()));
  EXPECT_EQ(Checkpoint::Load(path_).status().code(), absl::StatusCode::kDataLoss);

  Write(generation / "metadata.json", "{\"checkpoint_version\":1,\"checkpoint_version\":2}");
  EXPECT_EQ(Checkpoint::Load(path_).status().code(), absl::StatusCode::kDataLoss);
}

TEST_F(CheckpointTest, RejectsSymlinkMetadataAndSnapshotDirectories) {
  ASSERT_TRUE(Checkpoint::Save(path_, Sample()).ok());
  const auto generation = Generation();
  const auto metadata = generation / "metadata.json";

  fs::rename(metadata, root_ / "metadata.json");
  fs::create_symlink(root_ / "metadata.json", metadata);
  EXPECT_FALSE(Checkpoint::Load(path_).ok());

  fs::remove(metadata);
  fs::rename(root_ / "metadata.json", metadata);
  fs::rename(generation / "programs", root_ / "programs");
  fs::create_directory_symlink(root_ / "programs", generation / "programs");
  EXPECT_FALSE(Checkpoint::Load(path_).ok());

  fs::remove(generation / "programs");
  fs::rename(root_ / "programs", generation / "programs");
  fs::rename(generation, root_ / "generation");
  fs::create_directory_symlink(root_ / "generation", generation);
  EXPECT_FALSE(Checkpoint::Load(path_).ok());
}

TEST_F(CheckpointTest, BundlesArtifactsAndLoadsAfterOriginalDirectoriesDisappear) {
  DatabaseConfig config;
  config.artifacts_base_path = (root_ / "original-artifacts").string();
  config.artifact_size_threshold = 4;

  auto store = ArtifactStore::Create(config);
  ASSERT_TRUE(store.ok()) << store.status();

  const ArtifactMap artifacts = {{"small", std::string("ok")},
                                 {"../large.txt", std::string("large 你好 text")},
                                 {"bytes", ArtifactBytes{0, 255, 1, 2, 3}}};

  auto program = store->Store(MakeProgram("../arbitrary"), artifacts);
  ASSERT_TRUE(program.ok()) << program.status();

  CheckpointData data;
  data.programs = {*program};

  ASSERT_TRUE(Checkpoint::Save(path_, data, config).ok());

  const auto saved = ReadJson(ProgramFile());
  ASSERT_TRUE(saved["artifact_dir"].is_string());
  EXPECT_TRUE(fs::path(saved["artifact_dir"].get<std::string>()).is_relative());

  fs::copy(path_, root_ / "moved", fs::copy_options::recursive);
  fs::remove_all(path_);
  fs::remove_all(root_ / "original-artifacts");

  auto loaded = Checkpoint::Load(root_ / "moved");
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  ASSERT_EQ(loaded->programs.size(), 1u);
  ASSERT_TRUE(loaded->programs[0].artifact_dir);
  EXPECT_TRUE(fs::path(*loaded->programs[0].artifact_dir).is_absolute());

  const auto actual = ArtifactStore::Load(loaded->programs[0]);
  ASSERT_TRUE(actual.ok()) << actual.status();

  EXPECT_EQ(*actual, artifacts);
}

TEST_F(CheckpointTest, InlineBytesSurviveWithoutArtifactDirectory) {
  auto data = Sample();
  data.programs[0].artifacts_json = R"({"raw":{"__bytes__":"AP8B"},"log":"你好"})";

  ASSERT_TRUE(Checkpoint::Save(path_, data).ok());

  EXPECT_FALSE(fs::exists(Generation() / "artifacts"));

  auto loaded = Checkpoint::Load(path_);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  const auto artifacts = ArtifactStore::Load(loaded->programs[0]);
  ASSERT_TRUE(artifacts.ok()) << artifacts.status();

  EXPECT_EQ(std::get<ArtifactBytes>(artifacts->at("raw")), (ArtifactBytes{0, 255, 1}));
  EXPECT_EQ(std::get<std::string>(artifacts->at("log")), "你好");
}

TEST_F(CheckpointTest, RejectsMissingAndEscapingArtifactReferences) {
  ASSERT_TRUE(Checkpoint::Save(path_, Sample()).ok());
  const auto filename = ProgramFile();
  auto program = ReadJson(filename);

  for (const std::string reference : {"artifacts/missing", "../outside", "/tmp"}) {
    program["artifact_dir"] = reference;
    Write(filename, program.dump());
    EXPECT_FALSE(Checkpoint::Load(path_).ok()) << reference;
  }

  program["artifact_dir"] = nullptr;
  program["artifacts_json"] = R"({"bad":{"__bytes__":"not-base64"}})";
  Write(filename, program.dump());
  EXPECT_FALSE(Checkpoint::Load(path_).ok());
}

TEST_F(CheckpointTest, LegacyArtifactReferencesMustRemainInsideCheckpoint) {
  fs::create_directories(path_ / "programs");
  fs::create_directory(path_ / "assets");
  Write(path_ / "metadata.json", "{}");
  Write(path_ / "assets" / "report.txt", "legacy report");

  auto program = MakeProgram("legacy");
  program.artifact_dir = "assets";
  const auto filename = path_ / "programs" / "legacy.json";
  Write(filename, program.ToJson()->dump());

  auto loaded = Checkpoint::Load(path_);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  const auto artifacts = ArtifactStore::Load(loaded->programs[0]);
  ASSERT_TRUE(artifacts.ok()) << artifacts.status();

  EXPECT_EQ(std::get<std::string>(artifacts->at("report.txt")), "legacy report");

  program.artifact_dir = (path_ / "assets").string();
  Write(filename, program.ToJson()->dump());
  EXPECT_TRUE(Checkpoint::Load(path_).ok());

  program.artifact_dir = (root_ / "external-assets").string();
  Write(filename, program.ToJson()->dump());
  EXPECT_EQ(Checkpoint::Load(path_).status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST_F(CheckpointTest, ImportsUnchangedPythonSaveFixture) {
  const fs::path fixture = fs::path(IEVOLVE_DATABASE_TEST_DATA_DIR) / "checkpoint_legacy";
  fs::copy(fixture, path_, fs::copy_options::recursive);

  auto loaded = Checkpoint::Load(path_);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  EXPECT_TRUE(loaded->legacy);
  EXPECT_EQ(loaded->metadata["current_island"], 1);
  EXPECT_EQ(loaded->metadata["island_generations"], Metrics::array({2, 3}));
  ASSERT_EQ(loaded->programs.size(), 2u);
  EXPECT_EQ(loaded->programs[0].id, "legacy-a");
  EXPECT_EQ(loaded->programs[1].id, "legacy-z");
  ASSERT_TRUE(loaded->programs[0].prompts);
  EXPECT_EQ((*loaded->programs[0].prompts)["evolve"]["user"], "你好");

  const auto disk = ArtifactStore::Load(loaded->programs[0]);
  ASSERT_TRUE(disk.ok()) << disk.status();

  EXPECT_EQ(std::get<std::string>(disk->at("report.txt")), "legacy report 你好\n");

  const auto inline_artifacts = ArtifactStore::Load(loaded->programs[1]);
  ASSERT_TRUE(inline_artifacts.ok()) << inline_artifacts.status();

  EXPECT_EQ(std::get<ArtifactBytes>(inline_artifacts->at("raw")), (ArtifactBytes{0, 255, 1}));

  EXPECT_TRUE(Checkpoint::Save(root_ / "native", *loaded).ok());
}

TEST_F(CheckpointTest, EmptyNativeCheckpointPreservesMetadata) {
  CheckpointData empty;
  empty.metadata = {{"islands", Metrics::array({Metrics::array()})}};

  ASSERT_TRUE(Checkpoint::Save(path_, empty).ok());

  auto loaded = Checkpoint::Load(path_);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  EXPECT_FALSE(loaded->legacy);
  EXPECT_TRUE(loaded->programs.empty());
  EXPECT_EQ(loaded->metadata, empty.metadata);
}

TEST_F(CheckpointTest, RejectsMissingArtifactPayloadAndSymlinkArtifactParent) {
  auto data = Sample();
  data.programs[0].artifacts_json = R"({"log":"artifact text"})";
  DatabaseConfig config;
  config.artifact_size_threshold = 0;

  ASSERT_TRUE(Checkpoint::Save(path_, data, config).ok());

  auto loaded = Checkpoint::Load(path_);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  const fs::path directory = *loaded->programs[0].artifact_dir;
  const auto manifest = ReadJson(directory / ".ievolve-artifacts.json");
  const auto payload = directory / manifest["artifacts"]["log"]["file"].get<std::string>();

  fs::rename(payload, root_ / "saved-payload");
  EXPECT_EQ(Checkpoint::Load(path_).status().code(), absl::StatusCode::kDataLoss);

  fs::create_symlink(root_ / "saved-payload", payload);
  EXPECT_FALSE(Checkpoint::Load(path_).ok());

  fs::remove(payload);
  fs::rename(root_ / "saved-payload", payload);
  fs::rename(Generation() / "artifacts", root_ / "saved-artifacts");
  fs::create_directory_symlink(root_ / "saved-artifacts", Generation() / "artifacts");
  EXPECT_FALSE(Checkpoint::Load(path_).ok());
}

#ifndef _WIN32
TEST_F(CheckpointTest, RejectsSpecialFilesWithoutBlocking) {
  ASSERT_TRUE(Checkpoint::Save(path_, Sample()).ok());
  const auto program = ProgramFile();

  fs::remove(program);
  ASSERT_EQ(mkfifo(program.c_str(), 0600), 0);

  EXPECT_FALSE(Checkpoint::Load(path_).ok());

  fs::remove(program);
  fs::remove(path_ / "CURRENT");
  ASSERT_EQ(mkfifo((path_ / "CURRENT").c_str(), 0600), 0);

  EXPECT_FALSE(Checkpoint::Load(path_).ok());
  EXPECT_FALSE(Checkpoint::Save(path_, Sample()).ok());
}
#endif

}  // namespace
}  // namespace ievolve
