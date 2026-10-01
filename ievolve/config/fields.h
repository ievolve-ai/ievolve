#ifndef IEVOLVE_CONFIG_FIELDS_H_
#define IEVOLVE_CONFIG_FIELDS_H_

#include <type_traits>

#include "ievolve/config/config.h"

namespace ievolve::config_internal {

// One field inventory drives both decoding and encoding. Everything that walks
// a configuration - reading YAML or JSON, writing it back, and validating a
// parameter mapping against the known field set - goes through these visitors,
// so a field cannot be added to one direction and forgotten in the other. The
// declaration order here is also the emitted order, which is why the JSON type
// is ordered_json.
template <typename T, typename F>
void VisitModelFields(T& value, F field) {
  field("api_base", value.api_base);
  field("api_key", value.api_key);
  field("name", value.name);
  field("provider", value.provider);
  field("init_client", value.init_client);
  field("weight", value.weight);

  field("system_message", value.system_message);
  field("temperature", value.temperature);
  field("top_p", value.top_p);
  field("max_tokens", value.max_tokens);

  field("timeout", value.timeout);
  field("retries", value.retries);
  field("retry_delay", value.retry_delay);

  field("random_seed", value.random_seed);
  field("reasoning_effort", value.reasoning_effort);
  field("max_ai_credits", value.max_ai_credits);

  field("allow_all_tools", value.allow_all_tools);
  field("manual_mode", value.manual_mode);
  field("_manual_queue_dir", value._manual_queue_dir);
}

template <typename T, typename F>
void VisitFields(T& value, F field) {
  using U = std::remove_const_t<T>;

  if constexpr (std::is_same_v<U, RunSettings>) {
    field("initial_program", value.initial_program);
    field("evaluation_file", value.evaluation_file);
    field("output_directory", value.output_directory);
    field("checkpoint", value.checkpoint);
    field("target_score", value.target_score);

    field("python_executable", value.python_executable);
    field("codex_executable", value.codex_executable);
    field("claude_executable", value.claude_executable);
  } else if constexpr (std::is_same_v<U, LLMModelConfig>) {
    VisitModelFields(value, field);
  } else if constexpr (std::is_same_v<U, LLMConfig>) {
    VisitModelFields(value, field);

    field("models", value.models);
    field("evaluator_models", value.evaluator_models);

    field("primary_model", value.primary_model);
    field("primary_model_weight", value.primary_model_weight);
    field("secondary_model", value.secondary_model);
    field("secondary_model_weight", value.secondary_model_weight);
  } else if constexpr (std::is_same_v<U, PromptConfig>) {
    field("template_dir", value.template_dir);
    field("system_message", value.system_message);
    field("evaluator_system_message", value.evaluator_system_message);

    field("programs_as_changes_description", value.programs_as_changes_description);
    field("system_message_changes_description", value.system_message_changes_description);
    field("initial_changes_description", value.initial_changes_description);

    field("num_top_programs", value.num_top_programs);
    field("num_diverse_programs", value.num_diverse_programs);
    field("use_template_stochasticity", value.use_template_stochasticity);
    field("template_variations", value.template_variations);

    field("use_meta_prompting", value.use_meta_prompting);
    field("meta_prompt_weight", value.meta_prompt_weight);

    field("include_artifacts", value.include_artifacts);
    field("max_artifact_bytes", value.max_artifact_bytes);
    field("artifact_security_filter", value.artifact_security_filter);

    field("suggest_simplification_after_chars", value.suggest_simplification_after_chars);
    field("include_changes_under_chars", value.include_changes_under_chars);
    field("concise_implementation_max_lines", value.concise_implementation_max_lines);
    field("comprehensive_implementation_min_lines", value.comprehensive_implementation_min_lines);

    field("diff_summary_max_line_len", value.diff_summary_max_line_len);
    field("diff_summary_max_lines", value.diff_summary_max_lines);
    field("code_length_threshold", value.code_length_threshold);
  } else if constexpr (std::is_same_v<U, DatabaseConfig>) {
    field("db_path", value.db_path);
    field("log_prompts", value.log_prompts);

    field("population_size", value.population_size);
    field("archive_size", value.archive_size);
    field("num_islands", value.num_islands);

    field("elite_selection_ratio", value.elite_selection_ratio);
    field("exploration_ratio", value.exploration_ratio);
    field("exploitation_ratio", value.exploitation_ratio);

    field("diversity_metric", value.diversity_metric);
    field("feature_dimensions", value.feature_dimensions);
    field("feature_bins", value.feature_bins);
    field("diversity_reference_size", value.diversity_reference_size);

    field("migration_interval", value.migration_interval);
    field("migration_rate", value.migration_rate);

    field("random_seed", value.random_seed);

    field("artifacts_base_path", value.artifacts_base_path);
    field("artifact_size_threshold", value.artifact_size_threshold);
    field("cleanup_old_artifacts", value.cleanup_old_artifacts);
    field("artifact_retention_days", value.artifact_retention_days);
    field("max_snapshot_artifacts", value.max_snapshot_artifacts);
  } else if constexpr (std::is_same_v<U, EvaluatorConfig>) {
    field("timeout", value.timeout);
    field("max_retries", value.max_retries);

    field("memory_limit_mb", value.memory_limit_mb);
    field("cpu_limit", value.cpu_limit);

    field("cascade_evaluation", value.cascade_evaluation);
    field("cascade_thresholds", value.cascade_thresholds);

    field("parallel_evaluations", value.parallel_evaluations);
    field("distributed", value.distributed);

    field("use_llm_feedback", value.use_llm_feedback);
    field("llm_feedback_weight", value.llm_feedback_weight);

    field("enable_artifacts", value.enable_artifacts);
    field("max_artifact_storage", value.max_artifact_storage);
  } else if constexpr (std::is_same_v<U, EvolutionTraceConfig>) {
    field("enabled", value.enabled);
    field("format", value.format);

    field("include_code", value.include_code);
    field("include_prompts", value.include_prompts);
    field("output_path", value.output_path);

    field("buffer_size", value.buffer_size);
    field("compress", value.compress);
  } else if constexpr (std::is_same_v<U, Config>) {
    field("run", value.run);

    field("max_iterations", value.max_iterations);
    field("checkpoint_interval", value.checkpoint_interval);

    field("log_level", value.log_level);
    field("log_dir", value.log_dir);

    field("random_seed", value.random_seed);
    field("language", value.language);
    field("file_suffix", value.file_suffix);

    field("llm", value.llm);
    field("prompt", value.prompt);
    field("database", value.database);
    field("evaluator", value.evaluator);
    field("evolution_trace", value.evolution_trace);

    field("diff_based_evolution", value.diff_based_evolution);
    field("max_code_length", value.max_code_length);
    field("enforce_evolve_blocks", value.enforce_evolve_blocks);
    field("diff_pattern", value.diff_pattern);

    field("early_stopping_patience", value.early_stopping_patience);
    field("convergence_threshold", value.convergence_threshold);
    field("early_stopping_metric", value.early_stopping_metric);

    field("max_tasks_per_child", value.max_tasks_per_child);
  }
}

}  // namespace ievolve::config_internal

#endif  // IEVOLVE_CONFIG_FIELDS_H_
