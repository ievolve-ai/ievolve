#include "ievolve/database/feature_mapper.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>
#include <variant>

#include "absl/status/status.h"
#include "ievolve/code/code_distance.h"

namespace ievolve {
absl::Status FeatureMapper::RestoreStatistics(const std::map<std::string, FeatureStats>& stats) {
  for (const auto& [dimension, value] : stats) {
    if (!bins_.count(dimension) || !std::isfinite(value.min) || !std::isfinite(value.max) || value.min > value.max ||
        value.count == 0) {
      return absl::InvalidArgumentError("Invalid restored feature statistics");
    }
  }

  stats_ = stats;
  return absl::OkStatus();
}
namespace {

int AutomaticBins(int archive_size, std::size_t dimensions) {
  if (archive_size <= 1) return archive_size;

  // Find ceil(archive_size^(1 / dimensions)) with integer arithmetic. Floating
  // pow can round exact roots upward and spuriously add a bin.
  int lower = 2;
  int upper = archive_size;
  while (lower < upper) {
    const int middle = lower + (upper - lower) / 2;
    std::int64_t cells = 1;
    for (std::size_t index = 0; index < dimensions; ++index) {
      cells *= middle;
      if (cells >= archive_size) break;
    }

    if (cells >= archive_size) {
      upper = middle;
    } else {
      lower = middle + 1;
    }
  }

  return lower;
}

absl::StatusOr<double> MetricValue(const Metrics& metric, const std::string& dimension) {
  if (!metric.is_number() && !metric.is_boolean()) {
    return absl::InvalidArgumentError("Feature must be numeric: " + dimension);
  }

  const double value = metric.is_boolean() ? (metric.get<bool>() ? 1.0 : 0.0) : metric.get<double>();
  if (!std::isfinite(value)) {
    return absl::InvalidArgumentError("Feature must be finite: " + dimension);
  }

  return value;
}

double ScaleMinMax(double value, const FeatureStats& stats) {
  if (stats.max == stats.min) return 0.5;

  const double range = stats.max - stats.min;
  // Finite opposite-sign endpoints can overflow subtraction. Halving both
  // operands keeps their ratio well-defined across the full double range.
  const double scaled = std::isfinite(range) ? (value - stats.min) / range
                                             : (value / 2 - stats.min / 2) / (stats.max / 2 - stats.min / 2);

  return std::clamp(scaled, 0.0, 1.0);
}

absl::StatusOr<double> Diversity(const Program& program, const std::vector<Program>& population,
                                 std::size_t reference_size) {
  double total = 0.0;
  std::size_t count = 0;
  // The caller supplies a stable population order. Equal-code peers contribute
  // zero distance; only the candidate's own ID is excluded.
  for (const auto& reference : population) {
    if (reference.id == program.id) continue;

    const auto distance = CodeDistance::Levenshtein(program.code, reference.code);
    if (!distance.ok()) return distance.status();

    total += static_cast<double>(*distance);
    if (++count == reference_size) break;
  }

  return count == 0 ? 0.0 : total / static_cast<double>(count);
}

}  // namespace

absl::StatusOr<FeatureMapper> FeatureMapper::Create(const DatabaseConfig& config) {
  if (config.feature_dimensions.empty()) {
    return absl::InvalidArgumentError("Feature dimensions must not be empty");
  }

  std::set<std::string> seen;
  for (const auto& dimension : config.feature_dimensions) {
    if (dimension.empty() || !seen.insert(dimension).second) {
      return absl::InvalidArgumentError("Feature dimensions must be nonempty and unique");
    }
  }

  if (config.archive_size < 0 || config.diversity_reference_size <= 0) {
    return absl::InvalidArgumentError(
        "Archive size must be nonnegative and diversity reference size "
        "positive");
  }

  FeatureMapper mapper;
  mapper.dimensions_ = config.feature_dimensions;
  mapper.reference_size_ = static_cast<std::size_t>(config.diversity_reference_size);

  // A scalar bin count is a floor, not the final value: it is raised to the
  // per-dimension count that lets the grid hold the whole archive. A map
  // instead configures each dimension independently.
  if (const auto* bins = std::get_if<int>(&config.feature_bins)) {
    if (*bins <= 0) {
      return absl::InvalidArgumentError("Feature bins must be positive");
    }

    const int automatic = AutomaticBins(config.archive_size, mapper.dimensions_.size());
    for (const auto& dimension : mapper.dimensions_) {
      mapper.bins_[dimension] = std::max(*bins, automatic);
    }
  } else {
    const auto& per_dimension = std::get<std::map<std::string, int>>(config.feature_bins);
    for (const auto& [dimension, bins] : per_dimension) {
      if (bins <= 0) {
        return absl::InvalidArgumentError("Feature bins must be positive: " + dimension);
      }
    }

    for (const auto& dimension : mapper.dimensions_) {
      const auto found = per_dimension.find(dimension);
      mapper.bins_[dimension] = found == per_dimension.end() ? 10 : found->second;
    }
  }

  return mapper;
}

absl::StatusOr<std::vector<int>> FeatureMapper::Coordinates(const Program& program,
                                                            const std::vector<Program>& population) {
  if (!program.metrics.is_object()) {
    return absl::InvalidArgumentError("Program metrics must be an object");
  }

  auto next_stats = stats_;
  std::vector<int> coordinates;
  coordinates.reserve(dimensions_.size());

  for (const auto& dimension : dimensions_) {
    double value = 0;
    const auto metric = program.metrics.find(dimension);
    if (metric != program.metrics.end()) {
      const auto numeric = MetricValue(*metric, dimension);
      if (!numeric.ok()) return numeric.status();
      value = *numeric;
    } else if (dimension == "complexity") {
      const auto length = CodeDistance::Levenshtein(program.code, "");
      if (!length.ok()) return length.status();
      value = static_cast<double>(*length);
    } else if (dimension == "diversity") {
      if (population.size() < 2) {
        coordinates.push_back(0);
        continue;
      }

      const auto diversity = Diversity(program, population, reference_size_);
      if (!diversity.ok()) return diversity.status();
      value = *diversity;
    } else if (dimension == "score") {
      if (program.metrics.empty()) {
        coordinates.push_back(0);
        continue;
      }

      value = GetFitnessScore(program.metrics, dimensions_);
    } else {
      return absl::InvalidArgumentError("Missing feature metric: " + dimension);
    }

    if (!std::isfinite(value)) {
      return absl::InvalidArgumentError("Feature must be finite: " + dimension);
    }

    auto& stats = next_stats[dimension];
    if (stats.count == std::numeric_limits<std::size_t>::max()) {
      return absl::OutOfRangeError("Feature statistics count overflow");
    }

    if (stats.count == 0) {
      stats.min = value;
      stats.max = value;
    } else {
      stats.min = std::min(stats.min, value);
      stats.max = std::max(stats.max, value);
    }
    ++stats.count;

    const int bins = bins_.at(dimension);
    coordinates.push_back(std::clamp(static_cast<int>(ScaleMinMax(value, stats) * bins), 0, bins - 1));
  }

  stats_ = std::move(next_stats);
  return coordinates;
}

}  // namespace ievolve
