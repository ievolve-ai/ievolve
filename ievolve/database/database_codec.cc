#include "ievolve/database/database_codec.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace ievolve::database_codec {
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
// Population settings plus the feature settings FeatureMapper was built from.
// Keys and their order are part of the checkpoint format.
Metrics PopulationConfiguration(const Population& population) {
  const auto& config = population.config();
  const auto& mapper = population.mapper();
  return {{"num_islands", config.num_islands},
          {"population_size", config.population_size},
          {"archive_size", config.archive_size},
          {"feature_dimensions", mapper.dimensions()},
          {"feature_bins", mapper.bins()},
          {"diversity_reference_size", mapper.reference_size()},
          {"diversity_metric", config.diversity_metric},
          {"exploration_ratio", config.exploration_ratio},
          {"exploitation_ratio", config.exploitation_ratio},
          {"elite_selection_ratio", config.elite_selection_ratio},
          {"migration_interval", config.migration_interval},
          {"migration_rate", config.migration_rate}};
}
}  // namespace

// Captures everything a resumed run needs to continue rather than restart:
// programs, island and cell ownership, archive, best pointers, generation and
// migration counters, feature statistics, and the RNG stream position. The
// serialized RNG is why sampling must go through Mutate.
absl::StatusOr<CheckpointData> Encode(const ProgramStore& programs, const Population* population) {
  CheckpointData data;
  data.programs = programs.programs();
  data.metadata = {{"format", "ievolve.database"},
                   {"version", 1},
                   {"mode", population ? "population" : "core"},
                   {"feature_dimensions", programs.feature_dimensions()}};
  if (!population) return data;

  const auto& state = population->state();
  auto& metadata = data.metadata;
  metadata["population_config"] = PopulationConfiguration(*population);

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
  for (const auto& [dimension, stats] : population->mapper().stats())
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
absl::Status Decode(const CheckpointData& data, ProgramStore& programs, std::optional<Population>& target) {
  try {
    const auto& metadata = data.metadata;
    Require(metadata.is_object());
    if (!data.legacy) {
      Require(String(metadata.at("format")) == "ievolve.database");
      Require(Unsigned(metadata.at("version")) == 1);
      const auto mode = String(metadata.at("mode"));
      Require(mode == "population" || mode == "core");
      if ((mode == "population") != target.has_value() ||
          metadata.at("feature_dimensions") != Metrics(programs.feature_dimensions())) {
        return absl::FailedPreconditionError("Checkpoint mode or feature dimensions differ");
      }
    } else if (!target) {
      return absl::FailedPreconditionError("Python checkpoints require population mode");
    }

    if (target && !data.legacy &&
        nlohmann::json(metadata.at("population_config")) != nlohmann::json(PopulationConfiguration(*target))) {
      return absl::FailedPreconditionError("Checkpoint population configuration differs");
    }

    programs.Clear();
    for (const auto& program : data.programs) {
      const auto status = programs.Add(program);
      Require(status.ok());
    }
    if (!target) return absl::OkStatus();

    auto& population = *target;
    population.Reset();
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

}  // namespace ievolve::database_codec
