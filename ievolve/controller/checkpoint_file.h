#ifndef IEVOLVE_CONTROLLER_CHECKPOINT_FILE_H_
#define IEVOLVE_CONTROLLER_CHECKPOINT_FILE_H_

#include <filesystem>
#include <string>

#include "absl/status/statusor.h"
#include "ievolve/controller/run_progress.h"

namespace ievolve::controller {

// checkpoint.json: the controller half of a checkpoint directory. The other
// half, database/, is the population written by ProgramDatabase::Save; this
// file holds what the database must not know about.
struct CheckpointFile {
  std::string language;
  std::string file_suffix;
  std::string best_id;
  // Carries the stopping policy the run was saved under.
  RunProgress progress;
};

// DataLoss for malformed JSON, unknown schema or progress that is inconsistent
// with the stored stopping policy.
absl::StatusOr<CheckpointFile> ReadCheckpointFile(const std::filesystem::path& directory);
absl::Status WriteCheckpointFile(const std::filesystem::path& directory, const CheckpointFile& file);

}  // namespace ievolve::controller
#endif  // IEVOLVE_CONTROLLER_CHECKPOINT_FILE_H_
