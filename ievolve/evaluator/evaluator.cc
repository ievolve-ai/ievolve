#include "ievolve/evaluator/evaluator.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <thread>
#include <utility>

#include "absl/strings/ascii.h"
#include "ievolve/program/program.h"
#include "ievolve/utils/threads.h"

namespace ievolve::evaluator {
namespace {

absl::Status ValidateSettings(const EvaluatorConfig& config, const EvaluatorOptions& options) {
  if (config.timeout <= 0 || config.max_retries < 0 || config.parallel_evaluations <= 0 ||
      config.max_artifact_storage < 0 || !std::isfinite(config.llm_feedback_weight) || config.llm_feedback_weight < 0 ||
      options.retry_delay.count() < 0) {
    return absl::InvalidArgumentError("Invalid evaluator limits or weights");
  }

  if (config.cascade_evaluation && config.cascade_thresholds.empty()) {
    return absl::InvalidArgumentError("Cascade evaluation needs a threshold");
  }
  for (double threshold : config.cascade_thresholds) {
    if (!std::isfinite(threshold)) return absl::InvalidArgumentError("Cascade thresholds must be finite");
  }

  if (config.memory_limit_mb || config.cpu_limit || config.distributed) {
    return absl::UnimplementedError(
        "Evaluator CPU/memory limits and distributed execution are "
        "unsupported");
  }

  const auto& suffix = options.file_suffix;
  if (suffix.size() < 2 || suffix.size() > 32 || suffix.front() != '.' ||
      !std::all_of(suffix.begin() + 1, suffix.end(),
                   [](unsigned char c) { return absl::ascii_isalnum(c) || c == '_'; })) {
    return absl::InvalidArgumentError(
        "Candidate suffix must be a dot followed by 1-31 ASCII letters, digits "
        "or underscores");
  }

  return absl::OkStatus();
}

// mkdtemp creates the directory with owner-only permissions atomically.
class CandidateFile {
 public:
  static absl::StatusOr<std::unique_ptr<CandidateFile>> Create(const std::string& code, const std::string& suffix) {
#if defined(_WIN32)
    return absl::UnimplementedError("Native candidate files require POSIX");
#else
    std::error_code error;
    auto root = std::filesystem::temp_directory_path(error);
    if (error) return absl::UnavailableError("Cannot locate temporary directory");

    std::string name = (root / "ievolve-evaluation-XXXXXX").string();
    if (mkdtemp(name.data()) == nullptr) return absl::UnavailableError("Cannot create candidate directory");

    auto candidate = std::unique_ptr<CandidateFile>(new CandidateFile(name, suffix));
    std::ofstream stream(candidate->path_, std::ios::binary | std::ios::trunc);
    stream.write(code.data(), static_cast<std::streamsize>(code.size()));
    stream.close();
    if (!stream) return absl::UnavailableError("Cannot write candidate file");

    return candidate;
#endif
  }
  ~CandidateFile() {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  CandidateFile(std::filesystem::path directory, const std::string& suffix)
      : directory_(std::move(directory)), path_(directory_ / ("candidate" + suffix)) {}
  std::filesystem::path directory_;
  std::filesystem::path path_;
};

class Admission {
 public:
  Admission(std::mutex& mutex, std::condition_variable& cv, std::size_t& active, std::size_t limit)
      : mutex_(mutex), cv_(cv), active_(active) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return active_ < limit; });
    ++active_;
  }
  ~Admission() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --active_;
    }

    cv_.notify_one();
  }

 private:
  std::mutex& mutex_;
  std::condition_variable& cv_;
  std::size_t& active_;
};

// A timeout is reported through the metrics, not through a Status. It is a
// verdict about the candidate (the program scores zero) rather than a failure
// of the evaluator, so it flows back to the caller as a normal result.
bool TimedOut(const EvaluationResult& result) {
  auto it = result.metrics.find("timeout");
  return it != result.metrics.end() && it->is_boolean() && it->get<bool>();
}

