#include "ievolve/database/checkpoint.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <system_error>

#include "ievolve/database/artifact_store.h"
#include "ievolve/utils/file.h"

namespace ievolve {
namespace {
namespace fs = std::filesystem;

absl::Status Corrupt(const std::string& message) { return absl::DataLossError("Invalid checkpoint: " + message); }

absl::Status IoError(const std::string& message) {
  return absl::FailedPreconditionError("Checkpoint filesystem error: " + message);
}

bool SafeComponent(const std::string& name) {
  return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string::npos &&
         name.find('\0') == std::string::npos;
}

bool SafeGeneration(const std::string& name) {
  constexpr char prefix[] = "snapshot-";
  if (name.size() <= sizeof(prefix) - 1 || name.compare(0, sizeof(prefix) - 1, prefix) != 0) return false;

  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

absl::StatusOr<fs::file_status> EntryStatus(const fs::path& path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) return IoError("cannot inspect " + path.string());

  return status;
}

absl::Status RequireEntry(const fs::path& path, bool directory) {
  const auto status = EntryStatus(path);
  if (!status.ok()) return status.status();
  if (!fs::exists(*status)) return Corrupt("missing " + path.string());
  if (directory ? !fs::is_directory(*status) : !fs::is_regular_file(*status)) {
    return Corrupt("unsafe file type at " + path.string());
  }

  return absl::OkStatus();
}

absl::Status EnsureDirectory(const fs::path& path) {
  auto status = EntryStatus(path);
  if (!status.ok()) return status.status();
  if (!fs::exists(*status)) {
    fs::create_directories(path);
    status = EntryStatus(path);
    if (!status.ok()) return status.status();
  }

  if (!fs::is_directory(*status)) return IoError("expected a directory at " + path.string());

  return absl::OkStatus();
}

absl::Status ValidateMetadata(const Metrics& metadata) {
  Program validator;
  validator.id = "checkpoint-state";
  validator.metadata = metadata;
  return validator.Validate();
}

absl::StatusOr<fs::path> AbsolutePath(const fs::path& path) {
  if (path.empty() || path.string().find('\0') != std::string::npos) {
    return absl::InvalidArgumentError("Invalid checkpoint path");
  }

  auto absolute = fs::absolute(path).lexically_normal();
  while (absolute.has_relative_path() && absolute.filename().empty()) absolute = absolute.parent_path();

  // System paths such as macOS /var may be aliases. Canonicalize the caller's
  // parent path, but preserve the final entry for the no-symlink type check.
  if (!absolute.has_relative_path()) return absolute;
  return fs::weakly_canonical(absolute.parent_path()) / absolute.filename();
}

absl::StatusOr<std::string> ReadFile(const fs::path& path) {
  const auto status = RequireEntry(path, false);
  if (!status.ok()) return status;

  auto value = utils::ReadFile(path);
  if (!value.ok()) return Corrupt("cannot read " + path.string());

  return value;
}

absl::StatusOr<Metrics> ReadJson(const fs::path& path) {
  const auto text = ReadFile(path);
  if (!text.ok()) return text.status();

  try {
    // Reject ambiguous duplicate JSON object keys instead of accepting the
    // last.
    std::vector<std::set<std::string>> keys;
    bool duplicate = false;
    const auto parsed = Metrics::parse(*text, [&](int, Metrics::parse_event_t event, Metrics& value) {
      if (event == Metrics::parse_event_t::object_start) keys.emplace_back();
      if (event == Metrics::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second) {
        duplicate = true;
      }
      if (event == Metrics::parse_event_t::object_end) keys.pop_back();
      return true;
    });
    if (duplicate) return Corrupt("duplicate JSON key in " + path.string());

    return parsed;
  } catch (const Metrics::exception&) {
    return Corrupt("invalid JSON in " + path.string());
  }
}

absl::Status WriteFile(const fs::path& path, const std::string& text) {
  // All writes are inside our newly created, unpublished generation directory.
  const auto status = utils::WriteFile(path, text);
  return status.ok() ? status : IoError("cannot write " + path.string());
}

// A generation directory is built in full before anything points at it. Until
// Publish() is called the directory is garbage, so any failure or exception on
// the way removes it and leaves the previous generation as CURRENT.
class UnpublishedGeneration {
 public:
  explicit UnpublishedGeneration(fs::path path) : path_(std::move(path)) {}
  ~UnpublishedGeneration() {
    if (!published_) {
      std::error_code ignored;
      fs::remove_all(path_, ignored);
    }
  }
  void Publish() { published_ = true; }

 private:
  fs::path path_;
  bool published_ = false;
};

absl::StatusOr<fs::path> CreateGeneration(const fs::path& snapshots) {
  static std::atomic<unsigned long long> sequence{0};
  const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto candidate =
        snapshots / ("snapshot-" + std::to_string(stamp) + "-" + std::to_string(sequence.fetch_add(1)));
    if (fs::create_directory(candidate)) return candidate;
  }

