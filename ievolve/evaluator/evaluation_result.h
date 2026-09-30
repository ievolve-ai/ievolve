#ifndef IEVOLVE_EVALUATOR_EVALUATION_RESULT_H_
#define IEVOLVE_EVALUATOR_EVALUATION_RESULT_H_

#include <cstddef>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "ievolve/program/artifact.h"
#include "ievolve/program/metrics.h"

namespace ievolve::evaluator {

inline constexpr std::size_t kDefaultMaxArtifactBytes = 100 * 1024 * 1024;

struct EvaluationResult {
  Metrics metrics = Metrics::object();
  ArtifactMap artifacts;

  // Metrics contain finite JSON scalars. Artifact keys and text are UTF-8;
  // the limit applies to the sum of decoded text and binary payload bytes.
  absl::Status Validate(std::size_t max_artifact_bytes = kDefaultMaxArtifactBytes) const;
  static absl::StatusOr<EvaluationResult> FromJson(const Metrics& value,
                                                   std::size_t max_artifact_bytes = kDefaultMaxArtifactBytes);
  // Validates the representation; storage limits belong to Validate/FromJson.
  absl::StatusOr<Metrics> ToJson() const;

  std::size_t GetArtifactSize(std::string_view key) const;
  std::size_t GetTotalArtifactSize() const;

  // Uses numeric/bool combined_score, otherwise the average of numeric/bool
  // metrics except error. A score equal to the threshold passes.
  bool PassesThreshold(double threshold) const;

  // Keeps numeric/bool metrics except error as doubles. Later metrics and
  // artifacts override earlier entries with the same key.
  static EvaluationResult Merge(const EvaluationResult& earlier, const EvaluationResult& later);
};

}  // namespace ievolve::evaluator

#endif  // IEVOLVE_EVALUATOR_EVALUATION_RESULT_H_
