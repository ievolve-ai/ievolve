#include "ievolve/controller/iteration.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <set>
#include <utility>

#include "ievolve/code/code_parser.h"
#include "ievolve/code/project_editor.h"
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

// The candidate a model response turns into, or why the response is unusable.
// A rejection is a model outcome, not an error: the caller keeps the response
// and reports no child. Infrastructure failures are returned as a Status.
struct Edit {
  SourceFiles files;
  std::string description;
  std::string summary;
  std::optional<std::string> rejection;
};

Edit Rejected(std::string reason) { return Edit{{}, {}, {}, std::move(reason)}; }

// Applies the response's SEARCH/REPLACE blocks to the parent's files; a path
// line before a block names its file. With programs_as_changes_description,
// blocks may also target the changes description, which must actually change.
absl::StatusOr<Edit> EditFromDiffs(const Config& config, const std::string& response, const SourceFiles& parent,
                                   std::string description) {
  auto diffs = CodeParser::ExtractDiffs(response, config.diff_pattern);
  if (!diffs.ok()) return Rejected(std::string(diffs.status().message()));
  if (diffs->empty()) return Rejected("No valid diffs found in response");

  const bool with_description = config.prompt.programs_as_changes_description;
  auto grouped = ProjectEditor::GroupDiffs(
      *diffs, response, parent, with_description ? std::optional<std::string_view>(description) : std::nullopt);
  if (!grouped.ok()) return Rejected(std::string(grouped.status().message()));
  auto updated = ProjectEditor::ApplyDiffs(parent, *grouped);

  Edit edit;
  if (with_description) {
    auto text = CodeParser::ApplyDiffBlocks(description, grouped->changes_description);
    if (text.applied_count == 0 || utils::Trim(text.text).empty() ||
        utils::Trim(text.text) == utils::Trim(description)) {
      return Rejected("Changes description was not updated or is empty");
    }
    edit.description = std::move(text.text);
  } else {
    if (updated.applied_count == 0) return Rejected("No SEARCH block matched the parent program");
    if (updated.files == parent) return Rejected("Diff did not change the parent program");
    edit.description = std::move(description);
  }
  edit.files = std::move(updated.files);

  // Summarize every block addressed to a file, matched or not, in response order.
  std::set<std::size_t> file_blocks;
  for (const auto& [path, blocks] : grouped->files) {
    for (const auto& block : blocks) file_blocks.insert(block.begin);
  }
  std::vector<DiffBlock> summary_blocks;
  for (auto& block : *diffs) {
    if (file_blocks.count(block.begin)) summary_blocks.push_back(std::move(block));
  }
  auto formatted = CodeParser::FormatDiffSummary(summary_blocks, config.prompt.diff_summary_max_line_len,
                                                 config.prompt.diff_summary_max_lines);
  if (!formatted.ok()) return formatted.status();
  edit.summary = std::move(*formatted);

  return edit;
}

Edit EditFromRewrite(const std::string& response, const SourceFiles& parent, std::string description) {
  auto files = ProjectEditor::ParseRewrite(response, parent);
  if (!files.ok()) return Rejected(std::string(files.status().message()));
  if (*files == parent) return Rejected("Rewrite is identical to the parent program");

  return Edit{std::move(*files), std::move(description), "Full rewrite", std::nullopt};
}

// ProjectEditor::EnforceEditableBlocks, keeping CodeParser's unprefixed messages for one file.
absl::StatusOr<SourceFiles> EnforceEditable(const SourceFiles& parent, const SourceFiles& files) {
  if (parent.size() != 1) return ProjectEditor::EnforceEditableBlocks(parent, files);

  const auto& [path, code] = *files.begin();
  auto restored = CodeParser::EnforceEditableBlocks(parent.begin()->second, code);
  if (!restored.ok()) return restored.status();
  return SourceFiles{{path, std::move(*restored)}};
}

// Turns a model response into a candidate that may be evaluated, or the reason
// it may not: parse the edit, revert changes outside EVOLVE-BLOCK regions, then
// check the files' size and encoding.
absl::StatusOr<Edit> CandidateFromResponse(const Config& config, const std::string& response, const SourceFiles& parent,
                                           const std::string& description) {
  if (!utils::IsValidUtf8(response)) return Rejected("Model response is not valid UTF-8");

  auto edit = config.diff_based_evolution ? EditFromDiffs(config, response, parent, description)
                                          : EditFromRewrite(response, parent, description);
  if (!edit.ok() || edit->rejection) return edit;

  // Edits outside EVOLVE-BLOCK regions are reverted, so a candidate can become
  // identical to its parent here even though the raw response differed.
  if (config.enforce_evolve_blocks) {
    auto restored = EnforceEditable(parent, edit->files);
    if (!restored.ok()) return Rejected(std::string(restored.status().message()));
    if (*restored != edit->files) {
      if (*restored == parent) return Rejected("All edits were outside editable regions");
      edit->files = std::move(*restored);
    }
  }

  // A single file may be emptied as long as the project keeps some content.
  const auto& files = edit->files;
  if (std::all_of(files.begin(), files.end(), [](const auto& file) { return utils::Trim(file.second).empty(); })) {
    return Rejected("Generated code is empty or whitespace");
  }
  if (!std::all_of(files.begin(), files.end(), [](const auto& file) { return utils::IsValidUtf8(file.second); })) {
    return Rejected("Generated code is not valid UTF-8");
  }
  if (TotalLength(files) > static_cast<std::size_t>(config.max_code_length)) {
    return Rejected("Generated code exceeds maximum length");
  }

  // Custom ECMAScript patterns match bytes and can split a UTF-8 character in
  // descriptions or summaries even when the response and final code are valid.
  if (!utils::IsValidUtf8(edit->description) || !utils::IsValidUtf8(edit->summary)) {
    return Rejected("Generated description or summary is not valid UTF-8");
  }

  return edit;
}

