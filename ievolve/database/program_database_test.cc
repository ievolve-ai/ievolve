#include "ievolve/database/program_database.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>

#include "gtest/gtest.h"
#include "ievolve/prompt/prompt_sampler.h"

namespace ievolve {
namespace {

Program MakeProgram(std::string id, Metrics metrics) {
  Program program;
  program.id = std::move(id);
  program.code = "x = 1\n";
  program.metrics = std::move(metrics);
  program.timestamp = 1.0;

  return program;
}

std::vector<std::string> Ids(const std::vector<Program>& programs) {
  std::vector<std::string> result;
  for (const auto& program : programs) result.push_back(program.id);

  return result;
}

TEST(ProgramDatabaseTest, MatchesPythonGlobalQueries) {
  std::ifstream stream(std::string(IEVOLVE_DATABASE_TEST_DATA_DIR) + "/python_golden.json");
  ASSERT_TRUE(stream.good());

  const auto fixtures = Metrics::parse(stream);
  ASSERT_EQ(fixtures.at("cases").size(), 7);

  for (const auto& item : fixtures.at("cases")) {
    SCOPED_TRACE(item.at("name").get<std::string>());
    ProgramDatabase database(item.at("feature_dimensions").get<std::vector<std::string>>());
    for (const auto& input : item.at("programs")) {
      auto program = Program::FromJson(input);
      ASSERT_TRUE(program.ok()) << program.status();
      ASSERT_TRUE(database.Add(*program).ok());

      auto stored = database.Get(program->id);
      ASSERT_TRUE(stored.ok());

      EXPECT_EQ(stored->code, program->code);
    }

    for (const auto& expected : item.at("results")) {
      std::optional<std::string> metric;
      if (!expected.at("metric").is_null()) metric = expected.at("metric").get<std::string>();

      auto best = database.GetBestProgram(metric);
      if (expected.at("best").is_null()) {
        EXPECT_EQ(best.status().code(), absl::StatusCode::kNotFound);
      } else {
        ASSERT_TRUE(best.ok()) << best.status();
        EXPECT_EQ(best->id, expected.at("best"));
      }

      for (const auto& top : expected.at("tops")) {
        auto result = database.GetTopPrograms(top.at("n"), metric);
        ASSERT_TRUE(result.ok()) << result.status();

        EXPECT_EQ(Metrics(Ids(*result)), top.at("ids"));
      }
    }
  }
}

// Several checks share InvalidArgument, so the message pins which one runs
// first: population ratios, then artifact storage, then the feature mapper.
TEST(ProgramDatabaseTest, CreateValidatesConfigurationInOrder) {
  DatabaseConfig config;
  config.feature_dimensions.clear();
  config.artifact_size_threshold = -1;

  auto storage_first = ProgramDatabase::Create(config);
  EXPECT_EQ(storage_first.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(storage_first.status().message(), "Invalid artifact data: threshold and retention must be nonnegative");

  config.num_islands = 0;
  auto population_first = ProgramDatabase::Create(config);
  EXPECT_EQ(population_first.status().message(), "Invalid population configuration");

  config.num_islands = 1;
  config.artifact_size_threshold = 0;
  auto features_last = ProgramDatabase::Create(config);
  EXPECT_EQ(features_last.status().message(), "Feature dimensions must not be empty");
}

TEST(ProgramDatabaseTest, CreatePassesPopulationSettingsToPopulation) {
  DatabaseConfig config;
  config.num_islands = 3;
  config.population_size = 11;
  config.archive_size = 4;
  config.migration_interval = 9;
  config.migration_rate = 0.5;

  auto database = ProgramDatabase::Create(config);
  ASSERT_TRUE(database.ok()) << database.status();
  auto snapshot = database->Snapshot();
  ASSERT_TRUE(snapshot.ok()) << snapshot.status();

  EXPECT_EQ(snapshot->islands.size(), 3u);
  EXPECT_EQ(snapshot->population_limit, 11);
  EXPECT_EQ(snapshot->archive_limit, 4);
  EXPECT_EQ(snapshot->migration_interval, 9);
  EXPECT_EQ(snapshot->migration_rate, 0.5);
}

TEST(ProgramDatabaseTest, OwnsInputAndReturnedSnapshots) {
  ProgramDatabase database;
  auto input = MakeProgram("a", {{"score", 1}});
  input.metadata = {{"nested", {1, 2}}};

  ASSERT_TRUE(database.Add(input).ok());

  input.code = "changed";
  input.metadata["nested"][0] = 9;

  auto stored = database.Get("a");
  ASSERT_TRUE(stored.ok());

  EXPECT_EQ(stored->code, "x = 1\n");
  EXPECT_EQ(stored->metadata["nested"][0], 1);

  stored->metrics["score"] = 100;
  auto top = database.GetTopPrograms();
  ASSERT_TRUE(top.ok());

  top->front().metrics["score"] = -100;
  auto best = database.GetBestProgram();
  ASSERT_TRUE(best.ok());

  EXPECT_EQ(best->metrics["score"], 1);
}

TEST(ProgramDatabaseTest, RejectsDuplicateIdsWithoutReplacingValues) {
  ProgramDatabase database;
  ASSERT_TRUE(database.Add(MakeProgram("a", {{"score", 1}})).ok());

  EXPECT_EQ(database.Add(MakeProgram("a", {{"score", 99}})).code(), absl::StatusCode::kAlreadyExists);
  EXPECT_EQ(database.size(), 1);

  auto best = database.GetBestProgram();
  ASSERT_TRUE(best.ok());

  EXPECT_EQ(best->metrics["score"], 1);
}

TEST(ProgramDatabaseTest, RejectsInvalidProgramsBeforeMutation) {
  ProgramDatabase database;
  ASSERT_TRUE(database.Add(MakeProgram("valid", {{"score", 1}})).ok());

  auto invalid = MakeProgram("invalid", {{"score", 99}});
  invalid.metrics = Metrics::array();

  EXPECT_EQ(database.Add(invalid).code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(database.Get("invalid").status().code(), absl::StatusCode::kNotFound);
  EXPECT_EQ(database.size(), 1);

  invalid = MakeProgram("", {{"score", 99}});
  EXPECT_FALSE(database.Add(invalid).ok());
  EXPECT_EQ(database.size(), 1);
}

TEST(ProgramDatabaseTest, RejectsNonfiniteDerivedFitness) {
  ProgramDatabase database;
  for (const auto& score :
       {Metrics(std::numeric_limits<double>::quiet_NaN()), Metrics(std::numeric_limits<double>::infinity()),
        Metrics(-std::numeric_limits<double>::infinity())}) {
    EXPECT_EQ(database.Add(MakeProgram("bad", {{"combined_score", score}})).code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(database.size(), 0);
  }

  EXPECT_FALSE(database.Add(MakeProgram("overflow", {{"a", 1e308}, {"b", 1e308}})).ok());
}

TEST(ProgramDatabaseTest, PreservesInsertionOrderForEqualScores) {
  ProgramDatabase database;
  for (const auto& id : {"z", "a", "m"}) ASSERT_TRUE(database.Add(MakeProgram(id, {{"score", 1}})).ok());

  auto top = database.GetTopPrograms(2);
  ASSERT_TRUE(top.ok());

  EXPECT_EQ(Ids(*top), (std::vector<std::string>{"z", "a"}));

  ASSERT_TRUE(database.Add(MakeProgram("best", {{"score", 2}})).ok());

  auto best = database.GetBestProgram();
  ASSERT_TRUE(best.ok());

  EXPECT_EQ(best->id, "best");
}

TEST(ProgramDatabaseTest, NamedMetricsSkipNonnumericValuesAndKeepZeroAndBool) {
  ProgramDatabase database;
  for (const auto& value : {Metrics("100"), Metrics(nullptr), Metrics(false), Metrics(true), Metrics(0), Metrics(-1)}) {
    const auto id = value.dump();
    ASSERT_TRUE(database.Add(MakeProgram(id, {{"named", value}, {"combined_score", 1}})).ok());
  }

  auto result = database.GetTopPrograms(10, "named");
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(Ids(*result), (std::vector<std::string>{"true", "false", "0", "-1"}));

  EXPECT_EQ(database.GetBestProgram("missing").status().code(), absl::StatusCode::kNotFound);
}

TEST(ProgramDatabaseTest, NamedMetricOrderingKeepsIntegerPrecision) {
  ProgramDatabase database;
  const std::vector<std::pair<std::string, Metrics>> values = {
      {"lower", 9007199254740992LL},
      {"higher", 9007199254740993LL},
      {"float_upper", 0x1p64},
      {"unsigned_max", std::numeric_limits<std::uint64_t>::max()},
      {"negative", -1},
      {"zero", std::uint64_t{0}},
      {"signed_min", std::numeric_limits<std::int64_t>::min()},
      {"float_min", -0x1p64}};
  for (const auto& [id, value] : values) ASSERT_TRUE(database.Add(MakeProgram(id, {{"score", value}})).ok());

  auto result = database.GetTopPrograms(10, "score");
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(Ids(*result), (std::vector<std::string>{"float_upper", "unsigned_max", "higher", "lower", "zero",
                                                    "negative", "signed_min", "float_min"}));
}

TEST(ProgramDatabaseTest, EmptyAndInvalidQueriesHaveExplicitResults) {
  ProgramDatabase database;

  EXPECT_EQ(database.Get("absent").status().code(), absl::StatusCode::kNotFound);
  EXPECT_EQ(database.GetBestProgram().status().code(), absl::StatusCode::kNotFound);
  EXPECT_EQ(database.GetTopPrograms(-1).status().code(), absl::StatusCode::kInvalidArgument);

  auto top = database.GetTopPrograms(0);
  ASSERT_TRUE(top.ok());

  EXPECT_TRUE(top->empty());
}

TEST(ProgramDatabaseTest, ProjectsQueriedProgramsIntoPrompts) {
  ProgramDatabase database;
  auto program = MakeProgram("best", {{"score", 0.9}});
  program.changes_description = "optimized loop";
  program.metadata = {{"changes", "loop"},
                      {"parent_metrics", {{"score", 0.5}}},
                      {"key_features", {"small", "fast"}},
                      {"migrant", true}};

  ASSERT_TRUE(database.Add(program).ok());

  auto best = database.GetBestProgram();
  ASSERT_TRUE(best.ok());

  auto display = PromptProgram::FromProgram(*best);
  ASSERT_TRUE(display.ok()) << display.status();

  EXPECT_EQ(display->id, "best");
  EXPECT_EQ(display->parent_metrics["score"], 0.5);
  EXPECT_EQ(display->changes, "loop");
  EXPECT_TRUE(display->migrant);

  PromptRequest request;
  request.top_programs.push_back(*display);
  request.previous_programs.push_back(*display);

  auto prompt = PromptSampler().BuildPrompt(request);
  ASSERT_TRUE(prompt.ok()) << prompt.status();

  EXPECT_NE(prompt->user.find("small, fast"), std::string::npos);
  EXPECT_NE(prompt->user.find("optimized loop"), std::string::npos);
  EXPECT_NE(prompt->user.find("Improvement in all metrics"), std::string::npos);

  display->parent_metrics["score"] = 99;
  EXPECT_EQ(best->metadata["parent_metrics"]["score"], 0.5);
}

namespace fs = std::filesystem;
class DatabasePersistenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::temp_directory_path() / ("ievolve-persistence-test-" + std::to_string(std::random_device{}()));
    ASSERT_TRUE(fs::create_directory(root));
  }
  void TearDown() override {
    std::error_code error;
    fs::remove_all(root, error);
  }
  DatabaseConfig Config() const {
    DatabaseConfig config;
    config.num_islands = 2;
    config.feature_dimensions = {"axis"};
    config.feature_bins = std::map<std::string, int>{{"axis", 8}};
    config.archive_size = 3;
    config.artifacts_base_path = (root / "live-artifacts").string();
    config.artifact_size_threshold = 4;

    return config;
  }
  Program Candidate(std::string id, double score, double axis = 0) const {
    Program p;
    p.id = std::move(id);
    p.code = "代码 " + p.id;
    p.timestamp = 123;
    p.metrics = {{"combined_score", score}, {"axis", axis}};

    return p;
  }
  void Seed(ProgramDatabase& database) {
    ASSERT_TRUE(database.Add(Candidate("best", 10), AddOptions{0, 4}).ok());
    ASSERT_TRUE(database.Add(Candidate("other", 5, 10), AddOptions{1, 8}).ok());
    ASSERT_TRUE(database.Add(Candidate("loser", 2), AddOptions{0, 9}).ok());
  }
  fs::path root;
};

TEST_F(DatabasePersistenceTest, RoundTripPreservesPopulationAndRandomContinuation) {
  auto config = Config();
  auto database = ProgramDatabase::Create(config);
  ASSERT_TRUE(database.ok()) << database.status();

  Seed(*database);
  ASSERT_TRUE(database->SetCurrentIsland(1).ok());
  ASSERT_TRUE(database->IncrementGeneration(1).ok());
  ASSERT_TRUE(database->Sample().ok());

  ASSERT_TRUE(database->Save(root / "checkpoint").ok());

  config.random_seed = 999;
  auto resumed = ProgramDatabase::Create(config);
  ASSERT_TRUE(resumed.ok());
  ASSERT_TRUE(resumed->Add(Candidate("discard", 100)).ok());

  ASSERT_TRUE(resumed->Load(root / "checkpoint").ok());

  EXPECT_FALSE(resumed->Get("discard").ok());

  auto expected = database->Snapshot(), actual = resumed->Snapshot();
  ASSERT_TRUE(expected.ok());
  ASSERT_TRUE(actual.ok());

  EXPECT_EQ(actual->islands, expected->islands);
  EXPECT_EQ(actual->feature_maps, expected->feature_maps);
  EXPECT_EQ(actual->archive, expected->archive);
  EXPECT_EQ(actual->best_program_id, expected->best_program_id);
  EXPECT_EQ(actual->island_best_programs, expected->island_best_programs);
  EXPECT_EQ(actual->generations, (std::vector<std::int64_t>{0, 1}));
  EXPECT_EQ(actual->last_iteration, 9);
  EXPECT_EQ(actual->current_island, 1);
  EXPECT_EQ(resumed->FeatureStatistics()->at("axis").count, 3);

  for (int i = 0; i < 30; ++i) {
    auto a = database->Sample(2), b = resumed->Sample(2);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());

    EXPECT_EQ(a->parent.id, b->parent.id);
    ASSERT_EQ(a->inspirations.size(), b->inspirations.size());
    for (std::size_t j = 0; j < a->inspirations.size(); ++j) EXPECT_EQ(a->inspirations[j].id, b->inspirations[j].id);
  }

  ASSERT_TRUE(resumed->Add(Candidate("after", 12, 5)).ok());

  EXPECT_EQ(resumed->FeatureStatistics()->at("axis").min, 0);
  EXPECT_EQ(resumed->FeatureStatistics()->at("axis").max, 10);
  EXPECT_EQ(resumed->FeatureStatistics()->at("axis").count, 4);
}

TEST_F(DatabasePersistenceTest, RestoresCoreOrderingAndDoesNotMergeOrCallHooks) {
  ProgramDatabase core;
  ASSERT_TRUE(core.Add(Candidate("z", 1)).ok());
  ASSERT_TRUE(core.Add(Candidate("a", 1)).ok());

  ASSERT_TRUE(core.Save(root / "core").ok());

  ASSERT_TRUE(core.Add(Candidate("extra", 9)).ok());
  ASSERT_TRUE(core.Load(root / "core").ok());

  auto top = core.GetTopPrograms();
  ASSERT_TRUE(top.ok());

  ASSERT_EQ(top->size(), 2);
  EXPECT_EQ((*top)[0].id, "z");
  EXPECT_EQ((*top)[1].id, "a");

  auto population = ProgramDatabase::Create(Config());
  ASSERT_TRUE(population.ok());
  Seed(*population);
  ASSERT_TRUE(population->Save(root / "population").ok());

  int calls = 0;
  PopulationStrategy hooks;
  hooks.admit = [&](const auto&, const auto&, int) {
    ++calls;
    return false;
  };

  auto resumed = ProgramDatabase::Create(Config(), hooks);
  ASSERT_TRUE(resumed.ok());

  ASSERT_TRUE(resumed->Load(root / "population").ok());

  EXPECT_EQ(calls, 0);
  EXPECT_EQ(resumed->size(), 3);

  EXPECT_FALSE(core.Load(root / "population").ok());
  EXPECT_EQ(core.size(), 2);
}

TEST_F(DatabasePersistenceTest, InvalidPopulationReferencesRollBackWholeLoad) {
  auto source = ProgramDatabase::Create(Config());
  ASSERT_TRUE(source.ok());

  Seed(*source);
  ASSERT_TRUE(source->Save(root / "valid").ok());

  auto data = Checkpoint::Load(root / "valid");
  ASSERT_TRUE(data.ok());

  auto target = ProgramDatabase::Create(Config());
  ASSERT_TRUE(target.ok());
  ASSERT_TRUE(target->Add(Candidate("keep", 1)).ok());

  const std::vector<std::pair<std::string, Metrics>> malformed = {
      {"archive", {"missing"}},
      {"best_program_id", "missing"},
      {"islands", {{"best", "other"}, {"other", "loser"}}},
      {"island_generations", {-1, 0}},
      {"current_island", 2},
      {"last_iteration", -1},
      {"rng_state", "invalid"},
      {"next_migrant", -1},
      {"feature_stats", {{"axis", {{"min", 2}, {"max", 1}, {"count", 1}}}}}};

  for (const auto& [key, value] : malformed) {
    SCOPED_TRACE(key);
    auto bad = *data;
    bad.metadata[key] = value;

    ASSERT_TRUE(Checkpoint::Save(root / "invalid", bad, Config()).ok());

    EXPECT_FALSE(target->Load(root / "invalid").ok());
    EXPECT_EQ(target->size(), 1);
    EXPECT_TRUE(target->Get("keep").ok());
  }
}

TEST_F(DatabasePersistenceTest, RejectsConfigurationMismatchAndMissingFiles) {
  auto source = ProgramDatabase::Create(Config());
  ASSERT_TRUE(source.ok());

  Seed(*source);
  ASSERT_TRUE(source->Save(root / "valid").ok());

  auto config = Config();
  config.num_islands = 3;
  auto target = ProgramDatabase::Create(config);
  ASSERT_TRUE(target.ok());

  EXPECT_EQ(target->Load(root / "valid").code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_EQ(target->size(), 0);

  EXPECT_EQ(target->Load(root / "missing").code(), absl::StatusCode::kNotFound);
}

TEST_F(DatabasePersistenceTest, LegacyOptionalFieldsDefaultWithoutDroppingPrograms) {
  auto database = ProgramDatabase::Create(Config());
  ASSERT_TRUE(database.ok());

  Seed(*database);
  ASSERT_TRUE(database->Save(root / "native").ok());

  auto data = Checkpoint::Load(root / "native");
  ASSERT_TRUE(data.ok());

  fs::create_directories(root / "legacy" / "programs");
  for (const auto& program : data->programs)
    std::ofstream(root / "legacy" / "programs" / (program.id + ".json")) << program.ToJson()->dump();

  for (const auto* key :
       {"best_program_id", "island_best_programs", "current_island", "last_iteration", "island_generations",
        "last_migration_generation", "feature_stats", "rng_state", "next_migrant"})
    data->metadata.erase(key);
  std::ofstream(root / "legacy" / "metadata.json") << data->metadata.dump();

  auto resumed = ProgramDatabase::Create(Config());
  ASSERT_TRUE(resumed.ok());

  auto loaded = resumed->Load(root / "legacy");
  ASSERT_TRUE(loaded.ok()) << loaded;

  EXPECT_EQ(resumed->size(), 3);
  EXPECT_EQ(resumed->GetBestProgram()->id, "best");
  EXPECT_EQ(resumed->Snapshot()->current_island, 0);
  // Preserve the observed iteration floor when older metadata omits its
  // counter.
  EXPECT_EQ(resumed->Snapshot()->last_iteration, 9);

  ASSERT_TRUE(resumed->Save(root / "converted").ok());
  ASSERT_TRUE(database->Load(root / "converted").ok());
}

TEST_F(DatabasePersistenceTest, CleanupProtectsLiveBundlesAndRemovesReleasedOnes) {
  auto config = Config();
  config.artifact_retention_days = 1;

  auto database = ProgramDatabase::Create(config);
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("seed", 1)).ok());

