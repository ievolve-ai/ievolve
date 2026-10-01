#include "ievolve/database/population.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ievolve {
namespace {
Population::State InitialState(const PopulationConfig& config) {
  Population::State state;
  state.islands.resize(config.num_islands);
  state.feature_maps.resize(config.num_islands);
  state.island_best.resize(config.num_islands);
  state.generations.assign(config.num_islands, 0);
  state.random.seed(config.random_seed ? static_cast<std::uint64_t>(*config.random_seed) : std::random_device{}());

  return state;
}
}  // namespace

Population::Population(PopulationConfig config, FeatureMapper mapper, PopulationStrategy strategy)
    : config_(std::move(config)),
      mapper_(std::move(mapper)),
      strategy_(std::make_shared<const PopulationStrategy>(std::move(strategy))),
      state_(InitialState(config_)) {}

void Population::Reset() {
  mapper_.ClearStatistics();
  state_ = InitialState(config_);
}

absl::Status Population::CheckIsland(int island) const {
  if (island < 0 || island >= config_.num_islands) return absl::InvalidArgumentError("Island index is out of range");

  return absl::OkStatus();
}

// Total order used for cells, archive and eviction. A program with no metrics
// ranks below one with any, and two unscored programs fall back to recency.
bool Population::Better(const ProgramStore& store, const Program& left, const Program& right) const {
  if (left.metrics.empty() && right.metrics.empty()) return left.timestamp > right.timestamp;
  if (left.metrics.empty() != right.metrics.empty()) return !left.metrics.empty();

  return store.Fitness(left) > store.Fitness(right);
}

bool Population::Owned(const std::string& id) const {
  for (const auto& island : state_.islands)
    if (island.count(id)) return true;

  return false;
}

bool Population::OwnsCell(const std::string& id) const {
  for (const auto& grid : state_.feature_maps)
    for (const auto& [cell, owner] : grid)
      if (owner == id) return true;

  return false;
}

