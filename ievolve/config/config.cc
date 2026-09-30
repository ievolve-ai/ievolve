#include "ievolve/config/config.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <regex>
#include <type_traits>
#include <utility>

#include "ievolve/config/fields.h"
#include "ievolve/utils/yaml.h"

namespace ievolve {
namespace {
using Json = nlohmann::ordered_json;
using config_internal::VisitFields;

template <typename T>
struct Optional : std::false_type {};
template <typename T>
struct Optional<std::optional<T>> : std::true_type {
  using Value = T;
};
template <typename T>
struct Vector : std::false_type {};
template <typename T>
struct Vector<std::vector<T>> : std::true_type {
  using Value = T;
};
template <typename T>
struct Map : std::false_type {};
template <typename T>
struct Map<std::map<std::string, T>> : std::true_type {
  using Value = T;
};
template <>
struct Map<TemplateVariations> : std::true_type {
  using Value = std::vector<std::string>;
};
using FeatureBins = std::variant<int, std::map<std::string, int>>;

absl::Status Invalid(const std::string& path, const std::string& reason) {
  return absl::InvalidArgumentError((path.empty() ? "config" : path) + ": " + reason);
}
std::string Child(const std::string& path, const std::string& name) { return path.empty() ? name : path + "." + name; }

template <typename T>
absl::Status Read(const Json& input, T& output, const std::string& path) {
  if constexpr (Optional<T>::value) {
    if (input.is_null()) {
      output.reset();
      return absl::OkStatus();
    }

    typename Optional<T>::Value value{};
    auto status = Read(input, value, path);
    if (status.ok()) output = std::move(value);
    return status;
  } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
    if (!input.is_null()) return Invalid(path, "runtime objects and callbacks are not supported");
  } else if constexpr (std::is_same_v<T, std::string>) {
    if (!input.is_string()) return Invalid(path, "expected a string");
    output = input.get<std::string>();
  } else if constexpr (std::is_same_v<T, bool>) {
    if (!input.is_boolean()) return Invalid(path, "expected a boolean");
    output = input.get<bool>();
  } else if constexpr (std::is_integral_v<T>) {
    if (!input.is_number_integer()) return Invalid(path, "expected an integer");
    if (input.is_number_unsigned()) {
      if (input.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
        return Invalid(path, "integer is out of range");
      }
    } else {
      auto value = input.get<std::int64_t>();
      if (value < std::numeric_limits<T>::min() || value > std::numeric_limits<T>::max()) {
        return Invalid(path, "integer is out of range");
      }
    }

    output = input.get<T>();
  } else if constexpr (std::is_floating_point_v<T>) {
    if (!input.is_number() || !std::isfinite(input.get<double>())) return Invalid(path, "expected a finite number");
    output = input.get<T>();
  } else if constexpr (Vector<T>::value) {
    if (!input.is_array()) return Invalid(path, "expected a sequence");

    output.clear();
    for (std::size_t i = 0; i < input.size(); ++i) {
      typename Vector<T>::Value value{};
      auto status = Read(input[i], value, path + "[" + std::to_string(i) + "]");
      if (!status.ok()) return status;
      output.push_back(std::move(value));
    }
  } else if constexpr (Map<T>::value) {
    if (!input.is_object()) return Invalid(path, "expected a mapping");

    output.clear();
    for (const auto& item : input.items()) {
      typename Map<T>::Value value{};
      auto status = Read(item.value(), value, Child(path, item.key()));
      if (!status.ok()) return status;
      output.emplace(item.key(), std::move(value));
    }
  } else if constexpr (std::is_same_v<T, FeatureBins>) {
    if (input.is_object()) {
      std::map<std::string, int> value;
      auto status = Read(input, value, path);
      if (status.ok()) output = std::move(value);
      return status;
    }

    int value = 0;
    auto status = Read(input, value, path);
    if (status.ok()) output = value;
    return status;
  } else {
    if (!input.is_object()) return Invalid(path, "expected a mapping");

    // The C++ run section rejects typos; reference configuration sections keep
    // Python's behavior of ignoring unknown fields.
    if constexpr (std::is_same_v<T, RunSettings>) {
      for (const auto& item : input.items()) {
        bool known = false;
        VisitFields(output, [&](const char* name, const auto&) { known |= item.key() == name; });
        if (!known) return Invalid(Child(path, item.key()), "unknown run setting");
      }
    }

    absl::Status status;
    VisitFields(output, [&](const char* name, auto& value) {
      if (!status.ok()) return;
      auto it = input.find(name);
      if (it != input.end()) status = Read(*it, value, Child(path, name));
    });

    return status;
  }