  ASSERT_TRUE(database->StoreArtifacts("seed", {{"large", std::string(20, 'x')}}).ok());

  const auto directory = *database->Get("seed")->artifact_dir;
  fs::last_write_time(directory, fs::file_time_type::clock::now() - std::chrono::hours(48));

  auto protected_cleanup = database->CleanupArtifacts();
  ASSERT_TRUE(protected_cleanup.ok());

  EXPECT_EQ(*protected_cleanup, 0);
  EXPECT_TRUE(fs::exists(directory));

  ASSERT_TRUE(database->StoreArtifacts("seed", {}).ok());

  auto released_cleanup = database->CleanupArtifacts();
  ASSERT_TRUE(released_cleanup.ok());

  EXPECT_EQ(*released_cleanup, 1);
  EXPECT_FALSE(fs::exists(directory));
}

TEST_F(DatabasePersistenceTest, DisabledPromptLoggingAndEmptyDatabaseRoundTrip) {
  auto config = Config();
  config.log_prompts = false;

  auto database = ProgramDatabase::Create(config);
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Save(root / "empty").ok());

  ASSERT_TRUE(database->Add(Candidate("seed", 1)).ok());
  ASSERT_TRUE(database->LogPrompt("seed", "name", {{"user", "secret"}}).ok());

