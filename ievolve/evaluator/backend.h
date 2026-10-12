#ifndef IEVOLVE_EVALUATOR_BACKEND_H_
#define IEVOLVE_EVALUATOR_BACKEND_H_

#include <filesystem>
#include <functional>

#include "absl/status/statusor.h"
#include "ievolve/evaluator/evaluation_outcome.h"

namespace ievolve::evaluator {

struct EvaluationBackend {
  // Receives the materialized project root. Injected runners must support
  // concurrent calls and return explicit verdicts.
  std::function<absl::StatusOr<EvaluationOutcome>(const std::filesystem::path&)> run;
};

}  // namespace ievolve::evaluator

#endif  // IEVOLVE_EVALUATOR_BACKEND_H_
