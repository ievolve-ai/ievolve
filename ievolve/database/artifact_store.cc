#include "ievolve/database/artifact_store.h"

#include <atomic>
#include <chrono>

#include "ievolve/utils/file.h"
#include "ievolve/utils/text.h"

namespace ievolve {
namespace {
namespace fs = std::filesystem;
constexpr char kManifest[] = ".ievolve-artifacts.json";
constexpr char kFormat[] = "ievolve.artifacts.v1";
constexpr char kDirectoryPrefix[] = "ievolve-artifacts-";

absl::Status Invalid(const std::string& message) {
  return absl::InvalidArgumentError("Invalid artifact data: " + message);
}

absl::Status IoError(const std::string& message) {
  return absl::FailedPreconditionError("Artifact filesystem error: " + message);
}

// Guards every name read back from a manifest. Artifact keys and program IDs
// are arbitrary text and are never used as paths themselves; the manifest maps
// them to numbered files. This rejects what a hand-edited or hostile manifest
// could otherwise point at: traversal, absolute paths, separators, and the
// manifest itself.
bool SafeFilename(const std::string& name) {
  const fs::path path(name);
  return !name.empty() && name != "." && name != ".." && name != kManifest &&
         name.find_first_of("/\\") == std::string::npos && name.find('\0') == std::string::npos &&
         !path.has_root_path() && path.filename() == path;
}

// Symlinks are refused here rather than followed: a manifest directory is
// trusted only for the regular files it contains.
absl::StatusOr<std::string> ReadFile(const fs::path& path) {
  if (!fs::is_regular_file(fs::symlink_status(path))) return IoError("expected a regular file: " + path.string());

  auto content = utils::ReadFile(path);
  if (!content.ok()) return IoError("cannot read " + path.string());

  return content;
}

absl::Status WriteFile(const fs::path& path, const std::string& content) {
  const auto status = utils::WriteFile(path, content);
  return status.ok() ? status : IoError("cannot write " + path.string());
}

absl::StatusOr<Metrics> ReadManifest(const fs::path& directory) {
  auto content = ReadFile(directory / kManifest);
  if (!content.ok()) return content.status();

  const auto manifest = Metrics::parse(*content);
  if (!manifest.is_object() || manifest.size() != 2 || !manifest.contains("format") || manifest["format"] != kFormat ||
      !manifest.contains("artifacts") || !manifest["artifacts"].is_object()) {
    return Invalid("invalid manifest format");
  }

  std::set<std::string> filenames;
  for (const auto& entry : manifest["artifacts"]) {
    if (!entry.is_object() || entry.size() != 2 || !entry.contains("file") || !entry["file"].is_string() ||
        !entry.contains("type") || (entry["type"] != "text" && entry["type"] != "bytes")) {
      return Invalid("invalid manifest entry");
    }

    const auto filename = entry["file"].get<std::string>();
    if (!SafeFilename(filename) || !filenames.insert(filename).second) {
      return Invalid("unsafe or repeated manifest filename");
    }
  }

  return manifest;
}

bool ManagedName(const fs::path& directory) { return directory.filename().string().find(kDirectoryPrefix) == 0; }

absl::Status LoadDirectory(fs::path directory, ArtifactMap& result) {
  directory = fs::absolute(directory);
  // lstat("link/") and lstat("link/.") follow a directory symlink. Remove
  // those suffixes before inspecting the directory itself.
  while (directory.has_relative_path() && (directory.filename().empty() || directory.filename() == "."))
    directory = directory.parent_path();
  if (!fs::is_directory(fs::symlink_status(directory))) {
    return IoError("expected an artifact directory: " + directory.string());
  }

  if (fs::exists(fs::symlink_status(directory / kManifest))) {
    auto manifest = ReadManifest(directory);
    if (!manifest.ok()) return manifest.status();
    for (const auto& [key, entry] : (*manifest)["artifacts"].items()) {
      auto content = ReadFile(directory / entry["file"].get<std::string>());
      if (!content.ok()) return content.status();

      ArtifactValue value;
      if (entry["type"] == "text") {
        if (!utils::IsValidUtf8(*content)) return Invalid("manifest text is not UTF-8");
        value = std::move(*content);
      } else {
        value = ArtifactBytes(content->begin(), content->end());
      }

      if (!result.emplace(key, std::move(value)).second) return Invalid("duplicate inline and disk artifact key");
    }
  } else {
    if (ManagedName(directory)) return Invalid("managed manifest is missing");

    for (const auto& entry : fs::directory_iterator(directory)) {
      const auto key = entry.path().filename().string();
      if (!utils::IsValidUtf8(key)) return Invalid("legacy filename is not UTF-8");

      auto content = ReadFile(entry.path());
      if (!content.ok()) return content.status();

      ArtifactValue value;
      if (utils::IsValidUtf8(*content)) {
        value = std::move(*content);
      } else {
        value = ArtifactBytes(content->begin(), content->end());
      }

      if (!result.emplace(key, std::move(value)).second) return Invalid("duplicate inline and disk artifact key");
    }
  }

  return absl::OkStatus();
}

// The guard owns only a newly created directory. Failure never removes an old
// program's files or another caller's directory.
struct NewDirectory {
  fs::path path;
  ~NewDirectory() {
    if (!path.empty()) {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  }
};

absl::StatusOr<fs::path> CreateDirectory(const fs::path& root) {
  fs::create_directories(root);
  if (!fs::is_directory(fs::symlink_status(root))) {
    return IoError("artifact root must be a directory without a symlink");
  }

  static std::atomic<std::uint64_t> sequence{0};
  const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
  for (int attempt = 0; attempt < 64; ++attempt) {
    const auto candidate = root / (std::string(kDirectoryPrefix) + std::to_string(timestamp) + "-" +
                                   std::to_string(sequence.fetch_add(1)));
    if (fs::create_directory(candidate)) return candidate;
  }

  return IoError("cannot allocate a unique artifact directory");
}

}  // namespace

absl::StatusOr<ArtifactStore> ArtifactStore::Create(const DatabaseConfig& config) {
  if (config.artifact_size_threshold < 0 || config.artifact_retention_days < 0) {
    return Invalid("threshold and retention must be nonnegative");
  }

  try {
    ArtifactStore store;
    if (config.artifacts_base_path && !config.artifacts_base_path->empty()) {
      store.root_ = *config.artifacts_base_path;
    } else if (config.db_path && !config.db_path->empty()) {
      store.root_ = fs::path(*config.db_path) / "artifacts";
    } else {
      store.root_ = "artifacts";
    }

    const auto text = store.root_.string();
    if (text.find('\0') != std::string::npos || !utils::IsValidUtf8(text)) return Invalid("artifact root path");

    store.root_ = fs::absolute(store.root_).lexically_normal();
    store.size_threshold_ = config.artifact_size_threshold;
    store.cleanup_enabled_ = config.cleanup_old_artifacts;
    store.retention_days_ = config.artifact_retention_days;

    return store;
  } catch (const fs::filesystem_error& error) {
    return IoError(error.what());
  }
}

absl::StatusOr<Program> ArtifactStore::Store(const Program& program, const ArtifactMap& artifacts) const {
  const auto status = program.Validate();
  if (!status.ok()) return status;

  Program replacement = program;
  replacement.artifacts_json.reset();
  replacement.artifact_dir.reset();
  Metrics small = Metrics::object();
  ArtifactMap large;

  for (const auto& [key, value] : artifacts) {
    if (!utils::IsValidUtf8(key)) return Invalid("key must be UTF-8");
    if (const auto* text = std::get_if<std::string>(&value)) {
      if (!utils::IsValidUtf8(*text)) return Invalid("text must be UTF-8");
    }

    const auto size = std::visit([](const auto& payload) { return payload.size(); }, value);
    if (size > size_threshold_) {
      large.emplace(key, value);
    } else {
      auto encoded = EncodeArtifactValue(value);
      if (!encoded.ok()) return encoded.status();
      small[key] = std::move(*encoded);
    }
  }

  if (!small.empty()) replacement.artifacts_json = small.dump();
  if (!large.empty()) {
    try {
      auto directory = CreateDirectory(root_);
      if (!directory.ok()) return directory.status();

      NewDirectory guard{*directory};
      Metrics manifest = {{"format", kFormat}, {"artifacts", Metrics::object()}};
      std::size_t index = 0;
      for (const auto& [key, value] : large) {
        const auto filename = std::to_string(index++) + ".bin";
        const auto* text = std::get_if<std::string>(&value);
        std::string content;
        if (text) {
          content = *text;
        } else {
          const auto& bytes = std::get<ArtifactBytes>(value);
          content.assign(bytes.begin(), bytes.end());
        }

        const auto write_status = WriteFile(*directory / filename, content);
        if (!write_status.ok()) return write_status;
        manifest["artifacts"][key] = {{"file", filename}, {"type", text ? "text" : "bytes"}};
      }

      const auto write_status = WriteFile(*directory / kManifest, manifest.dump());
      if (!write_status.ok()) return write_status;

      replacement.artifact_dir = fs::canonical(*directory).string();
      guard.path.clear();
    } catch (const fs::filesystem_error& error) {
      return IoError(error.what());
    }
  }

  return replacement;
}

absl::StatusOr<ArtifactMap> ArtifactStore::Load(const Program& program) {
  const auto status = program.Validate();
  if (!status.ok()) return status;

  ArtifactMap result;
  try {
    if (program.artifacts_json && !program.artifacts_json->empty()) {
      const auto small = Metrics::parse(*program.artifacts_json);
      if (!small.is_object()) return Invalid("inline JSON must be an object");
      for (const auto& [key, value] : small.items()) {
        auto decoded = DecodeArtifactValue(value);
        if (!decoded.ok()) return decoded.status();
        result.emplace(key, std::move(*decoded));
      }
    }

    if (program.artifact_dir && !program.artifact_dir->empty()) {
      if (program.artifact_dir->find('\0') != std::string::npos) return Invalid("artifact directory path");

      const auto load_status = LoadDirectory(*program.artifact_dir, result);
      if (!load_status.ok()) return load_status;
    }

    return result;
  } catch (const Metrics::exception&) {
    return Invalid("artifact JSON");
  } catch (const fs::filesystem_error& error) {
    return IoError(error.what());
  }
}

absl::StatusOr<std::size_t> ArtifactStore::Cleanup(const std::set<std::string>& protected_directories) const {
  if (!cleanup_enabled_) return std::size_t{0};

  try {
    const auto root_status = fs::symlink_status(root_);
    if (!fs::exists(root_status)) return std::size_t{0};
    if (!fs::is_directory(root_status)) return IoError("cleanup root must be a directory without a symlink");

    std::set<fs::path> protected_paths;
    for (const auto& directory : protected_directories) {
      if (directory.find('\0') != std::string::npos || !utils::IsValidUtf8(directory)) {
        return Invalid("protected directory path");
      }
      protected_paths.insert(fs::weakly_canonical(directory));
    }

    std::size_t removed = 0;
    const auto now = fs::file_time_type::clock::now();
    const auto retention_seconds = static_cast<std::int64_t>(retention_days_) * 86400;
    for (const auto& entry : fs::directory_iterator(root_)) {
      const auto& directory = entry.path();
      if (!ManagedName(directory) || !fs::is_directory(entry.symlink_status()) ||
          protected_paths.count(fs::canonical(directory))) {
        continue;
      }

      const double age = std::chrono::duration<double>(now - fs::last_write_time(directory)).count();
      if (age <= retention_seconds) continue;
      if (!fs::is_regular_file(fs::symlink_status(directory / kManifest))) continue;

      absl::StatusOr<Metrics> manifest = Invalid("unrecognized manifest");
      try {
        manifest = ReadManifest(directory);
      } catch (const Metrics::exception&) {
        continue;
      }
      if (!manifest.ok()) {
        if (absl::IsInvalidArgument(manifest.status())) continue;
        return manifest.status();
      }

      std::set<std::string> expected_files{kManifest};
      for (const auto& artifact : (*manifest)["artifacts"]) expected_files.insert(artifact["file"].get<std::string>());

      bool owns_contents = true;
      for (const auto& file : fs::directory_iterator(directory)) {
        if (!fs::is_regular_file(file.symlink_status()) || expected_files.erase(file.path().filename().string()) != 1) {
          owns_contents = false;
          break;
        }
      }
      if (!owns_contents || !expected_files.empty()) continue;

      fs::remove_all(directory);
      ++removed;
    }

    return removed;
  } catch (const fs::filesystem_error& error) {
    return IoError(error.what());
  }
}
}  // namespace ievolve