  EXPECT_FALSE(database->Get("seed")->prompts);

  ASSERT_TRUE(database->Load(root / "empty").ok());

  EXPECT_EQ(database->size(), 0);

  ASSERT_TRUE(database->Add(Candidate("restart", 2)).ok());
}

TEST_F(DatabasePersistenceTest, ResumePreservesMigrationSequenceAndHistoricalBest) {
  auto config = Config();
  config.migration_interval = 1;

  PopulationStrategy strategy;
  strategy.replace_cell = [](const auto&, const auto&, const auto&, int) { return true; };

  auto database = ProgramDatabase::Create(config, strategy);
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("best", 10)).ok());
  ASSERT_TRUE(database->Add(Candidate("replacement", 1)).ok());
  ASSERT_TRUE(database->IncrementGeneration(0).ok());
  ASSERT_TRUE(database->Migrate().ok());

  ASSERT_TRUE(database->Save(root / "migration").ok());

  auto resumed = ProgramDatabase::Create(config, strategy);
  ASSERT_TRUE(resumed.ok());

  ASSERT_TRUE(resumed->Load(root / "migration").ok());

  EXPECT_EQ(resumed->GetBestProgram()->id, "best");
  EXPECT_FALSE(resumed->Snapshot()->islands[0].count("best"));
  EXPECT_FALSE(*resumed->ShouldMigrate());

  ASSERT_TRUE(resumed->Add(Candidate("next", 2)).ok());
  ASSERT_TRUE(resumed->Migrate().ok());

  EXPECT_TRUE(resumed->Get("next.migrant.1").ok());
}

