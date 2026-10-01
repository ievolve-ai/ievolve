#include "ievolve/database/program_database.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace ievolve {
namespace {
struct InvalidState {};
void Require(bool valid) {
  if (!valid) throw InvalidState{};
}

std::uint64_t Unsigned(const Metrics& value) {
  Require(value.is_number_integer());
  if (!value.is_number_unsigned()) Require(value.get<std::int64_t>() >= 0);

  return value.get<std::uint64_t>();
}
std::int64_t Counter(const Metrics& value) {
  const auto number = AsCounter(value);
  Require(number.has_value());
  return *number;
}
std::string String(const Metrics& value) {
  Require(value.is_string());
  return value.get<std::string>();
}
std::optional<std::string> OptionalId(const Metrics& value) {
  if (value.is_null()) return std::nullopt;
  return String(value);
}
std::set<std::string> IdSet(const Metrics& value) {
  Require(value.is_array());

  std::set<std::string> result;
  for (const auto& id : value) Require(result.insert(String(id)).second);

  return result;
}
Metrics MaybeId(const std::optional<std::string>& id) { return id ? Metrics(*id) : Metrics(nullptr); }
std::string CellKey(const FeatureCoordinates& coordinates) {
  std::string result;
  for (int coordinate : coordinates) {
    if (!result.empty()) result += '-';
    result += std::to_string(coordinate);
  }

  return result;
}
FeatureCoordinates ReadCell(const std::string& key, const std::vector<std::string>& dimensions,
                            const std::map<std::string, int>& bins) {
  FeatureCoordinates coordinates;
  std::size_t start = 0;
  while (start < key.size()) {
    auto end = key.find('-', start);
    if (end == std::string::npos) end = key.size();

    int number = -1;
    auto parsed = std::from_chars(key.data() + start, key.data() + end, number);
    Require(parsed.ec == std::errc{} && parsed.ptr == key.data() + end && number >= 0);

    Require(coordinates.size() < dimensions.size());
    Require(number < bins.at(dimensions[coordinates.size()]));
    coordinates.push_back(number);
    start = end + 1;
  }

  Require(coordinates.size() == dimensions.size() && CellKey(coordinates) == key);

  return coordinates;
}
Metrics PopulationConfiguration(const DatabaseConfig& config, const std::map<std::string, int>& bins) {
  return {{"num_islands", config.num_islands},
          {"population_size", config.population_size},
          {"archive_size", config.archive_size},
          {"feature_dimensions", config.feature_dimensions},
          {"feature_bins", bins},
          {"diversity_reference_size", config.diversity_reference_size},
          {"diversity_metric", config.diversity_metric},
          {"exploration_ratio", config.exploration_ratio},
          {"exploitation_ratio", config.exploitation_ratio},
          {"elite_selection_ratio", config.elite_selection_ratio},
          {"migration_interval", config.migration_interval},
          {"migration_rate", config.migration_rate}};
}
}  // namespace

ProgramDatabase::ProgramDatabase(std::vector<std::string> dimensions)
    : state_{ProgramStore(std::move(dimensions)), std::nullopt} {}

absl::StatusOr<ProgramDatabase> ProgramDatabase::Create(const DatabaseConfig& config, PopulationStrategy strategy) {
  auto population = Population::Create(config, std::move(strategy));
  if (!population.ok()) return population.status();
  if ((!config.in_memory && !config.db_path) ||
      (config.db_path && (config.db_path->empty() || config.db_path->find('\0') != std::string::npos))) {
    return absl::InvalidArgumentError("Disk mode requires a database path");
  }

  auto artifacts = ArtifactStore::Create(config);
  if (!artifacts.ok()) return artifacts.status();

  ProgramDatabase database(config.feature_dimensions);
  database.state_.population = std::move(*population);

  if (config.db_path) {
    std::error_code error;
    const std::filesystem::path path(*config.db_path);
    const auto current_entry = std::filesystem::symlink_status(path / "CURRENT", error);
    if (error && error != std::errc::no_such_file_or_directory) {
      return absl::FailedPreconditionError("Cannot inspect database path");
    }

    error.clear();
    const auto legacy_entry = std::filesystem::symlink_status(path / "metadata.json", error);
    if (error && error != std::errc::no_such_file_or_directory) {
      return absl::FailedPreconditionError("Cannot inspect database path");
    }

    const bool current = std::filesystem::exists(current_entry);
    const bool legacy = std::filesystem::exists(legacy_entry);
    if (current || legacy) {
      auto status = database.Load(path);
      if (!status.ok()) return status;
    }
  }

  return database;
}

