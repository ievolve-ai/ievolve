#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "gtest/gtest.h"
#include "ievolve/database/program_database.h"

namespace ievolve {
namespace {
DatabaseConfig Configuration() {
  DatabaseConfig config;
  config.num_islands = 3;
  config.population_size = 20;
  config.archive_size = 5;
  config.feature_dimensions = {"axis"};
  config.feature_bins = 10;
  config.random_seed = 7;

  return config;
}

Program Candidate(std::string id, double score, double axis = 0) {
  Program program;
  program.id = std::move(id);
  program.code = program.id;
  program.metrics = {{"combined_score", score}, {"axis", axis}};
  program.timestamp = 1;

  return program;
}

void CheckInvariants(const ProgramDatabase& database) {
  auto snapshot = database.Snapshot();
  ASSERT_TRUE(snapshot.ok()) << snapshot.status();

  std::set<std::string> owned;
  for (std::size_t i = 0; i < snapshot->islands.size(); ++i) {
    for (const auto& id : snapshot->islands[i]) {
      ASSERT_TRUE(snapshot->programs.count(id));
      EXPECT_TRUE(owned.insert(id).second);
      EXPECT_EQ(snapshot->programs.at(id).metadata.at("island"), i);
    }

    for (const auto& [cell, id] : snapshot->feature_maps[i]) {
      EXPECT_EQ(cell.size(), snapshot->feature_dimensions.size());
      EXPECT_TRUE(snapshot->islands[i].count(id));
    }

    if (snapshot->island_best_programs[i]) EXPECT_TRUE(snapshot->islands[i].count(*snapshot->island_best_programs[i]));
  }

  for (const auto& id : snapshot->archive) EXPECT_TRUE(snapshot->programs.count(id));
  EXPECT_LE(snapshot->archive.size(), snapshot->archive_limit);

  if (snapshot->best_program_id) EXPECT_TRUE(snapshot->programs.count(*snapshot->best_program_id));
  EXPECT_LE(database.size(), snapshot->population_limit + 1);
}

TEST(PopulationTest, MatchesPythonDefaultPopulationTransitions) {
  std::ifstream input(std::string(IEVOLVE_DATABASE_TEST_DATA_DIR) + "/population.json");
  ASSERT_TRUE(input.good());

  const auto fixture = Metrics::parse(input);
  ASSERT_EQ(fixture.at("cases").size(), 7);

  for (const auto& item : fixture.at("cases")) {
    SCOPED_TRACE(item.at("name").get<std::string>());
    auto config = Configuration();
    const auto& settings = item.at("config");
    config.feature_dimensions = settings.at("feature_dimensions").get<std::vector<std::string>>();
    config.feature_bins = settings.at("feature_bins").get<std::map<std::string, int>>();
    config.archive_size = settings.at("archive_size").get<int>();
    config.population_size = settings.at("population_size").get<int>();
    config.num_islands = settings.at("num_islands").get<int>();

    auto database = ProgramDatabase::Create(config);
    ASSERT_TRUE(database.ok()) << database.status();

    for (const auto& step : item.at("steps")) {
      SCOPED_TRACE(step.at("program").at("id").get<std::string>());
      if (step.contains("set_current_island")) {
        ASSERT_TRUE(database->SetCurrentIsland(step.at("set_current_island").get<int>()).ok());
      }

      auto program = Program::FromJson(step.at("program"));
      ASSERT_TRUE(program.ok()) << program.status();

      AddOptions options;
      const auto& fields = step.at("options");
      if (fields.contains("island")) options.island = fields.at("island").get<int>();
      if (fields.contains("iteration")) options.iteration = fields.at("iteration").get<std::int64_t>();

      auto added = database->Add(*program, options);
      ASSERT_TRUE(added.ok()) << added.status();
      ASSERT_TRUE(*added);

      auto snapshot = database->Snapshot();
      ASSERT_TRUE(snapshot.ok());
      Metrics actual = {
          {"program_ids", Metrics::array()},
          {"islands", snapshot->islands},
          {"feature_maps", Metrics::array()},
          {"archive", snapshot->archive},
          {"best_program_id", snapshot->best_program_id ? Metrics(*snapshot->best_program_id) : Metrics(nullptr)},
          {"island_best_programs", Metrics::array()},
          {"last_iteration", snapshot->last_iteration},
          {"current_island", snapshot->current_island}};

      for (const auto& [id, stored] : snapshot->programs) actual["program_ids"].push_back(id);

      for (const auto& grid : snapshot->feature_maps) {
        auto cells = Metrics::array();
        for (const auto& [coordinates, id] : grid) cells.push_back({{"coordinates", coordinates}, {"program_id", id}});
        actual["feature_maps"].push_back(cells);
      }

      for (const auto& best : snapshot->island_best_programs)
        actual["island_best_programs"].push_back(best ? Metrics(*best) : Metrics(nullptr));

      EXPECT_EQ(actual, step.at("expected"));
      CheckInvariants(*database);
    }
  }
}

TEST(PopulationTest, ValidatesConfigurationAndRequiresPopulationMode) {
  for (int field = 0; field < 9; ++field) {
    auto config = Configuration();
    switch (field) {
      case 0:
        config.num_islands = 0;
        break;
      case 1:
        config.population_size = 0;
        break;
      case 2:
        config.archive_size = -1;
        break;
      case 3:
        config.exploration_ratio = -0.1;
        break;
      case 4:
        config.exploitation_ratio = 1;
        break;
      case 5:
        config.migration_rate = std::numeric_limits<double>::quiet_NaN();
        break;
      case 6:
        config.migration_interval = 0;
        break;
      case 7:
        config.elite_selection_ratio = 2;
        break;
      case 8:
        config.feature_dimensions = {};
        break;
    }

    EXPECT_EQ(ProgramDatabase::Create(config).status().code(), absl::StatusCode::kInvalidArgument);
  }

  auto config = Configuration();
  config.db_path = std::string("a\0b", 3);
  EXPECT_EQ(ProgramDatabase::Create(config).status().code(), absl::StatusCode::kInvalidArgument);

  ProgramDatabase core;
  EXPECT_EQ(core.Snapshot().status().code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_EQ(core.Sample().status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST(PopulationTest, AssignsIslandsByExplicitParentAndCurrentOptions) {
  auto database = ProgramDatabase::Create(Configuration());
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->SetCurrentIsland(1).ok());
  ASSERT_TRUE(database->Add(Candidate("a", 1)).ok());

  auto child = Candidate("b", 0);
  child.parent_id = "a";
  ASSERT_TRUE(database->SetCurrentIsland(2).ok());
  ASSERT_TRUE(database->Add(child).ok());

  auto result = database->Add(Candidate("c", 2), AddOptions{0, 9});
  ASSERT_TRUE(result.ok());

  EXPECT_TRUE(*result);

  auto snapshot = database->Snapshot();
  ASSERT_TRUE(snapshot.ok());

  EXPECT_EQ(snapshot->islands[0], (std::set<std::string>{"c"}));
  EXPECT_EQ(snapshot->islands[1], (std::set<std::string>{"a", "b"}));
  EXPECT_EQ(snapshot->current_island, 2);
  EXPECT_EQ(snapshot->last_iteration, 9);
  EXPECT_EQ(database->Get("c")->iteration_found, 9);

  snapshot->programs.at("a").code = "mutated";
  EXPECT_EQ(database->Get("a")->code, "a");

  EXPECT_FALSE(database->Add(Candidate("bad", 99), AddOptions{3, 10}).ok());
  EXPECT_FALSE(database->Add(Candidate("bad", 99), AddOptions{0, -1}).ok());
  EXPECT_EQ(database->size(), 3);
  CheckInvariants(*database);
}

TEST(PopulationTest, CellsReplaceOnlyOnImprovementAndKeepLosingCandidates) {
  auto database = ProgramDatabase::Create(Configuration());
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("first", 1)).ok());
  ASSERT_TRUE(database->Add(Candidate("loser", 0)).ok());

  EXPECT_EQ(database->Snapshot()->feature_maps[0].begin()->second, "first");

  ASSERT_TRUE(database->Add(Candidate("winner", 2)).ok());

  auto snapshot = database->Snapshot();
  ASSERT_TRUE(snapshot.ok());

  EXPECT_EQ(snapshot->feature_maps[0].begin()->second, "winner");
  EXPECT_FALSE(snapshot->programs.count("first"));
  EXPECT_TRUE(snapshot->programs.count("loser"));
  EXPECT_EQ(snapshot->best_program_id, "winner");
  CheckInvariants(*database);
}

TEST(PopulationTest, AdmissionRejectsSeedAndPreservesLaterState) {
  bool accept = false;
  PopulationStrategy strategy;
  strategy.admit = [&](const auto& state, const Program& program, int island) {
    EXPECT_EQ(island, 0);
    EXPECT_FALSE(state.programs.count(program.id));

    return accept;
  };

  auto database = ProgramDatabase::Create(Configuration(), strategy);
  ASSERT_TRUE(database.ok());

  EXPECT_EQ(database->Add(Candidate("seed", 1), {}).status().code(), absl::StatusCode::kFailedPrecondition);

  accept = true;
  ASSERT_TRUE(database->Add(Candidate("seed", 1)).ok());

  accept = false;
  auto rejected = database->Add(Candidate("later", 2), AddOptions{0, 10});
  ASSERT_TRUE(rejected.ok());

  EXPECT_FALSE(*rejected);
  EXPECT_EQ(database->size(), 1);
  EXPECT_EQ(database->Snapshot()->last_iteration, 0);
  EXPECT_EQ(database->FeatureStatistics()->at("axis").count, 1);
  EXPECT_FALSE(database->Add(Candidate("later", 2)).ok());
}

TEST(PopulationTest, KeepsDisplacedHistoricalBestUntilBetterArrives) {
  PopulationStrategy strategy;
  strategy.replace_cell = [](const auto&, const auto&, const auto&, int) { return true; };

  auto config = Configuration();
  config.population_size = 1;

  auto database = ProgramDatabase::Create(config, strategy);
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("best", 10)).ok());
  ASSERT_TRUE(database->Add(Candidate("replacement", 1)).ok());

