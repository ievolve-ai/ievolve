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

ProgramDatabase::ProgramDatabase(std::vector<std::string> dimensions) : feature_dimensions_(std::move(dimensions)) {}

absl::Status ProgramDatabase::Add(const Program& program) {
  if (population_) {
    auto added = Add(program, AddOptions{});
    if (!added.ok()) return added.status();

    return *added ? absl::OkStatus() : absl::FailedPreconditionError("Candidate admission declined");
  }

  return Store(program);
}

absl::Status ProgramDatabase::Store(const Program& program) {
  auto status = program.Validate();
  if (!status.ok()) return status;

  if (!std::isfinite(GetFitnessScore(program.metrics, feature_dimensions_))) {
    return absl::InvalidArgumentError("Program fitness must be finite");
  }
  if (index_.count(program.id) != 0) return absl::AlreadyExistsError("Program ID already exists");

  // The vector and the index must agree. If building the index entry throws,
  // undo the push so the two never drift apart.
  programs_.push_back(program);
  try {
    index_.emplace(program.id, programs_.size() - 1);
  } catch (...) {
    programs_.pop_back();
    throw;
  }

  return absl::OkStatus();
}

absl::StatusOr<Program> ProgramDatabase::Get(std::string_view id) const {
  const auto found = index_.find(std::string(id));
  if (found == index_.end()) return absl::NotFoundError("Program ID not found");

  return programs_[found->second];
}

absl::StatusOr<Program> ProgramDatabase::GetBestProgram(const std::optional<std::string>& metric) const {
  if (population_ && (!metric || metric->empty()) && population_->best) return Get(*population_->best);

  auto top = GetTopPrograms(1, metric);
  if (!top.ok()) return top.status();
  if (top->empty()) return absl::NotFoundError("No program matches the requested metric");

  return std::move(top->front());
}

// Ranks by a named metric, or by the shared fitness when none is given. A
// named metric that a program lacks, or holds as a non-number, drops that
// program from the ranking instead of scoring it zero. Comparison goes through
// CompareMetricNumbers so large integers keep full precision, and the sort is
// stable so equal scores preserve insertion order.
absl::StatusOr<std::vector<Program>> ProgramDatabase::GetTopPrograms(int n,
                                                                     const std::optional<std::string>& metric) const {
  if (n < 0) return absl::InvalidArgumentError("Top-N count must be nonnegative");
  if (n == 0) return std::vector<Program>{};

  struct Ranked {
    const Program* program;
    Metrics score;
  };
  std::vector<Ranked> ranked;
  ranked.reserve(programs_.size());
  for (const auto& program : programs_) {
    if (metric && !metric->empty()) {
      const auto score = program.metrics.find(*metric);
      if (score == program.metrics.end() || !(score->is_number() || score->is_boolean())) continue;
      ranked.push_back({&program, *score});
    } else {
      ranked.push_back({&program, GetFitnessScore(program.metrics, feature_dimensions_)});
    }
  }

  std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& left, const Ranked& right) {
    return CompareMetricNumbers(left.score, right.score) > 0;
  });
  const auto count = std::min(ranked.size(), static_cast<std::size_t>(n));

  std::vector<Program> result;
  result.reserve(count);
  for (std::size_t i = 0; i < count; ++i) result.push_back(*ranked[i].program);

  return result;
}