TEST_F(DatabasePersistenceTest, ArtifactsAndPromptsSurviveCheckpointRelocation) {
  auto database = ProgramDatabase::Create(Config());
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("../名字", 1)).ok());

  ArtifactMap artifacts{{"text", std::string("large UTF-8 你好")},
                        {"bytes", ArtifactBytes{0, 1, 255, 2, 3}},
                        {"small", ArtifactBytes{65, 66}}};

  ASSERT_TRUE(database->StoreArtifacts("../名字", artifacts).ok());
  ASSERT_TRUE(database
                  ->LogPrompt("../名字", "evolve", {{"system", "s"}, {"user", "u"}}, {"response"},
                              Metrics{{"input_tokens", 10}})
                  .ok());

  const auto saved = database->Save(root / "old");
  ASSERT_TRUE(saved.ok()) << saved;

  fs::rename(root / "old", root / "moved");
  fs::remove_all(root / "live-artifacts");

  auto resumed = ProgramDatabase::Create(Config());
  ASSERT_TRUE(resumed.ok());

  ASSERT_TRUE(resumed->Load(root / "moved").ok());

  auto loaded = resumed->GetArtifacts("../名字");
  ASSERT_TRUE(loaded.ok()) << loaded.status();

  EXPECT_EQ(*loaded, artifacts);

  auto program = resumed->Get("../名字");
  ASSERT_TRUE(program.ok());

  ASSERT_TRUE(program->prompts);
  EXPECT_EQ(program->prompts->at("evolve").at("responses"), Metrics::array({"response"}));
  EXPECT_EQ(program->prompts->at("evolve").at("token_usage").at("input_tokens"), 10);
}