  EXPECT_EQ(database->GetBestProgram()->id, "best");
  EXPECT_FALSE(database->Snapshot()->islands[0].count("best"));
  EXPECT_EQ(database->size(), 2);

  ASSERT_TRUE(database->Add(Candidate("improved", 20)).ok());

  EXPECT_EQ(database->size(), 1);
  EXPECT_EQ(database->GetBestProgram()->id, "improved");
  CheckInvariants(*database);
}

TEST(PopulationTest, ArchivePolicyMayRetainDisplacedOwners) {
  PopulationStrategy strategy;
  strategy.archive = [](const auto&, const auto&) { return ArchiveDecision{true, {}}; };

  auto database = ProgramDatabase::Create(Configuration(), strategy);
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("old", 1)).ok());
  ASSERT_TRUE(database->Add(Candidate("new", 2)).ok());

  EXPECT_TRUE(database->Get("old").ok());
  EXPECT_FALSE(database->Snapshot()->islands[0].count("old"));
  EXPECT_EQ(database->Snapshot()->archive, (std::set<std::string>{"old", "new"}));
  CheckInvariants(*database);
}

TEST(PopulationTest, ArchiveEvictionReleasesPreviouslyRetainedOrphans) {
  PopulationStrategy strategy;
  strategy.archive = [](const auto&, const Program& candidate) {
    return ArchiveDecision{true, candidate.id == "third" ? std::optional<std::string>("first") : std::nullopt};
  };

  auto database = ProgramDatabase::Create(Configuration(), strategy);
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("first", 1)).ok());
  ASSERT_TRUE(database->Add(Candidate("second", 2)).ok());
  ASSERT_TRUE(database->Get("first").ok());

  ASSERT_TRUE(database->Add(Candidate("third", 3)).ok());

  EXPECT_EQ(database->Get("first").status().code(), absl::StatusCode::kNotFound);
  EXPECT_TRUE(database->Get("second").ok());
  CheckInvariants(*database);
}

