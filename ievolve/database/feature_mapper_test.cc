#include "ievolve/database/feature_mapper.h"

#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace ievolve {
namespace {

Program MakeProgram(std::string id, std::string code, Metrics metrics = Metrics::object()) {
  Program program;
  program.id = std::move(id);
  program.code = std::move(code);
  program.metrics = std::move(metrics);

  return program;
}

TEST(FeatureMapperTest, RejectsInvalidConfiguration) {
  std::vector<DatabaseConfig> cases;
  DatabaseConfig config;
  config.feature_dimensions.clear();
  cases.push_back(config);

  config.feature_dimensions = {"complexity", "complexity"};
  cases.push_back(config);

  config.feature_dimensions = {""};
  cases.push_back(config);

  config = DatabaseConfig{};
  config.feature_bins = 0;
  cases.push_back(config);

  config.feature_bins = -1;
  cases.push_back(config);

  config.feature_bins = std::map<std::string, int>{{"unused", 0}};
  cases.push_back(config);

  config = DatabaseConfig{};
  config.diversity_reference_size = 0;
  cases.push_back(config);

  config.diversity_reference_size = -1;
  cases.push_back(config);

  config = DatabaseConfig{};
  config.archive_size = -1;
  cases.push_back(config);

  for (const auto& invalid : cases) {
    EXPECT_EQ(FeatureMapper::Create(invalid).status().code(), absl::StatusCode::kInvalidArgument);
  }
}

TEST(FeatureMapperTest, MatchesPythonGoldenFeatureSequences) {
  std::ifstream input(std::string(IEVOLVE_DATABASE_TEST_DATA_DIR) + "/features.json");
  ASSERT_TRUE(input.good());

  const Metrics fixture = Metrics::parse(input);

  for (const auto& item : fixture.at("cases")) {
    SCOPED_TRACE(item.at("name").get<std::string>());
    DatabaseConfig config;
    config.feature_dimensions = item.at("dimensions").get<std::vector<std::string>>();
    config.archive_size = item.at("archive_size").get<int>();
    if (item.at("bins").is_number_integer()) {
      config.feature_bins = item.at("bins").get<int>();
    } else {
      config.feature_bins = item.at("bins").get<std::map<std::string, int>>();
    }

    auto mapper = FeatureMapper::Create(config);
    ASSERT_TRUE(mapper.ok()) << mapper.status();

    std::vector<Program> population;
    for (const auto& step : item.at("steps")) {
      auto program = Program::FromJson(step.at("program"));
      ASSERT_TRUE(program.ok()) << program.status();
      population.push_back(*program);

      const auto coordinates = mapper->Coordinates(*program, population);
      ASSERT_TRUE(coordinates.ok()) << coordinates.status();

      EXPECT_EQ(*coordinates, step.at("coordinates").get<std::vector<int>>());

      ASSERT_EQ(mapper->stats().size(), step.at("stats").size());
      for (const auto& stat : step.at("stats").items()) {
        const auto actual = mapper->stats().find(stat.key());
        ASSERT_NE(actual, mapper->stats().end());
        EXPECT_DOUBLE_EQ(actual->second.min, stat.value().at("min"));
        EXPECT_DOUBLE_EQ(actual->second.max, stat.value().at("max"));
        EXPECT_EQ(actual->second.count, stat.value().at("count"));
      }
    }
  }
}

TEST(FeatureMapperTest, DiversityUsesBoundedDeterministicUnicodeDistances) {
  DatabaseConfig config;
  config.feature_dimensions = {"diversity"};
  config.feature_bins = std::map<std::string, int>{{"diversity", 8}};
  config.diversity_reference_size = 2;

  auto mapper = FeatureMapper::Create(config);
  ASSERT_TRUE(mapper.ok()) << mapper.status();

  const auto candidate = MakeProgram("candidate", "你🙂");
  const auto one = MakeProgram("one", "你");
  const auto two = MakeProgram("two", "好🙃");
  const auto ignored = MakeProgram("ignored", std::string("\xff"));

  auto coordinates = mapper->Coordinates(candidate, {candidate});
  ASSERT_TRUE(coordinates.ok()) << coordinates.status();

  EXPECT_EQ(*coordinates, std::vector<int>({0}));
  EXPECT_TRUE(mapper->stats().empty());

  coordinates = mapper->Coordinates(candidate, {candidate, one, two, ignored});
  ASSERT_TRUE(coordinates.ok()) << coordinates.status();

  EXPECT_EQ(*coordinates, std::vector<int>({4}));
  EXPECT_DOUBLE_EQ(mapper->stats().at("diversity").min, 1.5);
  EXPECT_EQ(mapper->stats().at("diversity").count, 1);

  coordinates = mapper->Coordinates(candidate, {one, candidate});
  ASSERT_TRUE(coordinates.ok()) << coordinates.status();

  EXPECT_EQ(*coordinates, std::vector<int>({0}));
  EXPECT_DOUBLE_EQ(mapper->stats().at("diversity").max, 1.5);
  EXPECT_DOUBLE_EQ(mapper->stats().at("diversity").min, 1.0);
}

TEST(FeatureMapperTest, DiversityCountsIdenticalCodeAsZeroDistance) {
  DatabaseConfig config;
  config.feature_dimensions = {"diversity"};
  config.diversity_reference_size = 1;

  auto mapper = FeatureMapper::Create(config);
  ASSERT_TRUE(mapper.ok()) << mapper.status();

  const auto candidate = MakeProgram("candidate", "abc");
  const auto duplicate = MakeProgram("duplicate", "abc");
  const auto other = MakeProgram("other", "def");

  const auto coordinates = mapper->Coordinates(candidate, {duplicate, candidate, other});
  ASSERT_TRUE(coordinates.ok()) << coordinates.status();

  EXPECT_DOUBLE_EQ(mapper->stats().at("diversity").min, 0.0);
}

TEST(FeatureMapperTest, RejectsMissingAndNonnumericFeaturesTransactionally) {
  DatabaseConfig config;
  config.feature_dimensions = {"x", "y"};

  auto mapper = FeatureMapper::Create(config);
  ASSERT_TRUE(mapper.ok()) << mapper.status();

  const auto initial = MakeProgram("initial", "", {{"x", 2}, {"y", 3}});
  ASSERT_TRUE(mapper->Coordinates(initial, {initial}).ok());

  std::vector<Metrics> invalid_metrics = {{{"x", 100}},
                                          {{"x", 100}, {"y", "1"}},
                                          {{"x", 100}, {"y", nullptr}},
                                          {{"x", 100}, {"y", Metrics::array({1})}},
                                          {{"x", 100}, {"y", std::numeric_limits<double>::infinity()}},
                                          {{"x", 100}, {"y", std::numeric_limits<double>::quiet_NaN()}},
                                          Metrics::array({1, 2})};
  for (auto metrics : invalid_metrics) {
    const auto program = MakeProgram("invalid", "", std::move(metrics));

    const auto result = mapper->Coordinates(program, {program});
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);

    ASSERT_EQ(mapper->stats().size(), 2);
    EXPECT_DOUBLE_EQ(mapper->stats().at("x").min, 2);
    EXPECT_DOUBLE_EQ(mapper->stats().at("x").max, 2);
    EXPECT_EQ(mapper->stats().at("x").count, 1);
    EXPECT_EQ(mapper->stats().at("y").count, 1);
  }
}