  return absl::OkStatus();
}

template <typename T>
Json Write(const T& value) {
  if constexpr (Optional<T>::value) {
    return value ? Write(*value) : Json(nullptr);
  } else if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, std::string> || std::is_same_v<T, std::nullptr_t>) {
    return value;
  } else if constexpr (Vector<T>::value) {
    Json result = Json::array();
    for (const auto& item : value) result.push_back(Write(item));
    return result;
  } else if constexpr (Map<T>::value) {
    Json result = Json::object();
    for (const auto& item : value) result[item.first] = Write(item.second);
    return result;
  } else if constexpr (std::is_same_v<T, FeatureBins>) {
    return std::visit([](const auto& item) { return Write(item); }, value);
  } else {
    Json result = Json::object();
    VisitFields(value, [&](const char* name, const auto& field) { result[name] = Write(field); });

    return result;
  }
}

absl::Status ResolveKey(LLMModelConfig& model, const std::string& path) {
  if (!model.api_key) return absl::OkStatus();

  const auto& key = *model.api_key;
  if (key.size() > 3 && key.compare(0, 2, "${") == 0 && key.back() == '}' && key.find('}', 2) == key.size() - 1) {
    const auto name = key.substr(2, key.size() - 3);
    if (name.find('\0') != std::string::npos) return Invalid(path + ".api_key", "invalid environment variable name");

    const char* value = std::getenv(name.c_str());
    if (!value) return Invalid(path + ".api_key", "referenced environment variable is not set");

    model.api_key = value;
  }

  return absl::OkStatus();
}

bool HasName(const std::optional<std::string>& name) { return name && !name->empty(); }

void ResolveRunPaths(RunSettings& run, const std::filesystem::path& directory) {
  const auto resolve = [&](const std::string& text) {
    const std::filesystem::path path(text);
    return (path.is_absolute() ? path : directory / path).lexically_normal().string();
  };

  for (auto* path : {&run.initial_program, &run.evaluation_file, &run.checkpoint}) {
    if (*path) *path = resolve(**path);
  }
  run.output_directory = resolve(run.output_directory);

  for (auto* executable : {&run.python_executable, &run.codex_executable, &run.claude_executable}) {
    if (std::filesystem::path(*executable).has_parent_path()) *executable = resolve(*executable);
  }
}
}  // namespace