TEST(PopulationTest, InvalidArchiveDecisionRollsBackProgramsCellsAndFeatures) {
  bool invalid = false;
  PopulationStrategy strategy;
  strategy.archive = [&](const auto&, const auto&) {
    return ArchiveDecision{true, invalid ? std::optional<std::string>("missing") : std::nullopt};
  };

  auto database = ProgramDatabase::Create(Configuration(), strategy);
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("seed", 1)).ok());

  invalid = true;
  EXPECT_EQ(database->Add(Candidate("bad", 2, 100)).code(), absl::StatusCode::kInvalidArgument);

  EXPECT_EQ(database->size(), 1);
  EXPECT_EQ(database->Snapshot()->feature_maps[0].begin()->second, "seed");
  EXPECT_EQ(database->FeatureStatistics()->at("axis").max, 0);
  EXPECT_EQ(database->FeatureStatistics()->at("axis").count, 1);
  CheckInvariants(*database);
}

TEST(PopulationTest, CallbackErrorsExceptionsAndNestedMutationAreContained) {
  int mode = 0;
  ProgramDatabase* active = nullptr;
  PopulationStrategy strategy;
  strategy.replace_cell = [&](const auto&, const auto&, const auto&, int) -> absl::StatusOr<bool> {
    if (mode == 1) return absl::CancelledError("policy cancelled");
    if (mode == 2) throw std::runtime_error("private callback details");
    if (mode == 3) {
      EXPECT_EQ(active->SetCurrentIsland(1).code(), absl::StatusCode::kFailedPrecondition);
      EXPECT_EQ(active->Add(Candidate("nested", 1)).code(), absl::StatusCode::kFailedPrecondition);
      EXPECT_TRUE(active->Get("seed").ok());
    }

    return true;
  };

  auto database = ProgramDatabase::Create(Configuration(), strategy);
  ASSERT_TRUE(database.ok());
  active = &*database;
  ASSERT_TRUE(database->Add(Candidate("seed", 1)).ok());

  for (mode = 1; mode <= 2; ++mode) {
    auto status = database->Add(Candidate("bad", 2));
    EXPECT_EQ(status.code(), mode == 1 ? absl::StatusCode::kCancelled : absl::StatusCode::kInternal);
    EXPECT_EQ(status.message().find("private"), std::string::npos);
    EXPECT_EQ(database->size(), 1);
  }

  mode = 3;
  ASSERT_TRUE(database->Add(Candidate("success", 2)).ok());

  EXPECT_EQ(database->Snapshot()->current_island, 0);
}

