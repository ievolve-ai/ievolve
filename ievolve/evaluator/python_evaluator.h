#ifndef IEVOLVE_EVALUATOR_PYTHON_EVALUATOR_H_
#define IEVOLVE_EVALUATOR_PYTHON_EVALUATOR_H_

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

#include "absl/status/statusor.h"
#include "ievolve/evaluator/backend.h"
#include "ievolve/process/process.h"

namespace ievolve::evaluator {

struct PythonEvaluatorOptions {
  std::string python_executable = "python3";
  std::chrono::milliseconds timeout{300000};
  std::size_t max_output_bytes = 8 * 1024 * 1024;
  std::size_t max_artifact_bytes = kDefaultMaxArtifactBytes;
  bool enable_artifacts = true;
  process::ProcessRunner runner = process::RunProcess;
};

class PythonEvaluator {
 public:
  // Inspection and each stage execute in a separate bounded child process.
  // Requires Python 3.9+; evaluator dependencies must be installed separately.
  static absl::StatusOr<EvaluationBackend> Create(const std::filesystem::path& evaluation_file,
                                                  PythonEvaluatorOptions options = {});
};

}  // namespace ievolve::evaluator

#endif  // IEVOLVE_EVALUATOR_PYTHON_EVALUATOR_H_