// Brings an LLM section into its canonical shape: environment-referenced keys
// resolved, legacy primary/secondary model fields expanded into the models
// list, and shared defaults pushed down into each model that left them unset.
//
// Normalization must be idempotent, because a normalized configuration is
// written back out with its legacy fields intact and may be loaded again. That
// is what the duplicate check in append_legacy is for.
absl::Status Config::NormalizeLLM(LLMConfig& config, bool rebuilding) {
  auto status = ResolveKey(config, "llm");
  if (!status.ok()) return status;

  auto append_legacy = [&](const std::string& name, double weight) {
    // Serialized normalized configurations still carry legacy fields. Do not
    // add their generated entries a second time when reloading that snapshot.
    const bool exists = std::any_of(config.models.begin(), config.models.end(),
                                    [&](const auto& model) { return model.name == name && model.weight == weight; });
    if (!exists) {
      LLMModelConfig model;
      model.name = name;
      model.weight = weight;
      config.models.push_back(model);
    }
  };

  if (HasName(config.primary_model)) {
    append_legacy(*config.primary_model, config.primary_model_weight && *config.primary_model_weight != 0
                                             ? *config.primary_model_weight
                                             : 1.0);
  }
  if (HasName(config.secondary_model) && (!config.secondary_model_weight || *config.secondary_model_weight > 0)) {
    append_legacy(*config.secondary_model, config.secondary_model_weight.value_or(0.2));
  }

  if (!rebuilding && config.models.empty() &&
      (HasName(config.primary_model) || HasName(config.secondary_model) ||
       config.primary_model_weight.value_or(0) != 0 || config.secondary_model_weight.value_or(0) != 0)) {
    return Invalid("llm.models", "no models configured by legacy model options");
  }

  for (auto* models : {&config.models, &config.evaluator_models}) {
    for (std::size_t i = 0; i < models->size(); ++i) {
      const auto prefix = models == &config.models ? "llm.models" : "llm.evaluator_models";
      status = ResolveKey((*models)[i], std::string(prefix) + "[" + std::to_string(i) + "]");
      if (!status.ok()) return status;
    }
  }

  if (config.evaluator_models.empty()) config.evaluator_models = config.models;

  Json shared = Write(static_cast<const LLMModelConfig&>(config));
  Json parameters = Json::object();
  for (const char* field : {"provider", "api_base", "api_key", "temperature", "top_p", "max_tokens", "timeout",
                            "retries", "retry_delay", "random_seed", "reasoning_effort"})
    parameters[field] = shared[field];
  if (!rebuilding) parameters["manual_mode"] = shared["manual_mode"];

  return ApplyModelParams(config, parameters);
}

// Applies a parameter mapping to every generation and evaluator model.
//
// The probe round trip validates the mapping against the real field inventory,
// so an unknown or mistyped parameter is rejected before anything is touched.
// Work then happens on a candidate copy that replaces the caller's config only
// once every model has been updated, leaving a rejected mapping with no partial
// effect. Without overwrite, only fields the model left null are filled.
absl::Status Config::ApplyModelParams(LLMConfig& config, const Json& params, bool overwrite) {
  if (!params.is_object()) return Invalid("llm.parameters", "expected a mapping");

  LLMModelConfig probe;
  auto status = Read(params, probe, "llm.parameters");
  if (!status.ok()) return status;

  const auto known = Write(probe);
  for (const auto& item : params.items())
    if (!known.contains(item.key())) return Invalid("llm.parameters", "unknown model parameter");

  auto candidate = config;
  for (auto* models : {&candidate.models, &candidate.evaluator_models}) {
    for (auto& model : *models) {
      auto current = Write(model);
      for (const auto& item : params.items())
        if (overwrite || current[item.key()].is_null()) current[item.key()] = item.value();

      status = Read(current, model, "llm.parameters");
      if (!status.ok()) return status;
    }
  }

  config = std::move(candidate);
  return absl::OkStatus();
}

absl::Status Config::UpdateModelParams(const Json& params, bool overwrite) {
  return ApplyModelParams(llm, params, overwrite);
}

absl::Status Config::RebuildModels() {
  auto candidate = llm;
  candidate.models.clear();
  candidate.evaluator_models.clear();

  auto status = NormalizeLLM(candidate, true);
  if (!status.ok()) return status;

  llm = std::move(candidate);
  return absl::OkStatus();
}