TEST(PopulationTest, CapacityPrefersNonCellOwnersAndProtectsNewCandidate) {
  auto config = Configuration();
  config.population_size = 3;

  auto database = ProgramDatabase::Create(config);
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("best", 10)).ok());
  ASSERT_TRUE(database->Add(Candidate("loser1", 1)).ok());
  ASSERT_TRUE(database->Add(Candidate("cell", 0, 10)).ok());
  ASSERT_TRUE(database->Add(Candidate("loser2", -1)).ok());

  EXPECT_EQ(database->size(), 3);
  EXPECT_FALSE(database->Get("loser1").ok());
  EXPECT_TRUE(database->Get("cell").ok());
  EXPECT_TRUE(database->Get("loser2").ok());
  CheckInvariants(*database);
}

TEST(PopulationTest, EvictionPolicyMustReturnExactUniqueEligibleIds) {
  int mode = 0;
  PopulationStrategy strategy;
  strategy.evict = [&](const auto&, std::size_t required, const auto& protected_ids) {
    EXPECT_EQ(required, 1);
    EXPECT_TRUE(protected_ids.count("best"));
    EXPECT_TRUE(protected_ids.count("new"));

    if (mode == 0) return std::vector<std::string>{};
    if (mode == 1) return std::vector<std::string>{"best"};
    if (mode == 2) return std::vector<std::string>{"missing"};

    return std::vector<std::string>{"old"};
  };

  auto config = Configuration();
  config.population_size = 2;

  auto database = ProgramDatabase::Create(config, strategy);
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("best", 10)).ok());
  ASSERT_TRUE(database->Add(Candidate("old", 1)).ok());

  for (mode = 0; mode < 3; ++mode) {
    EXPECT_FALSE(database->Add(Candidate("new", 0)).ok());
    EXPECT_EQ(database->size(), 2);
    EXPECT_TRUE(database->Get("old").ok());
  }

  ASSERT_TRUE(database->Add(Candidate("new", 0)).ok());

  EXPECT_FALSE(database->Get("old").ok());
  CheckInvariants(*database);
}