Program MakeChild(const IterationInput& input, Edit edit, Metrics metrics, Metrics usage) {
  Program child;
  child.id = input.child_id;
  child.files = std::move(edit.files);
  child.changes_description = std::move(edit.description);
  child.parent_id = input.parent.id;
  child.generation = input.parent.generation + 1;
  child.iteration_found = input.iteration;
  child.metrics = std::move(metrics);

  // "changes" is the model's own summary, including diffs that matched nothing
  // or were later reverted, so it is not a guaranteed diff of the final code.
  // "island" records where the parent lived; ProgramDatabase::Add replaces it
  // with the island the child is actually admitted to.
  child.metadata = {{"changes", std::move(edit.summary)},
                    {"parent_metrics", input.parent.metrics},
                    {"island", input.parent_island},
                    {"token_usage", std::move(usage)}};
  return child;
}

absl::StatusOr<PromptProgram> ProjectProgram(const Program& program, const std::string& child_id) {
  auto status = ValidateProgram(program, child_id);
  if (!status.ok()) return status;
  return PromptProgram::FromProgram(program);
}

// The prompt's history and display lists from the parent island: the best
// num_top_programs become the history shown to the model, and top plus
// diverse are displayed. Sorting is stable so equal scores keep the caller's
// order.
absl::Status RankIslandContext(const std::vector<Program>& island, const std::string& child_id,
                               const PromptConfig& config, PromptRequest& request) {
  struct RankedProgram {
    PromptProgram program;
    Metrics score;
  };
  std::vector<RankedProgram> context;
  context.reserve(island.size());
  for (const auto& program : island) {
    auto projected = ProjectProgram(program, child_id);
    if (!projected.ok()) return projected.status();
    context.push_back({std::move(*projected), ContextScore(program)});
  }

  std::stable_sort(context.begin(), context.end(), [](const auto& left, const auto& right) {
    return CompareMetricNumbers(left.score, right.score) > 0;
  });

  const auto top_count = static_cast<std::size_t>(config.num_top_programs);
  const auto display_count = top_count + static_cast<std::size_t>(config.num_diverse_programs);
  for (std::size_t i = 0; i < context.size() && i < display_count; ++i) {
    if (i < top_count) request.previous_programs.push_back(context[i].program);
    request.top_programs.push_back(std::move(context[i].program));
  }
  return absl::OkStatus();
}

absl::StatusOr<Artifacts> ArtifactsAsText(const ArtifactMap& artifacts) {
  Artifacts result;
  for (const auto& [key, value] : artifacts) {
    if (!utils::IsValidUtf8(key)) return absl::InvalidArgumentError("Artifact names must be UTF-8");
    if (const auto* text = std::get_if<std::string>(&value)) {
      result.emplace_back(key, *text);
    } else {
      const auto& bytes = std::get<ArtifactBytes>(value);
      result.emplace_back(key, std::string(bytes.begin(), bytes.end()));
    }
  }
  return result;
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
    if (config.max_code_length < 0 || config.database.num_islands <= 0) {
      return absl::InvalidArgumentError("Invalid iteration code limit or island count");
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

  // Only the ID is under test; the placeholder file makes the rest valid.
  Program identity;
  identity.id = input.child_id;
  identity.files = {{"identity", ""}};
  auto status = identity.Validate();
  if (!status.ok()) return status;
  status = ValidateProgram(input.parent, input.child_id);
  if (!status.ok()) return status;

  PromptRequest request;
  request.current_program = input.parent.files;
  request.program_metrics = input.parent.metrics;
  request.diff_based_evolution = config_.diff_based_evolution;
  request.feature_dimensions = config_.database.feature_dimensions;
  if (config_.prompt.programs_as_changes_description) {
    request.current_changes_description = input.parent.changes_description.empty()
                                              ? config_.prompt.initial_changes_description
                                              : input.parent.changes_description;
  }

  status = RankIslandContext(input.island_programs, input.child_id, config_.prompt, request);
  if (!status.ok()) return status;

  for (const auto& program : input.inspirations) {
    auto projected = ProjectProgram(program, input.child_id);
    if (!projected.ok()) return projected.status();
    request.inspirations.push_back(std::move(*projected));
  }

  if (config_.prompt.include_artifacts) {
    auto artifacts = ArtifactsAsText(input.parent_artifacts);
    if (!artifacts.ok()) return artifacts.status();
    request.program_artifacts = std::move(*artifacts);
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
  result.prompt = std::move(*prompt);

  const auto start = std::chrono::steady_clock::now();
  const auto finish = [&]() -> IterationResult {
    result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return std::move(result);
  };

  // Two distinct outcomes below. reject() means the attempt produced no child:
  // the caller still gets the prompt, the full response, its usage and any
  // evaluation diagnostics, and Run returns OK. Returning a Status instead
  // means the infrastructure failed.
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
  auto edit =
      CandidateFromResponse(config_, result.response.text, input.parent.files, request->current_changes_description);
  if (!edit.ok()) return edit.status();
  if (edit->rejection) return reject(std::move(*edit->rejection));

  auto evaluation = evaluator_->Evaluate({edit->files, input.child_id});
  if (!evaluation.ok()) return evaluation.status();
  result.artifacts = std::move(evaluation->result.artifacts);
  if (!evaluation->passed) return reject(evaluation->reason);

  auto child = MakeChild(input, std::move(*edit), std::move(evaluation->result.metrics), std::move(*usage));
  auto status = child.Validate();
  if (!status.ok()) return status;
  result.child = std::move(child);

  return finish();
}

}  // namespace ievolve::controller