TEST_F(DatabasePersistenceTest, ImportsPythonFixtureAndContinuesWithSavedRanges) {
  auto config = Config();
  config.feature_bins = std::map<std::string, int>{{"axis", 4}};

  auto database = ProgramDatabase::Create(config);
  ASSERT_TRUE(database.ok());

  const auto loaded = database->Load(fs::path(IEVOLVE_DATABASE_TEST_DATA_DIR) / "checkpoint_legacy");
  ASSERT_TRUE(loaded.ok()) << loaded;

  const auto snapshot = database->Snapshot();
  ASSERT_TRUE(snapshot.ok());

  EXPECT_EQ(snapshot->islands[0], (std::set<std::string>{"legacy-z"}));
  EXPECT_EQ(snapshot->islands[1], (std::set<std::string>{"legacy-a"}));
  EXPECT_EQ(snapshot->best_program_id, "legacy-a");
  EXPECT_EQ(snapshot->generations, (std::vector<std::int64_t>{2, 3}));
  EXPECT_EQ(snapshot->last_iteration, 12);
  EXPECT_EQ(snapshot->last_migration_generation, 2);
  EXPECT_EQ(database->FeatureStatistics()->at("axis").min, 2);
  EXPECT_EQ(database->FeatureStatistics()->at("axis").max, 7);

  ASSERT_TRUE(database->Save(root / "native-conversion").ok());
  ASSERT_TRUE(database->Load(root / "native-conversion").ok());

  ASSERT_TRUE(database->Add(Candidate("continue", 3, 4), AddOptions{0, 13}).ok());

  EXPECT_EQ(database->Snapshot()->feature_maps[0].at(FeatureCoordinates{1}), "continue");
  EXPECT_EQ(database->FeatureStatistics()->at("axis").count, 3);
}