TEST(PopulationTest, ReplacingArchivedOwnerDoesNotShrinkFullArchive) {
  auto config = Configuration();
  config.archive_size = 2;

  auto database = ProgramDatabase::Create(config);
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("first", 5)).ok());
  ASSERT_TRUE(database->Add(Candidate("weaker", 1), AddOptions{1, {}}).ok());
  ASSERT_TRUE(database->Add(Candidate("winner", 10)).ok());

  EXPECT_EQ(database->Snapshot()->archive, (std::set<std::string>{"weaker", "winner"}));
  CheckInvariants(*database);
}

TEST(PopulationTest, ArchiveSizeZeroIsSupported) {
  auto config = Configuration();
  config.archive_size = 0;

  auto database = ProgramDatabase::Create(config);
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("seed", 1)).ok());

  EXPECT_TRUE(database->Snapshot()->archive.empty());
  EXPECT_TRUE(database->Sample(0).ok());
}

TEST(PopulationTest, SamplingIsSeededLocalAndFillsDistinctInspirations) {
  auto config = Configuration();
  config.exploration_ratio = 1;
  config.exploitation_ratio = 0;

  auto left = ProgramDatabase::Create(config), right = ProgramDatabase::Create(config);
  ASSERT_TRUE(left.ok());
  ASSERT_TRUE(right.ok());

  for (int i = 0; i < 8; ++i) {
    auto program = Candidate(std::to_string(i), 10 - i);
    ASSERT_TRUE(left->Add(program, AddOptions{i % 2, {}}).ok());
    ASSERT_TRUE(right->Add(program, AddOptions{i % 2, {}}).ok());
  }

  for (int i = 0; i < 20; ++i) {
    auto a = left->SampleFromIsland(1, 10), b = right->SampleFromIsland(1, 10);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());

    EXPECT_EQ(a->parent.id, b->parent.id);
    EXPECT_EQ(a->parent.metadata["island"], 1);
    ASSERT_EQ(a->inspirations.size(), 3);

    std::set<std::string> unique{a->parent.id};
    for (std::size_t j = 0; j < a->inspirations.size(); ++j) {
      EXPECT_EQ(a->inspirations[j].id, b->inspirations[j].id);
      EXPECT_EQ(a->inspirations[j].metadata["island"], 1);
      EXPECT_TRUE(unique.insert(a->inspirations[j].id).second);
    }
  }

  EXPECT_EQ(left->FeatureStatistics()->at("axis").count, 8);
  EXPECT_EQ(left->Snapshot()->current_island, 0);

  EXPECT_FALSE(left->SampleFromIsland(-1).ok());
  EXPECT_FALSE(left->Sample(-1).ok());
}

