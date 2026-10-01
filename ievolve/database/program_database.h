#ifndef IEVOLVE_DATABASE_PROGRAM_DATABASE_H_
#define IEVOLVE_DATABASE_PROGRAM_DATABASE_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "ievolve/database/artifact_store.h"
#include "ievolve/database/checkpoint.h"
#include "ievolve/database/feature_mapper.h"
#include "ievolve/database/population.h"
#include "ievolve/database/program_store.h"
#include "ievolve/database/selection.h"
#include "ievolve/program/program.h"

namespace ievolve {

// Owns snapshots; input and returned values cannot mutate stored programs.
// Concurrent access requires caller synchronization.
class ProgramDatabase {
 public:
  explicit ProgramDatabase(std::vector<std::string> feature_dimensions = {"complexity", "diversity"});
  // Enable population management; auto-load an existing configured checkpoint.
  // Disk mode persists successful state changes before committing memory.
  static absl::StatusOr<ProgramDatabase> Create(const DatabaseConfig& config, PopulationStrategy strategy = {});
  absl::Status Add(const Program& program);
  // False means a policy declined admission; no database state is changed.
  absl::StatusOr<bool> Add(const Program& program, const AddOptions& options);
  absl::StatusOr<Program> Get(std::string_view id) const;
  absl::StatusOr<Program> GetBestProgram(const std::optional<std::string>& metric = std::nullopt) const;
  absl::StatusOr<std::vector<Program>> GetTopPrograms(int n = 10,
                                                      const std::optional<std::string>& metric = std::nullopt) const;
  std::size_t size() const { return programs_.size(); }

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

  absl::Status Save(const std::filesystem::path& path = {}) const;
  absl::Status Load(const std::filesystem::path& path);
  absl::Status StoreArtifacts(std::string_view id, const ArtifactMap& artifacts);
  absl::StatusOr<ArtifactMap> GetArtifacts(std::string_view id) const;
  absl::StatusOr<std::size_t> CleanupArtifacts();
  absl::Status LogPrompt(std::string_view id, std::string_view template_key, const Metrics& prompt,
                         const std::vector<std::string>& responses = {},
                         const std::optional<Metrics>& token_usage = std::nullopt);

 private:
  struct PopulationState {
    PopulationState(DatabaseConfig configuration, FeatureMapper features);
    DatabaseConfig config;
    FeatureMapper mapper;
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

  absl::Status Mutate(const std::function<absl::Status(ProgramDatabase&)>& action, const bool* commit = nullptr);
  absl::StatusOr<bool> Insert(const Program& program, const AddOptions& options);
  absl::Status UpdateArchive(const Program& program);
  absl::Status EnforceCapacity(const std::string& candidate);
  absl::StatusOr<PopulationSample> DrawSample(int island, int count, bool global_random);
  absl::Status MigratePopulation();
  PopulationSnapshot MakeSnapshot() const;
  bool Better(const Program& left, const Program& right) const;
  bool Owned(const std::string& id) const;
  bool OwnsCell(const std::string& id) const;
  void Remove(const std::string& id);
  void RefreshBest();
  absl::Status CheckIsland(int island) const;
  DatabaseConfig StorageConfiguration() const;
  absl::StatusOr<CheckpointData> SerializeCheckpoint() const;
  absl::Status RestoreCheckpoint(const CheckpointData& data);
  absl::Status WriteCheckpoint(const std::filesystem::path& path) const;
  absl::Status ModifyProgram(std::string_view id, const std::function<absl::Status(Program&)>& modify);

  ProgramStore programs_;
  std::optional<PopulationState> population_;
  std::shared_ptr<const PopulationStrategy> strategy_;
  bool mutation_active_ = false;
};

}  // namespace ievolve

#endif  // IEVOLVE_DATABASE_PROGRAM_DATABASE_H_