absl::Status ProgramDatabase::CheckIsland(int island) const {
  if (!state_.population) return absl::FailedPreconditionError("Population mode is not enabled");

  return state_.population->CheckIsland(island);
}

// The transactional primitive every population mutation goes through. The
// action runs against a full copy of the state, so a failure anywhere inside it
// leaves this database untouched: programs, cells, archive, feature statistics,
// counters and RNG all roll back together. In disk mode the checkpoint is
// published before the in-memory swap, so a failed write never leaves memory
// ahead of disk. mutation_active_ rejects a strategy callback that tries to
// mutate while its own snapshot is being computed.
absl::Status ProgramDatabase::Mutate(const std::function<absl::Status(State&)>& action, const bool* commit) {
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

    // Admission rejection has no state to save.
    if (commit && !*commit) return absl::OkStatus();

    const auto& config = staged.population->config();
    if (!config.in_memory) {
      status = WriteCheckpoint(staged, *config.db_path);
      if (!status.ok()) return status;
    }

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
  auto status = Mutate(
      [&](State& staged) {
        auto result = staged.population->Insert(staged.programs, program, options);
        if (!result.ok()) return result.status();

        accepted = *result;
        return absl::OkStatus();
      },
      &accepted);
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

DatabaseConfig ProgramDatabase::StorageConfiguration() const {
  return state_.population ? state_.population->config() : DatabaseConfig{};
}

absl::Status ProgramDatabase::WriteCheckpoint(const State& state, const std::filesystem::path& path) {
  auto data = SerializeCheckpoint(state);
  if (!data.ok()) return data.status();

  return Checkpoint::Save(path, *data, state.population ? state.population->config() : DatabaseConfig{});
}

absl::Status ProgramDatabase::Save(const std::filesystem::path& path) const {
  if (mutation_active_) return absl::FailedPreconditionError("Cannot save during a database mutation");

  auto selected = path;
  if (selected.empty() && state_.population && state_.population->config().db_path) {
    selected = *state_.population->config().db_path;
  }
  if (selected.empty()) return absl::InvalidArgumentError("Checkpoint path is required");

  try {
    return WriteCheckpoint(state_, selected);
  } catch (...) {
    return absl::InternalError("Checkpoint serialization failed");
  }
}

// Captures everything a resumed run needs to continue rather than restart:
// programs, island and cell ownership, archive, best pointers, generation and
// migration counters, feature statistics, and the RNG stream position. The
// serialized RNG is why sampling must go through Mutate.
absl::StatusOr<CheckpointData> ProgramDatabase::SerializeCheckpoint(const State& db) {
  CheckpointData data;
  data.programs = db.programs.programs();
  data.metadata = {{"format", "ievolve.database"},
                   {"version", 1},
                   {"mode", db.population ? "population" : "core"},
                   {"feature_dimensions", db.programs.feature_dimensions()}};
  if (!db.population) return data;

  const auto& population = *db.population;
  const auto& state = population.state();
  auto& metadata = data.metadata;
  metadata["population_config"] = PopulationConfiguration(population.config(), population.mapper().bins());

  metadata["islands"] = state.islands;
  metadata["island_feature_maps"] = Metrics::array();
  for (const auto& grid : state.feature_maps) {
    auto object = Metrics::object();
    for (const auto& [coordinates, id] : grid) object[CellKey(coordinates)] = id;
    metadata["island_feature_maps"].push_back(std::move(object));
  }

  metadata["archive"] = state.archive;
  metadata["best_program_id"] = MaybeId(state.best);
  metadata["island_best_programs"] = Metrics::array();
  for (const auto& best : state.island_best) metadata["island_best_programs"].push_back(MaybeId(best));

  metadata["island_generations"] = state.generations;
  metadata["current_island"] = state.current_island;
  metadata["last_iteration"] = state.last_iteration;
  metadata["last_migration_generation"] = state.last_migration;
  metadata["next_migrant"] = state.next_migrant;

  metadata["feature_stats"] = Metrics::object();
  for (const auto& [dimension, stats] : population.mapper().stats())
    metadata["feature_stats"][dimension] = {{"min", stats.min}, {"max", stats.max}, {"count", stats.count}};

  std::ostringstream random;
  random.imbue(std::locale::classic());
  random << state.random;
  if (!random) return absl::InternalError("Cannot serialize random state");
  metadata["rng_state"] = random.str();

  return data;
}

// Rebuilds state from a checkpoint, validating as it goes. Require() throws on
// the first inconsistency and the catch below turns that into one DataLoss
// status, so a corrupt or hand-edited checkpoint is rejected whole rather than
// applied in part. Cross-references are checked, not trusted: every ID named by
// an island, cell, archive or best pointer must exist, cells and islands may
// not claim the same program twice, and the counts must add up. data.legacy
// marks an imported Python snapshot, which lacks fields this format requires.
absl::Status ProgramDatabase::RestoreCheckpoint(const CheckpointData& data, State& db) {
  try {
    const auto& metadata = data.metadata;
    Require(metadata.is_object());
    if (!data.legacy) {
      Require(String(metadata.at("format")) == "ievolve.database");
      Require(Unsigned(metadata.at("version")) == 1);
      const auto mode = String(metadata.at("mode"));
      Require(mode == "population" || mode == "core");
      if ((mode == "population") != db.population.has_value() ||
          metadata.at("feature_dimensions") != Metrics(db.programs.feature_dimensions())) {
        return absl::FailedPreconditionError("Checkpoint mode or feature dimensions differ");
      }
    } else if (!db.population) {
      return absl::FailedPreconditionError("Python checkpoints require population mode");
    }

    if (db.population && !data.legacy &&
        nlohmann::json(metadata.at("population_config")) !=
            nlohmann::json(PopulationConfiguration(db.population->config(), db.population->mapper().bins()))) {
      return absl::FailedPreconditionError("Checkpoint population configuration differs");
    }

    auto& programs = db.programs;
    programs.Clear();
    for (const auto& program : data.programs) {
      const auto status = programs.Add(program);
      Require(status.ok());
    }
    if (!db.population) return absl::OkStatus();

    auto& population = *db.population;
    const auto reset = population.Reset();
    if (!reset.ok()) return reset;
    const auto& config = population.config();
    auto& state = population.state();
    const std::size_t count = state.islands.size();
    const auto& islands = metadata.at("islands");
    const auto& grids = metadata.at("island_feature_maps");
    Require(islands.is_array() && grids.is_array() && islands.size() == count && grids.size() == count);

    std::set<std::string> owned, cell_owners;
    for (std::size_t island = 0; island < count; ++island) {
      state.islands[island] = IdSet(islands[island]);
      for (const auto& id : state.islands[island]) {
        Require(programs.Contains(id) && owned.insert(id).second);
        auto& program = programs.at(id);
        if (!data.legacy) Require(Counter(program.metadata.at("island")) == static_cast<std::int64_t>(island));
        program.metadata["island"] = island;
      }

      Require(grids[island].is_object());
      for (const auto& item : grids[island].items()) {
        const auto id = String(item.value());
        Require(state.islands[island].count(id) && cell_owners.insert(id).second);
        state.feature_maps[island].emplace(
            ReadCell(item.key(), programs.feature_dimensions(), population.mapper().bins()), id);
      }
    }

    state.archive = IdSet(metadata.at("archive"));
    Require(state.archive.size() <= static_cast<std::size_t>(config.archive_size));
    for (const auto& id : state.archive) Require(programs.Contains(id));

    if (!metadata.contains("best_program_id")) Require(data.legacy);
    state.best = metadata.contains("best_program_id") ? OptionalId(metadata.at("best_program_id")) : std::nullopt;
    if (state.best) Require(programs.Contains(*state.best));
    if (!data.legacy) Require(state.best.has_value() != programs.empty());

    // Older checkpoints can omit best_program_id while retaining a historical
    // champion outside every island. Recover it before validating ownership.
    if (data.legacy && !state.best) population.RefreshBest(programs);

    for (const auto& program : programs.programs()) {
      Require(owned.count(program.id) || state.archive.count(program.id) || state.best == program.id);
      if (!owned.count(program.id)) {
        Require(program.metadata.contains("island"));
        Require(Counter(program.metadata.at("island")) < static_cast<std::int64_t>(count));
      }
    }

    Require(programs.size() <= static_cast<std::size_t>(config.population_size) + 1);

    if (metadata.contains("island_best_programs")) {
      const auto& bests = metadata.at("island_best_programs");
      Require(bests.is_array() && bests.size() == count);
      for (std::size_t i = 0; i < count; ++i) {
        state.island_best[i] = OptionalId(bests[i]);
        if (state.island_best[i]) Require(state.islands[i].count(*state.island_best[i]));
        if (!data.legacy) Require(state.island_best[i].has_value() != state.islands[i].empty());
      }
    } else {
      Require(data.legacy);
    }

    const auto saved_best = state.best;
    const auto saved_island_best = state.island_best;
    population.RefreshBest(programs);
    if (!data.legacy) Require(saved_best == state.best && saved_island_best == state.island_best);

    if (metadata.contains("island_generations")) {
      const auto& generations = metadata.at("island_generations");
      Require(generations.is_array() && generations.size() == count);
      for (std::size_t i = 0; i < count; ++i) state.generations[i] = Counter(generations[i]);
    } else {
      Require(data.legacy);
    }

    if (!metadata.contains("current_island")) Require(data.legacy);
    const auto current = metadata.contains("current_island") ? Counter(metadata.at("current_island")) : 0;
    Require(current < static_cast<std::int64_t>(count));
    state.current_island = static_cast<int>(current);

    if (!metadata.contains("last_iteration")) Require(data.legacy);
    state.last_iteration = metadata.contains("last_iteration") ? Counter(metadata.at("last_iteration")) : 0;
    if (data.legacy) {
      for (const auto& program : programs.programs())
        state.last_iteration = std::max(state.last_iteration, program.iteration_found);
    }

    state.last_migration =
        metadata.contains("last_migration_generation") ? Counter(metadata.at("last_migration_generation")) : 0;
    if (!metadata.contains("last_migration_generation")) Require(data.legacy);
    Require(state.last_migration <= *std::max_element(state.generations.begin(), state.generations.end()));

    if (!data.legacy) {
      for (const auto& program : programs.programs()) Require(program.iteration_found <= state.last_iteration);
      state.next_migrant = Unsigned(metadata.at("next_migrant"));

      std::istringstream random(String(metadata.at("rng_state")));
      random.imbue(std::locale::classic());
      random >> state.random;
      Require(!random.fail());
      random >> std::ws;
      Require(random.eof());
    }

    std::map<std::string, FeatureStats> stats;
    if (metadata.contains("feature_stats")) {
      const auto& values = metadata.at("feature_stats");
      Require(values.is_object());
      for (const auto& item : values.items()) {
        const auto& value = item.value();
        Require(value.is_object() && value.at("min").is_number() && value.at("max").is_number());

        std::uint64_t samples = 0;
        if (value.contains("count")) {
          samples = Unsigned(value.at("count"));
        } else {
          Require(data.legacy && value.at("values").is_array());
          for (const auto& sample : value.at("values")) Require(sample.is_number());
          samples = std::max<std::size_t>(1, value.at("values").size());
        }

        Require(samples <= std::numeric_limits<std::size_t>::max());
        stats.emplace(item.key(), FeatureStats{value.at("min").get<double>(), value.at("max").get<double>(),
                                               static_cast<std::size_t>(samples)});
      }
    } else {
      Require(data.legacy);
    }
    Require(population.mapper().RestoreStatistics(stats).ok());

    return absl::OkStatus();
  } catch (...) {
    return absl::DataLossError("Invalid checkpoint database state");
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
    auto status = RestoreCheckpoint(*data, staged);
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
    auto store = ArtifactStore::Create(StorageConfiguration());
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

  auto store = ArtifactStore::Create(StorageConfiguration());
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
  if (!StorageConfiguration().log_prompts) return absl::OkStatus();
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