TEST(PopulationTest, ArchiveFallbackKeepsInspirationsInRequestedIsland) {
  auto config = Configuration();
  config.exploration_ratio = 0;
  config.exploitation_ratio = 1;

  PopulationStrategy strategy;
  strategy.archive = [](const auto&, const Program& candidate) { return ArchiveDecision{candidate.id == "elite", {}}; };

  auto database = ProgramDatabase::Create(config, strategy);
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("elite", 10)).ok());
  ASSERT_TRUE(database->Add(Candidate("local1", 2), AddOptions{1, {}}).ok());
  ASSERT_TRUE(database->Add(Candidate("local2", 1), AddOptions{1, {}}).ok());

  auto sample = database->SampleFromIsland(1, 5);
  ASSERT_TRUE(sample.ok());

  EXPECT_EQ(sample->parent.id, "elite");
  ASSERT_EQ(sample->inspirations.size(), 2);
  for (const auto& inspiration : sample->inspirations) EXPECT_EQ(inspiration.metadata["island"], 1);

  auto generic = database->Sample(5);
  ASSERT_TRUE(generic.ok());

  EXPECT_EQ(generic->parent.id, "elite");
  EXPECT_TRUE(generic->inspirations.empty());
}

TEST(PopulationTest, WeightedSamplingHandlesExtremeScoresWithoutOverflow) {
  auto config = Configuration();
  config.exploration_ratio = 0;
  config.exploitation_ratio = 0;
  config.archive_size = 0;

  auto database = ProgramDatabase::Create(config);
  ASSERT_TRUE(database.ok());

  const double maximum = std::numeric_limits<double>::max();
  ASSERT_TRUE(database->Add(Candidate("a", maximum)).ok());
  ASSERT_TRUE(database->Add(Candidate("b", maximum)).ok());
  ASSERT_TRUE(database->Add(Candidate("negative", -1)).ok());

  std::set<std::string> parents;
  for (int i = 0; i < 100; ++i) {
    auto sample = database->SampleFromIsland(0, 0);
    ASSERT_TRUE(sample.ok());
    parents.insert(sample->parent.id);
  }

  EXPECT_EQ(parents, (std::set<std::string>{"a", "b"}));

  auto context = database->SelectionContext(0, {0, 0, 0});
  ASSERT_TRUE(context.ok());

  EXPECT_TRUE(std::isfinite(context->islands[0].average_score));
  EXPECT_DOUBLE_EQ(context->islands[0].average_score, maximum * (2.0 / 3.0));
}

TEST(PopulationTest, EmptyMetricsUseTimestampAndMetricsBeatEmptyPrograms) {
  auto config = Configuration();
  config.feature_dimensions = {"complexity"};

  auto database = ProgramDatabase::Create(config);
  ASSERT_TRUE(database.ok());

  auto candidate = Candidate("a", 0);
  candidate.metrics = Metrics::object();
  candidate.code = "same";
  ASSERT_TRUE(database->Add(candidate).ok());

  candidate.id = "b";
  candidate.timestamp = 2;
  ASSERT_TRUE(database->Add(candidate).ok());

  EXPECT_FALSE(database->Get("a").ok());
  EXPECT_EQ(database->GetBestProgram()->id, "b");

  candidate.id = "c";
  candidate.metrics = {{"combined_score", -10}};
  ASSERT_TRUE(database->Add(candidate).ok());

  EXPECT_FALSE(database->Get("b").ok());
  EXPECT_EQ(database->GetBestProgram()->id, "c");
}

TEST(PopulationTest, SamplingEmptyIslandFallsBackWithoutInsertingCopies) {
  auto database = ProgramDatabase::Create(Configuration());
  ASSERT_TRUE(database.ok());

  EXPECT_EQ(database->Sample().status().code(), absl::StatusCode::kNotFound);

  ASSERT_TRUE(database->Add(Candidate("seed", 1)).ok());

  auto sample = database->SampleFromIsland(2, 5);
  ASSERT_TRUE(sample.ok());

  EXPECT_EQ(sample->parent.id, "seed");
  EXPECT_TRUE(sample->inspirations.empty());
  EXPECT_EQ(database->size(), 1);
}

