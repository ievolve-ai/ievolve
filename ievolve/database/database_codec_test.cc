#include "ievolve/database/database_codec.h"

#include "gtest/gtest.h"

namespace ievolve {
namespace {

DatabaseConfig Configuration() {
  DatabaseConfig config;
  config.num_islands = 2;
  config.population_size = 10;
  config.archive_size = 3;
  config.feature_dimensions = {"axis"};
  config.feature_bins = 4;
  config.random_seed = 11;

  return config;
}

// Mirrors what ProgramDatabase::Create derives from Configuration().
Population MakePopulation(const DatabaseConfig& config) {
  PopulationConfig population;
  population.population_size = config.population_size;
  population.archive_size = config.archive_size;
  population.num_islands = config.num_islands;
  population.elite_selection_ratio = config.elite_selection_ratio;
  population.exploration_ratio = config.exploration_ratio;
  population.exploitation_ratio = config.exploitation_ratio;
  population.diversity_metric = config.diversity_metric;
  population.migration_interval = config.migration_interval;
  population.migration_rate = config.migration_rate;
  population.random_seed = config.random_seed;

  auto mapper = FeatureMapper::Create(config);
  EXPECT_TRUE(mapper.ok()) << mapper.status();
  return Population(population, std::move(*mapper));
}

Program Candidate(std::string id, double score, double axis) {
  Program program;
  program.id = std::move(id);
  program.code = program.id;
  program.metrics = {{"combined_score", score}, {"axis", axis}};
  program.timestamp = 1;

  return program;
}

struct Populated {
  ProgramStore store;
  std::optional<Population> population;
};

Populated MakePopulated() {
  const auto config = Configuration();
  Populated result{ProgramStore(config.feature_dimensions), MakePopulation(config)};
  for (int i = 0; i < 5; ++i) {
    auto inserted =
        result.population->Insert(result.store, Candidate("p" + std::to_string(i), i, i * 0.2), AddOptions{i % 2, i});
    EXPECT_TRUE(inserted.ok() && *inserted);
  }
  // Advance the RNG so the round trip has to carry a non-initial stream.
  EXPECT_TRUE(result.population->Sample(result.store, std::nullopt, 2).ok());

  return result;
}

absl::StatusOr<CheckpointData> RoundTrip(const CheckpointData& data, Populated& target) {
  auto status = database_codec::Decode(data, target.store, target.population);
  if (!status.ok()) return status;

  return database_codec::Encode(target.store, target.population ? &*target.population : nullptr);
}

TEST(DatabaseCodecTest, PopulationModeRoundTripsEveryField) {
  auto source = MakePopulated();
  auto encoded = database_codec::Encode(source.store, &*source.population);
  ASSERT_TRUE(encoded.ok()) << encoded.status();

  auto target = Populated{ProgramStore(Configuration().feature_dimensions), MakePopulation(Configuration())};
  auto again = RoundTrip(*encoded, target);
  ASSERT_TRUE(again.ok()) << again.status();

  EXPECT_EQ(again->metadata, encoded->metadata);
  ASSERT_EQ(again->programs.size(), encoded->programs.size());
  for (std::size_t i = 0; i < again->programs.size(); ++i) {
    EXPECT_EQ(again->programs[i].id, encoded->programs[i].id);
  }

  // The restored RNG continues the same stream as the source.
  auto expected = source.population->Sample(source.store, std::nullopt, 2);
  auto actual = target.population->Sample(target.store, std::nullopt, 2);
  ASSERT_TRUE(expected.ok() && actual.ok());
  EXPECT_EQ(actual->parent.id, expected->parent.id);
}

// Checkpoints compare this object on resume, so its keys, order and values are
// part of the on-disk format.
TEST(DatabaseCodecTest, PopulationConfigRecordsPopulationAndFeatureSettingsInOrder) {
  auto source = MakePopulated();
  auto encoded = database_codec::Encode(source.store, &*source.population);
  ASSERT_TRUE(encoded.ok()) << encoded.status();

  EXPECT_EQ(encoded->metadata.at("population_config").dump(),
            R"({"num_islands":2,"population_size":10,"archive_size":3,"feature_dimensions":["axis"],)"
            R"("feature_bins":{"axis":4},"diversity_reference_size":20,"diversity_metric":"edit_distance",)"
            R"("exploration_ratio":0.2,"exploitation_ratio":0.7,"elite_selection_ratio":0.1,)"
            R"("migration_interval":50,"migration_rate":0.1})");
}

TEST(DatabaseCodecTest, CoreModeRoundTripsProgramsOnly) {
  ProgramStore store({});
  ASSERT_TRUE(store.Add(Candidate("a", 1, 0)).ok());
  ASSERT_TRUE(store.Add(Candidate("b", 2, 0)).ok());

  auto encoded = database_codec::Encode(store, nullptr);
  ASSERT_TRUE(encoded.ok()) << encoded.status();
  EXPECT_EQ(encoded->metadata.at("mode"), "core");

  Populated target{ProgramStore({}), std::nullopt};
  auto again = RoundTrip(*encoded, target);
  ASSERT_TRUE(again.ok()) << again.status();
  EXPECT_EQ(again->metadata, encoded->metadata);
  EXPECT_EQ(target.store.size(), 2u);
}

TEST(DatabaseCodecTest, DanglingIslandMemberIsDataLoss) {
  auto source = MakePopulated();
  auto encoded = database_codec::Encode(source.store, &*source.population);
  ASSERT_TRUE(encoded.ok());
  encoded->metadata["islands"][0].push_back("missing");

  auto target = Populated{ProgramStore(Configuration().feature_dimensions), MakePopulation(Configuration())};
  EXPECT_EQ(database_codec::Decode(*encoded, target.store, target.population).code(), absl::StatusCode::kDataLoss);
}

TEST(DatabaseCodecTest, ProgramClaimedByTwoIslandsIsDataLoss) {
  auto source = MakePopulated();
  auto encoded = database_codec::Encode(source.store, &*source.population);
  ASSERT_TRUE(encoded.ok());
  const auto claimed = encoded->metadata["islands"][0][0];
  encoded->metadata["islands"][1].push_back(claimed);

  auto target = Populated{ProgramStore(Configuration().feature_dimensions), MakePopulation(Configuration())};
  EXPECT_EQ(database_codec::Decode(*encoded, target.store, target.population).code(), absl::StatusCode::kDataLoss);
}

TEST(DatabaseCodecTest, DifferentPopulationConfigurationIsFailedPrecondition) {
  auto source = MakePopulated();
  auto encoded = database_codec::Encode(source.store, &*source.population);
  ASSERT_TRUE(encoded.ok());

  auto config = Configuration();
  config.archive_size = 4;
  auto target = Populated{ProgramStore(config.feature_dimensions), MakePopulation(config)};
  EXPECT_EQ(database_codec::Decode(*encoded, target.store, target.population).code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(DatabaseCodecTest, ModeMismatchIsFailedPrecondition) {
  ProgramStore store(Configuration().feature_dimensions);
  auto encoded = database_codec::Encode(store, nullptr);
  ASSERT_TRUE(encoded.ok());

  auto target = Populated{ProgramStore(Configuration().feature_dimensions), MakePopulation(Configuration())};
  EXPECT_EQ(database_codec::Decode(*encoded, target.store, target.population).code(),
            absl::StatusCode::kFailedPrecondition);
}

}  // namespace
}  // namespace ievolve
