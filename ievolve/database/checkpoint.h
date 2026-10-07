#ifndef IEVOLVE_DATABASE_CHECKPOINT_H_
#define IEVOLVE_DATABASE_CHECKPOINT_H_

#include <filesystem>
#include <vector>

#include "ievolve/config/types.h"
#include "ievolve/program/program.h"

namespace ievolve {

struct CheckpointData {
  Metrics metadata = Metrics::object();
  std::vector<Program> programs;  // Insertion order is significant.
};

class Checkpoint {
 public:
  // Immutable generation directories, published by atomically replacing
  // CURRENT. Artifacts are copied into each generation; old generations remain
  // readable.
  static absl::Status Save(const std::filesystem::path& path, const CheckpointData& data,
                           const DatabaseConfig& config = {});
  // Loads the current native generation format only.
  // No partial result on invalid JSON, missing files or unsupported versions.
  static absl::StatusOr<CheckpointData> Load(const std::filesystem::path& path);
};

}  // namespace ievolve

#endif  // IEVOLVE_DATABASE_CHECKPOINT_H_
