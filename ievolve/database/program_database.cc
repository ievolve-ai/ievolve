#include "ievolve/database/program_database.h"

#include <cmath>
#include <utility>

#include "ievolve/database/database_codec.h"

namespace ievolve {
namespace {
bool Ratio(double value) { return std::isfinite(value) && value >= 0 && value <= 1; }

PopulationConfig ToPopulationConfig(const DatabaseConfig& config) {
  PopulationConfig population;
  population.population_size = config.population_size;
  population.archive_size = config.archive_size;
  population.num_islands = config.num_islands;
  population.elite_selection_ratio = config.elite_selection_ratio;
  population.exploration_ratio = config.exploration_ratio;
  population.exploitation_ratio = config.exploitation_ratio;
  population.migration_interval = config.migration_interval;
  population.migration_rate = config.migration_rate;
  population.random_seed = config.random_seed;

  return population;
}
}  // namespace

ProgramDatabase::ProgramDatabase(std::vector<std::string> dimensions)
    : state_{ProgramStore(std::move(dimensions)), std::nullopt} {}

absl::Status ProgramDatabase::CheckConfig(const DatabaseConfig& config) {
  if (config.num_islands <= 0 || config.population_size <= 0 || config.archive_size < 0 ||
      config.migration_interval <= 0 || !Ratio(config.elite_selection_ratio) || !Ratio(config.exploration_ratio) ||
      !Ratio(config.exploitation_ratio) || !Ratio(config.migration_rate) ||
      config.exploration_ratio + config.exploitation_ratio > 1.0) {
    return absl::InvalidArgumentError("Invalid population configuration");
  }

  return absl::OkStatus();
}

// Validation order is observable when a configuration has several problems:
// population limits, then storage, then features.
absl::StatusOr<ProgramDatabase> ProgramDatabase::Create(const DatabaseConfig& config, PopulationStrategy strategy) {
  auto status = CheckConfig(config);
  if (!status.ok()) return status;

  auto artifacts = ArtifactStore::Create(config);
  if (!artifacts.ok()) return artifacts.status();

  auto mapper = FeatureMapper::Create(config);
  if (!mapper.ok()) return mapper.status();

  ProgramDatabase database(config.feature_dimensions);
  database.config_ = config;
  database.state_.population.emplace(ToPopulationConfig(config), std::move(*mapper), std::move(strategy));

  return database;
}

absl::Status ProgramDatabase::CheckIsland(int island) const {
  if (!state_.population) return absl::FailedPreconditionError("Population mode is not enabled");

  return state_.population->CheckIsland(island);
}

// The transactional primitive every population mutation goes through. The
// action runs against a full copy of the state, so a failure anywhere inside it
// leaves this database untouched: programs, cells, archive, feature statistics,
// counters and RNG all roll back together. mutation_active_ rejects a strategy
// callback that tries to mutate while its own snapshot is being computed.
absl::Status ProgramDatabase::Mutate(const std::function<absl::Status(State&)>& action) {
  if (!state_.population) return absl::FailedPreconditionError("Population mode is not enabled");
  if (mutation_active_) return absl::FailedPreconditionError("Nested population mutation is forbidden");

  mutation_active_ = true;
  struct Reset {
    bool& flag;
    ~Reset() { flag = false; }
  } reset{mutation_active_};

  try {
    State staged = state_;
    auto status = action(staged);
    if (!status.ok()) return status;

    std::swap(state_, staged);

    return absl::OkStatus();
  } catch (...) {
    return absl::InternalError("Population operation threw an exception");
  }
}

absl::Status ProgramDatabase::Add(const Program& program) {
  if (state_.population) {
    auto added = Add(program, AddOptions{});
    if (!added.ok()) return added.status();

    return *added ? absl::OkStatus() : absl::FailedPreconditionError("Candidate admission declined");
  }

  return state_.programs.Add(program);
}

absl::StatusOr<bool> ProgramDatabase::Add(const Program& program, const AddOptions& options) {
  bool accepted = false;
  auto status = Mutate([&](State& staged) {
    auto result = staged.population->Insert(staged.programs, program, options);
    if (!result.ok()) return result.status();

    accepted = *result;
    return absl::OkStatus();
  });
  if (!status.ok()) return status;

  return accepted;
}

absl::StatusOr<Program> ProgramDatabase::Get(std::string_view id) const { return state_.programs.Get(id); }

absl::StatusOr<Program> ProgramDatabase::GetBestProgram(const std::optional<std::string>& metric) const {
  const auto& population = state_.population;
  if (population && (!metric || metric->empty()) && population->state().best) return Get(*population->state().best);

  auto top = GetTopPrograms(1, metric);
  if (!top.ok()) return top.status();
  if (top->empty()) return absl::NotFoundError("No program matches the requested metric");

  return std::move(top->front());
}

absl::StatusOr<std::vector<Program>> ProgramDatabase::GetTopPrograms(int n,
                                                                     const std::optional<std::string>& metric) const {
  return state_.programs.Top(n, metric);
}

absl::StatusOr<PopulationSnapshot> ProgramDatabase::Snapshot() const {
  if (!state_.population) return absl::FailedPreconditionError("Population mode is not enabled");

  return state_.population->Snapshot(state_.programs);
}

absl::StatusOr<std::map<std::string, FeatureStats>> ProgramDatabase::FeatureStatistics() const {
  if (!state_.population) return absl::FailedPreconditionError("Population mode is not enabled");

  return state_.population->mapper().stats();
}

// Sampling runs through Mutate because it advances the RNG, which is persisted
// state: a resumed run must continue the same stream rather than replay it.
absl::StatusOr<PopulationSample> ProgramDatabase::Sample(int num_inspirations) {
  std::optional<PopulationSample> result;
  auto status = Mutate([&](State& staged) {
    auto sample = staged.population->Sample(staged.programs, std::nullopt, num_inspirations);
    if (!sample.ok()) return sample.status();

    result = std::move(*sample);
    return absl::OkStatus();
  });
  if (!status.ok()) return status;

  return std::move(*result);
}

absl::StatusOr<PopulationSample> ProgramDatabase::SampleFromIsland(int island, int num_inspirations) {
  auto status = CheckIsland(island);
  if (!status.ok()) return status;

  std::optional<PopulationSample> result;
  status = Mutate([&](State& staged) {
    auto sample = staged.population->Sample(staged.programs, island, num_inspirations);
    if (!sample.ok()) return sample.status();

    result = std::move(*sample);
    return absl::OkStatus();
  });
  if (!status.ok()) return status;

  return std::move(*result);
}

absl::Status ProgramDatabase::SetCurrentIsland(int island) {
  auto status = CheckIsland(island);
  if (!status.ok()) return status;

  return Mutate([&](State& staged) { return staged.population->SetCurrentIsland(island); });
}

absl::Status ProgramDatabase::IncrementGeneration(int island) {
  auto status = CheckIsland(island);
  if (!status.ok()) return status;

  return Mutate([&](State& staged) { return staged.population->IncrementGeneration(island); });
}

absl::StatusOr<bool> ProgramDatabase::ShouldMigrate() {
  if (!state_.population) return absl::FailedPreconditionError("Population mode is not enabled");
  if (mutation_active_) return absl::FailedPreconditionError("Nested population mutation is forbidden");

  mutation_active_ = true;
  struct Reset {
    bool& flag;
    ~Reset() { flag = false; }
  } reset{mutation_active_};

  try {
    return state_.population->MigrationDue(state_.programs);
  } catch (...) {
    return absl::InternalError("Migration query threw an exception");
  }
}

absl::Status ProgramDatabase::Migrate() {
  return Mutate([](State& staged) { return staged.population->Migrate(staged.programs); });
}

absl::StatusOr<IslandSelectionContext> ProgramDatabase::SelectionContext(std::int64_t iteration,
                                                                         const std::vector<int>& pending_counts) const {
  if (!state_.population) return absl::FailedPreconditionError("Population mode is not enabled");

  return state_.population->SelectionContext(state_.programs, iteration, pending_counts);
}

absl::Status ProgramDatabase::Save(const std::filesystem::path& path) const {
  if (mutation_active_) return absl::FailedPreconditionError("Cannot save during a database mutation");

  if (path.empty()) return absl::InvalidArgumentError("Checkpoint path is required");

  try {
    auto data = database_codec::Encode(state_.programs, state_.population ? &*state_.population : nullptr);
    if (!data.ok()) return data.status();

    return Checkpoint::Save(path, *data, config_);
  } catch (...) {
    return absl::InternalError("Checkpoint serialization failed");
  }
}

// Replaces the current state wholesale. Loading is not a merge: it runs no
// admission, replacement or eviction callbacks and writes nothing back to the
// configured path. Like Mutate, restoration happens on a copy that is swapped
// in only once every validation has passed.
absl::Status ProgramDatabase::Load(const std::filesystem::path& path) {
  if (mutation_active_) return absl::FailedPreconditionError("Cannot load during a database mutation");

  mutation_active_ = true;
  struct Reset {
    bool& flag;
    ~Reset() { flag = false; }
  } reset{mutation_active_};

  try {
    auto data = Checkpoint::Load(path);
    if (!data.ok()) return data.status();

    auto staged = state_;
    auto status = database_codec::Decode(*data, staged.programs, staged.population);
    if (!status.ok()) return status;

    std::swap(state_, staged);

    return absl::OkStatus();
  } catch (...) {
    return absl::InternalError("Checkpoint restoration failed");
  }
}

absl::Status ProgramDatabase::ModifyProgram(std::string_view id, const std::function<absl::Status(Program&)>& modify) {
  if (mutation_active_) return absl::FailedPreconditionError("Nested database mutation is forbidden");

  if (!state_.programs.Contains(id)) return absl::NotFoundError("Program ID not found");
  const std::string key(id);

  // The callback edits the staged copy, leaving this database untouched until
  // the swap.
  if (state_.population) {
    return Mutate([&](State& staged) {
      auto& candidate = staged.programs.at(key);
      auto status = modify(candidate);
      return status.ok() ? candidate.Validate() : status;
    });
  }

  try {
    auto candidate = state_.programs.at(key);
    auto status = modify(candidate);
    if (!status.ok()) return status;

    status = candidate.Validate();
    if (!status.ok()) return status;

    state_.programs.at(key) = std::move(candidate);
    return absl::OkStatus();
  } catch (...) {
    return absl::InternalError("Program update failed");
  }
}

absl::Status ProgramDatabase::StoreArtifacts(std::string_view id, const ArtifactMap& artifacts) {
  return ModifyProgram(id, [&](Program& candidate) {
    auto store = ArtifactStore::Create(config_);
    if (!store.ok()) return store.status();

    auto stored = store->Store(candidate, artifacts);
    if (!stored.ok()) return stored.status();

    candidate = std::move(*stored);
    return absl::OkStatus();
  });
}

absl::StatusOr<ArtifactMap> ProgramDatabase::GetArtifacts(std::string_view id) const {
  auto program = Get(id);
  if (!program.ok()) return program.status();

  return ArtifactStore::Load(*program);
}

absl::StatusOr<std::size_t> ProgramDatabase::CleanupArtifacts() {
  if (mutation_active_) return absl::FailedPreconditionError("Cannot clean artifacts during a database mutation");

  auto store = ArtifactStore::Create(config_);
  if (!store.ok()) return store.status();

  std::set<std::string> protected_directories;
  for (const auto& program : state_.programs.programs())
    if (program.artifact_dir) protected_directories.insert(*program.artifact_dir);

  return store->Cleanup(protected_directories);
}

absl::Status ProgramDatabase::LogPrompt(std::string_view id, std::string_view template_key, const Metrics& prompt,
                                        const std::vector<std::string>& responses,
                                        const std::optional<Metrics>& token_usage) {
  if (mutation_active_) return absl::FailedPreconditionError("Nested database mutation is forbidden");
  if (!config_.log_prompts) return absl::OkStatus();
  if (template_key.empty() || !prompt.is_object() || (token_usage && !token_usage->is_object())) {
    return absl::InvalidArgumentError("Invalid prompt log fields");
  }

  return ModifyProgram(id, [&](Program& candidate) {
    if (!candidate.prompts) candidate.prompts = Metrics::object();

    auto entry = prompt;
    entry["responses"] = responses;
    if (token_usage) entry["token_usage"] = *token_usage;
    (*candidate.prompts)[std::string(template_key)] = std::move(entry);

    return candidate.Validate();
  });
}
}  // namespace ievolve
