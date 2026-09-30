#ifndef IEVOLVE_EVALUATOR_EVALUATOR_H_
#define IEVOLVE_EVALUATOR_EVALUATOR_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ievolve/config/types.h"
#include "ievolve/evaluator/backend.h"
#include "ievolve/evaluator/python_evaluator.h"
#include "ievolve/llm/ensemble.h"
#include "ievolve/prompt/prompt_sampler.h"

namespace ievolve::evaluator {

struct EvaluationInput {
  std::string code;
  std::string id;
  std::string language = "python";
};

struct LLMFeedbackOptions {
  PromptConfig prompt;
  std::vector<LLMModelConfig> models;
  LLMFactory factory;
  std::vector<std::string> feature_dimensions;
  std::uint32_t random_seed = 0;
};

struct EvaluatorOptions {
  std::string file_suffix = ".py";
  PythonEvaluatorOptions python;
  std::chrono::milliseconds retry_delay{1000};
  LLMFeedbackOptions feedback;
};

class Evaluator {
 public:
  // Snapshots ENABLE_ARTIFACTS. Unsupported resource/distributed settings fail
  // here. The file overload uses config.timeout (seconds) for every subprocess.
  static absl::StatusOr<std::unique_ptr<Evaluator>> Create(const EvaluatorConfig& config, EvaluationBackend backend,
                                                           EvaluatorOptions options = {});
  static absl::StatusOr<std::unique_ptr<Evaluator>> Create(const EvaluatorConfig& config,
                                                           const std::filesystem::path& evaluation_file,
                                                           EvaluatorOptions options = {});

  // Thread-safe; at most parallel_evaluations calls run on this instance at
  // once. Injected backends enforce their own timeout and must be thread-safe.
  absl::StatusOr<EvaluationResult> Evaluate(const EvaluationInput& input);
  // Preserves input order and joins all workers before returning an error.
  absl::StatusOr<std::vector<EvaluationResult>> EvaluateMultiple(const std::vector<EvaluationInput>& inputs);

 private:
  static absl::StatusOr<std::unique_ptr<Evaluator>> Prepare(const EvaluatorConfig& config, EvaluatorOptions options);
  Evaluator(EvaluatorConfig config, EvaluationBackend backend, EvaluatorOptions options);
  absl::StatusOr<EvaluationResult> EvaluateImpl(const EvaluationInput& input);
  absl::StatusOr<EvaluationStageResult> RunStage(const std::filesystem::path& candidate, int stage);
  absl::StatusOr<EvaluationResult> RunCascade(const std::filesystem::path& candidate);
  absl::Status ApplyFeedback(const EvaluationInput& input, EvaluationResult& result);
  bool HasStage(int stage) const;

  EvaluatorConfig config_;
  EvaluationBackend backend_;
  EvaluatorOptions options_;
  std::unique_ptr<LLMEnsemble> feedback_;
  std::unique_ptr<PromptSampler> sampler_;
  std::vector<double> feedback_weights_;
  std::mutex sampler_mutex_;
  std::mutex admission_mutex_;
  std::condition_variable admission_cv_;
  std::size_t active_ = 0;
};

}  // namespace ievolve::evaluator
#endif  // IEVOLVE_EVALUATOR_EVALUATOR_H_