// Diagnostic payloads use only the remaining budget; they must not replace a
// valid program result with a capacity error. Successful script artifacts
// remain strict: their complete payload must fit the configured limit.
void AppendDiagnostics(ArtifactMap& artifacts, const ArtifactMap& diagnostics, std::size_t limit) {
  const auto size = [](const ArtifactValue& value) {
    return std::visit([](const auto& payload) { return payload.size(); }, value);
  };

  std::size_t total = 0;
  for (const auto& [key, value] : artifacts) total += size(value);

  for (const auto& [key, value] : diagnostics) {
    const auto old = artifacts.find(key);
    const auto old_size = old == artifacts.end() ? 0 : size(old->second);
    const auto available = limit - (total - old_size);
    if (available == 0) continue;

    ArtifactValue bounded = value;
    if (size(value) > available) {
      if (auto* text = std::get_if<std::string>(&bounded)) {
        std::size_t end = available;
        while (end > 0 && (static_cast<unsigned char>((*text)[end]) & 0xc0) == 0x80) --end;
        text->resize(end);
      } else {
        std::get<ArtifactBytes>(bounded).resize(available);
      }
    }

    total = total - old_size + size(bounded);
    artifacts[key] = std::move(bounded);
  }
}

// Records a failure without discarding what already succeeded. A direct
// evaluation failing yields just {"error": 0}; a cascade stage failing keeps
// every score the earlier stages produced and adds stageN_passed = 0, so a
// candidate that cleared stage 1 and died in stage 2 still carries stage 1's
// metrics. Diagnostics are appended only within the remaining artifact budget.
void AddFailure(EvaluationResult& result, const EvaluationStageResult& failure, int stage, bool artifacts,
                std::size_t limit) {
  const bool timeout = TimedOut(failure.result);
  if (stage == 0) {
    result.metrics = {{"error", 0}};
  } else {
    result.metrics["stage" + std::to_string(stage) + "_passed"] = 0;
    if (stage == 1) result.metrics["error"] = 0;
  }
  if (timeout) result.metrics["timeout"] = true;

  if (artifacts) {
    AppendDiagnostics(result.artifacts, failure.result.artifacts, limit);
    AppendDiagnostics(result.artifacts,
                      {{"failure_stage", stage == 0 ? "evaluation" : "stage" + std::to_string(stage)}}, limit);
  }
}

// Extracts the JSON object from a model's feedback reply. A fenced ```json
// block wins; otherwise the outermost braces are taken, which tolerates a model
// that wraps its object in prose.
absl::StatusOr<Metrics> FeedbackJson(const std::string& text) {
  auto start = text.find("```json\n");
  std::string json;
  if (start != std::string::npos) {
    start += 8;
    const auto end = text.find("\n```", start);
    if (end == std::string::npos) return absl::InvalidArgumentError("Unclosed feedback JSON fence");
    json = text.substr(start, end - start);
  } else {
    start = text.find('{');
    const auto end = text.rfind('}');
    if (start == std::string::npos || end < start) {
      return absl::InvalidArgumentError("Feedback contains no JSON object");
    }
    json = text.substr(start, end - start + 1);
  }

  auto value = Metrics::parse(json, nullptr, false);
  if (!value.is_object()) return absl::InvalidArgumentError("Feedback must be a JSON object");

  return value;
}

}  // namespace

Evaluator::Evaluator(EvaluatorConfig config, EvaluationBackend backend, EvaluatorOptions options)
    : config_(std::move(config)), backend_(std::move(backend)), options_(std::move(options)) {}

