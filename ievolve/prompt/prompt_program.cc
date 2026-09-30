#include "ievolve/prompt/prompt_program.h"

namespace ievolve {
absl::StatusOr<PromptProgram> PromptProgram::FromProgram(const Program& program) {
  const auto status = program.Validate();
  if (!status.ok()) return status;

  PromptProgram result;
  result.id = program.id;
  result.code = program.code;
  result.metrics = program.metrics;
  result.changes_description = program.changes_description;

  const auto invalid = [] { return absl::InvalidArgumentError("Invalid program metadata for prompt projection"); };

  const auto& metadata = program.metadata;
  const auto changes = metadata.find("changes");
  if (changes != metadata.end()) {
    if (!changes->is_string()) return invalid();
    result.changes = changes->get<std::string>();
  }

  const auto parents = metadata.find("parent_metrics");
  if (parents != metadata.end()) {
    if (!parents->is_object()) return invalid();
    for (const auto& value : *parents)
      if (value.is_structured()) return invalid();
    result.parent_metrics = *parents;
  }

  for (const auto& [name, destination] : {std::pair<const char*, bool*>{"diverse", &result.diverse},
                                          {"migrant", &result.migrant},
                                          {"random", &result.random}}) {
    const auto field = metadata.find(name);
    if (field == metadata.end()) continue;
    if (!field->is_boolean()) return invalid();
    *destination = field->get<bool>();
  }

  const auto features = metadata.find("key_features");
  if (features != metadata.end()) {
    if (!features->is_array()) return invalid();
    for (const auto& feature : *features) {
      if (!feature.is_string()) return invalid();
      result.key_features.push_back(feature.get<std::string>());
    }
  }

  return result;
}
}  // namespace ievolve
