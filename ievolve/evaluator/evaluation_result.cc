#include "ievolve/evaluator/evaluation_result.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace ievolve::evaluator {
namespace {

absl::Status Invalid(const std::string& message) {
  return absl::InvalidArgumentError("Invalid evaluation result: " + message);
}

absl::Status TooLarge() { return absl::ResourceExhaustedError("Evaluation artifact byte limit exceeded"); }

std::size_t ArtifactSize(const ArtifactValue& value) {
  return std::visit([](const auto& payload) { return payload.size(); }, value);
}

}  // namespace

absl::Status EvaluationResult::Validate(std::size_t max_artifact_bytes) const {
  if (!metrics.is_object()) return Invalid("metrics must be an object");

  for (const auto& value : metrics) {
    if (value.is_structured() || value.is_binary() || value.is_discarded() ||
        (value.is_number_float() && !std::isfinite(value.get<double>()))) {
      return Invalid("metrics must contain finite JSON scalars");
    }
  }

  try {
    // Strict JSON serialization validates UTF-8 in every metric key and value.
    (void)metrics.dump();

    std::size_t remaining = max_artifact_bytes;
    for (const auto& [key, value] : artifacts) {
      (void)Metrics(key).dump();
      if (const auto* text = std::get_if<std::string>(&value)) (void)Metrics(*text).dump();

      const auto size = ArtifactSize(value);
      if (size > remaining) return TooLarge();
      remaining -= size;
    }

    return absl::OkStatus();
  } catch (const Metrics::exception&) {
    return Invalid("keys and text must be UTF-8");
  }
}

absl::StatusOr<EvaluationResult> EvaluationResult::FromJson(const Metrics& value, std::size_t max_artifact_bytes) {
  if (!value.is_object() || !value.contains("metrics")) return Invalid("expected an object containing metrics");

  EvaluationResult result;
  result.metrics = value["metrics"];
  const auto metrics_status = result.Validate(0);
  if (!metrics_status.ok()) return metrics_status;

  const auto found = value.find("artifacts");
  if (found != value.end()) {
    if (!found->is_object()) return Invalid("artifacts must be an object");

    std::size_t remaining = max_artifact_bytes;
    for (const auto& [key, encoded] : found->items()) {
      auto decoded = DecodeArtifactValue(encoded);
      if (!decoded.ok()) return decoded.status();
      const auto size = ArtifactSize(*decoded);
      if (size > remaining) return TooLarge();
      remaining -= size;
      result.artifacts.emplace(key, std::move(*decoded));
    }
  }

  const auto status = result.Validate(max_artifact_bytes);
  if (!status.ok()) return status;

  return result;
}

absl::StatusOr<Metrics> EvaluationResult::ToJson() const {
  const auto status = Validate(std::numeric_limits<std::size_t>::max());
  if (!status.ok()) return status;

  Metrics encoded = {{"metrics", metrics}, {"artifacts", Metrics::object()}};
  for (const auto& [key, value] : artifacts) {
    auto artifact = EncodeArtifactValue(value);
    if (!artifact.ok()) return artifact.status();
    encoded["artifacts"][key] = std::move(*artifact);
  }

  return encoded;
}

std::size_t EvaluationResult::GetArtifactSize(std::string_view key) const {
  const auto found = artifacts.find(std::string(key));
  return found == artifacts.end() ? 0 : ArtifactSize(found->second);
}

std::size_t EvaluationResult::GetTotalArtifactSize() const {
  std::size_t total = 0;
  for (const auto& [key, value] : artifacts) total += ArtifactSize(value);
  return total;
}

// Cascade gate. combined_score decides when present; otherwise the mean of the
// numeric metrics does, excluding "error". With no numeric metric at all there
// is no mean to take, so the final count check refuses rather than treating an
// unscored candidate as scoring zero.
bool EvaluationResult::PassesThreshold(double threshold) const {
  if (!metrics.is_object()) return false;

  const auto combined = metrics.find("combined_score");
  if (combined != metrics.end() && IsNumeric(*combined)) return Number(*combined) >= threshold;

  double total = 0.0;
  double correction = 0.0;
  std::size_t count = 0;
  for (const auto& [key, value] : metrics.items()) {
    if (key != "error" && IsNumeric(value)) {
      // Neumaier summation matches Python 3.12+ sum(float values), retaining
      // small metrics when larger positive and negative scores cancel.
      const double number = Number(value);
      const double next = total + number;
      correction += std::abs(total) >= std::abs(number) ? (total - next) + number : (number - next) + total;
      total = next;
      ++count;
    }
  }

  // Preserve Python's overflow behavior instead of turning an infinite sum
  // into NaN through its non-finite compensation term.
  if (correction != 0.0 && std::isfinite(correction)) total += correction;
  return count != 0 && total / static_cast<double>(count) >= threshold;
}

// Folds a later cascade stage into an earlier one. Later values win on both
// metrics and artifacts. Only numeric metrics survive, and "error" is dropped,
// so a merged result carries scores rather than failure markers; strings, bools
// and nulls are preserved only on a direct, unmerged evaluation.
EvaluationResult EvaluationResult::Merge(const EvaluationResult& earlier, const EvaluationResult& later) {
  EvaluationResult merged;
  for (const auto* result : {&earlier, &later}) {
    if (result->metrics.is_object()) {
      for (const auto& [key, value] : result->metrics.items()) {
        if (key != "error" && IsNumeric(value)) merged.metrics[key] = Number(value);
      }
    }

    for (const auto& [key, value] : result->artifacts) merged.artifacts[key] = value;
  }

  return merged;
}
}  // namespace ievolve::evaluator