  return IoError("cannot allocate a unique generation");
}

absl::Status ResolveArtifacts(Program& program, const fs::path& root, bool legacy) {
  if (program.artifact_dir && !program.artifact_dir->empty()) {
    fs::path relative(*program.artifact_dir);
    if (relative.is_absolute()) {
      if (!legacy) return Corrupt("native artifact path must be relative");

      // Locate the first prefix naming the checkpoint root. This accommodates
      // system aliases without resolving symlinks inside the checkpoint itself.
      fs::path prefix;
      bool contained = false;
      for (const auto& component : relative) {
        prefix /= component;
        if (fs::weakly_canonical(prefix) == root) {
          relative = relative.lexically_relative(prefix);
          contained = true;
          break;
        }
      }
      if (!contained || relative.empty()) {
        return absl::FailedPreconditionError("Legacy artifact directory must be inside the checkpoint");
      }
    }

    if (relative.empty() || relative.has_root_path()) return Corrupt("invalid artifact directory");

    fs::path resolved = root;
    for (const auto& component : relative) {
      if (!SafeComponent(component.string())) {
        if (legacy) return absl::FailedPreconditionError("Legacy artifact directory must be inside the checkpoint");
        return Corrupt("unsafe artifact directory");
      }

      resolved /= component;
      const auto status = RequireEntry(resolved, true);
      if (!status.ok()) return status;
    }
    program.artifact_dir = resolved.string();
  }

  const auto artifacts = ArtifactStore::Load(program);
  if (!artifacts.ok()) return Corrupt("invalid program artifacts");

  return absl::OkStatus();
}

absl::StatusOr<CheckpointData> ReadPrograms(const fs::path& root, Metrics metadata,
                                            const std::vector<std::string>& filenames, bool legacy) {
  CheckpointData data;
  data.metadata = std::move(metadata);
  data.legacy = legacy;
  std::set<std::string> ids;

  for (const auto& filename : filenames) {
    const auto json = ReadJson(root / "programs" / filename);
    if (!json.ok()) return json.status();

    auto program = Program::FromJson(*json);
    if (!program.ok()) return Corrupt("invalid program in " + filename);
    if (!ids.insert(program->id).second) return Corrupt("duplicate program id");

    const auto status = ResolveArtifacts(*program, root, legacy);
    if (!status.ok()) return status;
    data.programs.push_back(std::move(*program));
  }

  return data;
}

absl::Status CheckProgramDirectory(const fs::path& path) {
  auto status = RequireEntry(path, true);
  if (!status.ok()) return status;

  for (const auto& entry : fs::directory_iterator(path)) {
    status = RequireEntry(entry.path(), false);
    if (!status.ok()) return status;
  }

  return absl::OkStatus();
}

absl::StatusOr<CheckpointData> LoadNative(const fs::path& root) {
  auto current = ReadFile(root / "CURRENT");
  if (!current.ok()) return current.status();
  if (!current->empty() && current->back() == '\n') current->pop_back();
  if (!SafeGeneration(*current)) return Corrupt("unsafe CURRENT pointer");

  auto status = RequireEntry(root / "snapshots", true);
  if (!status.ok()) return status;
  const auto generation = root / "snapshots" / *current;
  status = RequireEntry(generation, true);
  if (!status.ok()) return status;

  auto envelope = ReadJson(generation / "metadata.json");
  if (!envelope.ok()) return envelope.status();
  if (!envelope->is_object() || !envelope->contains("checkpoint_version") ||
      !(*envelope)["checkpoint_version"].is_number_integer()) {
    return Corrupt("missing checkpoint version");
  }
  if ((*envelope)["checkpoint_version"] != 1) return absl::FailedPreconditionError("Unsupported checkpoint version");
  if (!envelope->contains("state") || !ValidateMetadata((*envelope)["state"]).ok() ||
      !envelope->contains("program_files") || !(*envelope)["program_files"].is_array()) {
    return Corrupt("invalid metadata envelope");
  }

  std::set<std::string> unique;
  std::vector<std::string> filenames;
  for (const auto& filename : (*envelope)["program_files"]) {
    if (!filename.is_string()) return Corrupt("invalid program filename");

    const auto name = filename.get<std::string>();
    if (!SafeComponent(name) || fs::path(name).extension() != ".json" || !unique.insert(name).second) {
      return Corrupt("unsafe or duplicate program filename");
    }
    filenames.push_back(name);
  }

  status = CheckProgramDirectory(generation / "programs");
  if (!status.ok()) return status;

  return ReadPrograms(generation, std::move((*envelope)["state"]), filenames, false);
}

absl::StatusOr<CheckpointData> LoadLegacy(const fs::path& root) {
  auto metadata = ReadJson(root / "metadata.json");
  if (!metadata.ok()) return metadata.status();
  if (!ValidateMetadata(*metadata).ok()) return Corrupt("invalid metadata");

  const auto programs = EntryStatus(root / "programs");
  if (!programs.ok()) return programs.status();
  // Python omits the directory when saving an empty database. Semantic state
  // validation belongs to ProgramDatabase and still rejects dangling ids.
  if (!fs::exists(*programs)) return ReadPrograms(root, std::move(*metadata), {}, true);

  const auto status = CheckProgramDirectory(root / "programs");
  if (!status.ok()) return status;

  std::vector<std::string> filenames;
  for (const auto& entry : fs::directory_iterator(root / "programs")) {
    if (entry.path().extension() == ".json") filenames.push_back(entry.path().filename().string());
  }
  std::sort(filenames.begin(), filenames.end());

  return ReadPrograms(root, std::move(*metadata), filenames, true);
}

}  // namespace