absl::Status Config::Validate() const {
  // Also validate programmatically constructed non-finite floating values.
  Config checked;
  auto status = Read(ToJson(), checked, "");
  if (!status.ok()) return status;

  VisitFields(run, [&](const char* name, const auto& value) {
    if (!status.ok()) return;

    const std::string* text = nullptr;
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, std::string>) {
      text = &value;
    } else if constexpr (std::is_same_v<T, std::optional<std::string>>) {
      if (value) text = &*value;
    }

    if (text && (text->empty() || text->find('\0') != std::string::npos)) {
      status = Invalid(Child("run", name), "expected a nonempty string without NUL");
    }
  });
  if (!status.ok()) return status;

  if (prompt.programs_as_changes_description && !diff_based_evolution) {
    return Invalid("prompt.programs_as_changes_description", "requires diff_based_evolution=true");
  }

  const auto& p = prompt;
  if (p.num_top_programs < 0 || p.num_diverse_programs < 0 || p.max_artifact_bytes < 0) {
    return Invalid("prompt", "counts must be nonnegative");
  }
  for (auto value :
       {p.suggest_simplification_after_chars, p.include_changes_under_chars, p.concise_implementation_max_lines,
        p.comprehensive_implementation_min_lines, p.code_length_threshold})
    if (value && *value < 0) return Invalid("prompt", "thresholds must be nonnegative");

  try {
    const std::regex pattern(diff_pattern);
  } catch (const std::regex_error&) {
    return Invalid("diff_pattern", "invalid ECMAScript regular expression");
  }

  return absl::OkStatus();
}

absl::StatusOr<Config> Config::FromJson(const Json& input) {
  if (!input.is_object()) return Invalid("", "expected a mapping");

  auto normalized = input;
  auto llm = normalized.find("llm");
  if (llm != normalized.end() && llm->is_object()) {
    for (const char* field : {"temperature", "top_p"}) {
      auto item = llm->find(field);
      if (item != llm->end() && item->is_null()) llm->erase(item);
    }
  }

  Config config;
  auto status = Read(normalized, config, "");
  if (!status.ok()) return status;

  status = NormalizeLLM(config.llm);
  if (!status.ok()) return status;

  if (!config.database.random_seed && config.random_seed) config.database.random_seed = config.random_seed;

  status = config.Validate();
  if (!status.ok()) return status;

  return config;
}

Json Config::ToJson() const { return Write(*this); }

absl::StatusOr<Config> Config::ParseYaml(std::string_view yaml) {
  auto input = utils::ParseYaml(yaml);
  if (!input.ok()) return input.status();

  return FromJson(*input);
}

absl::StatusOr<Config> Config::Load(std::optional<std::filesystem::path> path, Json* source) {
  Json document = Json::object();
  if (path) {
    auto input = utils::ReadYaml(*path);
    if (!input.ok()) return input.status();
    document = std::move(*input);
  }

  auto result = FromJson(document);
  if (!result.ok()) return result.status();

  if (path) {
    std::error_code error;
    const auto config_path = std::filesystem::canonical(*path, error);
    if (error) return absl::UnknownError("cannot resolve config file path");

    ResolveRunPaths(result->run, config_path.parent_path());

    if (result->prompt.template_dir && !result->prompt.template_dir->empty()) {
      std::filesystem::path templates(*result->prompt.template_dir);
      if (templates.is_relative()) {
        auto resolved = std::filesystem::weakly_canonical(config_path.parent_path() / templates, error);
        if (error) return absl::UnknownError("cannot resolve prompt.template_dir");

        result->prompt.template_dir = resolved.string();
      }
    }
  }

  auto status = result->UpdateModelParams({{"system_message", result->prompt.system_message}});
  if (!status.ok()) return status;

  if (source) *source = std::move(document);
  return result;
}

absl::StatusOr<std::string> Config::ToYaml() const {
  auto status = Validate();
  if (!status.ok()) return status;

  return utils::EmitYaml(ToJson());
}

absl::Status Config::Save(const std::filesystem::path& path) const {
  auto output = ToYaml();
  if (!output.ok()) return output.status();

  return utils::WriteYaml(*output, path);
}
}  // namespace ievolve