TEST_F(DatabasePersistenceTest, PersistenceCallsFromCallbacksCannotMutateState) {
  ProgramDatabase* active = nullptr;
  PopulationStrategy strategy;
  strategy.admit = [&](const auto&, const auto&, int) {
    EXPECT_EQ(active->Save(root / "nested").code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(active->Load(root / "missing").code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(active->StoreArtifacts("seed", {}).code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(active->CleanupArtifacts().status().code(), absl::StatusCode::kFailedPrecondition);
    if (active->size() != 0) EXPECT_TRUE(active->GetArtifacts("seed").ok());

    return true;
  };

  auto database = ProgramDatabase::Create(Config(), strategy);
  ASSERT_TRUE(database.ok());
  active = &*database;

  ASSERT_TRUE(database->Add(Candidate("seed", 1)).ok());
  ASSERT_TRUE(database->Add(Candidate("second", 0)).ok());

  EXPECT_FALSE(fs::exists(root / "nested"));
}

TEST_F(DatabasePersistenceTest, FailedArtifactWritesAndPromptValidationPreserveProgram) {
  auto database = ProgramDatabase::Create(Config());
  ASSERT_TRUE(database.ok());

  ASSERT_TRUE(database->Add(Candidate("seed", 1)).ok());

  ASSERT_TRUE(database->StoreArtifacts("seed", {{"small", std::string("old")}}).ok());

  std::ofstream(root / "live-artifacts") << "blocked";
  EXPECT_FALSE(database->StoreArtifacts("seed", {{"large", std::string(100, 'x')}}).ok());

  auto old = database->GetArtifacts("seed");
  ASSERT_TRUE(old.ok());

  EXPECT_EQ(std::get<std::string>(old->at("small")), "old");

  EXPECT_FALSE(database->LogPrompt("seed", "name", Metrics::array()).ok());
  EXPECT_FALSE(database->Get("seed")->prompts);

  EXPECT_EQ(database->GetArtifacts("missing").status().code(), absl::StatusCode::kNotFound);
  EXPECT_EQ(database->Save({}).code(), absl::StatusCode::kInvalidArgument);
}
TEST_F(DatabasePersistenceTest, LegacyMissingBestRetainsHistoricalChampion) {
  PopulationStrategy strategy;
  strategy.replace_cell = [](const auto&, const auto&, const auto&, int) { return true; };

  auto source = ProgramDatabase::Create(Config(), strategy);
  ASSERT_TRUE(source.ok());

  ASSERT_TRUE(source->Add(Candidate("champion", 10)).ok());
  ASSERT_TRUE(source->Add(Candidate("member", 5)).ok());

  ASSERT_TRUE(source->Save(root / "source").ok());

  auto data = Checkpoint::Load(root / "source");
  ASSERT_TRUE(data.ok());

  data->metadata.erase("best_program_id");
  fs::create_directories(root / "legacy" / "programs");
  for (const auto& program : data->programs)
    std::ofstream(root / "legacy" / "programs" / (program.id + ".json")) << program.ToJson()->dump();
  std::ofstream(root / "legacy" / "metadata.json") << data->metadata.dump();

  auto resumed = ProgramDatabase::Create(Config(), strategy);
  ASSERT_TRUE(resumed.ok());

  const auto loaded = resumed->Load(root / "legacy");
  ASSERT_TRUE(loaded.ok()) << loaded;

  EXPECT_EQ(resumed->GetBestProgram()->id, "champion");
  EXPECT_FALSE(resumed->Snapshot()->islands[0].count("champion"));
  EXPECT_EQ(resumed->size(), 2);
}

}  // namespace
}  // namespace ievolve
