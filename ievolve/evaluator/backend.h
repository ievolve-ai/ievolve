#ifndef IEVOLVE_EVALUATOR_BACKEND_H_
#define IEVOLVE_EVALUATOR_BACKEND_H_

#include <filesystem>
#include <functional>
#include <vector>

#include "absl/status/statusor.h"
#include "ievolve/evaluator/evaluation_result.h"

namespace ievolve::evaluator {

struct EvaluationStageResult {
  EvaluationResult result;
  // A script exception carries diagnostics in result; infrastructure and
  // protocol failures are represented by the runner's Status instead.
  bool failed = false;
};

using EvaluationStageRunner = std::function<absl::StatusOr<EvaluationStageResult>(const std::filesystem::path&, int)>;

struct EvaluationBackend {
  // Stage 0 is direct evaluate; optional stages 1..3 are evaluate_stageN.
  // Every backend supports stage 0. The runner must permit concurrent calls.
  std::vector<int> stages;
  EvaluationStageRunner run;
};

}  // namespace ievolve::evaluator

#endif  // IEVOLVE_EVALUATOR_BACKEND_H_
