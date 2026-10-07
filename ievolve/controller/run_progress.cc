#include "ievolve/controller/run_progress.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace ievolve::controller {
namespace {

bool Improved(const Metrics& current, const Metrics& previous, double threshold) {
  if (current.is_number_float() || previous.is_number_float()) {
    return NumberAs<long double>(current) - NumberAs<long double>(previous) >= threshold;
  }

  // Integer deltas need up to 65 magnitude bits (uint64 - negative int64).
  // Subtract before converting: long double has only 53 bits on some targets.
  const auto magnitude = [](const Metrics& value) {
    const bool negative = !value.is_number_unsigned() && !value.is_boolean() && value.get<std::int64_t>() < 0;
    const auto raw = value.is_boolean() ? std::uint64_t(value.get<bool>()) : value.get<std::uint64_t>();
    return std::make_pair(negative, negative ? std::uint64_t{0} - raw : raw);
  };

  const auto [left_negative, left] = magnitude(current);
  const auto [right_negative, right] = magnitude(previous);
  bool negative = left_negative;
  bool carry = false;
  std::uint64_t delta;
  if (left_negative != right_negative) {
    delta = left + right;
    carry = delta < left;
  } else if (left >= right) {
    delta = left - right;
  } else {
    delta = right - left;
    negative = !negative;
  }

  if (!carry && delta == 0) negative = false;
  if (negative != (threshold < 0)) return !negative;

  const double required = std::abs(threshold);
  // Subtraction from 2^64 is exact for a double between 2^64 and 2^65.
  const int comparison =
      carry ? (required < 0x1p64 ? 1 : CompareMetricNumbers(Metrics(delta), Metrics(required - 0x1p64)))
            : CompareMetricNumbers(Metrics(delta), Metrics(required));

  return negative ? comparison <= 0 : comparison >= 0;
}

std::optional<Metrics> StoppingScore(const Metrics& metrics, const std::string& name) {
  if (metrics.empty()) return std::nullopt;

  const auto score = metrics.find(name);
  if (score != metrics.end()) return IsNumeric(*score) ? std::optional<Metrics>(*score) : std::nullopt;

  // Divide first to retain small residuals after cancellation of large terms.
  // If rounding still overflows (e.g. three DBL_MAX/3), scale and clamp
  // instead.
  std::size_t count = 0;
  long double scale = 0;
  for (const auto& value : metrics)
    if (value.is_number()) {
      ++count;
      scale = std::max(scale, std::abs(NumberAs<long double>(value)));
    }

  // A mean over zero numeric metrics does not exist; reporting 0.0 would be a
  // fabricated score. Leave early-stopping state unchanged in that case.
  if (count == 0) return std::nullopt;

  long double average = 0;
  for (const auto& value : metrics)
    if (value.is_number()) average += NumberAs<long double>(value) / count;
  if (std::isfinite(average)) return Metrics(static_cast<double>(std::clamp(average, -scale, scale)));

  average = 0;
  if (scale > 0) {
    for (const auto& value : metrics)
      if (value.is_number()) average += (NumberAs<long double>(value) / scale) / count;
  }

  return Metrics(static_cast<double>(std::clamp(average, -1.0L, 1.0L) * scale));
}

absl::Status Invalid(const std::string& field) {
  return absl::DataLossError("Invalid stopping policy field: " + field);
}

absl::Status Inconsistent(const std::string& what) { return absl::DataLossError("Inconsistent run progress: " + what); }

}  // namespace

Metrics StoppingPolicy::ToJson() const {
  return {{"patience", patience ? Metrics(*patience) : Metrics(nullptr)}, {"metric", metric}, {"threshold", threshold}};
}

