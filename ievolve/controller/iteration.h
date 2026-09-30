#ifndef IEVOLVE_CONTROLLER_ITERATION_H_
#define IEVOLVE_CONTROLLER_ITERATION_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ievolve/config/config.h"
#include "ievolve/evaluator/evaluator.h"
#include "ievolve/llm/base.h"
#include "ievolve/program/program.h"
#include "ievolve/prompt/prompt_sampler.h"

namespace ievolve::controller {

struct IterationInput {
  Program parent;
  std::vector<Program> island_programs;
  std::vector<Program> inspirations;
  ArtifactMap parent_artifacts;
  // Reserve a unique ID before dispatch. The runner checks supplied context;
  // the caller remains responsible for uniqueness across the full database.
  std::string child_id;
  std::int64_t iteration = 0;
  int parent_island = 0;
  std::optional<int> target_island;
};

struct IterationResult {
  // A rejected edit has no child and a nonempty reason. It retains its prompt,
  // model response and usage, and never reaches evaluation.
  std::optional<Program> child;
  std::string rejection_reason;
  std::string parent_id;
  std::int64_t iteration = 0;
  std::optional<int> target_island;
  Prompt prompt;
  LLMResponse response;
  ArtifactMap artifacts;
  double elapsed_seconds = 0;
};

class IterationRunner {
 public:
  // Dependencies are already configured and shared; LLMEnsemble implements
  // LLMInterface. Uses Config's iteration, prompt and island/context settings.
  static absl::StatusOr<std::unique_ptr<IterationRunner>> Create(const Config& config,
                                                                 std::shared_ptr<const LLMInterface> llm,
                                                                 std::shared_ptr<evaluator::Evaluator> evaluator);

  // Thread-safe when the supplied LLM is thread-safe. Each call owns its
  // result. Does not mutate inputs or write to a ProgramDatabase.
  absl::StatusOr<IterationResult> Run(const IterationInput& input);

 private:
  IterationRunner(Config config, std::shared_ptr<const LLMInterface> llm,
                  std::shared_ptr<evaluator::Evaluator> evaluator);
  absl::StatusOr<IterationResult> RunImpl(const IterationInput& input);
  absl::StatusOr<PromptRequest> PreparePrompt(const IterationInput& input) const;

  Config config_;
  std::shared_ptr<const LLMInterface> llm_;
  std::shared_ptr<evaluator::Evaluator> evaluator_;
  PromptSampler sampler_;
  std::mutex sampler_mutex_;
};

}  // namespace ievolve::controller
#endif  // IEVOLVE_CONTROLLER_ITERATION_H_
