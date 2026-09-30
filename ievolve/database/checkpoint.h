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
  bool legacy = false;            // Python directory import; never emitted by Save.
};

class Checkpoint {
 public:
  // Immutable generation directories, published by atomically replacing
  // CURRENT. Artifacts are copied into each generation; old generations remain
  // readable.
  static absl::Status Save(const std::filesystem::path& path, const CheckpointData& data,
                           const DatabaseConfig& config = {});
  // Native generations or Python metadata.json + programs/*.json directories.
  // No partial result on invalid JSON, missing files or unsupported versions.
  static absl::StatusOr<CheckpointData> Load(const std::filesystem::path& path);
};

}  // namespace ievolve

#endif  // IEVOLVE_DATABASE_CHECKPOINT_H_