absl::StatusOr<std::unique_ptr<Evaluator>> Evaluator::Prepare(const EvaluatorConfig& config, EvaluatorOptions options) {
  auto status = ValidateSettings(config, options);
  if (!status.ok()) return status;

  try {
    auto evaluator = std::unique_ptr<Evaluator>(new Evaluator(config, {}, std::move(options)));
    if (const char* enabled = std::getenv("ENABLE_ARTIFACTS")) {
      evaluator->config_.enable_artifacts &= absl::AsciiStrToLower(enabled) == "true";
    }

    auto& settings = evaluator->options_;
    settings.python.timeout = std::chrono::seconds(config.timeout);
    settings.python.max_artifact_bytes = config.max_artifact_storage;
    settings.python.enable_artifacts = evaluator->config_.enable_artifacts;

    if (config.use_llm_feedback) {
      auto ensemble =
          LLMEnsemble::Create(settings.feedback.models, settings.feedback.factory, config.parallel_evaluations);
      if (!ensemble.ok()) return ensemble.status();
      evaluator->feedback_ = std::move(*ensemble);

      double maximum = 0;
      for (const auto& model : settings.feedback.models) maximum = std::max(maximum, model.weight);
      double sum = 0;
      for (const auto& model : settings.feedback.models) sum += model.weight / maximum;
      for (const auto& model : settings.feedback.models)
        evaluator->feedback_weights_.push_back((model.weight / maximum) / sum);

      evaluator->sampler_ = std::make_unique<PromptSampler>(settings.feedback.prompt, settings.feedback.random_seed);
      evaluator->sampler_->SetTemplates(settings.feedback.prompt.evaluator_system_message);
    }

    return evaluator;
  } catch (...) {
    return absl::InternalError("Evaluator initialization threw an exception");
  }
}

absl::StatusOr<std::unique_ptr<Evaluator>> Evaluator::Create(const EvaluatorConfig& config, EvaluationBackend backend,
                                                             EvaluatorOptions options) {
  if (!backend.run) return absl::InvalidArgumentError("Evaluator backend is empty");

  std::set<int> stages;
  for (int stage : backend.stages) {
    if (stage < 1 || stage > 3 || !stages.insert(stage).second) {
      return absl::InvalidArgumentError("Backend stages must be a unique subset of 1,2,3");
    }
  }

  auto evaluator = Prepare(config, std::move(options));
  if (!evaluator.ok()) return evaluator.status();
  (*evaluator)->backend_ = std::move(backend);

  return evaluator;
}

absl::StatusOr<std::unique_ptr<Evaluator>> Evaluator::Create(const EvaluatorConfig& config,
                                                             const std::filesystem::path& evaluation_file,
                                                             EvaluatorOptions options) {
  auto evaluator = Prepare(config, std::move(options));
  if (!evaluator.ok()) return evaluator.status();

  auto backend = PythonEvaluator::Create(evaluation_file, (*evaluator)->options_.python);
  if (!backend.ok()) return backend.status();

  (*evaluator)->backend_ = std::move(*backend);
  return evaluator;
}

bool Evaluator::HasStage(int stage) const {
  return std::find(backend_.stages.begin(), backend_.stages.end(), stage) != backend_.stages.end();
}

// Runs one stage, where stage 0 means direct evaluation. A deadline becomes a
// failed result carrying timeout = true; every other backend status propagates,
// because it means the evaluator itself could not run rather than that the
// candidate is bad.
absl::StatusOr<EvaluationStageResult> Evaluator::RunStage(const std::filesystem::path& candidate, int stage) {
  auto result = backend_.run(candidate, stage);
  if (!result.ok()) {
    if (!absl::IsDeadlineExceeded(result.status())) return result.status();

    EvaluationStageResult failure;
    failure.failed = true;
    failure.result.metrics = {{"timeout", true}};
    if (config_.enable_artifacts) failure.result.artifacts["error_type"] = "timeout";

    return failure;
  }

  if (!config_.enable_artifacts) result->result.artifacts.clear();
  auto status = result->result.Validate(config_.max_artifact_storage);
  if (!status.ok()) return status;

  return result;
}

