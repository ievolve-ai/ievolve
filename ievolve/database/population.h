#ifndef IEVOLVE_DATABASE_POPULATION_H_
#define IEVOLVE_DATABASE_POPULATION_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "ievolve/config/types.h"
#include "ievolve/database/feature_mapper.h"
#include "ievolve/database/program_store.h"
#include "ievolve/database/selection.h"
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

// Island-based MAP-Elites population over programs held in a ProgramStore:
// islands, feature cells, the shared elite archive, best pointers, sampling and
// migration. It never keeps a pointer or reference to the store; every
// operation receives it explicitly, so copying a Population together with its
// store yields an independent pair, which the database transaction relies on.
// Operations that fail may leave the population and store inconsistent, so
// callers run them on copies and discard the copies on error.
class Population {
 public:
  struct State {
    std::vector<std::set<std::string>> islands;
    std::vector<FeatureMap> feature_maps;
    std::set<std::string> archive;
    std::optional<std::string> best;
    std::vector<std::optional<std::string>> island_best;
    std::vector<std::int64_t> generations;
    std::int64_t last_migration = 0;
    std::int64_t last_iteration = 0;
    int current_island = 0;
    std::uint64_t next_migrant = 0;
    std::mt19937_64 random;
  };

  // Island, capacity and ratio limits; needs no state.
  // Create runs it first, then checks the diversity metric and feature setup.
  static absl::Status CheckConfig(const DatabaseConfig& config);
  static absl::StatusOr<Population> Create(const DatabaseConfig& config, PopulationStrategy strategy = {});
  // Back to the freshly created state: empty islands, new feature statistics,
  // reseeded RNG. Configuration and strategy are kept.
  absl::Status Reset();

  // Returns false when a strategy declined the candidate.
  absl::StatusOr<bool> Insert(ProgramStore& store, const Program& program, const AddOptions& options);
  // Without an island, samples from the current island and lets inspirations
  // follow the parent's island. Advances the RNG.
  absl::StatusOr<PopulationSample> Sample(const ProgramStore& store, std::optional<int> island, int count);
  absl::StatusOr<IslandSelectionContext> SelectionContext(const ProgramStore& store, std::int64_t iteration,
                                                          const std::vector<int>& pending_counts) const;
  PopulationSnapshot Snapshot(const ProgramStore& store) const;
  absl::Status SetCurrentIsland(int island);
  absl::Status IncrementGeneration(int island);
  absl::StatusOr<bool> MigrationDue(const ProgramStore& store) const;
  absl::Status Migrate(ProgramStore& store);
  // Recomputes the global and per-island best from the current members.
  void RefreshBest(const ProgramStore& store);
  absl::Status CheckIsland(int island) const;

  const DatabaseConfig& config() const { return config_; }
  const FeatureMapper& mapper() const { return mapper_; }
  FeatureMapper& mapper() { return mapper_; }
  const State& state() const { return state_; }
  // For checkpoint restoration, which validates what it writes.
  State& state() { return state_; }

 private:
  Population(DatabaseConfig config, FeatureMapper mapper, std::shared_ptr<const PopulationStrategy> strategy);

  absl::Status UpdateArchive(const ProgramStore& store, const Program& candidate);
  absl::Status EnforceCapacity(ProgramStore& store, const std::string& candidate);
  void Remove(ProgramStore& store, const std::string& id);
  bool Better(const ProgramStore& store, const Program& left, const Program& right) const;
  bool Owned(const std::string& id) const;
  bool OwnsCell(const std::string& id) const;

  DatabaseConfig config_;
  FeatureMapper mapper_;
  std::shared_ptr<const PopulationStrategy> strategy_;
  State state_;
};

}  // namespace ievolve

#endif  // IEVOLVE_DATABASE_POPULATION_H_