// Erases a program and every reference to it. Cells, islands, archive and the
// best pointers are cleared too, because a stale ID there would later be looked
// up in the store.
void Population::Remove(ProgramStore& store, const std::string& id) {
  if (!store.Contains(id)) return;

  store.Erase(id);

  auto& state = state_;
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

void Population::RefreshBest(const ProgramStore& store) {
  auto& state = state_;
  for (const auto& program : store.programs()) {
    if (!state.best || Better(store, program, store.at(*state.best))) state.best = program.id;
  }

  for (std::size_t i = 0; i < state.islands.size(); ++i) {
    auto& best = state.island_best[i];
    if (best && !state.islands[i].count(*best)) best.reset();

    for (const auto& program : store.programs()) {
      if (state.islands[i].count(program.id) && (!best || Better(store, program, store.at(*best)))) {
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
absl::StatusOr<bool> Population::Insert(ProgramStore& store, const Program& input, const AddOptions& options) {
  auto status = input.Validate();
  if (!status.ok()) return status;

  if (!std::isfinite(store.Fitness(input))) return absl::InvalidArgumentError("Program fitness must be finite");
  if (store.Contains(input.id)) return absl::AlreadyExistsError("Program ID already exists");
  if (options.iteration && *options.iteration < 0) return absl::InvalidArgumentError("Iteration must be nonnegative");

  auto& state = state_;
  int island = state.current_island;
  if (options.island) {
    island = *options.island;
  } else if (input.parent_id && store.Contains(*input.parent_id)) {
    const auto& parent = store.at(*input.parent_id);
    island = parent.metadata.at("island").get<int>();
  }
  status = CheckIsland(island);
  if (!status.ok()) return status;

  Program candidate = input;
  candidate.metadata["island"] = island;
  if (options.iteration) candidate.iteration_found = *options.iteration;

  if (strategy_->admit) {
    const auto snapshot = Snapshot(store);
    auto admitted = strategy_->admit(snapshot, candidate, island);
    if (!admitted.ok()) return admitted.status();
    if (!*admitted) {
      if (store.empty()) return absl::FailedPreconditionError("The initial seed cannot be rejected");

      return false;
    }
  }

  status = store.Add(candidate);
  if (!status.ok()) return status;

  auto coordinates = mapper_.Coordinates(candidate, store.programs());
  if (!coordinates.ok()) return coordinates.status();

  auto& grid = state.feature_maps[island];
  const auto cell = grid.find(*coordinates);
  if (cell == grid.end()) {
    grid.emplace(*coordinates, candidate.id);
  } else {
    const Program incumbent = store.at(cell->second);
    bool replace = Better(store, candidate, incumbent);
    if (strategy_->replace_cell) {
      const auto snapshot = Snapshot(store);
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
  status = UpdateArchive(store, candidate);
  if (!status.ok()) return status;
  RefreshBest(store);

  // A custom archive may release an owner displaced in an earlier operation.
  // Recheck all orphans, not just this insertion's displaced or previous best.
  std::vector<std::string> released;
  for (const auto& program : store.programs())
    if (program.id != state.best && !Owned(program.id) && !(strategy_->archive && state.archive.count(program.id))) {
      released.push_back(program.id);
    }
  for (const auto& id : released) Remove(store, id);

  status = EnforceCapacity(store, candidate.id);
  if (!status.ok()) return status;

  RefreshBest(store);
  state.last_iteration = std::max(state.last_iteration, candidate.iteration_found);

  return true;
}

// Maintains the elite archive, which is shared across islands. A custom
// strategy must name the victim itself and its decision is validated against
// capacity; the default path evicts the weakest member only when the candidate
// beats it, so a full archive never shrinks.
absl::Status Population::UpdateArchive(const ProgramStore& store, const Program& candidate) {
  auto& state = state_;
  if (strategy_->archive) {
    const auto snapshot = Snapshot(store);
    auto decision = strategy_->archive(snapshot, candidate);
    if (!decision.ok()) return decision.status();

    if ((!decision->add && decision->evict_id) || (state.archive.count(candidate.id) && decision->evict_id) ||
        (decision->evict_id && !state.archive.count(*decision->evict_id))) {
      return absl::InvalidArgumentError("Invalid archive eviction decision");
    }
    if (!decision->add || state.archive.count(candidate.id)) return absl::OkStatus();
    if (config_.archive_size == 0 ||
        (state.archive.size() >= static_cast<std::size_t>(config_.archive_size) && !decision->evict_id)) {
      return absl::InvalidArgumentError("Archive admission exceeds its capacity");
    }

    if (decision->evict_id) state.archive.erase(*decision->evict_id);
    state.archive.insert(candidate.id);
    return absl::OkStatus();
  }

  if (config_.archive_size == 0 || state.archive.count(candidate.id)) return absl::OkStatus();
  if (state.archive.size() < static_cast<std::size_t>(config_.archive_size)) {
    state.archive.insert(candidate.id);
    return absl::OkStatus();
  }

  const Program* worst = nullptr;
  for (const auto& program : store.programs())
    if (state.archive.count(program.id) && (!worst || Better(store, *worst, program))) worst = &program;
  if (worst && Better(store, candidate, *worst)) {
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
absl::Status Population::EnforceCapacity(ProgramStore& store, const std::string& candidate) {
  auto& state = state_;
  const std::size_t orphan_best = state.best && !Owned(*state.best) ? 1 : 0;
  const auto count = store.size() - orphan_best;
  const auto limit = static_cast<std::size_t>(config_.population_size);
  if (count <= limit) return absl::OkStatus();

  std::set<std::string> protected_ids{candidate};
  if (state.best) protected_ids.insert(*state.best);

  std::vector<std::string> eligible;
  for (const auto& program : store.programs())
    if (!protected_ids.count(program.id)) eligible.push_back(program.id);
  const auto required = std::min(count - limit, eligible.size());
  if (required == 0) return absl::OkStatus();

  std::vector<std::string> removed;
  if (strategy_->evict) {
    const auto snapshot = Snapshot(store);
    auto decision = strategy_->evict(snapshot, required, protected_ids);
    if (!decision.ok()) return decision.status();

    std::set<std::string> unique(decision->begin(), decision->end());
    if (decision->size() != required || unique.size() != required) {
      return absl::InvalidArgumentError("Eviction must return exactly the required unique IDs");
    }
    for (const auto& id : *decision)
      if (!store.Contains(id) || protected_ids.count(id)) {
        return absl::InvalidArgumentError("Eviction selected an ineligible program");
      }

    removed = std::move(*decision);
  } else {
    std::stable_sort(eligible.begin(), eligible.end(), [&](const auto& a, const auto& b) {
      const bool a_cell = OwnsCell(a), b_cell = OwnsCell(b);
      if (a_cell != b_cell) return !a_cell;

      return store.Fitness(store.at(a)) < store.Fitness(store.at(b));
    });
    removed.assign(eligible.begin(), eligible.begin() + required);
  }

  for (const auto& id : removed) Remove(store, id);

  return absl::OkStatus();
}

PopulationSnapshot Population::Snapshot(const ProgramStore& store) const {
  const auto& state = state_;
  PopulationSnapshot snapshot;
  for (const auto& program : store.programs()) snapshot.programs.emplace(program.id, program);

  snapshot.islands = state.islands;
  snapshot.feature_maps = state.feature_maps;
  snapshot.archive = state.archive;
  snapshot.best_program_id = state.best;
  snapshot.island_best_programs = state.island_best;

  snapshot.population_limit = config_.population_size;
  snapshot.archive_limit = config_.archive_size;
  snapshot.feature_dimensions = store.feature_dimensions();

  snapshot.generations = state.generations;
  snapshot.last_migration_generation = state.last_migration;
  snapshot.migration_interval = config_.migration_interval;
  snapshot.migration_rate = config_.migration_rate;
  snapshot.last_iteration = state.last_iteration;
  snapshot.current_island = state.current_island;

  return snapshot;
}

// Picks a parent and its inspirations. Sampling advances the RNG, which is
// persisted state, so callers run it transactionally: a resumed run must
// continue the same stream rather than replay it. Without an explicit island it
// samples from the current island and lets inspirations follow the parent.
//
// The parent comes from one of three branches, chosen by exploration_ratio and
// exploitation_ratio: uniformly within the island, from the elite archive, or
// from the wider population. The archive branch may fall back across islands,
// and an empty island falls back to the whole population without creating any
// copies. Inspirations are then filled in priority order, each stage skipping
// what earlier stages already took: top elites first, then randomly ordered
// cell owners tagged "diverse", then anything left tagged "random".
absl::StatusOr<PopulationSample> Population::Sample(const ProgramStore& store, std::optional<int> requested,
                                                    int count) {
  const bool global_random = !requested;
  int island = requested.value_or(state_.current_island);
  auto status = CheckIsland(island);
  if (!status.ok()) return status;

  if (count < 0) return absl::InvalidArgumentError("Inspiration count must be nonnegative");
  if (store.empty()) return absl::NotFoundError("Cannot sample an empty population");

  auto& state = state_;
  std::vector<const Program*> local, all, archive, local_archive;
  for (const auto& program : store.programs()) {
    all.push_back(&program);
    const bool member = state.islands[island].count(program.id);
    if (member) local.push_back(&program);
    if (state.archive.count(program.id)) {
      archive.push_back(&program);
      if (member) local_archive.push_back(&program);
    }
  }

  const auto uniform = [&](const std::vector<const Program*>& choices) {
    return choices[std::uniform_int_distribution<std::size_t>(0, choices.size() - 1)(state.random)];
  };

  // Fitness-proportional choice. The 0.001 floor keeps zero- and
  // negative-scoring programs reachable, and normalizing by the maximum keeps
  // the weights in a stable range regardless of the metric's scale.
  const auto weighted = [&](const std::vector<const Program*>& choices) {
    std::vector<double> weights;
    double maximum = 0.001;
    for (const auto* program : choices) {
      weights.push_back(std::max(0.001, store.Fitness(*program)));
      maximum = std::max(maximum, weights.back());
    }

    for (auto& weight : weights) weight /= maximum;

    return choices[std::discrete_distribution<std::size_t>(weights.begin(), weights.end())(state.random)];
  };

  const Program* parent = nullptr;
  if (local.empty()) {
    parent = uniform(all);
  } else {
    const double branch = std::uniform_real_distribution<double>(0, 1)(state.random);
    if (branch < config_.exploration_ratio) {
      parent = uniform(local);
    } else if (branch < config_.exploration_ratio + config_.exploitation_ratio) {
      if (!local_archive.empty()) {
        parent = uniform(local_archive);
      } else if (!archive.empty()) {
        parent = uniform(archive);
      } else {
        parent = global_random ? uniform(local) : weighted(local);
      }
    } else {
      parent = global_random ? uniform(all) : weighted(local);
    }
  }

  PopulationSample result{*parent, {}};
  // Generic Sample follows its chosen parent's island; explicit island sampling
  // always keeps inspirations in the caller's requested island.
  if (global_random) island = parent->metadata.at("island").get<int>();

  std::vector<const Program*> candidates;
  for (const auto& program : store.programs())
    if (program.id != parent->id && state.islands[island].count(program.id)) candidates.push_back(&program);
  const auto wanted = std::min(static_cast<std::size_t>(count), candidates.size());
  if (wanted == 0) return result;

  std::stable_sort(candidates.begin(), candidates.end(),
                   [&](const auto* a, const auto* b) { return Better(store, *a, *b); });

  std::set<std::string> selected;
  const auto append = [&](const Program& program, const char* flag) {
    if (result.inspirations.size() >= wanted || program.id == parent->id || !selected.insert(program.id).second) return;

    result.inspirations.push_back(program);
    if (flag) result.inspirations.back().metadata[flag] = true;
  };

  const auto elites =
      std::min(wanted, std::max<std::size_t>(1, static_cast<std::size_t>(count * config_.elite_selection_ratio)));
  for (std::size_t i = 0; i < elites; ++i) append(*candidates[i], nullptr);

  std::vector<std::string> cell_owners;
  for (const auto& [coordinates, id] : state.feature_maps[island]) cell_owners.push_back(id);
  std::shuffle(cell_owners.begin(), cell_owners.end(), state.random);
  for (const auto& id : cell_owners) append(store.at(id), "diverse");

  std::shuffle(candidates.begin(), candidates.end(), state.random);
  for (const auto* program : candidates) append(*program, "random");

  return result;
}

absl::StatusOr<IslandSelectionContext> Population::SelectionContext(const ProgramStore& store, std::int64_t iteration,
                                                                    const std::vector<int>& pending_counts) const {
  const auto& state = state_;
  if (iteration < 0 || pending_counts.size() != state.islands.size() ||
      std::any_of(pending_counts.begin(), pending_counts.end(), [](int n) { return n < 0; })) {
    return absl::InvalidArgumentError("Invalid iteration or pending counts");
  }

  IslandSelectionContext context{iteration, pending_counts, {}};
  for (std::size_t i = 0; i < state.islands.size(); ++i) {
    IslandState island;
    island.population_size = state.islands[i].size();
    island.generation = state.generations[i];
    if (island.population_size != 0) {
      island.best_score = -std::numeric_limits<double>::infinity();
      long double mean = 0;
      std::set<std::string> codes;
      for (const auto& id : state.islands[i]) {
        const auto& program = store.at(id);
        const double score = store.Fitness(program);
        island.best_score = std::max(island.best_score, score);
        mean += static_cast<long double>(score) / island.population_size;
        codes.insert(program.code);
      }

      const long double limit = std::numeric_limits<double>::max();
      island.average_score = static_cast<double>(std::clamp(mean, -limit, limit));
      island.diversity = static_cast<double>(codes.size()) / island.population_size;
    }
    context.islands.push_back(island);
  }

  return context;
}

absl::Status Population::SetCurrentIsland(int island) {
  auto status = CheckIsland(island);
  if (!status.ok()) return status;

  state_.current_island = island;
  return absl::OkStatus();
}

absl::Status Population::IncrementGeneration(int island) {
  auto status = CheckIsland(island);
  if (!status.ok()) return status;

  auto& generation = state_.generations[island];
  if (generation == std::numeric_limits<std::int64_t>::max())
    return absl::OutOfRangeError("Island generation overflow");

  ++generation;
  return absl::OkStatus();
}

absl::StatusOr<bool> Population::MigrationDue(const ProgramStore& store) const {
  if (config_.num_islands < 2) return false;
  if (strategy_->migration_due) return strategy_->migration_due(Snapshot(store));

  return *std::max_element(state_.generations.begin(), state_.generations.end()) - state_.last_migration >=
         config_.migration_interval;
}

// Copies elites to neighbouring islands. Migration never moves a program: the
// source stays put and a renamed copy is admitted, so an island losing a
// migration still keeps its own elite. Already-migrated programs and code that
// the target island already holds are skipped, which is what stops copies from
// compounding around the ring on every interval.
absl::Status Population::Migrate(ProgramStore& store) {
  auto& state = state_;
  if (config_.num_islands < 2) return absl::OkStatus();

  const auto snapshot = Snapshot(store);
  std::vector<MigrationMove> moves;
  if (strategy_->migrate) {
    auto decision = strategy_->migrate(snapshot);
    if (!decision.ok()) return decision.status();
    moves = std::move(*decision);
  } else if (config_.migration_rate > 0) {
    for (int island = 0; island < config_.num_islands; ++island) {
      std::vector<const Program*> members;
      for (const auto& program : store.programs())
        if (state.islands[island].count(program.id)) members.push_back(&program);

      std::stable_sort(members.begin(), members.end(),
                       [&](const auto* a, const auto* b) { return Better(store, *a, *b); });
      const auto count = std::min(
          members.size(), std::max<std::size_t>(1, static_cast<std::size_t>(members.size() * config_.migration_rate)));

      // Ring topology. With exactly two islands both neighbours are the same
      // one, so the guard below avoids planning the identical move twice.
      const int next = (island + 1) % config_.num_islands;
      const int previous = (island + config_.num_islands - 1) % config_.num_islands;
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
      if (store.at(id).code == source.code) {
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
    } while (store.Contains(copy.id));

    copy.parent_id = source.id;
    copy.timestamp = Program().timestamp;
    copy.metadata["migrant"] = true;

    auto inserted = Insert(store, copy, AddOptions{move.target_island, std::nullopt});
    if (!inserted.ok()) return inserted.status();
  }

  state.last_migration = *std::max_element(state.generations.begin(), state.generations.end());

  return absl::OkStatus();
}
}  // namespace ievolve
