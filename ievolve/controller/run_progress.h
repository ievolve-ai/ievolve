#ifndef IEVOLVE_CONTROLLER_RUN_PROGRESS_H_
#define IEVOLVE_CONTROLLER_RUN_PROGRESS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "absl/status/statusor.h"
#include "ievolve/program/metrics.h"

namespace ievolve::controller {

// Early-stopping settings a run was started with. Positive patience stops after
// that many accepted children without an improvement of at least threshold;
// non-positive patience stops when the score equals threshold exactly; no
// patience disables early stopping.
struct StoppingPolicy {
  std::optional<int> patience;
  std::string metric;
  double threshold = 0;

  // {"patience", "metric", "threshold"}, in that order.
  Metrics ToJson() const;
  // Structural validation only; DataLoss names the offending field.
  static absl::StatusOr<StoppingPolicy> FromJson(const Metrics& json);
};
bool operator==(const StoppingPolicy& left, const StoppingPolicy& right);
inline bool operator!=(const StoppingPolicy& left, const StoppingPolicy& right) { return !(left == right); }

// Attempt counters and early-stopping state of one evolutionary run. A copyable
// value: Controller::Step updates a copy and commits it with the database. Its
// invariants are maintained by the Record methods and checked by Restore, so a
// restored run continues exactly as an uninterrupted one would.
class RunProgress {
 public:
  // Plain snapshot of the state, mirroring the checkpoint "progress" object.
  struct Counters {
    std::int64_t iteration = 0;
    std::int64_t accepted = 0;
    std::int64_t rejected = 0;
    std::int64_t without_improvement = 0;
    std::optional<Metrics> best_score;
    bool early_stopped = false;
  };

  explicit RunProgress(StoppingPolicy policy) : policy_(std::move(policy)) {}
  // DataLoss for counters that no run under policy could have produced.
  static absl::StatusOr<RunProgress> Restore(StoppingPolicy policy, const Counters& counters);

  void RecordRejected();
  // Children whose metrics yield no stopping score leave early-stopping state
  // unchanged.
  void RecordAccepted(const Metrics& child_metrics);

  std::int64_t iteration() const { return iteration_; }
  std::int64_t accepted() const { return accepted_; }
  std::int64_t rejected() const { return iteration_ - accepted_; }
  bool early_stopped() const { return early_stopped_; }
  const StoppingPolicy& policy() const { return policy_; }
  Counters counters() const;

 private:
  void TrackScore(const Metrics& child_metrics);

  StoppingPolicy policy_;
  std::int64_t iteration_ = 0;
  std::int64_t accepted_ = 0;
  std::int64_t without_improvement_ = 0;
  std::optional<Metrics> best_score_;
  bool early_stopped_ = false;
};
bool operator==(const RunProgress::Counters& left, const RunProgress::Counters& right);

}  // namespace ievolve::controller
#endif  // IEVOLVE_CONTROLLER_RUN_PROGRESS_H_