absl::StatusOr<PopulationSample> ProgramDatabase::Sample(int num_inspirations) {
  std::optional<PopulationSample> result;
  auto status = Mutate([&](ProgramDatabase& staged) {
    auto sample = staged.DrawSample(staged.population_->current_island, num_inspirations, true);
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
  status = Mutate([&](ProgramDatabase& staged) {
    auto sample = staged.DrawSample(island, num_inspirations, false);
    if (!sample.ok()) return sample.status();

    result = std::move(*sample);
    return absl::OkStatus();
  });
  if (!status.ok()) return status;

  return std::move(*result);
}

// Picks a parent and its inspirations. Sampling runs through Mutate because it
// advances the RNG, which is persisted state: a resumed run must continue the
// same stream rather than replay it.
//
// The parent comes from one of three branches, chosen by exploration_ratio and
// exploitation_ratio: uniformly within the island, from the elite archive, or
// from the wider population. The archive branch may fall back across islands,
// and an empty island falls back to the whole population without creating any
// copies. Inspirations are then filled in priority order, each stage skipping
// what earlier stages already took: top elites first, then randomly ordered
// cell owners tagged "diverse", then anything left tagged "random".
absl::StatusOr<PopulationSample> ProgramDatabase::DrawSample(int island, int count, bool global_random) {
  if (count < 0) return absl::InvalidArgumentError("Inspiration count must be nonnegative");
  if (programs_.empty()) return absl::NotFoundError("Cannot sample an empty population");

  auto& state = *population_;
  std::vector<const Program*> local, all, archive, local_archive;
  for (const auto& program : programs_) {
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
      weights.push_back(std::max(0.001, Fitness(*program)));
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
    if (branch < state.config.exploration_ratio) {
      parent = uniform(local);
    } else if (branch < state.config.exploration_ratio + state.config.exploitation_ratio) {
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
  for (const auto& program : programs_)
    if (program.id != parent->id && state.islands[island].count(program.id)) candidates.push_back(&program);
  const auto wanted = std::min(static_cast<std::size_t>(count), candidates.size());
  if (wanted == 0) return result;

  std::stable_sort(candidates.begin(), candidates.end(), [&](const auto* a, const auto* b) { return Better(*a, *b); });

  std::set<std::string> selected;
  const auto append = [&](const Program& program, const char* flag) {
    if (result.inspirations.size() >= wanted || program.id == parent->id || !selected.insert(program.id).second) return;

    result.inspirations.push_back(program);
    if (flag) result.inspirations.back().metadata[flag] = true;
  };

  const auto elites =
      std::min(wanted, std::max<std::size_t>(1, static_cast<std::size_t>(count * state.config.elite_selection_ratio)));
  for (std::size_t i = 0; i < elites; ++i) append(*candidates[i], nullptr);

  std::vector<std::string> cell_owners;
  for (const auto& [coordinates, id] : state.feature_maps[island]) cell_owners.push_back(id);
  std::shuffle(cell_owners.begin(), cell_owners.end(), state.random);
  for (const auto& id : cell_owners) append(programs_[index_.at(id)], "diverse");

  std::shuffle(candidates.begin(), candidates.end(), state.random);
  for (const auto* program : candidates) append(*program, "random");

  return result;
}

absl::StatusOr<IslandSelectionContext> ProgramDatabase::SelectionContext(std::int64_t iteration,
                                                                         const std::vector<int>& pending_counts) const {
  if (!population_) return absl::FailedPreconditionError("Population mode is not enabled");

  const auto& state = *population_;
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
        const auto& program = programs_[index_.at(id)];
        const double score = Fitness(program);
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

DatabaseConfig ProgramDatabase::StorageConfiguration() const {
  return population_ ? population_->config : DatabaseConfig{};
}

// Captures everything a resumed run needs to continue rather than restart:
// programs, island and cell ownership, archive, best pointers, generation and
// migration counters, feature statistics, and the RNG stream position. The
// serialized RNG is why sampling must go through Mutate.
absl::StatusOr<CheckpointData> ProgramDatabase::SerializeCheckpoint() const {
  CheckpointData data;
  data.programs = programs_;
  data.metadata = {{"format", "ievolve.database"},
                   {"version", 1},
                   {"mode", population_ ? "population" : "core"},
                   {"feature_dimensions", feature_dimensions_}};
  if (!population_) return data;

  const auto& state = *population_;
  auto& metadata = data.metadata;
  metadata["population_config"] = PopulationConfiguration(state.config, state.mapper.bins());

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
  for (const auto& [dimension, stats] : state.mapper.stats())
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
absl::Status ProgramDatabase::RestoreCheckpoint(const CheckpointData& data) {
  try {
    const auto& metadata = data.metadata;
    Require(metadata.is_object());
    if (!data.legacy) {
      Require(String(metadata.at("format")) == "ievolve.database");
      Require(Unsigned(metadata.at("version")) == 1);
      const auto mode = String(metadata.at("mode"));
      Require(mode == "population" || mode == "core");
      if ((mode == "population") != population_.has_value() ||
          metadata.at("feature_dimensions") != Metrics(feature_dimensions_)) {
        return absl::FailedPreconditionError("Checkpoint mode or feature dimensions differ");
      }
    } else if (!population_) {
      return absl::FailedPreconditionError("Python checkpoints require population mode");
    }

    if (population_ && !data.legacy &&
        nlohmann::json(metadata.at("population_config")) !=
            nlohmann::json(PopulationConfiguration(population_->config, population_->mapper.bins()))) {
      return absl::FailedPreconditionError("Checkpoint population configuration differs");
    }

    programs_.clear();
    index_.clear();
    for (const auto& program : data.programs) {
      const auto status = Store(program);
      Require(status.ok());
    }
    if (!population_) return absl::OkStatus();

    const auto config = population_->config;
    auto mapper = FeatureMapper::Create(config);
    if (!mapper.ok()) return mapper.status();
    population_.emplace(config, std::move(*mapper));
    auto& state = *population_;
    const std::size_t count = state.islands.size();
    const auto& islands = metadata.at("islands");
    const auto& grids = metadata.at("island_feature_maps");
    Require(islands.is_array() && grids.is_array() && islands.size() == count && grids.size() == count);

    std::set<std::string> owned, cell_owners;
    for (std::size_t island = 0; island < count; ++island) {
      state.islands[island] = IdSet(islands[island]);
      for (const auto& id : state.islands[island]) {
        Require(index_.count(id) && owned.insert(id).second);
        auto& program = programs_[index_.at(id)];
        if (!data.legacy) Require(Counter(program.metadata.at("island")) == static_cast<std::int64_t>(island));
        program.metadata["island"] = island;
      }

      Require(grids[island].is_object());
      for (const auto& item : grids[island].items()) {
        const auto id = String(item.value());
        Require(state.islands[island].count(id) && cell_owners.insert(id).second);
        state.feature_maps[island].emplace(ReadCell(item.key(), feature_dimensions_, state.mapper.bins()), id);
      }
    }

    state.archive = IdSet(metadata.at("archive"));
    Require(state.archive.size() <= static_cast<std::size_t>(config.archive_size));
    for (const auto& id : state.archive) Require(index_.count(id));

    if (!metadata.contains("best_program_id")) Require(data.legacy);
    state.best = metadata.contains("best_program_id") ? OptionalId(metadata.at("best_program_id")) : std::nullopt;
    if (state.best) Require(index_.count(*state.best));
    if (!data.legacy) Require(state.best.has_value() != programs_.empty());

    // Older checkpoints can omit best_program_id while retaining a historical
    // champion outside every island. Recover it before validating ownership.
    if (data.legacy && !state.best) RefreshBest();

    for (const auto& program : programs_) {
      Require(owned.count(program.id) || state.archive.count(program.id) || state.best == program.id);
      if (!owned.count(program.id)) {
        Require(program.metadata.contains("island"));
        Require(Counter(program.metadata.at("island")) < static_cast<std::int64_t>(count));
      }
    }

    Require(programs_.size() <= static_cast<std::size_t>(config.population_size) + 1);

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
    RefreshBest();
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
      for (const auto& program : programs_)
        state.last_iteration = std::max(state.last_iteration, program.iteration_found);
    }

    state.last_migration =
        metadata.contains("last_migration_generation") ? Counter(metadata.at("last_migration_generation")) : 0;
    if (!metadata.contains("last_migration_generation")) Require(data.legacy);
    Require(state.last_migration <= *std::max_element(state.generations.begin(), state.generations.end()));

    if (!data.legacy) {
      for (const auto& program : programs_) Require(program.iteration_found <= state.last_iteration);
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
    Require(state.mapper.RestoreStatistics(stats).ok());

    return absl::OkStatus();
  } catch (...) {
    return absl::DataLossError("Invalid checkpoint database state");
  }
}

absl::Status ProgramDatabase::WriteCheckpoint(const std::filesystem::path& path) const {
  auto data = SerializeCheckpoint();
  if (!data.ok()) return data.status();

  return Checkpoint::Save(path, *data, StorageConfiguration());
}

absl::Status ProgramDatabase::Save(const std::filesystem::path& path) const {
  if (mutation_active_) return absl::FailedPreconditionError("Cannot save during a database mutation");

  auto selected = path;
  if (selected.empty() && population_ && population_->config.db_path) selected = *population_->config.db_path;
  if (selected.empty()) return absl::InvalidArgumentError("Checkpoint path is required");

  try {
    return WriteCheckpoint(selected);
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

    auto staged = *this;
    auto status = staged.RestoreCheckpoint(*data);
    if (!status.ok()) return status;

    programs_.swap(staged.programs_);
    index_.swap(staged.index_);
    population_.swap(staged.population_);

    return absl::OkStatus();
  } catch (...) {
    return absl::InternalError("Checkpoint restoration failed");
  }
}

absl::Status ProgramDatabase::ModifyProgram(std::string_view id, const std::function<absl::Status(Program&)>& modify) {
  if (mutation_active_) return absl::FailedPreconditionError("Nested database mutation is forbidden");

  const auto found = index_.find(std::string(id));
  if (found == index_.end()) return absl::NotFoundError("Program ID not found");

  // The offset stays valid inside Mutate: the callback edits the staged copy,
  // leaving this database's vector and index untouched until the swap.
  if (population_) {
    return Mutate([&](ProgramDatabase& staged) {
      auto& candidate = staged.programs_[found->second];
      auto status = modify(candidate);
      return status.ok() ? candidate.Validate() : status;
    });
  }

  try {
    auto candidate = programs_[found->second];
    auto status = modify(candidate);
    if (!status.ok()) return status;

    status = candidate.Validate();
    if (!status.ok()) return status;

    programs_[found->second] = std::move(candidate);
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
  for (const auto& program : programs_)
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
