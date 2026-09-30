#include "ievolve/prompt/prompt_sampler.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <utility>

#include "absl/strings/ascii.h"
#include "absl/strings/str_join.h"
#include "ievolve/utils/text.h"

namespace ievolve {
namespace {

// ASCII-only, unlike utils::Trim, which follows Python str.isspace() and also
// strips U+00A0, U+3000 and friends. Prompt sections are trimmed for layout, so
// ASCII is enough here; the name keeps the two from being confused.
std::string TrimAscii(const std::string& text) { return std::string(absl::StripAsciiWhitespace(text)); }

bool HasValue(const std::optional<std::string>& value) { return value && !value->empty(); }

bool Enabled(const std::optional<int>& value) { return value && *value > 0; }

absl::Status ValidateMetrics(const Metrics& metrics) {
  if (!metrics.is_object()) {
    return absl::InvalidArgumentError("Metrics must be an object");
  }

  for (const auto& item : metrics.items()) {
    if (item.value().is_structured() || item.value().is_binary() || item.value().is_discarded()) {
      return absl::InvalidArgumentError("Metric must be a scalar: " + item.key());
    }
  }

  return absl::OkStatus();
}

absl::Status ValidateRequest(const PromptRequest& request) {
  auto status = ValidateMetrics(request.program_metrics);
  if (!status.ok()) return status;
  if (!request.extra_values.is_object()) {
    return absl::InvalidArgumentError("Extra template values must be an object");
  }

  for (const auto* programs : {&request.previous_programs, &request.top_programs, &request.inspirations}) {
    for (const auto& program : *programs) {
      status = ValidateMetrics(program.metrics);
      if (!status.ok()) return status;
      status = ValidateMetrics(program.parent_metrics);
      if (!status.ok()) return status;
    }
  }

  return absl::OkStatus();
}

absl::StatusOr<std::string> RenderTemplate(const TemplateManager& manager, const std::string& name,
                                           const Metrics& values) {
  auto text = manager.GetTemplate(name);
  if (!text.ok()) return text.status();

  return TemplateManager::Format(*text, values);
}

std::string ProgramText(const PromptProgram& program, bool use_changes) {
  if (!use_changes) return program.code;

  return HasValue(program.changes_description) ? *program.changes_description : "<missing changes_description>";
}

}  // namespace

// The constructor cannot fail, so configuration and template-directory errors
// are latched into initialization_status_ and surfaced by the first BuildPrompt
// call. A template directory that does not exist is not an error: the embedded
// defaults stand. One that exists but cannot be read is, and the latched status
// makes sure that never degrades into silently using the defaults instead.
PromptSampler::PromptSampler(PromptConfig config, std::uint32_t random_seed)
    : config_(std::move(config)), random_(random_seed) {
  if (config_.num_top_programs < 0 || config_.num_diverse_programs < 0 || config_.max_artifact_bytes < 0) {
    initialization_status_ = absl::InvalidArgumentError("Counts must be nonnegative");
    return;
  }

  for (const auto value : {config_.suggest_simplification_after_chars, config_.include_changes_under_chars,
                           config_.concise_implementation_max_lines, config_.comprehensive_implementation_min_lines,
                           config_.code_length_threshold}) {
    if (value && *value < 0) {
      initialization_status_ = absl::InvalidArgumentError("Thresholds must be nonnegative");
      return;
    }
  }

  if (config_.template_dir && !config_.template_dir->empty()) {
    initialization_status_ = template_manager_.LoadDirectory(*config_.template_dir);
  }
}

void PromptSampler::SetTemplates(std::optional<std::string> system_template, std::optional<std::string> user_template) {
  system_template_override_ = std::move(system_template);
  user_template_override_ = std::move(user_template);
}

// Assembles the system and user prompts from templates, fragments and the
// caller's context.
//
// Template selection is a precedence chain, weakest first: the diff or
// full-rewrite default, then a SetTemplates override, then the request's own
// template_key. The system message is a template name when one is registered
// under it and otherwise literal text, which is what lets a caller supply a
// system prompt inline without registering anything.
absl::StatusOr<Prompt> PromptSampler::BuildPrompt(const PromptRequest& request) {
  if (!initialization_status_.ok()) return initialization_status_;

  const auto status = ValidateRequest(request);
  if (!status.ok()) return status;

  std::string user_key = request.diff_based_evolution ? "diff_user" : "full_rewrite_user";
  if (HasValue(user_template_override_)) user_key = *user_template_override_;
  if (HasValue(request.template_key)) user_key = *request.template_key;
  auto user_template = template_manager_.GetTemplate(user_key);
  if (!user_template.ok()) return user_template.status();

  std::string system = config_.system_message;
  if (HasValue(system_template_override_) || template_manager_.HasTemplate(system)) {
    auto text =
        template_manager_.GetTemplate(HasValue(system_template_override_) ? *system_template_override_ : system);
    if (!text.ok()) return text.status();
    system = std::move(*text);
  }

  if (config_.programs_as_changes_description) {
    auto instructions = HasValue(config_.system_message_changes_description)
                            ? absl::StatusOr<std::string>(TrimAscii(*config_.system_message_changes_description))
                            : template_manager_.GetTemplate("system_message_changes_description");
    if (!instructions.ok()) return instructions.status();

    auto wrapped = RenderTemplate(template_manager_, "system_message_with_changes_description",
                                  {{"system_message", system}, {"system_message_changes_description", *instructions}});
    if (!wrapped.ok()) return wrapped.status();
    system = std::move(*wrapped);
  }

  auto history = FormatHistory(request);
  if (!history.ok()) return history.status();

  std::string artifacts;
  if (config_.include_artifacts) {
    artifacts = RenderArtifacts(request.program_artifacts, config_.max_artifact_bytes, config_.artifact_security_filter,
                                template_manager_.GetFragment("artifact_title"));
  }

  if (config_.use_template_stochasticity) {
    *user_template = ApplyVariations(std::move(*user_template));
  }

  Metrics values = {
      {"metrics", FormatMetrics(request.program_metrics)},
      {"fitness_score", FormatValue(GetFitnessScore(request.program_metrics, request.feature_dimensions), 4)},
      {"feature_coords", FormatFeatureCoordinates(request.program_metrics, request.feature_dimensions)},
      {"feature_dimensions",
       request.feature_dimensions.empty() ? "None" : absl::StrJoin(request.feature_dimensions, ", ")},
      {"improvement_areas", IdentifyImprovementAreas(request)},
      {"evolution_history", *history},
      {"current_program", request.current_program},
      {"language", request.language},
      {"artifacts", artifacts}};
  for (const auto& item : request.extra_values.items()) {
    if (values.contains(item.key())) {
      return absl::InvalidArgumentError("Reserved template value: " + item.key());
    }
    values[item.key()] = item.value();
  }

  auto user = TemplateManager::Format(*user_template, values);
  if (!user.ok()) return user.status();
  if (config_.programs_as_changes_description) {
    user = RenderTemplate(template_manager_, "user_message_with_changes_description",
                          {{"user_message", *user},
                           {"changes_description",
                            std::string(absl::StripTrailingAsciiWhitespace(request.current_changes_description))}});
    if (!user.ok()) return user.status();
  }

  return Prompt{std::move(system), std::move(*user)};
}

std::string PromptSampler::IdentifyImprovementAreas(const PromptRequest& request) const {
  std::vector<std::string> areas;
  const double current = GetFitnessScore(request.program_metrics, request.feature_dimensions);
  if (!request.previous_programs.empty()) {
    const double previous = GetFitnessScore(request.previous_programs.back().metrics, request.feature_dimensions);
    std::string key;
    if (current > previous) {
      key = "fitness_improved";
    } else if (current < previous) {
      key = "fitness_declined";
    } else if (std::abs(current - previous) < 1e-6) {
      key = "fitness_stable";
    }
    if (!key.empty()) {
      areas.push_back(template_manager_.GetFragment(key, {{"prev", previous}, {"current", current}}));
    }
  }

  if (!request.feature_dimensions.empty()) {
    const auto coordinates = FormatFeatureCoordinates(request.program_metrics, request.feature_dimensions);
    areas.push_back(template_manager_.GetFragment(coordinates.empty() ? "no_feature_coordinates" : "exploring_region",
                                                  {{"features", coordinates}}));
  }

  const auto threshold = Enabled(config_.suggest_simplification_after_chars)
                             ? config_.suggest_simplification_after_chars
                             : config_.code_length_threshold;
  if (Enabled(threshold) && utils::Utf8Length(request.current_program) > static_cast<std::size_t>(*threshold)) {
    areas.push_back(template_manager_.GetFragment("code_too_long", {{"threshold", *threshold}}));
  }

  if (areas.empty()) areas.push_back(template_manager_.GetFragment("no_specific_guidance"));

  return "- " + absl::StrJoin(areas, "\n- ");
}

absl::StatusOr<std::string> PromptSampler::FormatProgram(const PromptProgram& program, const PromptRequest& request,
                                                         const std::string& number, bool diverse) const {
  auto features = program.key_features;
  if (features.empty()) {
    for (const auto& metric : program.metrics.items()) {
      if (diverse && features.size() == 2) break;
      features.push_back(
          template_manager_.GetFragment(diverse ? "diverse_program_metrics_prefix" : "top_program_metrics_prefix") +
          " " + metric.key() + (diverse ? "" : " (" + FormatValue(metric.value(), 4) + ")"));
    }
  }

  return RenderTemplate(template_manager_, "top_program",
                        {{"program_number", number},
                         {"score", FormatValue(GetFitnessScore(program.metrics, request.feature_dimensions), 4)},
                         {"language", config_.programs_as_changes_description ? "text" : request.language},
                         {"program_snippet", ProgramText(program, config_.programs_as_changes_description)},
                         {"key_features", absl::StrJoin(features, ", ")}});
}

absl::StatusOr<std::string> PromptSampler::FormatHistory(const PromptRequest& request) {
  std::string attempts;
  const auto count = request.previous_programs.size();
  for (std::size_t offset = 0; offset < std::min<std::size_t>(3, count); ++offset) {
    const auto& program = request.previous_programs[count - offset - 1];
    const auto changes = HasValue(program.changes_description)
                             ? *program.changes_description
                             : program.changes.value_or(template_manager_.GetFragment("attempt_unknown_changes"));

    bool improved = true;
    bool regressed = true;
    bool compared = false;
    for (const auto& metric : program.metrics.items()) {
      const auto parent = program.parent_metrics.find(metric.key());
      const Metrics parent_value = parent == program.parent_metrics.end() ? Metrics(0) : *parent;
      if (!IsNumeric(metric.value()) || !IsNumeric(parent_value)) continue;

      compared = true;
      const int comparison = CompareMetricNumbers(metric.value(), parent_value);
      improved = improved && comparison > 0;
      regressed = regressed && comparison < 0;
    }

    const auto outcome = compared && improved    ? "attempt_all_metrics_improved"
                         : compared && regressed ? "attempt_all_metrics_regressed"
                                                 : "attempt_mixed_metrics";
    auto text = RenderTemplate(template_manager_, "previous_attempt",
                               {{"attempt_number", count - offset},
                                {"changes", changes},
                                {"performance", FormatMetrics(program.metrics, false)},
                                {"outcome", template_manager_.GetFragment(outcome)}});
    if (!text.ok()) return text.status();
    attempts += *text + "\n\n";
  }

  std::string programs;
  std::set<std::string> shown_ids;
  const auto top_count = std::min<std::size_t>(config_.num_top_programs, request.top_programs.size());
  for (std::size_t i = 0; i < top_count; ++i) {
    const auto& program = request.top_programs[i];
    auto text = FormatProgram(program, request, std::to_string(i + 1), false);
    if (!text.ok()) return text.status();
    programs += *text + "\n\n";
    if (program.id) shown_ids.insert(*program.id);
  }

  std::vector<std::size_t> remaining(request.top_programs.size() - top_count);
  std::iota(remaining.begin(), remaining.end(), top_count);
  const auto diverse_count = std::min<std::size_t>(config_.num_diverse_programs, remaining.size());
  if (diverse_count > 0) {
    std::shuffle(remaining.begin(), remaining.end(), random_);
    programs += "\n\n## " + template_manager_.GetFragment("diverse_programs_title") + "\n\n";
    for (std::size_t i = 0; i < diverse_count; ++i) {
      const auto& program = request.top_programs[remaining[i]];
      auto text = FormatProgram(program, request, "D" + std::to_string(i + 1), true);
      if (!text.ok()) return text.status();
      programs += *text + "\n\n";
      if (program.id) shown_ids.insert(*program.id);
    }
  }

  std::vector<const PromptProgram*> inspirations;
  for (const auto& program : request.inspirations) {
    if (!program.id || shown_ids.count(*program.id) == 0) inspirations.push_back(&program);
  }
  auto inspiration_text = FormatInspirations(inspirations, request);
  if (!inspiration_text.ok()) return inspiration_text.status();

  return RenderTemplate(template_manager_, "evolution_history",
                        {{"previous_attempts", TrimAscii(attempts)},
                         {"top_programs", TrimAscii(programs)},
                         {"inspirations_section", *inspiration_text}});
}

absl::StatusOr<std::string> PromptSampler::FormatInspirations(const std::vector<const PromptProgram*>& programs,
                                                              const PromptRequest& request) const {
  if (programs.empty()) return std::string();

  std::string text;
  for (std::size_t i = 0; i < programs.size(); ++i) {
    const auto& program = *programs[i];
    auto rendered =
        RenderTemplate(template_manager_, "inspiration_program",
                       {{"program_number", i + 1},
                        {"score", FormatValue(GetFitnessScore(program.metrics, request.feature_dimensions), 4)},
                        {"program_type", DetermineProgramType(program, request.feature_dimensions)},
                        {"language", config_.programs_as_changes_description ? "text" : request.language},
                        {"program_snippet", ProgramText(program, config_.programs_as_changes_description)},
                        {"unique_features", ExtractUniqueFeatures(program)}});
    if (!rendered.ok()) return rendered.status();
    text += *rendered + "\n\n";
  }

  return RenderTemplate(template_manager_, "inspirations_section", {{"inspiration_programs", TrimAscii(text)}});
}

std::string PromptSampler::DetermineProgramType(const PromptProgram& program,
                                                const std::vector<std::string>& feature_dimensions) const {
  std::string key;
  if (program.diverse) {
    key = "diverse";
  } else if (program.migrant) {
    key = "migrant";
  } else if (program.random) {
    key = "random";
  } else {
    const double score = GetFitnessScore(program.metrics, feature_dimensions);
    if (score >= 0.8) {
      key = "score_high_performer";
    } else if (score >= 0.6) {
      key = "score_alternative";
    } else if (score >= 0.4) {
      key = "score_experimental";
    } else {
      key = "score_exploratory";
    }
  }

  return template_manager_.GetFragment("inspiration_type_" + key);
}

std::string PromptSampler::ExtractUniqueFeatures(const PromptProgram& program) const {
  std::vector<std::string> features;
  // Preserve the upstream two-stage fragment formatting, including its missing
  // argument diagnostics, so existing custom fragment behavior remains stable.
  auto fragment = [this](const std::string& key, const Metrics& values) {
    const auto text = template_manager_.GetFragment(key);
    auto rendered = TemplateManager::Format(text, values);
    return rendered.ok() ? *rendered : text;
  };

  if (program.changes && Enabled(config_.include_changes_under_chars) &&
      utils::Utf8Length(*program.changes) < static_cast<std::size_t>(*config_.include_changes_under_chars)) {
    features.push_back(fragment("inspiration_changes_prefix", {{"changes", *program.changes}}));
  }

  for (const auto& metric : program.metrics.items()) {
    if (!IsNumeric(metric.value())) continue;

    const double value = Number(metric.value());
    if (value >= 0.9) {
      features.push_back(fragment("inspiration_metrics_excellent", {{"metric_name", metric.key()}, {"value", value}}));
    } else if (value <= 0.3) {
      features.push_back(fragment("inspiration_metrics_alternative", {{"metric_name", metric.key()}}));
    }
  }

  if (!program.code.empty()) {
    const auto lower = absl::AsciiStrToLower(program.code);
    if (lower.find("class") != std::string::npos && lower.find("def __init__") != std::string::npos) {
      features.push_back(template_manager_.GetFragment("inspiration_code_with_class"));
    }

    if (lower.find("numpy") != std::string::npos || lower.find("np.") != std::string::npos) {
      features.push_back(template_manager_.GetFragment("inspiration_code_with_numpy"));
    }

    if (lower.find("for") != std::string::npos && lower.find("while") != std::string::npos) {
      features.push_back(template_manager_.GetFragment("inspiration_code_with_mixed_iteration"));
    }

    const auto lines = 1 + std::count(program.code.begin(), program.code.end(), '\n');
    if (Enabled(config_.concise_implementation_max_lines) && lines <= *config_.concise_implementation_max_lines) {
      features.push_back(template_manager_.GetFragment("inspiration_code_with_concise_line"));
    } else if (Enabled(config_.comprehensive_implementation_min_lines) &&
               lines >= *config_.comprehensive_implementation_min_lines) {
      features.push_back(template_manager_.GetFragment("inspiration_code_with_comprehensive_line"));
    }
  }

  if (features.empty()) {
    features.push_back(fragment("inspiration_no_features_postfix", {{"program_type", DetermineProgramType(program)}}));
  }

  if (features.size() > static_cast<std::size_t>(config_.num_top_programs)) {
    features.resize(config_.num_top_programs);
  }

  return absl::StrJoin(features, ", ");
}

// Substitutes one randomly chosen variation per placeholder. The choice is made
// once per placeholder and applied to every occurrence, so repeated markers
// stay consistent within a prompt. Advancing past the inserted text keeps a
// variation that itself contains the marker from recursing.
std::string PromptSampler::ApplyVariations(std::string text) {
  for (const auto& entry : config_.template_variations) {
    const auto key = "{" + entry.first + "}";
    if (entry.second.empty() || text.find(key) == std::string::npos) continue;

    std::uniform_int_distribution<std::size_t> choose(0, entry.second.size() - 1);
    const auto& variation = entry.second[choose(random_)];

    std::size_t offset = 0;
    while ((offset = text.find(key, offset)) != std::string::npos) {
      text.replace(offset, key.size(), variation);
      offset += variation.size();
    }
  }

  return text;
}

}  // namespace ievolve
