#ifndef IEVOLVE_EVALUATOR_EVALUATION_OUTCOME_H_
#define IEVOLVE_EVALUATOR_EVALUATION_OUTCOME_H_

#include <string>

#include "ievolve/evaluator/evaluation_result.h"

namespace ievolve::evaluator {
struct EvaluationOutcome {
  bool passed = false;
  EvaluationResult result;
  std::string reason;

  absl::Status Validate(std::size_t max_artifact_bytes = kDefaultMaxArtifactBytes) const;
  absl::StatusOr<Metrics> ToJson() const;
  static absl::StatusOr<EvaluationOutcome> Parse(std::string_view text, bool enable_artifacts,
                                                 std::size_t max_artifact_bytes);
};
}  // namespace ievolve::evaluator
#endif  // IEVOLVE_EVALUATOR_EVALUATION_OUTCOME_H_
