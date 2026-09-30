#include "ievolve/program/metrics.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ievolve {

double GetFitnessScore(const Metrics& metrics, const std::vector<std::string>& feature_dimensions) {
  if (!metrics.is_object()) return 0.0;

  const auto combined = metrics.find("combined_score");
  // The evaluator worker guarantees a numeric combined_score; anything else
  // (e.g. an imported Python checkpoint) falls back to the mean below.
  if (combined != metrics.end() && (combined->is_number() || combined->is_boolean())) {
    return Number(*combined);
  }

  double total = 0.0;
  double fallback_total = 0.0;
  int count = 0;
  int fallback_count = 0;
  for (const auto& item : metrics.items()) {
    if (!item.value().is_number()) continue;
    const double value = item.value().get<double>();
    if (std::isnan(value)) continue;

    fallback_total += value;
    ++fallback_count;
    if (std::find(feature_dimensions.begin(), feature_dimensions.end(), item.key()) == feature_dimensions.end()) {
      total += value;
      ++count;
    }
  }

  if (count != 0) return total / count;
  return fallback_count == 0 ? 0.0 : fallback_total / fallback_count;
}

namespace {
bool IsNegativeInteger(const Metrics& value) {
  return value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0;
}

}  // namespace

// Returns zero for equal or unordered (NaN) values. Neither case represents an
// improvement or a regression. Avoid converting large integer counters to
// double.
int CompareMetricNumbers(const Metrics& left, const Metrics& right) {
  if (!left.is_number_float() && !right.is_number_float()) {
    const bool left_negative = IsNegativeInteger(left);
    const bool right_negative = IsNegativeInteger(right);
    if (left_negative != right_negative) return left_negative ? -1 : 1;

    if (left_negative) {
      const auto lhs = left.get<std::int64_t>();
      const auto rhs = right.get<std::int64_t>();
      return (lhs > rhs) - (lhs < rhs);
    }

    const auto lhs = left.is_boolean() ? static_cast<std::uint64_t>(left.get<bool>()) : left.get<std::uint64_t>();
    const auto rhs = right.is_boolean() ? static_cast<std::uint64_t>(right.get<bool>()) : right.get<std::uint64_t>();
    return (lhs > rhs) - (lhs < rhs);
  }

  if (left.is_number_float() && right.is_number_float()) {
    const double lhs = left.get<double>();
    const double rhs = right.get<double>();
    return (lhs > rhs) - (lhs < rhs);
  }

  if (left.is_number_float()) return -CompareMetricNumbers(right, left);
  const double number = right.get<double>();
  if (std::isnan(number)) return 0;

  // These power-of-two bounds are exactly representable as doubles.
  if (number >= 0x1p64) return -1;
  if (number < -0x1p63) return 1;

  const double truncated = std::trunc(number);
  const Metrics integer =
      number < 0 ? Metrics(static_cast<std::int64_t>(truncated)) : Metrics(static_cast<std::uint64_t>(truncated));
  const int comparison = CompareMetricNumbers(left, integer);
  if (comparison != 0) return comparison;

  return (truncated > number) - (truncated < number);
}

std::optional<std::int64_t> AsCounter(const Metrics& value) {
  if (!value.is_number_integer()) return std::nullopt;
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
    return static_cast<std::int64_t>(number);
  }

  const auto number = value.get<std::int64_t>();
  if (number < 0) return std::nullopt;
  return number;
}
}  // namespace ievolve
