#ifndef IEVOLVE_DATABASE_PROGRAM_DATABASE_H_
#define IEVOLVE_DATABASE_PROGRAM_DATABASE_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ievolve/database/artifact_store.h"
#include "ievolve/database/checkpoint.h"
#include "ievolve/database/feature_mapper.h"
#include "ievolve/database/island_selection.h"
#include "ievolve/database/population.h"
#include "ievolve/database/program_store.h"
#include "ievolve/program/program.h"

namespace ievolve {

// Owns snapshots; input and returned values cannot mutate stored programs.
// Concurrent access requires caller synchronization.
//
// A facade over three units: ProgramStore holds the programs, Population runs
// the island/MAP-Elites algorithms over that store, and database_codec maps
// both to and from checkpoints. This class owns what spans them: the
// copy-and-swap transaction, nested-mutation rejection, checkpoint save/load
// and artifacts, using its own copy of the storage settings. It stays copyable; the controller relies on that to roll
// back.
class ProgramDatabase {
 public:
  explicit ProgramDatabase(std::vector<std::string> feature_dimensions = {"complexity", "diversity"});
  // Enable population management on an empty database. Changes reach disk
  // only through Save; resume with Load.
  static absl::StatusOr<ProgramDatabase> Create(const DatabaseConfig& config, PopulationStrategy strategy = {});
  absl::Status Add(const Program& program);
  // False means a policy declined admission; no database state is changed.
  absl::StatusOr<bool> Add(const Program& program, const AddOptions& options);
  absl::StatusOr<Program> Get(std::string_view id) const;
  absl::StatusOr<Program> GetBestProgram(const std::optional<std::string>& metric = std::nullopt) const;
  absl::StatusOr<std::vector<Program>> GetTopPrograms(int n = 10,
                                                      const std::optional<std::string>& metric = std::nullopt) const;
  std::size_t size() const { return state_.programs.size(); }

  absl::StatusOr<PopulationSnapshot> Snapshot() const;
  absl::StatusOr<std::map<std::string, FeatureStats>> FeatureStatistics() const;
  absl::StatusOr<PopulationSample> Sample(int num_inspirations = 5);
  absl::StatusOr<PopulationSample> SampleFromIsland(int island, int num_inspirations = 5);
  absl::Status SetCurrentIsland(int island);
  absl::Status IncrementGeneration(int island);
  absl::StatusOr<bool> ShouldMigrate();
  absl::Status Migrate();
  absl::StatusOr<IslandSelectionContext> SelectionContext(std::int64_t iteration,
                                                          const std::vector<int>& pending_counts) const;

  absl::Status Save(const std::filesystem::path& path) const;
  absl::Status Load(const std::filesystem::path& path);
  absl::Status StoreArtifacts(std::string_view id, const ArtifactMap& artifacts);
  absl::StatusOr<ArtifactMap> GetArtifacts(std::string_view id) const;
  absl::StatusOr<std::size_t> CleanupArtifacts();
  absl::Status LogPrompt(std::string_view id, std::string_view template_key, const Metrics& prompt,
                         const std::vector<std::string>& responses = {},
                         const std::optional<Metrics>& token_usage = std::nullopt);

 private:
  // Everything a transaction rolls back. Copied wholesale before a change and
  // swapped in only after the change succeeds.
  struct State {
    ProgramStore programs;
    std::optional<Population> population;
  };

  // Island, capacity and ratio limits the population algorithms rely on.
  static absl::Status CheckConfig(const DatabaseConfig& config);
  absl::Status CheckIsland(int island) const;
  absl::Status Mutate(const std::function<absl::Status(State&)>& action);
  absl::Status ModifyProgram(std::string_view id, const std::function<absl::Status(Program&)>& modify);

  // Storage settings (log_prompts, artifacts) are read from here. It
  // is immutable after Create, so it stays outside the transactional State.
  DatabaseConfig config_;
  State state_;
  bool mutation_active_ = false;
};

}  // namespace ievolve

#endif  // IEVOLVE_DATABASE_PROGRAM_DATABASE_H_
