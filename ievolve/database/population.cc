#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "ievolve/database/program_database.h"

namespace ievolve {
namespace {
bool Ratio(double value) { return std::isfinite(value) && value >= 0 && value <= 1; }
}  // namespace

ProgramDatabase::PopulationState::PopulationState(DatabaseConfig configuration, FeatureMapper features)
    : config(std::move(configuration)),
      mapper(std::move(features)),
      islands(config.num_islands),
      feature_maps(config.num_islands),
      island_best(config.num_islands),
      generations(config.num_islands, 0),
      random(config.random_seed ? static_cast<std::uint64_t>(*config.random_seed) : std::random_device{}()) {}

absl::StatusOr<ProgramDatabase> ProgramDatabase::Create(const DatabaseConfig& config, PopulationStrategy strategy) {
  if (config.num_islands <= 0 || config.population_size <= 0 || config.archive_size < 0 ||
      config.migration_interval <= 0 || !Ratio(config.elite_selection_ratio) || !Ratio(config.exploration_ratio) ||
      !Ratio(config.exploitation_ratio) || !Ratio(config.migration_rate) ||
      config.exploration_ratio + config.exploitation_ratio > 1.0) {
    return absl::InvalidArgumentError("Invalid population configuration");
  }
  if (config.embedding_model || config.embedding_api_base) {
    return absl::UnimplementedError("Embedding novelty is not migrated");
  }
  if ((!config.in_memory && !config.db_path) ||
      (config.db_path && (config.db_path->empty() || config.db_path->find('\0') != std::string::npos))) {
    return absl::InvalidArgumentError("Disk mode requires a database path");
  }

  auto artifacts = ArtifactStore::Create(config);
  if (!artifacts.ok()) return artifacts.status();
  if (config.diversity_metric != "edit_distance") return absl::InvalidArgumentError("Unsupported diversity metric");

  auto mapper = FeatureMapper::Create(config);
  if (!mapper.ok()) return mapper.status();

  ProgramDatabase database(config.feature_dimensions);
  database.population_.emplace(config, std::move(*mapper));
  database.strategy_ = std::make_shared<const PopulationStrategy>(std::move(strategy));

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
  if (!population_) return absl::FailedPreconditionError("Population mode is not enabled");
  if (island < 0 || island >= population_->config.num_islands) {
    return absl::InvalidArgumentError("Island index is out of range");
  }

  return absl::OkStatus();
}

// The transactional primitive every population mutation goes through. The
// action runs against a full copy, so a failure anywhere inside it leaves this
// database untouched: programs, cells, archive, feature statistics, counters
// and RNG all roll back together. In disk mode the checkpoint is published
// before the in-memory swap, so a failed write never leaves memory ahead of
// disk. mutation_active_ rejects a strategy callback that tries to mutate
// while its own snapshot is being computed.
absl::Status ProgramDatabase::Mutate(const std::function<absl::Status(ProgramDatabase&)>& action, const bool* commit) {
  if (!population_) return absl::FailedPreconditionError("Population mode is not enabled");
  if (mutation_active_) return absl::FailedPreconditionError("Nested population mutation is forbidden");

  mutation_active_ = true;
  struct Reset {
    bool& flag;
    ~Reset() { flag = false; }
  } reset{mutation_active_};

  try {
    ProgramDatabase staged = *this;
    auto status = action(staged);
    if (!status.ok()) return status;

    // Admission rejection has no state to save.
    if (commit && !*commit) return absl::OkStatus();

    if (!staged.population_->config.in_memory) {
      status = staged.WriteCheckpoint(*staged.population_->config.db_path);
      if (!status.ok()) return status;
    }

    programs_.swap(staged.programs_);
    index_.swap(staged.index_);
    population_.swap(staged.population_);

    return absl::OkStatus();
  } catch (...) {
    return absl::InternalError("Population operation threw an exception");
  }
}

absl::StatusOr<bool> ProgramDatabase::Add(const Program& program, const AddOptions& options) {
  bool accepted = false;
  auto status = Mutate(
      [&](ProgramDatabase& staged) {
        auto result = staged.Insert(program, options);
        if (!result.ok()) return result.status();

        accepted = *result;
        return absl::OkStatus();
      },
      &accepted);
  if (!status.ok()) return status;

  return accepted;
}

double ProgramDatabase::Fitness(const Program& program) const {
  return GetFitnessScore(program.metrics, feature_dimensions_);
}

// Total order used for cells, archive and eviction. A program with no metrics
// ranks below one with any, and two unscored programs fall back to recency.
bool ProgramDatabase::Better(const Program& left, const Program& right) const {
  if (left.metrics.empty() && right.metrics.empty()) return left.timestamp > right.timestamp;
  if (left.metrics.empty() != right.metrics.empty()) return !left.metrics.empty();

  return Fitness(left) > Fitness(right);
}

bool ProgramDatabase::Owned(const std::string& id) const {
  for (const auto& island : population_->islands)
    if (island.count(id)) return true;

  return false;
}

bool ProgramDatabase::OwnsCell(const std::string& id) const {
  for (const auto& grid : population_->feature_maps)
    for (const auto& [cell, owner] : grid)
      if (owner == id) return true;

  return false;
}

// Erases a program and every reference to it. Removing from the middle of the
// vector shifts later entries, so the index has to be rebuilt from the hole
// onward; cells, islands, archive and the best pointers are cleared too,
// because a stale ID there would later be dereferenced through index_.
void ProgramDatabase::Remove(const std::string& id) {
  const auto found = index_.find(id);
  if (found == index_.end()) return;

  const auto offset = found->second;
  index_.erase(found);
  programs_.erase(programs_.begin() + offset);
  for (std::size_t i = offset; i < programs_.size(); ++i) index_.at(programs_[i].id) = i;

  auto& state = *population_;
  for (auto& island : state.islands) island.erase(id);
  for (auto& grid : state.feature_maps) {
    for (auto cell = grid.begin(); cell != grid.end();) {
      if (cell->second == id) {
        cell = grid.erase(cell);
      } else {
        ++cell;
      }
    }
  }

  state.archive.erase(id);
  if (state.best == id) state.best.reset();
  for (auto& best : state.island_best)
    if (best == id) best.reset();
}

void ProgramDatabase::RefreshBest() {
  auto& state = *population_;
  for (const auto& program : programs_) {
    if (!state.best || Better(program, programs_[index_.at(*state.best)])) state.best = program.id;
  }

  for (std::size_t i = 0; i < state.islands.size(); ++i) {
    auto& best = state.island_best[i];
    if (best && !state.islands[i].count(*best)) best.reset();

    for (const auto& program : programs_) {
      if (state.islands[i].count(program.id) && (!best || Better(program, programs_[index_.at(*best)]))) {
        best = program.id;
      }
    }
  }
}

// One MAP-Elites admission. The island is chosen explicitly, else inherited
// from the parent, else the current island. A candidate that loses its cell is
// still added to the island and can survive until capacity eviction; only
// eviction actually removes programs. Returns false when a strategy rejected
// the candidate, which is not an error, except for the very first seed.
absl::StatusOr<bool> ProgramDatabase::Insert(const Program& input, const AddOptions& options) {
  auto status = input.Validate();
  if (!status.ok()) return status;

  if (!std::isfinite(Fitness(input))) return absl::InvalidArgumentError("Program fitness must be finite");
  if (index_.count(input.id)) return absl::AlreadyExistsError("Program ID already exists");
  if (options.iteration && *options.iteration < 0) return absl::InvalidArgumentError("Iteration must be nonnegative");

  auto& state = *population_;
  int island = state.current_island;
  if (options.island) {
    island = *options.island;
  } else if (input.parent_id && index_.count(*input.parent_id)) {
    const auto& parent = programs_[index_.at(*input.parent_id)];
    island = parent.metadata.at("island").get<int>();
  }
  status = CheckIsland(island);
  if (!status.ok()) return status;

  Program candidate = input;
  candidate.metadata["island"] = island;
  if (options.iteration) candidate.iteration_found = *options.iteration;

  if (strategy_->admit) {
    const auto snapshot = MakeSnapshot();
    auto admitted = strategy_->admit(snapshot, candidate, island);
    if (!admitted.ok()) return admitted.status();
    if (!*admitted) {
      if (programs_.empty()) return absl::FailedPreconditionError("The initial seed cannot be rejected");

      return false;
    }
  }

  status = Store(candidate);
  if (!status.ok()) return status;

  auto coordinates = state.mapper.Coordinates(candidate, programs_);
  if (!coordinates.ok()) return coordinates.status();

  auto& grid = state.feature_maps[island];
  const auto cell = grid.find(*coordinates);
  if (cell == grid.end()) {
    grid.emplace(*coordinates, candidate.id);
  } else {
    const Program incumbent = programs_[index_.at(cell->second)];
    bool replace = Better(candidate, incumbent);
    if (strategy_->replace_cell) {
      const auto snapshot = MakeSnapshot();
      auto decision = strategy_->replace_cell(snapshot, candidate, incumbent, island);
      if (!decision.ok()) return decision.status();
      replace = *decision;
    }

    if (replace) {
      cell->second = candidate.id;
      state.islands[island].erase(incumbent.id);
      if (!strategy_->archive && state.archive.erase(incumbent.id)) state.archive.insert(candidate.id);
    }
  }

  state.islands[island].insert(candidate.id);
  status = UpdateArchive(candidate);
  if (!status.ok()) return status;
  RefreshBest();

  // A custom archive may release an owner displaced in an earlier operation.
  // Recheck all orphans, not just this insertion's displaced or previous best.
  std::vector<std::string> released;
  for (const auto& program : programs_)
    if (program.id != state.best && !Owned(program.id) && !(strategy_->archive && state.archive.count(program.id))) {
      released.push_back(program.id);
    }
  for (const auto& id : released) Remove(id);

  status = EnforceCapacity(candidate.id);
  if (!status.ok()) return status;

  RefreshBest();
  state.last_iteration = std::max(state.last_iteration, candidate.iteration_found);

  return true;
}

// Maintains the elite archive, which is shared across islands. A custom
// strategy must name the victim itself and its decision is validated against
// capacity; the default path evicts the weakest member only when the candidate
// beats it, so a full archive never shrinks.
absl::Status ProgramDatabase::UpdateArchive(const Program& candidate) {
  auto& state = *population_;
  if (strategy_->archive) {
    const auto snapshot = MakeSnapshot();
    auto decision = strategy_->archive(snapshot, candidate);
    if (!decision.ok()) return decision.status();

    if ((!decision->add && decision->evict_id) || (state.archive.count(candidate.id) && decision->evict_id) ||
        (decision->evict_id && !state.archive.count(*decision->evict_id))) {
      return absl::InvalidArgumentError("Invalid archive eviction decision");
    }
    if (!decision->add || state.archive.count(candidate.id)) return absl::OkStatus();
    if (state.config.archive_size == 0 ||
        (state.archive.size() >= static_cast<std::size_t>(state.config.archive_size) && !decision->evict_id)) {
      return absl::InvalidArgumentError("Archive admission exceeds its capacity");
    }

    if (decision->evict_id) state.archive.erase(*decision->evict_id);
    state.archive.insert(candidate.id);
    return absl::OkStatus();
  }

  if (state.config.archive_size == 0 || state.archive.count(candidate.id)) return absl::OkStatus();
  if (state.archive.size() < static_cast<std::size_t>(state.config.archive_size)) {
    state.archive.insert(candidate.id);
    return absl::OkStatus();
  }

  const Program* worst = nullptr;
  for (const auto& program : programs_)
    if (state.archive.count(program.id) && (!worst || Better(*worst, program))) worst = &program;
  if (worst && Better(candidate, *worst)) {
    state.archive.erase(worst->id);
    state.archive.insert(candidate.id);
  }

  return absl::OkStatus();
}

// Trims the population back to its limit. The new candidate and the global
// best are protected, so the limit can be exceeded by one while a historical
// best survives outside every island; that program is excluded from the count
// rather than allowed to displace a live one. The default order sheds
// non-cell-owners first, then the weakest by fitness.
absl::Status ProgramDatabase::EnforceCapacity(const std::string& candidate) {
  auto& state = *population_;
  const std::size_t orphan_best = state.best && !Owned(*state.best) ? 1 : 0;
  const auto count = programs_.size() - orphan_best;
  const auto limit = static_cast<std::size_t>(state.config.population_size);
  if (count <= limit) return absl::OkStatus();

  std::set<std::string> protected_ids{candidate};
  if (state.best) protected_ids.insert(*state.best);

  std::vector<std::string> eligible;
  for (const auto& program : programs_)
    if (!protected_ids.count(program.id)) eligible.push_back(program.id);
  const auto required = std::min(count - limit, eligible.size());
  if (required == 0) return absl::OkStatus();

  std::vector<std::string> removed;
  if (strategy_->evict) {
    const auto snapshot = MakeSnapshot();
    auto decision = strategy_->evict(snapshot, required, protected_ids);
    if (!decision.ok()) return decision.status();

    std::set<std::string> unique(decision->begin(), decision->end());
    if (decision->size() != required || unique.size() != required) {
      return absl::InvalidArgumentError("Eviction must return exactly the required unique IDs");
    }
    for (const auto& id : *decision)
      if (!index_.count(id) || protected_ids.count(id)) {
        return absl::InvalidArgumentError("Eviction selected an ineligible program");
      }

    removed = std::move(*decision);
  } else {
    std::stable_sort(eligible.begin(), eligible.end(), [&](const auto& a, const auto& b) {
      const bool a_cell = OwnsCell(a), b_cell = OwnsCell(b);
      if (a_cell != b_cell) return !a_cell;

      return Fitness(programs_[index_.at(a)]) < Fitness(programs_[index_.at(b)]);
    });
    removed.assign(eligible.begin(), eligible.begin() + required);
  }

  for (const auto& id : removed) Remove(id);

  return absl::OkStatus();
}

PopulationSnapshot ProgramDatabase::MakeSnapshot() const {
  const auto& state = *population_;
  PopulationSnapshot snapshot;
  for (const auto& program : programs_) snapshot.programs.emplace(program.id, program);

  snapshot.islands = state.islands;
  snapshot.feature_maps = state.feature_maps;
  snapshot.archive = state.archive;
  snapshot.best_program_id = state.best;
  snapshot.island_best_programs = state.island_best;

  snapshot.population_limit = state.config.population_size;
  snapshot.archive_limit = state.config.archive_size;
  snapshot.feature_dimensions = feature_dimensions_;

  snapshot.generations = state.generations;
  snapshot.last_migration_generation = state.last_migration;
  snapshot.migration_interval = state.config.migration_interval;
  snapshot.migration_rate = state.config.migration_rate;
  snapshot.last_iteration = state.last_iteration;
  snapshot.current_island = state.current_island;

  return snapshot;
}

absl::StatusOr<PopulationSnapshot> ProgramDatabase::Snapshot() const {
  if (!population_) return absl::FailedPreconditionError("Population mode is not enabled");

  return MakeSnapshot();
}

absl::StatusOr<std::map<std::string, FeatureStats>> ProgramDatabase::FeatureStatistics() const {
  if (!population_) return absl::FailedPreconditionError("Population mode is not enabled");

  return population_->mapper.stats();
}

absl::Status ProgramDatabase::SetCurrentIsland(int island) {
  auto status = CheckIsland(island);
  if (!status.ok()) return status;

  return Mutate([&](ProgramDatabase& staged) {
    staged.population_->current_island = island;
    return absl::OkStatus();
  });
}

absl::Status ProgramDatabase::IncrementGeneration(int island) {
  auto status = CheckIsland(island);
  if (!status.ok()) return status;

  return Mutate([&](ProgramDatabase& staged) {
    auto& generation = staged.population_->generations[island];
    if (generation == std::numeric_limits<std::int64_t>::max()) {
      return absl::OutOfRangeError("Island generation overflow");
    }

    ++generation;
    return absl::OkStatus();
  });
}

absl::StatusOr<bool> ProgramDatabase::ShouldMigrate() {
  if (!population_) return absl::FailedPreconditionError("Population mode is not enabled");
  if (mutation_active_) return absl::FailedPreconditionError("Nested population mutation is forbidden");

  mutation_active_ = true;
  struct Reset {
    bool& flag;
    ~Reset() { flag = false; }
  } reset{mutation_active_};

  try {
    const auto& state = *population_;
    if (state.config.num_islands < 2) return false;
    if (strategy_->migration_due) return strategy_->migration_due(MakeSnapshot());

    return *std::max_element(state.generations.begin(), state.generations.end()) - state.last_migration >=
           state.config.migration_interval;
  } catch (...) {
    return absl::InternalError("Migration query threw an exception");
  }
}

absl::Status ProgramDatabase::Migrate() {
  return Mutate([](ProgramDatabase& staged) { return staged.MigratePopulation(); });
}

// Copies elites to neighbouring islands. Migration never moves a program: the
// source stays put and a renamed copy is admitted, so an island losing a
// migration still keeps its own elite. Already-migrated programs and code that
// the target island already holds are skipped, which is what stops copies from
// compounding around the ring on every interval.
absl::Status ProgramDatabase::MigratePopulation() {
  auto& state = *population_;
  if (state.config.num_islands < 2) return absl::OkStatus();

  const auto snapshot = MakeSnapshot();
  std::vector<MigrationMove> moves;
  if (strategy_->migrate) {
    auto decision = strategy_->migrate(snapshot);
    if (!decision.ok()) return decision.status();
    moves = std::move(*decision);
  } else if (state.config.migration_rate > 0) {
    for (int island = 0; island < state.config.num_islands; ++island) {
      std::vector<const Program*> members;
      for (const auto& program : programs_)
        if (state.islands[island].count(program.id)) members.push_back(&program);

      std::stable_sort(members.begin(), members.end(), [&](const auto* a, const auto* b) { return Better(*a, *b); });
      const auto count =
          std::min(members.size(),
                   std::max<std::size_t>(1, static_cast<std::size_t>(members.size() * state.config.migration_rate)));

      // Ring topology. With exactly two islands both neighbours are the same
      // one, so the guard below avoids planning the identical move twice.
      const int next = (island + 1) % state.config.num_islands;
      const int previous = (island + state.config.num_islands - 1) % state.config.num_islands;
      for (std::size_t i = 0; i < count; ++i) {
        moves.push_back({members[i]->id, next});
        if (previous != next) moves.push_back({members[i]->id, previous});
      }
    }
  }

  // Validate the whole plan against its original snapshot, before admission.
  for (const auto& move : moves) {
    auto status = CheckIsland(move.target_island);
    if (!status.ok()) return status;

    const auto source = snapshot.programs.find(move.program_id);
    if (source == snapshot.programs.end() || !Owned(move.program_id) ||
        source->second.metadata.at("island") == move.target_island) {
      return absl::InvalidArgumentError("Invalid migration source or destination");
    }
  }

  for (const auto& move : moves) {
    const auto& source = snapshot.programs.at(move.program_id);
    if (source.metadata.contains("migrant") && source.metadata["migrant"] == true) continue;

    bool duplicate = false;
    for (const auto& id : state.islands[move.target_island])
      if (programs_[index_.at(id)].code == source.code) {
        duplicate = true;
        break;
      }
    if (duplicate) continue;

    Program copy = source;
    do {
      if (state.next_migrant == std::numeric_limits<std::uint64_t>::max()) {
        return absl::OutOfRangeError("Migration ID sequence exhausted");
      }
      copy.id = source.id + ".migrant." + std::to_string(state.next_migrant++);
    } while (index_.count(copy.id));

    copy.parent_id = source.id;
    copy.timestamp = Program().timestamp;
    copy.metadata["migrant"] = true;

    auto inserted = Insert(copy, AddOptions{move.target_island, std::nullopt});
    if (!inserted.ok()) return inserted.status();
  }

  state.last_migration = *std::max_element(state.generations.begin(), state.generations.end());

  return absl::OkStatus();
}
}  // namespace ievolve
