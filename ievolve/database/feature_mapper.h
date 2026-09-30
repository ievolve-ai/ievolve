#ifndef IEVOLVE_DATABASE_FEATURE_MAPPER_H_
#define IEVOLVE_DATABASE_FEATURE_MAPPER_H_

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "ievolve/config/types.h"
#include "ievolve/program/program.h"

namespace ievolve {

struct FeatureStats {
  double min = 0;
  double max = 0;
  std::size_t count = 0;
};

class FeatureMapper {
 public:
  static absl::StatusOr<FeatureMapper> Create(const DatabaseConfig& config);

  // Population includes the candidate. Failed calls leave stats unchanged.
  absl::StatusOr<std::vector<int>> Coordinates(const Program& program, const std::vector<Program>& population);
  const std::map<std::string, FeatureStats>& stats() const { return stats_; }
  const std::map<std::string, int>& bins() const { return bins_; }
  absl::Status RestoreStatistics(const std::map<std::string, FeatureStats>& stats);

 private:
  FeatureMapper() = default;

  std::vector<std::string> dimensions_;
  std::map<std::string, int> bins_;
  std::size_t reference_size_ = 0;
  std::map<std::string, FeatureStats> stats_;
};

}  // namespace ievolve

#endif  // IEVOLVE_DATABASE_FEATURE_MAPPER_H_
