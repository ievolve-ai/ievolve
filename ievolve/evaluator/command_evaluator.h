#ifndef IEVOLVE_EVALUATOR_COMMAND_EVALUATOR_H_
#define IEVOLVE_EVALUATOR_COMMAND_EVALUATOR_H_

#include "ievolve/config/types.h"
#include "ievolve/evaluator/command_template.h"
#include "ievolve/evaluator/evaluation_outcome.h"
#include "ievolve/evaluator/workspace.h"
#include "ievolve/process/process.h"

namespace ievolve::evaluator {
struct CommandEvaluatorOptions {
  std::filesystem::path base_directory;
  process::ProcessRunner runner = process::RunProcess;
};

// The fixed workspace is owned by the backend; callers own a distinct
// materialized project, inside its own workspace, for each Run. The runner
// must support concurrent calls.
class CommandEvaluator {
 public:
  static absl::StatusOr<std::shared_ptr<CommandEvaluator>> Create(CandidateConfig candidate, EvaluatorConfig evaluator,
                                                                  CommandEvaluatorOptions options);
  absl::StatusOr<EvaluationOutcome> Run(const std::filesystem::path& project) const;

 private:
  CommandEvaluator(CandidateConfig candidate, EvaluatorConfig evaluator, CommandEvaluatorOptions options,
                   std::unique_ptr<EvaluationWorkspace> workspace);
  CandidateConfig candidate_;
  EvaluatorConfig evaluator_;
  CommandEvaluatorOptions options_;
  std::unique_ptr<EvaluationWorkspace> workspace_;
  std::vector<std::string> evaluator_argv_;
};
}  // namespace ievolve::evaluator
#endif  // IEVOLVE_EVALUATOR_COMMAND_EVALUATOR_H_