absl::Status Checkpoint::Save(const fs::path& path, const CheckpointData& data, const DatabaseConfig& config) {
  try {
    auto status = ValidateMetadata(data.metadata);
    if (!status.ok()) return status;

    std::set<std::string> ids;
    for (const auto& program : data.programs) {
      status = program.Validate();
      if (!status.ok()) return status;
      if (!ids.insert(program.id).second) return absl::InvalidArgumentError("Duplicate checkpoint program id");
    }

    const auto root = AbsolutePath(path);
    if (!root.ok()) return root.status();

    DatabaseConfig artifact_config = config;
    artifact_config.artifacts_base_path = (*root / "snapshots").string();
    const auto validate_config = ArtifactStore::Create(artifact_config);
    if (!validate_config.ok()) return validate_config.status();

    status = EnsureDirectory(*root);
    if (!status.ok()) return status;
    const auto current = EntryStatus(*root / "CURRENT");
    if (!current.ok()) return current.status();
    if (fs::exists(*current) && !fs::is_regular_file(*current)) return IoError("CURRENT is not a regular file");

    status = EnsureDirectory(*root / "snapshots");
    if (!status.ok()) return status;
    const auto generation = CreateGeneration(*root / "snapshots");
    if (!generation.ok()) return generation.status();
    UnpublishedGeneration cleanup(*generation);
    status = EnsureDirectory(*generation / "programs");
    if (!status.ok()) return status;

    artifact_config.artifacts_base_path = (*generation / "artifacts").string();
    const auto store = ArtifactStore::Create(artifact_config);
    if (!store.ok()) return store.status();

    Metrics filenames = Metrics::array();
    for (std::size_t i = 0; i < data.programs.size(); ++i) {
      const auto artifacts = ArtifactStore::Load(data.programs[i]);
      if (!artifacts.ok()) return artifacts.status();

      auto program = store->Store(data.programs[i], *artifacts);
      if (!program.ok()) return program.status();
      if (program->artifact_dir) {
        const auto relative = fs::path(*program->artifact_dir).lexically_relative(*generation);
        if (relative.empty() || relative.is_absolute()) return IoError("cannot create relative artifact reference");
        for (const auto& component : relative)
          if (!SafeComponent(component.string())) return IoError("artifact reference escapes generation");
        program->artifact_dir = relative.generic_string();
      }

      const auto json = program->ToJson();
      if (!json.ok()) return json.status();
      const auto filename = std::to_string(i) + ".json";
      status = WriteFile(*generation / "programs" / filename, json->dump(2));
      if (!status.ok()) return status;
      filenames.push_back(filename);
    }

    const Metrics envelope = {
        {"checkpoint_version", 1}, {"state", data.metadata}, {"program_files", std::move(filenames)}};
    status = WriteFile(*generation / "metadata.json", envelope.dump(2));
    if (!status.ok()) return status;

    const auto pointer = *generation / "CURRENT.tmp";
    status = WriteFile(pointer, generation->filename().string() + "\n");
    if (!status.ok()) return status;

    // Rename is the sole publication point. Concurrent writers and power-loss
    // durability (fsync) are outside this API's guarantees.
    fs::rename(pointer, *root / "CURRENT");
    cleanup.Publish();

    return absl::OkStatus();
  } catch (const fs::filesystem_error& error) {
    return IoError(error.what());
  } catch (const Metrics::exception&) {
    return absl::InvalidArgumentError("Invalid checkpoint JSON");
  }
}

absl::StatusOr<CheckpointData> Checkpoint::Load(const fs::path& path) {
  try {
    const auto root = AbsolutePath(path);
    if (!root.ok()) return root.status();

    const auto root_status = EntryStatus(*root);
    if (!root_status.ok()) return root_status.status();
    if (!fs::exists(*root_status)) return absl::NotFoundError("Checkpoint directory does not exist");
    if (!fs::is_directory(*root_status)) return Corrupt("root is not a directory");

    const auto current = EntryStatus(*root / "CURRENT");
    if (!current.ok()) return current.status();

    return fs::exists(*current) ? LoadNative(*root) : LoadLegacy(*root);
  } catch (const fs::filesystem_error& error) {
    return IoError(error.what());
  } catch (const Metrics::exception&) {
    return Corrupt("invalid checkpoint JSON");
  }
}
}  // namespace ievolve
