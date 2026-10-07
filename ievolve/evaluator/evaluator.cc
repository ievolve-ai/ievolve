#include "ievolve/evaluator/evaluator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "absl/strings/ascii.h"
#include "ievolve/program/program.h"

namespace ievolve::evaluator {
namespace {

absl::Status ValidateSettings(const EvaluatorConfig& config, const EvaluatorOptions& options) {
  if (config.timeout <= 0 || config.max_output_bytes <= 0 || config.max_artifact_storage < 0 ||
      !std::isfinite(config.llm_feedback_weight) || config.llm_feedback_weight < 0) {
    return absl::InvalidArgumentError("Invalid evaluator limits or weights");
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

    if (config.use_llm_feedback) {
      auto ensemble = LLMEnsemble::Create(settings.feedback.models, settings.feedback.factory);
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

  auto evaluator = Prepare(config, std::move(options));
  if (!evaluator.ok()) return evaluator.status();
  (*evaluator)->backend_ = std::move(backend);

  return evaluator;
}

absl::StatusOr<std::unique_ptr<Evaluator>> Evaluator::Create(const CandidateConfig& candidate,
                                                             const EvaluatorConfig& config,
                                                             const std::filesystem::path& base_directory,
                                                             EvaluatorOptions options) {
  auto evaluator = Prepare(config, std::move(options));
  if (!evaluator.ok()) return evaluator.status();
  auto backend =
      CommandEvaluator::Create(candidate, (*evaluator)->config_, {base_directory, (*evaluator)->options_.runner});
  if (!backend.ok()) return backend.status();
  (*evaluator)->backend_.run = [backend = std::move(*backend)](const auto& source) { return backend->Run(source); };
  return evaluator;
}

absl::StatusOr<EvaluationOutcome> Evaluator::EvaluateImpl(const EvaluationInput& input) {
  Program program;
  program.id = input.id.empty() ? "candidate" : input.id;
  program.code = input.code;
  program.language = input.language;
  auto status = program.Validate();
  if (!status.ok()) return status;
  auto workspace = EvaluationWorkspace::Create();
  if (!workspace.ok()) return workspace.status();
  auto source = (*workspace)->WriteSource(input.code, options_.file_suffix);
  if (!source.ok()) return source.status();
  auto outcome = backend_.run(*source);
  if (!outcome.ok()) return outcome.status();
  if (!config_.enable_artifacts) outcome->result.artifacts.clear();
  status = outcome->Validate(config_.max_artifact_storage);
  if (!status.ok()) return status;
  if (outcome->passed && feedback_) {
    status = ApplyFeedback(input, outcome->result);
    if (!status.ok()) return status;
  }
  status = outcome->Validate(config_.max_artifact_storage);
  if (!status.ok()) return status;
  return outcome;
}

absl::StatusOr<EvaluationOutcome> Evaluator::Evaluate(const EvaluationInput& input) {
  try {
    return EvaluateImpl(input);
  } catch (...) {
    return absl::InternalError("Evaluation callback or operation threw an exception");
  }
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
