#include "ievolve/evaluator/evaluation_outcome.h"

#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace ievolve::evaluator {
namespace {
absl::Status Invalid(const std::string& reason) {
  return absl::InvalidArgumentError("Invalid evaluation outcome: " + reason);
}
}  // namespace

absl::Status EvaluationOutcome::Validate(std::size_t max_artifact_bytes) const {
  auto status = result.Validate(max_artifact_bytes);
  if (!status.ok()) return status;
  const auto score = result.metrics.find("combined_score");
  if (passed) {
    if (!reason.empty()) return Invalid("passed outcome cannot have a reason");
    if (score == result.metrics.end() || !score->is_number() || !std::isfinite(score->get<double>())) {
      return Invalid("passed outcome requires finite numeric combined_score");
    }
  } else {
    if (score != result.metrics.end()) return Invalid("rejected outcome cannot have combined_score");
    if (reason.empty() || reason.size() > 4096) return Invalid("rejection reason must contain 1-4096 UTF-8 bytes");
    try {
      (void)Metrics(reason).dump();
    } catch (const Metrics::exception&) {
      return Invalid("rejection reason must be UTF-8");
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<Metrics> EvaluationOutcome::ToJson() const {
  auto status = Validate(std::numeric_limits<std::size_t>::max());
  if (!status.ok()) return status;
  auto json = result.ToJson();
  if (!json.ok()) return json.status();
  (*json)["protocol_version"] = 1;
  (*json)["passed"] = passed;
  if (!passed) (*json)["reason"] = reason;
  return json;
}

absl::StatusOr<EvaluationOutcome> EvaluationOutcome::Parse(std::string_view text, bool enable_artifacts,
                                                           std::size_t max_artifact_bytes) {
  try {
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Metrics::parse_event_t event, Metrics& value) {
      using Event = Metrics::parse_event_t;
      if ((event == Event::object_start || event == Event::array_start) && depth >= 64) {
        throw std::invalid_argument("JSON exceeds 64 container levels");
      }
      if (event == Event::object_start) keys.emplace_back();
      if (event == Event::object_end) keys.pop_back();
      if (event == Event::key && !keys.back().insert(value.get<std::string>()).second) {
        throw std::invalid_argument("duplicate JSON key");
      }
      return true;
    };
    auto json = Metrics::parse(text, callback);
    if (!json.is_object()) return Invalid("expected an object");
    for (const auto& item : json.items()) {
      if (item.key() != "protocol_version" && item.key() != "passed" && item.key() != "metrics" &&
          item.key() != "artifacts" && item.key() != "reason")
        return Invalid("unknown envelope field");
    }
    if (!json.contains("protocol_version") || !json["protocol_version"].is_number_integer() ||
        json["protocol_version"] != 1)
      return Invalid("protocol_version must be integer 1");
    if (!json.contains("passed") || !json["passed"].is_boolean()) return Invalid("passed must be boolean");
    EvaluationOutcome outcome;
    outcome.passed = json["passed"].get<bool>();
    if (outcome.passed && json.contains("reason")) return Invalid("passed envelope must omit reason");
    if (!outcome.passed) {
      if (!json.contains("reason") || !json["reason"].is_string()) return Invalid("rejection requires reason");
      outcome.reason = json["reason"].get<std::string>();
    }
    if (!enable_artifacts) json.erase("artifacts");
    auto result = EvaluationResult::FromJson(json, max_artifact_bytes);
    if (!result.ok()) return result.status();
    outcome.result = std::move(*result);
    auto status = outcome.Validate(max_artifact_bytes);
    if (!status.ok()) return status;
    return outcome;
  } catch (const Metrics::exception&) {
    return Invalid("malformed JSON");
  } catch (const std::invalid_argument& error) {
    return Invalid(error.what());
  }
}
}  // namespace ievolve::evaluator
