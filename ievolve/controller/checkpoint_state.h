#ifndef IEVOLVE_CONTROLLER_CHECKPOINT_STATE_H_
#define IEVOLVE_CONTROLLER_CHECKPOINT_STATE_H_

#include <filesystem>

#include "absl/status/statusor.h"
#include "ievolve/program/metrics.h"

namespace ievolve::controller {
struct ControllerProgress {
  std::int64_t iteration = 0;
  std::int64_t accepted = 0;
  std::int64_t rejected = 0;
  std::int64_t without_improvement = 0;
  std::optional<Metrics> best_score;
  bool early_stopped = false;
};
struct CheckpointState {
  std::string language;
  std::string file_suffix;
  std::string best_id;
  ControllerProgress progress;
  Metrics stopping_policy;
};
absl::StatusOr<CheckpointState> ReadCheckpointState(const std::filesystem::path& directory);
absl::Status WriteCheckpointState(const std::filesystem::path& directory, const CheckpointState& state);
}  // namespace ievolve::controller
#endif  // IEVOLVE_CONTROLLER_CHECKPOINT_STATE_H_
