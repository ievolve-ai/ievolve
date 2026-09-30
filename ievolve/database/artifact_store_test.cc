#include "ievolve/database/artifact_store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"

namespace ievolve {
namespace {
namespace fs = std::filesystem;

ArtifactValue FixtureValue(const Metrics& tagged) {
  if (tagged["type"] == "text") return tagged["value"].get<std::string>();
  return tagged["value"].get<ArtifactBytes>();
}

class ArtifactStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = fs::temp_directory_path() /
            ("ievolve-artifact-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(path_);

    config_.artifacts_base_path = (path_ / "artifacts").string();
    config_.artifact_size_threshold = 4;
    program_.id = "../not-a-directory";
  }
  void TearDown() override {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  void Write(const fs::path& path, const std::string& content) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(content.data(), content.size());
    stream.close();
    ASSERT_TRUE(stream.good());
  }
  Metrics ReadManifest(const Program& program) {
    std::ifstream stream(fs::path(*program.artifact_dir) / ".ievolve-artifacts.json");
    return Metrics::parse(stream);
  }
  fs::path path_;
  DatabaseConfig config_;
  Program program_;
};

TEST_F(ArtifactStoreTest, ValidatesConfigurationWithoutCreatingRoot) {
  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  EXPECT_FALSE(fs::exists(store->root()));

  config_.artifact_size_threshold = -1;
  EXPECT_FALSE(ArtifactStore::Create(config_).ok());

  config_.artifact_size_threshold = 0;
  config_.artifact_retention_days = -1;
  EXPECT_FALSE(ArtifactStore::Create(config_).ok());
}

TEST_F(ArtifactStoreTest, ResolvesBasePathThenDatabasePathThenWorkingDirectory) {
  config_.artifacts_base_path.reset();
  config_.db_path = (path_ / "database").string();

  auto database_store = ArtifactStore::Create(config_);
  ASSERT_TRUE(database_store.ok()) << database_store.status();

  EXPECT_EQ(database_store->root(), path_ / "database" / "artifacts");

  config_.db_path.reset();

  auto current_store = ArtifactStore::Create(config_);
  ASSERT_TRUE(current_store.ok()) << current_store.status();

  EXPECT_EQ(current_store->root(), fs::current_path() / "artifacts");
}

TEST_F(ArtifactStoreTest, InlineValuesPreserveUtf8AndBinaryTypesAtThreshold) {
  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  const ArtifactMap artifacts = {{"unicode", std::string(u8"éé")},
                                 {"bytes", ArtifactBytes{0, 255, 1, 2}},
                                 {"empty-text", std::string()},
                                 {"empty-bytes", ArtifactBytes{}}};

  auto result = store->Store(program_, artifacts);
  ASSERT_TRUE(result.ok()) << result.status();

  ASSERT_TRUE(result->artifacts_json);
  EXPECT_FALSE(result->artifact_dir);
  EXPECT_FALSE(fs::exists(store->root()));

  auto loaded = ArtifactStore::Load(*result);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  EXPECT_EQ(*loaded, artifacts);

  const auto json = Metrics::parse(*result->artifacts_json);
  EXPECT_EQ(json["bytes"]["__bytes__"], "AP8BAg==");
  EXPECT_EQ(json["empty-bytes"]["__bytes__"], "");
}

TEST_F(ArtifactStoreTest, ReplacementAndEmptyInputClearOldReferences) {
  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  program_.artifacts_json = "{\"old\":\"value\"}";
  program_.artifact_dir = (path_ / "old").string();

  auto replacement = store->Store(program_, {{"new", std::string("one")}});
  ASSERT_TRUE(replacement.ok()) << replacement.status();

  EXPECT_FALSE(replacement->artifact_dir);

  auto loaded = ArtifactStore::Load(*replacement);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  EXPECT_EQ(loaded->size(), 1);
  EXPECT_EQ(std::get<std::string>(loaded->at("new")), "one");

  auto cleared = store->Store(program_, {});
  ASSERT_TRUE(cleared.ok()) << cleared.status();

  EXPECT_FALSE(cleared->artifact_dir);
  EXPECT_FALSE(cleared->artifacts_json);
  EXPECT_EQ(*program_.artifact_dir, (path_ / "old").string());
}

TEST_F(ArtifactStoreTest, RejectsInvalidProgramKeysTextAndInlinePayloads) {
  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  auto invalid = program_;
  invalid.id.clear();
  EXPECT_FALSE(store->Store(invalid, {}).ok());
  EXPECT_FALSE(store->Store(program_, {{std::string(1, '\xff'), "x"}}).ok());
  EXPECT_FALSE(store->Store(program_, {{"x", std::string(1, '\xff')}}).ok());

  for (const auto* json : {"[]", "{", "{\"x\":null}", "{\"x\":{\"__bytes__\":\"!===\"}}",
                           "{\"x\":{\"__bytes__\":\"Zg=\"}}", "{\"x\":{\"__bytes__\":\"Zh==\"}}",
                           "{\"x\":{\"__bytes__\":\"Zg==\",\"extra\":1}}", "{\"x\":{\"__bytes__\":42}}"}) {
    program_.artifacts_json = json;
    EXPECT_FALSE(ArtifactStore::Load(program_).ok()) << json;
  }
}

TEST_F(ArtifactStoreTest, DiskPreservesNamesTypesAndUsesImmutableDirectories) {
  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  const ArtifactMap artifacts = {{"a/b", std::string("first value")},
                                 {"ab", std::string("second value")},
                                 {"../outside", ArtifactBytes{'a', 'b', 'c', 'd', 'e'}},
                                 {"raw", ArtifactBytes{0, 255, 1, 2, 3}},
                                 {std::string("a\0b", 3), std::string("text\0content", 12)},
                                 {u8"名字", std::string(u8"ééé")},
                                 {"", std::string("empty key")},
                                 {"inline", std::string("tiny")}};

  auto result = store->Store(program_, artifacts);
  ASSERT_TRUE(result.ok()) << result.status();

  ASSERT_TRUE(result->artifact_dir);
  ASSERT_TRUE(result->artifacts_json);
  EXPECT_TRUE(fs::path(*result->artifact_dir).is_absolute());
  EXPECT_EQ(fs::canonical(fs::path(*result->artifact_dir).parent_path()), fs::canonical(store->root()));
  EXPECT_FALSE(fs::exists(path_ / "outside"));

  auto loaded = ArtifactStore::Load(*result);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  EXPECT_EQ(*loaded, artifacts);

  auto replaced = store->Store(*result, {{"replacement", std::string("large value")}});
  ASSERT_TRUE(replaced.ok()) << replaced.status();

  EXPECT_NE(replaced->artifact_dir, result->artifact_dir);
  EXPECT_FALSE(replaced->artifacts_json);
  EXPECT_TRUE(fs::exists(*result->artifact_dir));

  auto previous = ArtifactStore::Load(*result);
  ASSERT_TRUE(previous.ok()) << previous.status();

  EXPECT_EQ(*previous, artifacts);
}

TEST_F(ArtifactStoreTest, LegacyFlatDirectoryInfersTextAndRejectsDuplicateKeys) {
  fs::create_directory(path_ / "legacy");
  Write(path_ / "legacy" / "text", "hello\n");
  Write(path_ / "legacy" / "binary", std::string("a\xff", 2));
  program_.artifact_dir = (path_ / "legacy").string();

  auto result = ArtifactStore::Load(program_);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(std::get<std::string>(result->at("text")), "hello\n");
  EXPECT_EQ(std::get<ArtifactBytes>(result->at("binary")), (ArtifactBytes{'a', 255}));

  program_.artifacts_json = "{\"text\":\"duplicate\"}";
  EXPECT_FALSE(ArtifactStore::Load(program_).ok());
}

TEST_F(ArtifactStoreTest, RejectsCorruptManifestMissingFilesAndTraversal) {
  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  auto result = store->Store(program_, {{"large", std::string("12345")}});
  ASSERT_TRUE(result.ok()) << result.status();

  const auto manifest = ReadManifest(*result);
  const auto manifest_path = fs::path(*result->artifact_dir) / ".ievolve-artifacts.json";

  for (const auto* filename : {"../outside", "/outside", "a/b", ".", "..", ""}) {
    auto corrupt = manifest;
    corrupt["artifacts"]["large"]["file"] = filename;
    Write(manifest_path, corrupt.dump());
    EXPECT_FALSE(ArtifactStore::Load(*result).ok()) << filename;
  }

  for (const auto* content : {"{", "[]", "{\"format\":\"unknown\",\"artifacts\":{}}"}) {
    Write(manifest_path, content);
    EXPECT_FALSE(ArtifactStore::Load(*result).ok()) << content;
  }

  auto invalid_type = manifest;
  invalid_type["artifacts"]["large"]["type"] = "invalid";
  Write(manifest_path, invalid_type.dump());
  EXPECT_FALSE(ArtifactStore::Load(*result).ok());

  Write(manifest_path, manifest.dump());
  fs::remove(fs::path(*result->artifact_dir) / manifest["artifacts"]["large"]["file"].get<std::string>());
  EXPECT_FALSE(ArtifactStore::Load(*result).ok());

  fs::remove_all(*result->artifact_dir);
  EXPECT_FALSE(ArtifactStore::Load(*result).ok());
}

TEST_F(ArtifactStoreTest, RejectsSymlinkDirectoriesFilesAndSpecialEntries) {
  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  auto result = store->Store(program_, {{"large", std::string("12345")}});
  ASSERT_TRUE(result.ok()) << result.status();

  const auto manifest = ReadManifest(*result);
  const auto file = fs::path(*result->artifact_dir) / manifest["artifacts"]["large"]["file"].get<std::string>();

  fs::remove(file);
  Write(path_ / "target", "secret");
  fs::create_symlink(path_ / "target", file);
  EXPECT_FALSE(ArtifactStore::Load(*result).ok());

  fs::remove(file);
  fs::create_directory(file);
  EXPECT_FALSE(ArtifactStore::Load(*result).ok());

  fs::create_directory_symlink(*result->artifact_dir, path_ / "link");
  result->artifact_dir = (path_ / "link").string();
  EXPECT_FALSE(ArtifactStore::Load(*result).ok());

  fs::create_directory(path_ / "legacy");
  fs::create_symlink(path_ / "target", path_ / "legacy" / "secret");
  result->artifact_dir = (path_ / "legacy").string();
  EXPECT_FALSE(ArtifactStore::Load(*result).ok());
}

TEST_F(ArtifactStoreTest, RejectsDirectorySymlinkWithTrailingSlash) {
  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  auto result = store->Store(program_, {{"large", std::string("12345")}});
  ASSERT_TRUE(result.ok()) << result.status();

  fs::create_directory_symlink(*result->artifact_dir, path_ / "link");
  result->artifact_dir = (path_ / "link").string() + "/";
  EXPECT_FALSE(ArtifactStore::Load(*result).ok());

  result->artifact_dir = (path_ / "link" / ".").string();
  EXPECT_FALSE(ArtifactStore::Load(*result).ok());
}

TEST_F(ArtifactStoreTest, RejectsCorruptManagedTextAndConflictingInlineKey) {
  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  auto result = store->Store(program_, {{"large", std::string("12345")}});
  ASSERT_TRUE(result.ok()) << result.status();

  const auto manifest = ReadManifest(*result);
  const auto file = fs::path(*result->artifact_dir) / manifest["artifacts"]["large"]["file"].get<std::string>();

  result->artifacts_json = "{\"large\":\"duplicate\"}";
  EXPECT_FALSE(ArtifactStore::Load(*result).ok());

  result->artifacts_json.reset();
  Write(file, std::string(1, '\xff'));
  EXPECT_FALSE(ArtifactStore::Load(*result).ok());
}

TEST_F(ArtifactStoreTest, FailedReplacementLeavesExistingArtifactsUntouched) {
  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  auto original = store->Store(program_, {{"large", std::string("previous data")}});
  ASSERT_TRUE(original.ok()) << original.status();

  Write(path_ / "file-root", "cannot be a directory");
  config_.artifacts_base_path = (path_ / "file-root" / "child").string();
  auto bad_store = ArtifactStore::Create(config_);
  ASSERT_TRUE(bad_store.ok()) << bad_store.status();

  EXPECT_FALSE(bad_store->Store(*original, {{"new", std::string("new data")}}).ok());

  const auto loaded = ArtifactStore::Load(*original);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  EXPECT_EQ(std::get<std::string>(loaded->at("large")), "previous data");
}

TEST_F(ArtifactStoreTest, UnwritableRootFailsWithoutChangingPriorFiles) {
  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  auto original = store->Store(program_, {{"large", std::string("previous data")}});
  ASSERT_TRUE(original.ok()) << original.status();

  fs::permissions(store->root(), fs::perms::owner_read | fs::perms::owner_exec);
  auto failed = store->Store(*original, {{"large", std::string("replacement")}});
  fs::permissions(store->root(), fs::perms::owner_all);

  EXPECT_FALSE(failed.ok());

  auto loaded = ArtifactStore::Load(*original);
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  EXPECT_EQ(std::get<std::string>(loaded->at("large")), "previous data");
  EXPECT_EQ(std::distance(fs::directory_iterator(store->root()), fs::directory_iterator()), 1);
}

TEST_F(ArtifactStoreTest, CleanupDeletesOnlyExpiredManagedUnprotectedDirectories) {
  config_.artifact_retention_days = 1;

  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  auto expired = store->Store(program_, {{"large", std::string("expired value")}});
  auto protected_program = store->Store(program_, {{"large", std::string("protected value")}});
  auto recent = store->Store(program_, {{"large", std::string("recent value")}});
  ASSERT_TRUE(expired.ok()) << expired.status();
  ASSERT_TRUE(protected_program.ok()) << protected_program.status();
  ASSERT_TRUE(recent.ok()) << recent.status();

  const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(48);
  fs::last_write_time(*expired->artifact_dir, old_time);
  fs::last_write_time(*protected_program->artifact_dir, old_time);

  const auto unrelated = store->root() / "unrelated";
  const auto corrupt = store->root() / "ievolve-artifacts-corrupt";
  fs::create_directory(unrelated);
  Write(unrelated / "valuable", "keep me");
  fs::create_directory(corrupt);
  Write(corrupt / ".ievolve-artifacts.json", "{}");
  fs::last_write_time(unrelated, old_time);
  fs::last_write_time(corrupt, old_time);
  fs::create_directory_symlink(unrelated, store->root() / "ievolve-artifacts-link");

  const auto protected_alias = (fs::path(*protected_program->artifact_dir) / "..").string() + "/" +
                               fs::path(*protected_program->artifact_dir).filename().string();

  auto count = store->Cleanup({protected_alias});
  ASSERT_TRUE(count.ok()) << count.status();

  EXPECT_EQ(*count, 1);
  EXPECT_FALSE(fs::exists(*expired->artifact_dir));
  EXPECT_TRUE(ArtifactStore::Load(*protected_program).ok());
  EXPECT_TRUE(ArtifactStore::Load(*recent).ok());
  EXPECT_TRUE(fs::exists(unrelated / "valuable"));
  EXPECT_TRUE(fs::exists(corrupt));
  EXPECT_TRUE(fs::is_symlink(store->root() / "ievolve-artifacts-link"));
}

TEST_F(ArtifactStoreTest, DisabledCleanupAndAbsentRootDoNotTouchFilesystem) {
  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  auto count = store->Cleanup();
  ASSERT_TRUE(count.ok()) << count.status();

  EXPECT_EQ(*count, 0);
  EXPECT_FALSE(fs::exists(store->root()));

  Write(path_ / "file-root", "still here");
  config_.artifacts_base_path = (path_ / "file-root").string();
  config_.cleanup_old_artifacts = false;
  auto disabled = ArtifactStore::Create(config_);
  ASSERT_TRUE(disabled.ok()) << disabled.status();

  count = disabled->Cleanup();
  ASSERT_TRUE(count.ok()) << count.status();

  EXPECT_EQ(*count, 0);
  EXPECT_TRUE(fs::is_regular_file(path_ / "file-root"));
}

TEST_F(ArtifactStoreTest, CleanupSkipsManagedDirectoryContainingUnrelatedFiles) {
  config_.artifact_retention_days = 0;

  auto store = ArtifactStore::Create(config_);
  ASSERT_TRUE(store.ok()) << store.status();

  auto original = store->Store(program_, {{"large", std::string("previous data")}});
  ASSERT_TRUE(original.ok()) << original.status();

  Write(fs::path(*original->artifact_dir) / "unrelated", "retain me");
  fs::last_write_time(*original->artifact_dir, fs::file_time_type::clock::now() - std::chrono::hours(1));

  auto count = store->Cleanup();
  ASSERT_TRUE(count.ok()) << count.status();

  EXPECT_EQ(*count, 0);
  EXPECT_TRUE(fs::exists(fs::path(*original->artifact_dir) / "unrelated"));
}

TEST_F(ArtifactStoreTest, MatchesUnchangedPythonArtifactHelpers) {
  std::ifstream stream(fs::path(IEVOLVE_DATABASE_TEST_DATA_DIR) / "artifacts.json");
  ASSERT_TRUE(stream.good());

  const auto fixture = Metrics::parse(stream);
  fs::create_directory(path_ / "legacy");

  for (const auto& test_case : fixture["cases"]) {
    const auto key = test_case["name"].get<std::string>();
    SCOPED_TRACE(key);
    const auto value = FixtureValue(test_case["input"]);
    config_.artifact_size_threshold = test_case["size"].get<int>();
    auto inline_store = ArtifactStore::Create(config_);
    ASSERT_TRUE(inline_store.ok()) << inline_store.status();

    auto stored = inline_store->Store(program_, {{key, value}});
    ASSERT_TRUE(stored.ok()) << stored.status();

    ASSERT_TRUE(stored->artifacts_json);
    EXPECT_FALSE(stored->artifact_dir);
    EXPECT_EQ(Metrics::parse(*stored->artifacts_json), Metrics::parse(test_case["inline_json"].get<std::string>()));

    auto loaded = ArtifactStore::Load(*stored);
    ASSERT_TRUE(loaded.ok()) << loaded.status();

    EXPECT_EQ(loaded->at(key), FixtureValue(test_case["decoded"]));

    if (config_.artifact_size_threshold > 0) {
      --config_.artifact_size_threshold;
      auto disk_store = ArtifactStore::Create(config_);
      ASSERT_TRUE(disk_store.ok()) << disk_store.status();

      auto disk_stored = disk_store->Store(program_, {{key, value}});
      ASSERT_TRUE(disk_stored.ok()) << disk_stored.status();

      EXPECT_TRUE(disk_stored->artifact_dir);
      EXPECT_FALSE(disk_stored->artifacts_json);
    }

    if (const auto* text = std::get_if<std::string>(&value)) {
      Write(path_ / "legacy" / key, *text);
    } else {
      const auto& bytes = std::get<ArtifactBytes>(value);
      Write(path_ / "legacy" / key, std::string(bytes.begin(), bytes.end()));
    }
  }

  program_.artifact_dir = (path_ / "legacy").string();
  auto legacy = ArtifactStore::Load(program_);
  ASSERT_TRUE(legacy.ok()) << legacy.status();

  EXPECT_EQ(legacy->size(), fixture["legacy"].size());
  for (const auto& [key, tagged] : fixture["legacy"].items()) EXPECT_EQ(legacy->at(key), FixtureValue(tagged)) << key;
}

}  // namespace
}  // namespace ievolve
