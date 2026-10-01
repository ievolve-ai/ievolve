#ifndef IEVOLVE_CONFIG_TYPES_H_
#define IEVOLVE_CONFIG_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "nlohmann/ordered_map.hpp"

namespace ievolve {

// Application settings are plain configuration data. The CLI translates them
// into evaluator, model-client and controller options without dependencies here.
struct RunSettings {
  std::optional<std::string> initial_program = std::nullopt;
  std::optional<std::string> evaluation_file = std::nullopt;
  std::string output_directory = "ievolve_output";
  std::optional<std::string> checkpoint = std::nullopt;
  std::optional<double> target_score = std::nullopt;
  std::string python_executable = "python3";
  std::string codex_executable = "codex";
  std::string claude_executable = "claude";
};

struct LLMModelConfig {
  std::optional<std::string> api_base = std::nullopt;
  std::optional<std::string> api_key = std::nullopt;
  std::optional<std::string> name = std::nullopt;
  std::optional<std::string> provider = std::nullopt;
  std::nullptr_t init_client = nullptr;
  double weight = 1.0;
  std::optional<std::string> system_message = std::nullopt;
  std::optional<double> temperature = std::nullopt;
  std::optional<double> top_p = std::nullopt;
  std::optional<int> max_tokens = std::nullopt;
  std::optional<int> timeout = std::nullopt;
  std::optional<int> retries = std::nullopt;
  std::optional<int> retry_delay = std::nullopt;
  std::optional<std::int64_t> random_seed = std::nullopt;
  std::optional<std::string> reasoning_effort = std::nullopt;
  std::optional<double> max_ai_credits = std::nullopt;
  std::optional<bool> allow_all_tools = std::nullopt;
  std::optional<bool> manual_mode = std::nullopt;
  std::optional<std::string> _manual_queue_dir = std::nullopt;
};

struct LLMConfig : LLMModelConfig {
  LLMConfig() {
    api_base = "https://api.openai.com/v1";
    system_message = "system_message";

    temperature = 0.7;
    top_p = std::nullopt;
    max_tokens = 4096;

    timeout = 60;
    retries = 3;
    retry_delay = 5;

    reasoning_effort = std::nullopt;
    manual_mode = false;
  }
  std::vector<LLMModelConfig> models = {};
  std::vector<LLMModelConfig> evaluator_models = {};
  std::optional<std::string> primary_model = std::nullopt;
  std::optional<double> primary_model_weight = std::nullopt;
  std::optional<std::string> secondary_model = std::nullopt;
  std::optional<double> secondary_model_weight = std::nullopt;
};

// ordered_map stores const keys, so assigning its underlying vector is invalid.
// Copy-and-swap preserves ordinary value semantics for containing
// configurations.
class TemplateVariations : public nlohmann::ordered_map<std::string, std::vector<std::string>> {
 public:
  using Base = nlohmann::ordered_map<std::string, std::vector<std::string>>;
  using Base::Base;
  TemplateVariations() = default;
  TemplateVariations(const TemplateVariations&) = default;
  TemplateVariations(TemplateVariations&&) noexcept = default;
  TemplateVariations& operator=(TemplateVariations other) noexcept {
    swap(other);
    return *this;
  }
};

struct PromptConfig {
  std::optional<std::string> template_dir = std::nullopt;
  std::string system_message = "system_message";
  std::string evaluator_system_message = "evaluator_system_message";
  bool programs_as_changes_description = false;
  std::optional<std::string> system_message_changes_description = std::nullopt;
  std::string initial_changes_description = "";
  int num_top_programs = 3;
  int num_diverse_programs = 2;
  bool use_template_stochasticity = true;
  // Variations expand in declaration order and may refer to later entries.
  TemplateVariations template_variations = {};
  bool use_meta_prompting = false;
  double meta_prompt_weight = 0.1;
  bool include_artifacts = true;
  int max_artifact_bytes = 20 * 1024;
  bool artifact_security_filter = true;
  std::optional<int> suggest_simplification_after_chars = 500;
  std::optional<int> include_changes_under_chars = 100;
  std::optional<int> concise_implementation_max_lines = 10;
  std::optional<int> comprehensive_implementation_min_lines = 50;
  int diff_summary_max_line_len = 100;
  int diff_summary_max_lines = 30;
  std::optional<int> code_length_threshold = std::nullopt;
};

struct DatabaseConfig {
  // Storage: checkpoint to auto-load / default Save target, prompt logging.
  std::optional<std::string> db_path = std::nullopt;
  bool log_prompts = true;

  // Population: capacities, parent-selection ratios and the RNG seed.
  int population_size = 1000;
  int archive_size = 100;
  int num_islands = 5;
  double elite_selection_ratio = 0.1;
  double exploration_ratio = 0.2;
  double exploitation_ratio = 0.7;
  std::optional<std::int64_t> random_seed = 42;

  // Island migration.
  int migration_interval = 50;
  double migration_rate = 0.1;

  // MAP-Elites feature grid.
  std::vector<std::string> feature_dimensions = {"complexity", "diversity"};
  std::variant<int, std::map<std::string, int>> feature_bins = 10;
  int diversity_reference_size = 20;

  // Artifact storage.
  std::optional<std::string> artifacts_base_path = std::nullopt;
  int artifact_size_threshold = 32 * 1024;
  bool cleanup_old_artifacts = true;
  int artifact_retention_days = 30;
};

struct EvaluatorConfig {
  int timeout = 300;
  int max_retries = 3;
  std::optional<int> memory_limit_mb = std::nullopt;
  std::optional<double> cpu_limit = std::nullopt;
  bool cascade_evaluation = true;
  std::vector<double> cascade_thresholds = {0.5, 0.75, 0.9};
  int parallel_evaluations = 1;
  bool distributed = false;
  bool use_llm_feedback = false;
  double llm_feedback_weight = 0.1;
  bool enable_artifacts = true;
  int max_artifact_storage = 100 * 1024 * 1024;
};

struct EvolutionTraceConfig {
  bool enabled = false;
  std::string format = "jsonl";
  bool include_code = false;
  bool include_prompts = true;
  std::optional<std::string> output_path = std::nullopt;
  int buffer_size = 10;
  bool compress = false;
};

}  // namespace ievolve

#endif  // IEVOLVE_CONFIG_TYPES_H_