// Runs stages 1..3, stopping as soon as one is missing, has no threshold, or
// the accumulated result misses the previous threshold. The point of cascading
// is that expensive stages never see a candidate that already looks weak.
//
// Stage 1 establishes the result; later stages are merged into it, so a stage
// can overwrite a numeric metric an earlier one set. A failing stage records
// the failure and breaks, keeping the scores collected so far.
absl::StatusOr<EvaluationResult> Evaluator::RunCascade(const std::filesystem::path& candidate) {
  EvaluationResult result;
  for (int stage = 1; stage <= 3; ++stage) {
    if (stage > 1 && (!HasStage(stage) || config_.cascade_thresholds.size() < static_cast<std::size_t>(stage - 1) ||
                      !result.PassesThreshold(config_.cascade_thresholds[stage - 2]))) {
      break;
    }

    auto next = RunStage(candidate, stage);
    if (!next.ok()) return next.status();
    if (next->failed) {
      AddFailure(result, *next, stage, config_.enable_artifacts, config_.max_artifact_storage);
      break;
    }

    result = stage == 1 ? std::move(next->result) : EvaluationResult::Merge(result, next->result);
    auto status = result.Validate(config_.max_artifact_storage);
    if (!status.ok()) return status;
  }

  return result;
}

// One evaluation, including retries and optional model feedback.
//
// Retries are deliberately narrow: only a direct evaluation retries, and only
// when it failed for a reason other than a timeout. A cascade failure never
// retries, because stages have already consumed real work, and a timeout will
// simply time out again. Every attempt gets a fresh candidate file, so no state
// leaks between them.
absl::StatusOr<EvaluationResult> Evaluator::EvaluateImpl(const EvaluationInput& input) {
  Program program;
  program.id = input.id.empty() ? "candidate" : input.id;
  program.code = input.code;
  program.language = input.language;
  auto status = program.Validate();
  if (!status.ok()) return status;

  for (int attempt = 0;; ++attempt) {
    auto candidate = CandidateFile::Create(input.code, options_.file_suffix);
    if (!candidate.ok()) return candidate.status();

    EvaluationResult result;
    if (config_.cascade_evaluation && HasStage(1)) {
      auto cascade = RunCascade((*candidate)->path());
      if (!cascade.ok()) return cascade.status();
      result = std::move(*cascade);
    } else {
      auto direct = RunStage((*candidate)->path(), 0);
      if (!direct.ok()) return direct.status();
      if (direct->failed) {
        if (!TimedOut(direct->result) && attempt < config_.max_retries) {
          candidate->reset();
          std::this_thread::sleep_for(options_.retry_delay);
          continue;
        }

        AddFailure(result, *direct, 0, config_.enable_artifacts, config_.max_artifact_storage);
      } else {
        result = std::move(direct->result);
      }
    }

    // Invalid or oversized program output must not trigger model requests.
    status = result.Validate(config_.max_artifact_storage);
    if (!status.ok()) return status;
    if (feedback_) {
      status = ApplyFeedback(input, result);
      if (!status.ok()) return status;
    }

    status = result.Validate(config_.max_artifact_storage);
    if (!status.ok()) return status;

    return result;
  }
}

absl::StatusOr<EvaluationResult> Evaluator::Evaluate(const EvaluationInput& input) {
  try {
    Admission admission(admission_mutex_, admission_cv_, active_, config_.parallel_evaluations);
    return EvaluateImpl(input);
  } catch (...) {
    return absl::InternalError("Evaluation callback or operation threw an exception");
  }
}

// Evaluates a batch. Workers pull from a shared counter, so results are filled
// by index and returned in input order regardless of completion order. Each
// worker calls Evaluate and therefore takes one admission slot, which is why
// spawning at most parallel_evaluations of them cannot deadlock against the
// same limit. JoinThreads waits for every worker before any status is read, so
// a failure is reported only once all work has stopped.
absl::StatusOr<std::vector<EvaluationResult>> Evaluator::EvaluateMultiple(const std::vector<EvaluationInput>& inputs) {
  if (inputs.empty()) return std::vector<EvaluationResult>{};

  std::vector<absl::StatusOr<EvaluationResult>> results(inputs.size());
  std::atomic<std::size_t> next{0};
  std::vector<std::thread> threads;

  try {
    utils::JoinThreads join(threads);
    const auto count = std::min(inputs.size(), static_cast<std::size_t>(config_.parallel_evaluations));
    threads.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      threads.emplace_back([&] {
        for (;;) {
          const auto index = next.fetch_add(1);
          if (index >= inputs.size()) break;
          results[index] = Evaluate(inputs[index]);
        }
      });
    }
  } catch (...) {
    return absl::InternalError("Cannot start evaluation workers");
  }

  std::vector<EvaluationResult> output;
  output.reserve(results.size());
  for (auto& result : results) {
    if (!result.ok()) return result.status();
    output.push_back(std::move(*result));
  }

  return output;
}

