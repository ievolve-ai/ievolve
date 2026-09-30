#include "ievolve/controller/iteration.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <utility>

#include "ievolve/code/code_parser.h"
#include "ievolve/utils/text.h"

namespace ievolve::controller {
namespace {

absl::Status ValidateProgram(const Program& program, const std::string& child_id) {
  auto status = program.Validate();
  if (!status.ok()) return status;

  if (program.id == child_id) return absl::InvalidArgumentError("Candidate ID collides with supplied context");
  if (!std::isfinite(GetFitnessScore(program.metrics))) {
    return absl::InvalidArgumentError("Context fitness must be finite");
  }

  return absl::OkStatus();
}

// Ranks island context for the prompt. Deliberately mirrors the Python worker:
// the fallback averages every numeric metric without excluding feature
// dimensions, unlike the fitness the prompt later displays.
Metrics ContextScore(const Program& program) {
  const auto combined = program.metrics.find("combined_score");
  if (combined != program.metrics.end() && (combined->is_number() || combined->is_boolean())) return *combined;

  return GetFitnessScore(program.metrics);
}

absl::StatusOr<Metrics> UsageJson(const LLMResponse& response) {
  if (!utils::IsValidUtf8(response.model) || !utils::IsValidUtf8(response.provider)) {
    return absl::DataLossError("Invalid LLM response metadata");
  }
  if (!response.usage) return Metrics(nullptr);

  const auto& usage = *response.usage;
  for (const auto& count : {usage.input_tokens, usage.output_tokens, usage.cached_input_tokens}) {
    if (count && *count < 0) return absl::DataLossError("LLM usage counters must be nonnegative");
  }
  if (usage.cost_usd && (!std::isfinite(*usage.cost_usd) || *usage.cost_usd < 0)) {
    return absl::DataLossError("LLM usage cost must be finite and nonnegative");
  }

  Metrics result = Metrics::object();
  if (usage.input_tokens) result["input_tokens"] = *usage.input_tokens;
  if (usage.output_tokens) result["output_tokens"] = *usage.output_tokens;
  if (usage.cached_input_tokens) result["cached_input_tokens"] = *usage.cached_input_tokens;
  if (usage.cost_usd) result["cost_usd"] = *usage.cost_usd;

  return result;
}

// Truncates the 64-bit configured seed to the sampler's 32-bit seed. Runs stay
// reproducible under one C++ standard library; they never match Python's RNG.
std::uint32_t PromptSeed(const Config& config) {
  return config.random_seed ? static_cast<std::uint32_t>(*config.random_seed) : std::random_device{}();
}

}  // namespace

IterationRunner::IterationRunner(Config config, std::shared_ptr<const LLMInterface> llm,
                                 std::shared_ptr<evaluator::Evaluator> evaluator)
    : config_(std::move(config)),
      llm_(std::move(llm)),
      evaluator_(std::move(evaluator)),
      sampler_(config_.prompt, PromptSeed(config_)) {}

absl::StatusOr<std::unique_ptr<IterationRunner>> IterationRunner::Create(
    const Config& config, std::shared_ptr<const LLMInterface> llm, std::shared_ptr<evaluator::Evaluator> evaluator) {
  if (!llm || !evaluator) return absl::InvalidArgumentError("Iteration requires an LLM and evaluator");

  try {
    auto status = config.Validate();
    if (!status.ok()) return status;

    // Serializing the whole config here surfaces invalid UTF-8 in any field at
    // construction time instead of midway through a run.
    (void)config.ToJson().dump();
    if (config.max_code_length < 0 || config.database.num_islands <= 0 ||
        (config.language && config.language->empty())) {
      return absl::InvalidArgumentError("Invalid iteration code limit, island count or language");
    }

    if (config.diff_based_evolution) {
      // Dry-run the diff pattern and summary limits so a bad regex or limit
      // fails here rather than after a model call has already been paid for.
      auto diffs = CodeParser::ExtractDiffs("", config.diff_pattern);
      if (!diffs.ok()) return diffs.status();
      auto summary = CodeParser::FormatDiffSummary({}, config.prompt.diff_summary_max_line_len,
                                                   config.prompt.diff_summary_max_lines);
      if (!summary.ok()) return summary.status();
    }

    return std::unique_ptr<IterationRunner>(new IterationRunner(config, std::move(llm), std::move(evaluator)));
  } catch (const Metrics::exception&) {
    return absl::InvalidArgumentError("Iteration configuration must contain valid UTF-8 and JSON values");
  } catch (...) {
    return absl::InternalError("Iteration initialization threw an exception");
  }
}

// Validates the caller-supplied context and projects it into a PromptRequest.
// Everything that can be rejected without spending a model call is checked
// here: island bounds, generation overflow, ID collisions and context fitness.
absl::StatusOr<PromptRequest> IterationRunner::PreparePrompt(const IterationInput& input) const {
  if (input.iteration < 0 || input.parent.generation == std::numeric_limits<std::int64_t>::max() ||
      input.parent_island < 0 || input.parent_island >= config_.database.num_islands ||
      (input.target_island && (*input.target_island < 0 || *input.target_island >= config_.database.num_islands))) {
    return absl::InvalidArgumentError("Invalid iteration, generation or island");
  }

  Program identity;
  identity.id = input.child_id;
  auto status = identity.Validate();
  if (!status.ok()) return status;
  status = ValidateProgram(input.parent, input.child_id);
  if (!status.ok()) return status;

  PromptRequest request;
  request.current_program = input.parent.code;
  request.program_metrics = input.parent.metrics;
  request.language = config_.language.value_or(input.parent.language);
  if (request.language.empty()) return absl::InvalidArgumentError("Iteration language must not be empty");
  request.diff_based_evolution = config_.diff_based_evolution;
  request.feature_dimensions = config_.database.feature_dimensions;
  if (config_.prompt.programs_as_changes_description) {
    request.current_changes_description = input.parent.changes_description.empty()
                                              ? config_.prompt.initial_changes_description
                                              : input.parent.changes_description;
  }

  struct RankedProgram {
    PromptProgram program;
    Metrics score;
  };
  std::vector<RankedProgram> context;
  context.reserve(input.island_programs.size());
  for (const auto& program : input.island_programs) {
    status = ValidateProgram(program, input.child_id);
    if (!status.ok()) return status;
    auto projected = PromptProgram::FromProgram(program);
    if (!projected.ok()) return projected.status();
    context.push_back({std::move(*projected), ContextScore(program)});
  }

  // Stable so equal scores keep the caller's order. The best num_top_programs
  // become the history shown to the model; top plus diverse are displayed.
  std::stable_sort(context.begin(), context.end(), [](const auto& left, const auto& right) {
    return CompareMetricNumbers(left.score, right.score) > 0;
  });

  const auto top_count = static_cast<std::size_t>(config_.prompt.num_top_programs);
  const auto display_count = top_count + static_cast<std::size_t>(config_.prompt.num_diverse_programs);
  for (std::size_t i = 0; i < context.size() && i < display_count; ++i) {
    if (i < top_count) request.previous_programs.push_back(context[i].program);
    request.top_programs.push_back(std::move(context[i].program));
  }

  for (const auto& program : input.inspirations) {
    status = ValidateProgram(program, input.child_id);
    if (!status.ok()) return status;
    auto projected = PromptProgram::FromProgram(program);
    if (!projected.ok()) return projected.status();
    request.inspirations.push_back(std::move(*projected));
  }

  if (config_.prompt.include_artifacts) {
    for (const auto& [key, value] : input.parent_artifacts) {
      if (!utils::IsValidUtf8(key)) return absl::InvalidArgumentError("Artifact names must be UTF-8");
      if (const auto* text = std::get_if<std::string>(&value)) {
        request.program_artifacts.emplace_back(key, *text);
      } else {
        const auto& bytes = std::get<ArtifactBytes>(value);
        request.program_artifacts.emplace_back(key, std::string(bytes.begin(), bytes.end()));
      }
    }
  }

  return request;
}

absl::StatusOr<IterationResult> IterationRunner::Run(const IterationInput& input) {
  try {
    return RunImpl(input);
  } catch (...) {
    return absl::InternalError("Iteration dependency or operation threw an exception");
  }
}

absl::StatusOr<IterationResult> IterationRunner::RunImpl(const IterationInput& input) {
  auto request = PreparePrompt(input);
  if (!request.ok()) return request.status();

  auto prompt = [&] {
    std::lock_guard<std::mutex> lock(sampler_mutex_);
    return sampler_.BuildPrompt(*request);
  }();
  if (!prompt.ok()) return prompt.status();

  IterationResult result;
  result.parent_id = input.parent.id;
  result.iteration = input.iteration;
  result.target_island = input.target_island;
  result.prompt = std::move(*prompt);

  const auto start = std::chrono::steady_clock::now();
  const auto finish = [&]() -> IterationResult {
    result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return std::move(result);
  };

  // Two distinct outcomes below. reject() means the model produced an unusable
  // candidate: the caller still gets the prompt, the full response and its
  // usage, the attempt is never evaluated, and Run returns OK with no child.
  // Returning a Status instead means the infrastructure failed.
  const auto reject = [&](std::string reason) -> IterationResult {
    result.rejection_reason = std::move(reason);
    return finish();
  };

  LLMRequest model_request;
  model_request.system_message = result.prompt.system;
  model_request.messages.push_back({"user", result.prompt.user});
  auto response = llm_->Generate(model_request);
  if (!response.ok()) return response.status();
  result.response = std::move(*response);

  auto usage = UsageJson(result.response);
  if (!usage.ok()) return usage.status();
  if (!utils::IsValidUtf8(result.response.text)) return reject("Model response is not valid UTF-8");

  std::string code;
  std::string description = request->current_changes_description;
  std::string summary;
  if (config_.diff_based_evolution) {
    auto diffs = CodeParser::ExtractDiffs(result.response.text, config_.diff_pattern);
    if (!diffs.ok()) return reject(std::string(diffs.status().message()));
    if (diffs->empty()) return reject("No valid diffs found in response");

    std::vector<DiffBlock> summary_blocks;
    if (config_.prompt.programs_as_changes_description) {
      auto targets = CodeParser::SplitDiffsByTarget(*diffs, input.parent.code, description);
      if (!targets.ok()) return reject(std::string(targets.status().message()));
      code = CodeParser::ApplyDiffBlocks(input.parent.code, targets->code).text;

      auto updated = CodeParser::ApplyDiffBlocks(description, targets->changes_description);
      if (updated.applied_count == 0 || utils::Trim(updated.text).empty() ||
          utils::Trim(updated.text) == utils::Trim(description)) {
        return reject("Changes description was not updated or is empty");
      }
      description = std::move(updated.text);
      summary_blocks = std::move(targets->code);
    } else {
      auto updated = CodeParser::ApplyDiffBlocks(input.parent.code, *diffs);
      if (updated.applied_count == 0) return reject("No SEARCH block matched the parent program");
      code = std::move(updated.text);
      if (code == input.parent.code) return reject("Diff did not change the parent program");
      summary_blocks = std::move(*diffs);
    }

    auto formatted = CodeParser::FormatDiffSummary(summary_blocks, config_.prompt.diff_summary_max_line_len,
                                                   config_.prompt.diff_summary_max_lines);
    if (!formatted.ok()) return formatted.status();
    summary = std::move(*formatted);
  } else {
    code = CodeParser::ParseFullRewrite(result.response.text, request->language);
    if (code == input.parent.code) return reject("Rewrite is identical to the parent program");
    summary = "Full rewrite";
  }

  // Edits outside EVOLVE-BLOCK regions are reverted, so a candidate can become
  // identical to its parent here even though the raw response differed.
  if (config_.enforce_evolve_blocks) {
    auto restored = CodeParser::EnforceEditableBlocks(input.parent.code, code);
    if (!restored.ok()) return reject(std::string(restored.status().message()));
    if (*restored != code) {
      code = std::move(*restored);
      if (code == input.parent.code) return reject("All edits were outside editable regions");
    }
  }

  if (utils::Trim(code).empty()) return reject("Generated code is empty or whitespace");
  if (!utils::IsValidUtf8(code)) return reject("Generated code is not valid UTF-8");
  if (utils::Utf8Length(code) > static_cast<std::size_t>(config_.max_code_length)) {
    return reject("Generated code exceeds maximum length");
  }

  // Custom ECMAScript patterns match bytes and can split a UTF-8 character in
  // descriptions or summaries even when the response and final code are valid.
  if (!utils::IsValidUtf8(description) || !utils::IsValidUtf8(summary)) {
    return reject("Generated description or summary is not valid UTF-8");
  }

  auto evaluation = evaluator_->Evaluate({code, input.child_id, request->language});
  if (!evaluation.ok()) return evaluation.status();

  Program child;
  child.id = input.child_id;
  child.code = std::move(code);
  child.changes_description = std::move(description);
  child.language = request->language;
  child.parent_id = input.parent.id;
  child.generation = input.parent.generation + 1;
  child.iteration_found = input.iteration;
  child.metrics = std::move(evaluation->metrics);

  // "changes" is the model's own summary, including diffs that matched nothing
  // or were later reverted, so it is not a guaranteed diff of the final code.
  // "island" records where the parent lived; ProgramDatabase::Add replaces it
  // with the island the child is actually admitted to.
  child.metadata = {{"changes", summary},
                    {"parent_metrics", input.parent.metrics},
                    {"island", input.parent_island},
                    {"token_usage", std::move(*usage)}};

  auto status = child.Validate();
  if (!status.ok()) return status;

  result.child = std::move(child);
  result.artifacts = std::move(evaluation->artifacts);

  return finish();
}

}  // namespace ievolve::controller