absl::StatusOr<StoppingPolicy> StoppingPolicy::FromJson(const Metrics& json) {
  if (!json.is_object() || json.size() != 3) return Invalid("stopping_policy");
  for (const char* name : {"patience", "metric", "threshold"}) {
    if (!json.contains(name)) return Invalid(name);
  }

  StoppingPolicy policy;
  const auto& patience = json.at("patience");
  if (!patience.is_null()) {
    if (!patience.is_number_integer()) return Invalid("patience");
    const bool in_range = patience.is_number_unsigned()
                              ? patience.get<std::uint64_t>() <= std::numeric_limits<int>::max()
                              : patience.get<std::int64_t>() >= std::numeric_limits<int>::min() &&
                                    patience.get<std::int64_t>() <= std::numeric_limits<int>::max();
    if (!in_range) return Invalid("patience");
    policy.patience = patience.get<int>();
  }

  const auto& metric = json.at("metric");
  if (!metric.is_string() || metric.get<std::string>().empty() ||
      metric.get<std::string>().find('\0') != std::string::npos) {
    return Invalid("metric");
  }
  policy.metric = metric.get<std::string>();

  const auto& threshold = json.at("threshold");
  if (!threshold.is_number() || !std::isfinite(threshold.get<double>())) return Invalid("threshold");
  policy.threshold = threshold.get<double>();

  return policy;
}

bool operator==(const StoppingPolicy& left, const StoppingPolicy& right) {
  return left.patience == right.patience && left.metric == right.metric && left.threshold == right.threshold;
}

absl::StatusOr<RunProgress> RunProgress::Restore(StoppingPolicy policy, const Counters& counters) {
  const auto& [iteration, accepted, rejected, without_improvement, best_score, early_stopped] = counters;
  if (iteration < 0 || accepted < 0 || rejected < 0 || without_improvement < 0) {
    return Inconsistent("negative counter");
  }
  if (accepted > iteration || rejected != iteration - accepted) {
    return Inconsistent("accepted and rejected do not sum to iteration");
  }
  if (without_improvement > accepted) return Inconsistent("without_improvement exceeds accepted");
  if (best_score && (!IsNumeric(*best_score) || !std::isfinite(NumberAs<long double>(*best_score)))) {
    return Inconsistent("best_score is not a finite number");
  }
  if (best_score && accepted == 0) return Inconsistent("best_score without accepted children");
  if (without_improvement != 0 && !best_score) return Inconsistent("without_improvement without best_score");

  if (!policy.patience) {
    if (best_score || without_improvement != 0 || early_stopped) {
      return Inconsistent("early-stopping state without patience");
    }
  } else if (*policy.patience > 0) {
    if (early_stopped != (without_improvement >= *policy.patience)) {
      return Inconsistent("early_stopped disagrees with patience");
    }
  } else {
    // Non-positive patience only ever records the score that met the threshold.
    if (without_improvement != 0 || early_stopped != best_score.has_value() ||
        (best_score && CompareMetricNumbers(*best_score, Metrics(policy.threshold)) != 0)) {
      return Inconsistent("early_stopped disagrees with threshold");
    }
  }

  RunProgress progress(std::move(policy));
  progress.iteration_ = iteration;
  progress.accepted_ = accepted;
  progress.without_improvement_ = without_improvement;
  progress.best_score_ = best_score;
  progress.early_stopped_ = early_stopped;
  return progress;
}

void RunProgress::RecordRejected() { ++iteration_; }

void RunProgress::RecordAccepted(const Metrics& child_metrics) {
  TrackScore(child_metrics);
  ++iteration_;
  ++accepted_;
}

void RunProgress::TrackScore(const Metrics& child_metrics) {
  if (!policy_.patience) return;

  const auto score = StoppingScore(child_metrics, policy_.metric);
  if (!score) return;

  if (*policy_.patience <= 0) {
    if (CompareMetricNumbers(*score, Metrics(policy_.threshold)) == 0) {
      best_score_ = *score;
      early_stopped_ = true;
    }
    return;
  }

  if (!best_score_ || Improved(*score, *best_score_, policy_.threshold)) {
    best_score_ = *score;
    without_improvement_ = 0;
  } else {
    ++without_improvement_;
  }

  early_stopped_ = without_improvement_ >= *policy_.patience;
}

RunProgress::Counters RunProgress::counters() const {
  return {iteration_, accepted_, rejected(), without_improvement_, best_score_, early_stopped_};
}

bool operator==(const RunProgress::Counters& left, const RunProgress::Counters& right) {
  return left.iteration == right.iteration && left.accepted == right.accepted && left.rejected == right.rejected &&
         left.without_improvement == right.without_improvement && left.best_score == right.best_score &&
         left.early_stopped == right.early_stopped;
}

}  // namespace ievolve::controller