absl::Status Evaluator::ApplyFeedback(const EvaluationInput& input, EvaluationResult& result) {
  const auto invalid_feedback = [&] {
    if (config_.enable_artifacts) {
      AppendDiagnostics(result.artifacts, {{"llm_feedback_error", std::string("LLM feedback could not be evaluated")}},
                        config_.max_artifact_storage);
    }
    return result.Validate(config_.max_artifact_storage);
  };

  try {
    PromptRequest prompt_request;
    prompt_request.current_program = input.code;
    prompt_request.language = input.language;
    prompt_request.template_key = "evaluation";
    prompt_request.feature_dimensions = options_.feedback.feature_dimensions;

    auto prompt = [&] {
      std::lock_guard<std::mutex> lock(sampler_mutex_);
      return sampler_->BuildPrompt(prompt_request);
    }();
    if (!prompt.ok()) return invalid_feedback();

    LLMRequest request;
    request.system_message = prompt->system;
    request.messages.push_back({"user", prompt->user});
    auto responses = feedback_->GenerateAll(request);
    if (!responses.ok()) return invalid_feedback();

    EvaluationResult feedback;
    for (std::size_t i = 0; i < responses->size(); ++i) {
      if (feedback_weights_[i] == 0) continue;

      auto value = FeedbackJson((*responses)[i].text);
      if (!value.ok()) return invalid_feedback();
      for (const auto& entry : value->items()) {
        if (IsNumeric(entry.value())) {
          const double score = Number(entry.value());
          if (!std::isfinite(score)) return invalid_feedback();

          const double previous =
              feedback.metrics.contains(entry.key()) ? feedback.metrics[entry.key()].get<double>() : 0.0;
          const double weighted = previous + score * feedback_weights_[i];
          if (!std::isfinite(weighted)) return invalid_feedback();
          feedback.metrics[entry.key()] = weighted;
        } else if (config_.enable_artifacts) {
          feedback.artifacts[entry.key()] =
              entry.value().is_string() ? entry.value().get<std::string>() : entry.value().dump();
        }
      }
    }

    auto status = feedback.Validate(config_.max_artifact_storage);
    if (!status.ok()) return status;

    EvaluationResult updated = result;
    // Divide each term before summing to avoid overflowing a finite average.
    long double average = 0;
    for (const auto& entry : feedback.metrics.items()) {
      const double value = entry.value().get<double>();
      const double scaled = value * config_.llm_feedback_weight;
      if (!std::isfinite(scaled)) return invalid_feedback();
      updated.metrics["llm_" + entry.key()] = scaled;
      average += static_cast<long double>(value) / feedback.metrics.size();
    }

    if (!feedback.metrics.empty()) {
      const double scaled = static_cast<double>(average * config_.llm_feedback_weight);
      if (!std::isfinite(scaled)) return invalid_feedback();
      updated.metrics["llm_average"] = scaled;

      const auto combined = result.metrics.find("combined_score");
      if (combined != result.metrics.end() && IsNumeric(*combined)) {
        // Guard like the values above: a long double average can be finite
        // while its double cast is not, on targets with a wider long double.
        const double merged = Number(*combined) * 0.7 + static_cast<double>(average) * 0.3;
        if (!std::isfinite(merged)) return invalid_feedback();
        updated.metrics["combined_score"] = merged;
      }
    }

    for (auto& [key, value] : feedback.artifacts) updated.artifacts[key] = std::move(value);
    status = updated.Validate(config_.max_artifact_storage);
    if (!status.ok()) return status;

    result = std::move(updated);
    return absl::OkStatus();
  } catch (...) {
    return invalid_feedback();
  }
}

}  // namespace ievolve::evaluator
