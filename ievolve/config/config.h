#ifndef IEVOLVE_CONFIG_CONFIG_H_
#define IEVOLVE_CONFIG_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "ievolve/config/types.h"
#include "nlohmann/json.hpp"

namespace ievolve {

class Config {
 public:
  // Loaders normalize inheritance and validate types before returning a value.
  static absl::StatusOr<Config> FromJson(const nlohmann::ordered_json& input);
  static absl::StatusOr<Config> ParseYaml(std::string_view yaml);
  // A supplied missing file is an error. No path returns default configuration.
  // On success, source receives the original parsed document, before defaults,
  // inheritance, environment resolution or path normalization. With
  // no path it receives an empty object. Errors leave source unchanged.
  // Relative run paths resolve against the YAML directory; bare executable
  // names retain PATH lookup. ParseYaml/FromJson preserve paths as written.
  static absl::StatusOr<Config> Load(std::optional<std::filesystem::path> path = std::nullopt,
                                     nlohmann::ordered_json* source = nullptr);

  // Serialization includes resolved API keys; store output as confidential
  // data.
  nlohmann::ordered_json ToJson() const;
  absl::StatusOr<std::string> ToYaml() const;
  absl::Status Save(const std::filesystem::path& path) const;
  absl::Status Validate() const;

  // Update llm models transactionally: errors leave this configuration intact.
  absl::Status UpdateModelParams(const nlohmann::ordered_json& params, bool overwrite = false);
  absl::Status RebuildModels();

  RunSettings run = {};
  int max_iterations = 10000;
  int checkpoint_interval = 100;
  std::string log_level = "INFO";
  std::optional<std::string> log_dir = std::nullopt;
  std::optional<std::int64_t> random_seed = 42;
  std::optional<std::string> language = std::nullopt;
  std::string file_suffix = ".py";
  LLMConfig llm = {};
  PromptConfig prompt = {};
  DatabaseConfig database = {};
  EvaluatorConfig evaluator = {};
  EvolutionTraceConfig evolution_trace = {};
  bool diff_based_evolution = true;
  int max_code_length = 10000;
  bool enforce_evolve_blocks = false;
  std::string diff_pattern = "<<<<<<< SEARCH\\n(.*?)=======\\n(.*?)>>>>>>> REPLACE";
  std::optional<int> early_stopping_patience = std::nullopt;
  double convergence_threshold = 0.001;
  std::string early_stopping_metric = "combined_score";
  std::optional<int> max_tasks_per_child = std::nullopt;

 private:
  static absl::Status NormalizeLLM(LLMConfig& config, bool rebuilding = false);
  static absl::Status ApplyModelParams(LLMConfig& config, const nlohmann::ordered_json& params, bool overwrite = false);
};

}  // namespace ievolve

#endif  // IEVOLVE_CONFIG_CONFIG_H_
