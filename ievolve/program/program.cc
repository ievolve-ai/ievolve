#include "ievolve/program/program.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <type_traits>

namespace ievolve {
namespace {

absl::Status Invalid(const std::string& field) { return absl::InvalidArgumentError("Invalid program field: " + field); }

template <typename T>
bool ReadValue(const Metrics& input, T& output) {
  if constexpr (std::is_same_v<T, std::string>) {
    if (!input.is_string()) return false;
  } else if constexpr (std::is_same_v<T, double>) {
    if (!input.is_number()) return false;
  } else if constexpr (std::is_same_v<T, std::int64_t>) {
    if (!input.is_number_integer()) return false;
    if (input.is_number_unsigned() &&
        input.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
  } else if constexpr (std::is_same_v<T, Metrics>) {
    if (!input.is_object()) return false;
  } else if constexpr (std::is_same_v<T, std::vector<double>>) {
    if (!input.is_array()) return false;
    for (const auto& item : input)
      if (!item.is_number()) return false;
  }

  output = input.get<T>();
  return true;
}

template <typename T>
bool ReadValue(const Metrics& input, std::optional<T>& output) {
  if (input.is_null()) {
    output.reset();
    return true;
  }

  T value;
  if (!ReadValue(input, value)) return false;
  output = std::move(value);
  return true;
}

Metrics Encode(const Program& program) {
  Metrics result = {{"id", program.id},
                    {"code", program.code},
                    {"changes_description", program.changes_description},
                    {"language", program.language},
                    {"parent_id", program.parent_id ? Metrics(*program.parent_id) : Metrics(nullptr)},
                    {"generation", program.generation},
                    {"timestamp", program.timestamp},
                    {"iteration_found", program.iteration_found},
                    {"metrics", program.metrics},
                    {"complexity", program.complexity},
                    {"diversity", program.diversity},
                    {"metadata", program.metadata},
                    {"prompts", program.prompts ? *program.prompts : Metrics(nullptr)},
                    {"artifacts_json", program.artifacts_json ? Metrics(*program.artifacts_json) : Metrics(nullptr)},
                    {"artifact_dir", program.artifact_dir ? Metrics(*program.artifact_dir) : Metrics(nullptr)},
                    {"embedding", program.embedding ? Metrics(*program.embedding) : Metrics(nullptr)}};

  return result;
}

bool IsFiniteJson(const Metrics& value) {
  if (value.is_binary() || value.is_discarded()) return false;
  if (value.is_number_float()) return std::isfinite(value.get<double>());

  if (value.is_structured()) {
    for (const auto& child : value)
      if (!IsFiniteJson(child)) return false;
  }

  return true;
}

}  // namespace

Program::Program()
    : timestamp(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()) {}

// Decodes a stored program. Only id and code are required; every other field is
// optional so that older snapshots still load. Unknown keys are ignored rather
// than rejected, which is what lets a newer writer's extra fields pass through
// an older reader. The read helper latches the first failure instead of
// returning
// early, so one malformed field names itself rather than aborting mid-object.
absl::StatusOr<Program> Program::FromJson(const Metrics& input) {
  if (!input.is_object()) return Invalid("root");
  if (!input.contains("id")) return Invalid("id");
  if (!input.contains("code")) return Invalid("code");

  try {
    Program program;
    absl::Status status;
    const auto read = [&](const char* name, auto& field) {
      const auto found = input.find(name);
      if (status.ok() && found != input.end() && !ReadValue(*found, field)) status = Invalid(name);
    };

    read("id", program.id);
    read("code", program.code);
    read("changes_description", program.changes_description);
    read("language", program.language);

    read("parent_id", program.parent_id);
    read("generation", program.generation);
    read("timestamp", program.timestamp);
    read("iteration_found", program.iteration_found);

    read("metrics", program.metrics);
    read("complexity", program.complexity);
    read("diversity", program.diversity);

    read("metadata", program.metadata);
    read("prompts", program.prompts);
    read("artifacts_json", program.artifacts_json);
    read("artifact_dir", program.artifact_dir);
    read("embedding", program.embedding);
    if (!status.ok()) return status;

    if (!input.contains("changes_description")) {
      program.changes_description = "empty";
      for (const auto* name : {"changes_description", "changes"}) {
        const auto found = program.metadata.find(name);
        if (found == program.metadata.end() || found->is_null()) continue;
        if (!found->is_string()) return Invalid("metadata description");

        const auto& description = found->get_ref<const std::string&>();
        if (!description.empty()) {
          program.changes_description = description;
          break;
        }
      }
    }

    status = program.Validate();
    if (!status.ok()) return status;

    return program;
  } catch (const Metrics::exception&) {
    return Invalid("JSON representation");
  }
}

absl::StatusOr<Metrics> Program::ToJson() const {
  if (id.empty()) return Invalid("id");
  if (generation < 0) return Invalid("generation");
  if (iteration_found < 0) return Invalid("iteration_found");

  if (!metrics.is_object()) return Invalid("metrics");
  for (const auto& value : metrics) {
    if (value.is_structured()) return Invalid("metrics scalar");
  }

  if (!metadata.is_object()) return Invalid("metadata");
  if (prompts && !prompts->is_object()) return Invalid("prompts");

  try {
    auto value = Encode(*this);
    if (!IsFiniteJson(value)) return Invalid("finite JSON values");

    // Strict serialization also checks UTF-8 in keys and nested strings.
    (void)value.dump();
    return value;
  } catch (const Metrics::exception&) {
    return Invalid("JSON representation");
  }
}

// Serialization is the validation: ToJson enforces every field rule, the UTF-8
// requirement and the numeric ranges, so a program that can be written is one
// that can be stored and restored. Keeping a single implementation stops the
// two from drifting.
absl::Status Program::Validate() const { return ToJson().status(); }
}  // namespace ievolve
