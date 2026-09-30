#ifndef IEVOLVE_PROGRAM_METRICS_H_
#define IEVOLVE_PROGRAM_METRICS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace ievolve {
// Preserve Python dictionary order; metric values may be scalar mixed types.
using Metrics = nlohmann::ordered_json;

// Booleans count as numbers and convert to 1 and 0, the way Python treats bool
// as a subtype of int. These live beside the Metrics alias because every
// component that reads metrics needs them.
inline bool IsNumeric(const Metrics& value) { return value.is_number() || value.is_boolean(); }

// T is double at every call site except the controller's integer-delta
// arithmetic, which widens to long double. Widening is not a precision
// guarantee: long double is only 53 bits on some targets.
template <typename T>
T NumberAs(const Metrics& value) {
  return value.is_boolean() ? static_cast<T>(value.get<bool>() ? 1 : 0) : value.get<T>();
}

inline double Number(const Metrics& value) { return NumberAs<double>(value); }

// A nonnegative integer that fits in int64, or nullopt. Reporting the failure
// stays with the caller, because the database and the controller throw
// different types that their own handlers expect.
std::optional<std::int64_t> AsCounter(const Metrics& value);

double GetFitnessScore(const Metrics& metrics, const std::vector<std::string>& feature_dimensions = {});
// Both operands must be numeric or bool. Returns -1/0/1; NaN is unordered (0).
// Keeps full integer precision across signed, unsigned and floating operands.
int CompareMetricNumbers(const Metrics& left, const Metrics& right);
}  // namespace ievolve

#endif  // IEVOLVE_PROGRAM_METRICS_H_
