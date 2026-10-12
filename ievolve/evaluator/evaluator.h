#ifndef IEVOLVE_EVALUATOR_EVALUATOR_H_
#define IEVOLVE_EVALUATOR_EVALUATOR_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ievolve/config/types.h"
#include "ievolve/evaluator/backend.h"
#include "ievolve/evaluator/command_evaluator.h"
#include "ievolve/llm/ensemble.h"
#include "ievolve/program/source_files.h"
#include "ievolve/prompt/prompt_sampler.h"

namespace ievolve::evaluator {

struct EvaluationInput {
  SourceFiles files;
  std::string id;
};

struct LLMFeedbackOptions {
  PromptConfig prompt;
  std::vector<LLMModelConfig> models;
  LLMFactory factory;
  std::vector<std::string> feature_dimensions;
  std::uint32_t random_seed = 0;
};

struct EvaluatorOptions {
  // Base project copied for each evaluation, with the candidate's files written
  // over the copy. Absolute; empty means the project holds only those files.
  std::filesystem::path project_dir;
  process::ProcessRunner runner = process::RunProcess;
  LLMFeedbackOptions feedback;
};

class Evaluator {
 public:
  // Snapshots ENABLE_ARTIFACTS before building the command backend.
  static absl::StatusOr<std::unique_ptr<Evaluator>> Create(const EvaluatorConfig& config, EvaluationBackend backend,
                                                           EvaluatorOptions options = {});
  static absl::StatusOr<std::unique_ptr<Evaluator>> Create(const CandidateConfig& candidate,
                                                           const EvaluatorConfig& config,
                                                           const std::filesystem::path& base_directory,
                                                           EvaluatorOptions options = {});

  // Thread-safe. Injected backends enforce their own timeout and must be
  // thread-safe.
  absl::StatusOr<EvaluationOutcome> Evaluate(const EvaluationInput& input);

 private:
  static absl::StatusOr<std::unique_ptr<Evaluator>> Prepare(const EvaluatorConfig& config, EvaluatorOptions options);
  Evaluator(EvaluatorConfig config, EvaluationBackend backend, EvaluatorOptions options);
  absl::StatusOr<EvaluationOutcome> EvaluateImpl(const EvaluationInput& input);
  absl::Status ApplyFeedback(const EvaluationInput& input, EvaluationResult& result);

  EvaluatorConfig config_;
  EvaluationBackend backend_;
  EvaluatorOptions options_;
  std::unique_ptr<LLMEnsemble> feedback_;
  std::unique_ptr<PromptSampler> sampler_;
  std::vector<double> feedback_weights_;
  std::mutex sampler_mutex_;
};

}  // namespace ievolve::evaluator
#endif  // IEVOLVE_EVALUATOR_EVALUATOR_H_
