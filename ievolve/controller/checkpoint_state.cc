#include "ievolve/controller/checkpoint_state.h"

#include <cmath>
#include <limits>
#include <regex>
#include <set>
#include <stdexcept>

#include "ievolve/utils/file.h"

namespace ievolve::controller {
namespace {
void Require(bool valid) {
  if (!valid) throw std::invalid_argument("Invalid checkpoint state");
}
void Fields(const Metrics& object, std::initializer_list<const char*> names) {
  Require(object.is_object() && object.size() == names.size());
  for (const char* name : names) Require(object.contains(name));
}
std::int64_t Counter(const Metrics& value) {
  const auto count = AsCounter(value);
  Require(count.has_value());
  return *count;
}
std::string Text(const Metrics& value) {
  Require(value.is_string());
  auto text = value.get<std::string>();
  Require(!text.empty() && text.find('\0') == std::string::npos);
  return text;
}

CheckpointState Decode(const Metrics& json) {
  Fields(json, {"format", "schema_version", "evaluation_protocol_version", "program", "progress", "stopping_policy"});
  Require(json.at("format") == "ievolve.cli-checkpoint");
  Require(Counter(json.at("schema_version")) == 1 && Counter(json.at("evaluation_protocol_version")) == 1);
  const auto& program = json.at("program");
  Fields(program, {"language", "file_suffix", "best_id"});
  CheckpointState result;
  result.language = Text(program.at("language"));
  result.file_suffix = Text(program.at("file_suffix"));
  Require(std::regex_match(result.file_suffix, std::regex("\\.[A-Za-z0-9_]{1,31}")));
  result.best_id = Text(program.at("best_id"));
  const auto& value = json.at("progress");
  Fields(value, {"iteration", "accepted", "rejected", "without_improvement", "best_score", "early_stopped"});
  auto& progress = result.progress;
  progress.iteration = Counter(value.at("iteration"));
  progress.accepted = Counter(value.at("accepted"));
  progress.rejected = Counter(value.at("rejected"));
  progress.without_improvement = Counter(value.at("without_improvement"));
  Require(value.at("early_stopped").is_boolean());
  progress.early_stopped = value.at("early_stopped").get<bool>();
  if (!value.at("best_score").is_null()) {
    Require(IsNumeric(value.at("best_score")) && std::isfinite(NumberAs<long double>(value.at("best_score"))));
    progress.best_score = value.at("best_score");
  }
  Require(progress.accepted <= progress.iteration && progress.rejected == progress.iteration - progress.accepted);
  Require(progress.without_improvement <= progress.accepted && (!progress.best_score || progress.accepted != 0) &&
          (progress.without_improvement == 0 || progress.best_score));
  const auto& policy = json.at("stopping_policy");
  Fields(policy, {"patience", "metric", "threshold"});
  (void)Text(policy.at("metric"));
  Require(policy.at("threshold").is_number() && std::isfinite(policy.at("threshold").get<double>()));
  const auto& patience = policy.at("patience");
  if (patience.is_null()) {
    Require(!progress.best_score && progress.without_improvement == 0 && !progress.early_stopped);
  } else {
    Require(patience.is_number_integer());
    if (patience.is_number_unsigned()) Require(patience.get<std::uint64_t>() <= std::numeric_limits<int>::max());
    const auto count = patience.get<std::int64_t>();
    Require(count >= std::numeric_limits<int>::min() && count <= std::numeric_limits<int>::max());
    if (count > 0) {
      Require(progress.early_stopped == (progress.without_improvement >= count));
    } else {
      Require(progress.without_improvement == 0 && progress.early_stopped == progress.best_score.has_value());
      if (progress.best_score) Require(CompareMetricNumbers(*progress.best_score, policy.at("threshold")) == 0);
    }
  }
  result.stopping_policy = policy;
  (void)json.dump();  // Validate UTF-8 in programmatically constructed values.
  return result;
}

Metrics Encode(const CheckpointState& state) {
  const auto& progress = state.progress;
  return {{"format", "ievolve.cli-checkpoint"},
          {"schema_version", 1},
          {"evaluation_protocol_version", 1},
          {"program", {{"language", state.language}, {"file_suffix", state.file_suffix}, {"best_id", state.best_id}}},
          {"progress",
           {{"iteration", progress.iteration},
            {"accepted", progress.accepted},
            {"rejected", progress.rejected},
            {"without_improvement", progress.without_improvement},
            {"best_score", progress.best_score.value_or(Metrics(nullptr))},
            {"early_stopped", progress.early_stopped}}},
          {"stopping_policy", state.stopping_policy}};
}
}  // namespace

absl::StatusOr<CheckpointState> ReadCheckpointState(const std::filesystem::path& directory) {
  const auto text = utils::ReadFile(directory / "checkpoint.json");
  if (!text.ok()) return text.status();
  try {
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Metrics::parse_event_t event, Metrics& value) {
      using Event = Metrics::parse_event_t;
      if (event == Event::object_start || event == Event::array_start) Require(depth < 64);
      if (event == Event::object_start) keys.emplace_back();
      if (event == Event::key) Require(keys.back().insert(value.get<std::string>()).second);
      if (event == Event::object_end) keys.pop_back();
      return true;
    };
    return Decode(Metrics::parse(*text, callback));
  } catch (const std::exception&) {
    return absl::DataLossError("Invalid CLI checkpoint state");
  }
}
absl::Status WriteCheckpointState(const std::filesystem::path& directory, const CheckpointState& state) {
  try {
    const auto json = Encode(state);
    (void)Decode(json);
    auto status = utils::WriteFile(directory / "checkpoint.json", json.dump(2));
    if (!status.ok()) return absl::UnavailableError("Cannot write checkpoint state: " + std::string(status.message()));
    return absl::OkStatus();
  } catch (const std::exception&) {
    return absl::InvalidArgumentError("Invalid CLI checkpoint state");
  }
}
}  // namespace ievolve::controller
