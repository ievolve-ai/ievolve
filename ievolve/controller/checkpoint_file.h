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

  // Reads <directory>/checkpoint.json. DataLoss for malformed JSON, unknown
  // schema or progress that is inconsistent with the stored stopping policy.
  static absl::StatusOr<CheckpointFile> Read(const std::filesystem::path& directory);
  // Writes <directory>/checkpoint.json. InvalidArgument for values that would
  // not read back.
  absl::Status Write(const std::filesystem::path& directory) const;
};

}  // namespace ievolve::controller
#endif  // IEVOLVE_CONTROLLER_CHECKPOINT_FILE_H_
