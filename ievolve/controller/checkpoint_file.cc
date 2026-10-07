#include "ievolve/controller/checkpoint_file.h"

#include <initializer_list>
#include <regex>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ievolve/utils/file.h"

namespace ievolve::controller {
namespace {

// Thrown inside Decode with the offending field path; ReadCheckpointFile turns
// it into DataLoss.
struct InvalidField : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void Require(bool valid, const std::string& field) {
  if (!valid) throw InvalidField(field);
}

void Fields(const Metrics& object, const std::string& path, std::initializer_list<const char*> names) {
  Require(object.is_object() && object.size() == names.size(), path.empty() ? "<root>" : path);
  for (const char* name : names) Require(object.contains(name), path.empty() ? name : path + "." + name);
}

std::int64_t Counter(const Metrics& value, const std::string& field) {
  const auto count = AsCounter(value);
  Require(count.has_value(), field);
  return *count;
}

std::string Text(const Metrics& value, const std::string& field) {
  Require(value.is_string(), field);
  auto text = value.get<std::string>();
  Require(!text.empty() && text.find('\0') == std::string::npos, field);
  return text;
}

RunProgress::Counters DecodeCounters(const Metrics& json) {
  Fields(json, "progress", {"iteration", "accepted", "rejected", "without_improvement", "best_score", "early_stopped"});
  RunProgress::Counters counters;
  counters.iteration = Counter(json.at("iteration"), "progress.iteration");
  counters.accepted = Counter(json.at("accepted"), "progress.accepted");
  counters.rejected = Counter(json.at("rejected"), "progress.rejected");
  counters.without_improvement = Counter(json.at("without_improvement"), "progress.without_improvement");
  Require(json.at("early_stopped").is_boolean(), "progress.early_stopped");
  counters.early_stopped = json.at("early_stopped").get<bool>();
  if (!json.at("best_score").is_null()) counters.best_score = json.at("best_score");
  return counters;
}

// Returns the value or throws, so Decode reads as one sequence of checks.
template <typename T>
T Unwrap(absl::StatusOr<T> value, const std::string& field) {
  if (!value.ok()) throw InvalidField(field + ": " + std::string(value.status().message()));
  return std::move(*value);
}

CheckpointFile Decode(const Metrics& json) {
  Fields(json, "",
         {"format", "schema_version", "evaluation_protocol_version", "program", "progress", "stopping_policy"});
  // "cli-checkpoint" and the evaluation protocol version are historical names
  // kept for compatibility with existing checkpoints.
  Require(json.at("format") == "ievolve.cli-checkpoint", "format");
  Require(Counter(json.at("schema_version"), "schema_version") == 1, "schema_version");
  Require(Counter(json.at("evaluation_protocol_version"), "evaluation_protocol_version") == 1,
          "evaluation_protocol_version");

  const auto& program = json.at("program");
  Fields(program, "program", {"language", "file_suffix", "best_id"});
  const auto language = Text(program.at("language"), "program.language");
  const auto file_suffix = Text(program.at("file_suffix"), "program.file_suffix");
  Require(std::regex_match(file_suffix, std::regex("\\.[A-Za-z0-9_]{1,31}")), "program.file_suffix");
  const auto best_id = Text(program.at("best_id"), "program.best_id");

  const auto counters = DecodeCounters(json.at("progress"));
  auto policy = Unwrap(StoppingPolicy::FromJson(json.at("stopping_policy")), "stopping_policy");
  auto progress = Unwrap(RunProgress::Restore(std::move(policy), counters), "progress");

  (void)json.dump();  // Validate UTF-8 in programmatically constructed values.
  return {language, file_suffix, best_id, std::move(progress)};
}

Metrics Encode(const CheckpointFile& file) {
  const auto counters = file.progress.counters();
  return {{"format", "ievolve.cli-checkpoint"},
          {"schema_version", 1},
          {"evaluation_protocol_version", 1},
          {"program", {{"language", file.language}, {"file_suffix", file.file_suffix}, {"best_id", file.best_id}}},
          {"progress",
           {{"iteration", counters.iteration},
            {"accepted", counters.accepted},
            {"rejected", counters.rejected},
            {"without_improvement", counters.without_improvement},
            {"best_score", counters.best_score.value_or(Metrics(nullptr))},
            {"early_stopped", counters.early_stopped}}},
          {"stopping_policy", file.progress.policy().ToJson()}};
}

}  // namespace

absl::StatusOr<CheckpointFile> ReadCheckpointFile(const std::filesystem::path& directory) {
  const auto text = utils::ReadFile(directory / "checkpoint.json");
  if (!text.ok()) return text.status();
  try {
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Metrics::parse_event_t event, Metrics& value) {
      using Event = Metrics::parse_event_t;
      if (event == Event::object_start || event == Event::array_start) Require(depth < 64, "<nesting depth>");
      if (event == Event::object_start) keys.emplace_back();
      if (event == Event::key) Require(keys.back().insert(value.get<std::string>()).second, "<duplicate key>");
      if (event == Event::object_end) keys.pop_back();
      return true;
    };
    return Decode(Metrics::parse(*text, callback));
  } catch (const InvalidField& error) {
    return absl::DataLossError("Invalid checkpoint field: " + std::string(error.what()));
  } catch (const std::exception&) {
    return absl::DataLossError("Malformed checkpoint JSON");
  }
}

absl::Status WriteCheckpointFile(const std::filesystem::path& directory, const CheckpointFile& file) {
  try {
    const auto json = Encode(file);
    (void)Decode(json);
    auto status = utils::WriteFile(directory / "checkpoint.json", json.dump(2));
    if (!status.ok()) return absl::UnavailableError("Cannot write checkpoint file: " + std::string(status.message()));
    return absl::OkStatus();
  } catch (const InvalidField& error) {
    return absl::InvalidArgumentError("Invalid checkpoint field: " + std::string(error.what()));
  } catch (const std::exception&) {
    return absl::InvalidArgumentError("Checkpoint values must be valid UTF-8");
  }
}

}  // namespace ievolve::controller