TEST(FeatureMapperTest, InvalidCodeAndFitnessLeaveStatsUnchanged) {
  DatabaseConfig config;
  config.feature_dimensions = {"x", "complexity"};

  auto mapper = FeatureMapper::Create(config);
  ASSERT_TRUE(mapper.ok()) << mapper.status();

  const auto bad_code = MakeProgram("bad", "\xff", {{"x", 1}});
  EXPECT_EQ(mapper->Coordinates(bad_code, {bad_code}).status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(mapper->stats().empty());

  config.feature_dimensions = {"x", "score"};
  mapper = FeatureMapper::Create(config);
  ASSERT_TRUE(mapper.ok()) << mapper.status();

  const auto bad_score =
      MakeProgram("bad", "", {{"x", 1}, {"combined_score", std::numeric_limits<double>::infinity()}});
  EXPECT_EQ(mapper->Coordinates(bad_score, {bad_score}).status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(mapper->stats().empty());

  config.feature_dimensions = {"x", "diversity"};
  mapper = FeatureMapper::Create(config);
  ASSERT_TRUE(mapper.ok()) << mapper.status();

  const auto good_code = MakeProgram("good", "abc", {{"x", 1}});
  EXPECT_EQ(mapper->Coordinates(good_code, {good_code, bad_code}).status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(mapper->stats().empty());
}

TEST(FeatureMapperTest, SupportsFiniteValuesAcrossTheDoubleRange) {
  DatabaseConfig config;
  config.feature_dimensions = {"x"};
  config.feature_bins = std::map<std::string, int>{{"x", 10}};

  auto mapper = FeatureMapper::Create(config);
  ASSERT_TRUE(mapper.ok()) << mapper.status();

  const double largest = std::numeric_limits<double>::max();
  const std::vector<std::pair<double, int>> cases = {{-largest, 5}, {largest, 9}, {0, 5}, {-largest, 0}};

  for (const auto& [value, expected] : cases) {
    const auto program = MakeProgram("p", "", {{"x", value}});

    const auto coordinates = mapper->Coordinates(program, {program});
    ASSERT_TRUE(coordinates.ok()) << coordinates.status();

    EXPECT_EQ(*coordinates, std::vector<int>({expected}));
  }
}

TEST(FeatureMapperTest, CopiesOwnIndependentStatistics) {
  DatabaseConfig config;
  config.feature_dimensions = {"x"};
  config.archive_size = 0;
  config.feature_bins = 1;

  auto mapper = FeatureMapper::Create(config);
  ASSERT_TRUE(mapper.ok()) << mapper.status();

  auto copy = *mapper;
  const auto program = MakeProgram("p", "", {{"x", true}});

  const auto result = copy.Coordinates(program, {program});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(*result, std::vector<int>({0}));
  EXPECT_TRUE(mapper->stats().empty());
  EXPECT_DOUBLE_EQ(copy.stats().at("x").min, 1);
}

TEST(FeatureMapperTest, RestoresHistoricalRangesWithoutRecomputingPopulation) {
  DatabaseConfig config;
  config.feature_dimensions = {"x"};
  config.feature_bins = std::map<std::string, int>{{"x", 10}};

  auto mapper = FeatureMapper::Create(config);
  ASSERT_TRUE(mapper.ok());
  ASSERT_TRUE(mapper->RestoreStatistics({{"x", {-10, 10, 20}}}).ok());

  auto program = MakeProgram("resume", "", {{"x", 0}});

  auto coords = mapper->Coordinates(program, {program});
  ASSERT_TRUE(coords.ok());

  EXPECT_EQ(*coords, std::vector<int>{5});
  EXPECT_EQ(mapper->stats().at("x").count, 21);

  for (const auto& invalid :
       std::vector<std::map<std::string, FeatureStats>>{{{"missing", {0, 1, 1}}},
                                                        {{"x", {2, 1, 1}}},
                                                        {{"x", {0, 1, 0}}},
                                                        {{"x", {0, std::numeric_limits<double>::infinity(), 1}}}}) {
    EXPECT_EQ(mapper->RestoreStatistics(invalid).code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(mapper->stats().at("x").count, 21);
  }
}

TEST(FeatureMapperTest, ExhaustedRestoredCounterCannotWrapAndLoseHistory) {
  DatabaseConfig config;
  config.feature_dimensions = {"x"};

  auto mapper = FeatureMapper::Create(config);
  ASSERT_TRUE(mapper.ok());

  const auto maximum = std::numeric_limits<std::size_t>::max();
  ASSERT_TRUE(mapper->RestoreStatistics({{"x", {0, 10, maximum}}}).ok());

  auto program = MakeProgram("overflow", "", {{"x", 20}});

  EXPECT_EQ(mapper->Coordinates(program, {program}).status().code(), absl::StatusCode::kOutOfRange);

  EXPECT_EQ(mapper->stats().at("x").count, maximum);
  EXPECT_EQ(mapper->stats().at("x").max, 10);
}

}  // namespace
}  // namespace ievolve