TEST(PopulationTest, RingMigrationPreservesFieldsAndDoesNotCascade) {
  auto config = Configuration();
  config.migration_interval = 2;

  auto database = ProgramDatabase::Create(config);
  ASSERT_TRUE(database.ok());

  auto seed = Candidate("seed", 1);
  seed.changes_description = "keep";
  seed.generation = 3;
  seed.artifacts_json = "data";
  seed.prompts = Metrics{{"user", "text"}};

  ASSERT_TRUE(database->Add(seed).ok());
  EXPECT_FALSE(*database->ShouldMigrate());

  ASSERT_TRUE(database->IncrementGeneration(0).ok());
  ASSERT_TRUE(database->IncrementGeneration(0).ok());
  EXPECT_TRUE(*database->ShouldMigrate());

  ASSERT_TRUE(database->Migrate().ok());

  EXPECT_EQ(database->size(), 3);

  auto snapshot = database->Snapshot();
  ASSERT_TRUE(snapshot.ok());

  EXPECT_EQ(snapshot->last_migration_generation, 2);
  for (int island = 1; island < 3; ++island) {
    ASSERT_EQ(snapshot->islands[island].size(), 1);
    const auto& copy = snapshot->programs.at(*snapshot->islands[island].begin());
    EXPECT_EQ(copy.parent_id, "seed");
    EXPECT_EQ(copy.generation, 3);
    EXPECT_EQ(copy.metadata["migrant"], true);
    EXPECT_EQ(copy.changes_description, "keep");
    EXPECT_EQ(copy.artifacts_json, seed.artifacts_json);
    EXPECT_EQ(copy.prompts, seed.prompts);
  }

  EXPECT_FALSE(*database->ShouldMigrate());

  ASSERT_TRUE(database->Migrate().ok());

  EXPECT_EQ(database->size(), 3);
  CheckInvariants(*database);
}

TEST(PopulationTest, InvalidMigrationPlanAndLaterFailureRollBackAllMoves) {
  int mode = 0;
  PopulationStrategy strategy;
  strategy.migrate = [&](const auto&) {
    return std::vector<MigrationMove>{{"seed", 1}, {mode == 0 ? "missing" : "seed", 2}};
  };

  strategy.admit = [&](const auto&, const auto&, int island) -> absl::StatusOr<bool> {
    if (mode == 1 && island == 2) return absl::CancelledError("stop");
    return true;
  };

  auto database = ProgramDatabase::Create(Configuration(), strategy);
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("seed", 1)).ok());

  for (mode = 0; mode < 2; ++mode) {
    EXPECT_FALSE(database->Migrate().ok());
    EXPECT_EQ(database->size(), 1);
    EXPECT_EQ(database->FeatureStatistics()->at("axis").count, 1);
    CheckInvariants(*database);
  }

  ASSERT_TRUE(database->Migrate().ok());

  EXPECT_EQ(database->size(), 3);
  EXPECT_EQ(database->Get("seed.migrant.0")->metadata["island"], 1);
}

TEST(PopulationTest, MigrationDueHookAndSingleIslandBehaviorAreExplicit) {
  bool fail = false;
  PopulationStrategy strategy;
  strategy.migration_due = [&](const auto& snapshot) -> absl::StatusOr<bool> {
    if (fail) throw std::runtime_error("private");
    return snapshot.generations[1] >= 1;
  };

  auto database = ProgramDatabase::Create(Configuration(), strategy);
  ASSERT_TRUE(database.ok());

  EXPECT_FALSE(*database->ShouldMigrate());

  ASSERT_TRUE(database->IncrementGeneration(1).ok());
  EXPECT_TRUE(*database->ShouldMigrate());

  fail = true;
  EXPECT_EQ(database->ShouldMigrate().status().code(), absl::StatusCode::kInternal);
  EXPECT_EQ(database->Snapshot()->generations[1], 1);

  auto config = Configuration();
  config.num_islands = 1;

  auto single = ProgramDatabase::Create(config, strategy);
  ASSERT_TRUE(single.ok());

  EXPECT_FALSE(*single->ShouldMigrate());
  EXPECT_TRUE(single->Migrate().ok());
}

