#ifndef IEVOLVE_DATABASE_POPULATION_H_
#define IEVOLVE_DATABASE_POPULATION_H_

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ievolve/program/program.h"

namespace ievolve {

using FeatureCoordinates = std::vector<int>;
using FeatureMap = std::map<FeatureCoordinates, std::string>;

// Detached values: modifying a returned snapshot cannot change the database.
struct PopulationSnapshot {
  std::map<std::string, Program> programs;
  std::vector<std::set<std::string>> islands;
  std::vector<FeatureMap> feature_maps;
  std::set<std::string> archive;
  std::optional<std::string> best_program_id;
  std::vector<std::optional<std::string>> island_best_programs;
  int population_limit = 0;
  int archive_limit = 0;
  std::vector<std::string> feature_dimensions;
  std::vector<std::int64_t> generations;
  std::int64_t last_migration_generation = 0;
  int migration_interval = 0;
  double migration_rate = 0;
  std::int64_t last_iteration = 0;
  int current_island = 0;
};

struct ArchiveDecision {
  bool add = false;
  std::optional<std::string> evict_id;
};

struct MigrationMove {
  std::string program_id;
  int target_island = 0;
};

// Hooks may return errors. Exceptions become Internal; database changes roll
// back. External hook side effects are the caller's responsibility.
struct PopulationStrategy {
  std::function<absl::StatusOr<bool>(const PopulationSnapshot&, const Program&, int)> admit;
  std::function<absl::StatusOr<bool>(const PopulationSnapshot&, const Program&, const Program&, int)> replace_cell;
  std::function<absl::StatusOr<ArchiveDecision>(const PopulationSnapshot&, const Program&)> archive;
  std::function<absl::StatusOr<std::vector<std::string>>(const PopulationSnapshot&, std::size_t,
                                                         const std::set<std::string>&)>
      evict;
  std::function<absl::StatusOr<bool>(const PopulationSnapshot&)> migration_due;
  std::function<absl::StatusOr<std::vector<MigrationMove>>(const PopulationSnapshot&)> migrate;
};

struct AddOptions {
  std::optional<int> island;
  std::optional<std::int64_t> iteration;
};

struct PopulationSample {
  Program parent;
  std::vector<Program> inspirations;
};

}  // namespace ievolve

#endif  // IEVOLVE_DATABASE_POPULATION_H_
