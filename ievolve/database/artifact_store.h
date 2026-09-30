#ifndef IEVOLVE_DATABASE_ARTIFACT_STORE_H_
#define IEVOLVE_DATABASE_ARTIFACT_STORE_H_

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>

#include "ievolve/config/types.h"
#include "ievolve/program/artifact.h"
#include "ievolve/program/program.h"

namespace ievolve {

class ArtifactStore {
 public:
  // Resolves the root without creating directories. Threshold and retention
  // must be nonnegative. Explicit Store calls ignore ENABLE_ARTIFACTS.
  static absl::StatusOr<ArtifactStore> Create(const DatabaseConfig& config);

  // Returns a copy with replacement artifact fields; empty input clears both.
  // Old directories are never modified. Large values use a fresh directory.
  absl::StatusOr<Program> Store(const Program& program, const ArtifactMap& artifacts) const;

  // Reads typed inline JSON, managed manifests, or legacy flat directories.
  // Malformed data, unsafe files and duplicate inline/disk keys are errors.
  static absl::StatusOr<ArtifactMap> Load(const Program& program);

  // Deletes only expired managed directories under root(), excluding canonical
  // protected paths. Disabled cleanup returns zero without filesystem access.
  absl::StatusOr<std::size_t> Cleanup(const std::set<std::string>& protected_directories = {}) const;

  const std::filesystem::path& root() const { return root_; }

 private:
  ArtifactStore() = default;

  std::filesystem::path root_;
  std::size_t size_threshold_ = 0;
  bool cleanup_enabled_ = true;
  int retention_days_ = 0;
};

}  // namespace ievolve

#endif  // IEVOLVE_DATABASE_ARTIFACT_STORE_H_