TEST(PopulationTest, MigrationQueryRejectsReentryAndReleasesGuardAfterErrors) {
  ProgramDatabase* active = nullptr;
  bool fail = true;
  PopulationStrategy strategy;
  strategy.migration_due = [&](const auto&) -> absl::StatusOr<bool> {
    EXPECT_EQ(active->ShouldMigrate().status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(active->IncrementGeneration(0).code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(active->Add(Candidate("nested", 2)).code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(active->Save().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_TRUE(active->Get("seed").ok());

    if (fail) return absl::CancelledError("query declined");
    return true;
  };

  auto database = ProgramDatabase::Create(Configuration(), strategy);
  ASSERT_TRUE(database.ok());
  active = &*database;
  ASSERT_TRUE(database->Add(Candidate("seed", 1)).ok());

  EXPECT_EQ(database->ShouldMigrate().status().code(), absl::StatusCode::kCancelled);
  EXPECT_EQ(database->size(), 1);
  EXPECT_EQ(database->Snapshot()->generations[0], 0);

  ASSERT_TRUE(database->IncrementGeneration(0).ok());
  fail = false;

  const auto due = database->ShouldMigrate();
  ASSERT_TRUE(due.ok()) << due.status();

  EXPECT_TRUE(*due);
  EXPECT_EQ(database->size(), 1);
  EXPECT_EQ(database->Snapshot()->generations[0], 1);

  ASSERT_TRUE(database->IncrementGeneration(0).ok());
}

TEST(PopulationTest, ZeroMigrationRateDisablesDefaultMovesAndTwoIslandsDeduplicate) {
  for (double rate : {0.0, 1.0}) {
    auto config = Configuration();
    config.num_islands = 2;
    config.migration_rate = rate;

    auto database = ProgramDatabase::Create(config);
    ASSERT_TRUE(database.ok());

    ASSERT_TRUE(database->Add(Candidate("a", 1)).ok());

    ASSERT_TRUE(database->Migrate().ok());

    EXPECT_EQ(database->size(), rate == 0 ? 1 : 2);

    ASSERT_TRUE(database->Migrate().ok());

    EXPECT_EQ(database->size(), rate == 0 ? 1 : 2);
    CheckInvariants(*database);
  }
}

TEST(PopulationTest, RepeatedInsertionSamplingAndMigrationPreserveInvariants) {
  auto config = Configuration();
  config.population_size = 7;
  config.archive_size = 3;

  auto database = ProgramDatabase::Create(config);
  ASSERT_TRUE(database.ok());

  for (int step = 0; step < 160; ++step) {
    auto candidate = Candidate("id" + std::to_string(step), (step * 17) % 41 - 20, (step * 7) % 13);

    auto admitted = database->Add(candidate, AddOptions{step % 3, step});
    ASSERT_TRUE(admitted.ok()) << admitted.status();
    ASSERT_TRUE(*admitted);

    if (step % 5 == 0) ASSERT_TRUE(database->Migrate().ok());
    ASSERT_TRUE(database->IncrementGeneration(step % 3).ok());

    auto sample = database->SampleFromIsland(step % 3, 10);
    ASSERT_TRUE(sample.ok()) << sample.status();

    std::set<std::string> ids{sample->parent.id};
    for (const auto& inspiration : sample->inspirations) {
      EXPECT_TRUE(ids.insert(inspiration.id).second);
      EXPECT_EQ(inspiration.metadata["island"], step % 3);
    }

    CheckInvariants(*database);
  }
}

TEST(PopulationTest, SelectionContextReportsDetachedFiniteIslandSummaries) {
  auto database = ProgramDatabase::Create(Configuration());
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("a", -1)).ok());
  ASSERT_TRUE(database->Add(Candidate("b", -3)).ok());

  auto context = database->SelectionContext(5, {1, 2, 0});
  ASSERT_TRUE(context.ok());

  EXPECT_EQ(context->islands[0].population_size, 2);
  EXPECT_EQ(context->islands[0].best_score, -1);
  EXPECT_EQ(context->islands[0].average_score, -2);
  EXPECT_EQ(context->islands[0].diversity, 1);
  EXPECT_EQ(context->islands[1].best_score, 0);

  EXPECT_FALSE(database->SelectionContext(-1, {0, 0, 0}).ok());
  EXPECT_FALSE(database->SelectionContext(0, {0}).ok());
}
}  // namespace
}  // namespace ievolve
