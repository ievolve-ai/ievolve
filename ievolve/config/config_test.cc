#include "ievolve/config/config.h"

#include <chrono>
#include <fstream>
#include <limits>
#include <utility>

#include "gtest/gtest.h"
#include "ievolve/prompt/prompt_sampler.h"

namespace ievolve {
namespace {
using Json = nlohmann::json;

// Python DatabaseConfig fields the port drops on purpose: embedding novelty;
// in_memory and diversity_metric, which Python never reads; db_path, since the
// database never persists itself (the controller checkpoints it); and
// max_snapshot_artifacts, which only serves parallel workers.
constexpr const char* kRemovedDatabaseFields[] = {
    "db_path",     "in_memory",       "diversity_metric",   "max_snapshot_artifacts",
    "novelty_llm", "embedding_model", "embedding_api_base", "similarity_threshold"};

// Python EvaluatorConfig fields the port drops: resource limits and distributed
// execution were never implemented, and parallel_evaluations had no effect on
// the serial controller.
constexpr const char* kRemovedEvaluatorFields[] = {"memory_limit_mb", "cpu_limit", "parallel_evaluations",
                                                   "distributed"};

std::vector<std::string> VariationKeys(const PromptConfig& config) {
  std::vector<std::string> keys;
  for (const auto& variation : config.template_variations) keys.push_back(variation.first);

  return keys;
}

absl::StatusOr<Prompt> VariationPrompt(const PromptConfig& config) {
  PromptSampler sampler(config, 42);
  sampler.template_manager().AddTemplate("chained", "{z}");

  PromptRequest request;
  request.template_key = "chained";

  return sampler.BuildPrompt(request);
}

TEST(ConfigTest, DefaultsMatchReference) {
  auto config = Config::FromJson(Json::object());
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ(config->max_iterations, 10000);
  EXPECT_EQ(config->llm.temperature, 0.7);
  EXPECT_EQ(config->database.feature_dimensions, (std::vector<std::string>{"complexity", "diversity"}));
  EXPECT_EQ(config->prompt.num_top_programs, 3);
  EXPECT_FALSE(config->prompt.template_dir.has_value());
  EXPECT_FALSE(config->early_stopping_patience.has_value());
}

TEST(ConfigTest, RunSettingsSurviveJsonAndYamlRoundTrips) {
  const Json run = {{"initial_program", "seed.py"},
                    {"evaluation_file", "evaluate.py"},
                    {"output_directory", "results"},
                    {"checkpoint", "previous/checkpoint_2"},
                    {"target_score", 0.9},
                    {"python_executable", "./bin/python3"},
                    {"codex_executable", "codex-custom"},
                    {"claude_executable", "./bin/claude"}};
  auto config = Config::FromJson({{"run", run}});
  ASSERT_TRUE(config.ok()) << config.status();
  EXPECT_EQ(config->run.initial_program, "seed.py");
  EXPECT_EQ(config->run.target_score, 0.9);

  const auto serialized = config->ToJson();
  ASSERT_TRUE(serialized.contains("run"));
  EXPECT_EQ(Json(serialized["run"]), run);

  auto yaml = config->ToYaml();
  ASSERT_TRUE(yaml.ok()) << yaml.status();

  auto restored = Config::ParseYaml(*yaml);
  ASSERT_TRUE(restored.ok()) << restored.status();
  EXPECT_EQ(restored->ToJson(), serialized);
}

TEST(ConfigTest, RejectsInvalidRunSettingsWithFieldPaths) {
  for (const auto& run : std::vector<Json>{nullptr,
                                           Json::array(),
                                           {{"initial_program", ""}},
                                           {{"evaluation_file", 42}},
                                           {{"output_directory", false}},
                                           {{"checkpoint", ""}},
                                           {{"python_executable", ""}},
                                           {{"codex_executable", nullptr}},
                                           {{"claude_executable", std::string("nul\0path", 8)}},
                                           {{"target_score", "nan"}},
                                           {{"target_score", true}},
                                           {{"target_score", std::numeric_limits<double>::infinity()}},
                                           {{"iterations", 2}}}) {
    SCOPED_TRACE(run.dump());
    auto config = Config::FromJson({{"run", run}});
    EXPECT_EQ(config.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_NE(std::string(config.status().message()).find("run"), std::string::npos);
  }
}

TEST(ConfigTest, ValidatesProgrammaticallyAssignedRunSettingsBeforeSaving) {
  Config config;
  config.run.target_score = std::numeric_limits<double>::infinity();
  EXPECT_EQ(config.ToYaml().status().code(), absl::StatusCode::kInvalidArgument);

  config.run.target_score = std::nullopt;
  config.run.initial_program = "";
  EXPECT_EQ(config.ToYaml().status().code(), absl::StatusCode::kInvalidArgument);

  config.run.initial_program = std::nullopt;
  EXPECT_TRUE(config.ToYaml().ok());
}

TEST(ConfigTest, InheritsSharedParametersWithoutClobberingExplicitValues) {
  auto config = Config::FromJson(
      {{"llm",
        {{"provider", "claude_code"},
         {"api_key", "test"},
         {"temperature", 0.9},
         {"manual_mode", true},
         {"models",
          {{{"name", "a"}, {"temperature", 0.0}, {"manual_mode", false}}, {{"name", "b"}, {"provider", "openai"}}}}}}});
  ASSERT_TRUE(config.ok()) << config.status();

  ASSERT_EQ(config->llm.models.size(), 2);
  EXPECT_EQ(config->llm.models[0].temperature, 0.0);
  EXPECT_EQ(config->llm.models[0].manual_mode, false);
  EXPECT_EQ(config->llm.models[0].provider, "claude_code");

  EXPECT_EQ(config->llm.models[1].provider, "openai");
  EXPECT_EQ(config->llm.models[1].temperature, 0.9);
  EXPECT_EQ(config->llm.evaluator_models[1].api_key, "test");
}

TEST(ConfigTest, DistinguishesNullMissingAndExplicitZero) {
  auto config = Config::ParseYaml(
      "llm:\n  temperature: null\n  top_p: null\n"
      "  models: [{name: a, temperature: null}, {name: b, temperature: 0}]\n"
      "random_seed: 7\ndatabase: {random_seed: null}\n"
      "prompt: {suggest_simplification_after_chars: null}\n");
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ(config->llm.temperature, 0.7);
  EXPECT_EQ(config->llm.models[0].temperature, 0.7);
  EXPECT_EQ(config->llm.models[1].temperature, 0.0);
  EXPECT_FALSE(config->llm.top_p);
  EXPECT_EQ(config->database.random_seed, 7);
  EXPECT_FALSE(config->prompt.suggest_simplification_after_chars);
}

TEST(ConfigTest, LegacyModelsRoundTripWithoutDuplication) {
  auto config = Config::ParseYaml("llm: {primary_model: a, secondary_model: b}\n");
  ASSERT_TRUE(config.ok()) << config.status();

  ASSERT_EQ(config->llm.models.size(), 2);
  EXPECT_EQ(config->llm.models[0].weight, 1.0);
  EXPECT_EQ(config->llm.models[1].weight, 0.2);

  auto yaml = config->ToYaml();
  ASSERT_TRUE(yaml.ok()) << yaml.status();

  auto again = Config::ParseYaml(*yaml);
  ASSERT_TRUE(again.ok()) << again.status();
  EXPECT_EQ(again->ToJson(), config->ToJson());

  auto json_again = Config::FromJson(config->ToJson());
  ASSERT_TRUE(json_again.ok()) << json_again.status();
  EXPECT_EQ(json_again->ToJson(), config->ToJson());
}

TEST(ConfigTest, ValidatesTypesAndCrossFieldConstraintsWithFieldPaths) {
  for (const auto& input :
       {Json{{"max_iterations", "12"}}, Json{{"max_iterations", true}}, Json{{"max_iterations", 1.5}},
        Json{{"max_iterations", std::numeric_limits<std::uint64_t>::max()}},
        Json{{"llm", {{"models", Json::object()}}}}, Json{{"database", {{"feature_bins", "10"}}}},
        Json{{"prompt", {{"num_top_programs", -1}}}}, Json{{"llm", {{"init_client", "callback"}}}},
        Json{{"diff_based_evolution", false}, {"prompt", {{"programs_as_changes_description", true}}}},
        Json{{"diff_pattern", "["}}}) {
    auto result = Config::FromJson(input);
    EXPECT_FALSE(result.ok()) << input.dump();
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  }

  auto bad = Config::FromJson({{"llm", {{"models", {{{"timeout", "secret-value"}}}}}}});
  EXPECT_NE(std::string(bad.status().message()).find("llm.models[0].timeout"), std::string::npos);
  EXPECT_EQ(std::string(bad.status().message()).find("secret-value"), std::string::npos);
}

// These settings are intentionally not part of the port: they are accepted
// like any unknown database field and dropped from the output.
TEST(ConfigTest, IgnoresRemovedDatabaseSettings) {
  auto config = Config::FromJson({{"database",
                                   {{"db_path", "checkpoints"},
                                    {"in_memory", false},
                                    {"diversity_metric", "feature_based"},
                                    {"max_snapshot_artifacts", 5},
                                    {"embedding_model", "text-embedding-3-small"},
                                    {"embedding_api_base", "http://localhost"},
                                    {"similarity_threshold", 0.8},
                                    {"novelty_llm", "object"}}}});
  ASSERT_TRUE(config.ok()) << config.status();

  const auto database = Json(config->ToJson()).at("database");
  for (const auto* key : kRemovedDatabaseFields) EXPECT_FALSE(database.contains(key)) << key;
}

TEST(ConfigTest, IgnoresRemovedEvaluatorFields) {
  auto config = Config::FromJson(
      {{"evaluator",
        {{"memory_limit_mb", 100}, {"cpu_limit", 1.5}, {"parallel_evaluations", 4}, {"distributed", true}}}});
  ASSERT_TRUE(config.ok()) << config.status();

  const auto evaluator = Json(config->ToJson()).at("evaluator");
  for (const auto* key : kRemovedEvaluatorFields) EXPECT_FALSE(evaluator.contains(key)) << key;
}

TEST(ConfigTest, SupportsFeatureBinVariantsAndUnknownReferenceFields) {
  auto config = Config::FromJson({{"unknown", 1}, {"database", {{"feature_bins", {{"quality", 12}, {"speed", 8}}}}}});
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ((std::get<std::map<std::string, int>>(config->database.feature_bins)).at("speed"), 8);
  EXPECT_FALSE(config->ToJson().contains("unknown"));
}

TEST(ConfigTest, IgnoresRemovedBudgetWhenLoadingAndSaving) {
  auto config = Config::ParseYaml(
      "max_iterations: 7\nllm:\n  max_budget_usd: 1\n"
      "  models: [{provider: codex, max_budget_usd: 2}]\n"
      "  evaluator_models: [{provider: claude_code, max_budget_usd: 3}]\n");
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ(config->max_iterations, 7);

  const auto serialized = config->ToJson();
  EXPECT_FALSE(serialized["llm"].contains("max_budget_usd"));
  EXPECT_FALSE(serialized["llm"]["models"][0].contains("max_budget_usd"));
  EXPECT_FALSE(serialized["llm"]["evaluator_models"][0].contains("max_budget_usd"));

  const auto yaml = config->ToYaml();
  ASSERT_TRUE(yaml.ok()) << yaml.status();
  EXPECT_EQ(yaml->find("max_budget_usd"), std::string::npos);

  EXPECT_EQ(config->UpdateModelParams({{"max_budget_usd", 4}}).code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(config->ToJson(), serialized);
}

TEST(ConfigTest, ResolvesEnvironmentReferencesWithoutEmbeddedInterpolation) {
  // IEVOLVE_CONFIG_TEST_KEY is supplied only to this test process by CTest.
  auto config = Config::ParseYaml(
      "llm:\n  api_key: ${IEVOLVE_CONFIG_TEST_KEY}\n"
      "  models: [{name: a}, {name: b, api_key: "
      "'prefix-${IEVOLVE_CONFIG_TEST_KEY}'}]\n");
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ(config->llm.models[0].api_key, "fixture-key");
  EXPECT_EQ(config->llm.models[1].api_key, "prefix-${IEVOLVE_CONFIG_TEST_KEY}");

  auto absent = Config::ParseYaml("llm: {api_key: '${IEVOLVE_MISSING_CONFIG_KEY_98173}'}");
  EXPECT_FALSE(absent.ok());
}

TEST(ConfigTest, UpdatesAndRebuildsModelParameters) {
  auto config = Config::ParseYaml("llm: {provider: claude_code, primary_model: a}");
  ASSERT_TRUE(config.ok()) << config.status();

  ASSERT_TRUE(config->UpdateModelParams({{"temperature", 0.1}}).ok());
  EXPECT_EQ(config->llm.models[0].temperature, 0.7);

  ASSERT_TRUE(config->UpdateModelParams({{"temperature", 0.1}}, true).ok());
  EXPECT_EQ(config->llm.evaluator_models[0].temperature, 0.1);

  const auto before = config->ToJson();
  EXPECT_FALSE(config->UpdateModelParams({{"temperature", "bad"}}, true).ok());
  EXPECT_EQ(config->ToJson(), before);

  config->llm.primary_model = "c";
  ASSERT_TRUE(config->RebuildModels().ok());
  ASSERT_EQ(config->llm.models.size(), 1);
  EXPECT_EQ(config->llm.models[0].name, "c");
  EXPECT_EQ(config->llm.models[0].provider, "claude_code");
}

TEST(ConfigTest, RejectsInvalidYamlRootsDuplicatesAndRecursiveAliases) {
  for (const std::string yaml : {"", "null", "[]", "a: [", "max_iterations: 1\nmax_iterations: 2",
                                 "llm: &loop {models: [*loop]}", "{}\n---\n{}", "max_iterations: '12'"}) {
    EXPECT_EQ(Config::ParseYaml(yaml).status().code(), absl::StatusCode::kInvalidArgument) << yaml;
  }
}

TEST(ConfigTest, PreservesQuotedScalarsAndMultilineTemplates) {
  auto config = Config::ParseYaml(
      "log_level: 'true'\nllm: {api_key: '00123'}\n"
      "prompt:\n  system_message: |\n    Be precise.\n    Preserve "
      "{braces}.\n");
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ(config->log_level, "true");
  EXPECT_EQ(config->llm.api_key, "00123");

  auto yaml = config->ToYaml();
  ASSERT_TRUE(yaml.ok());

  auto restored = Config::ParseYaml(*yaml);
  ASSERT_TRUE(restored.ok()) << restored.status();
  EXPECT_EQ(restored->ToJson(), config->ToJson());
}

TEST(ConfigTest, YamlMergeRespectsExplicitAndFirstMappingPrecedence) {
  auto config = Config::ParseYaml(R"yaml(
defaults: &defaults {temperature: 0.2, timeout: 11}
llm:
  <<: [*defaults, {temperature: 0.8, max_tokens: 100}]
  timeout: 22
  models: [{name: a}]
)yaml");
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ(config->llm.models[0].temperature, 0.2);
  EXPECT_EQ(config->llm.models[0].timeout, 22);
  EXPECT_EQ(config->llm.models[0].max_tokens, 100);
}

TEST(ConfigTest, PreservesVariationDeclarationOrderThroughRoundTrips) {
  const auto document = nlohmann::ordered_json::parse(R"json({"prompt": {
    "template_variations": {"z": ["{middle}"], "middle": ["{a}"],
                            "a": ["finished"]}}})json");

  auto from_json = Config::FromJson(document);
  auto from_yaml = Config::ParseYaml(
      "prompt:\n  template_variations:\n    z: ['{middle}']\n"
      "    middle: ['{a}']\n    a: [finished]\n");
  ASSERT_TRUE(from_json.ok()) << from_json.status();
  ASSERT_TRUE(from_yaml.ok()) << from_yaml.status();

  for (const auto* original : {&*from_json, &*from_yaml}) {
    auto yaml = original->ToYaml();
    ASSERT_TRUE(yaml.ok()) << yaml.status();

    auto yaml_roundtrip = Config::ParseYaml(*yaml);
    auto json_roundtrip = Config::FromJson(original->ToJson());
    ASSERT_TRUE(yaml_roundtrip.ok()) << yaml_roundtrip.status();
    ASSERT_TRUE(json_roundtrip.ok()) << json_roundtrip.status();

    for (const auto* config : std::vector<const Config*>{original, &*yaml_roundtrip, &*json_roundtrip}) {
      EXPECT_EQ(VariationKeys(config->prompt), (std::vector<std::string>{"z", "middle", "a"}));

      auto prompt = VariationPrompt(config->prompt);
      ASSERT_TRUE(prompt.ok()) << prompt.status();
      EXPECT_EQ(prompt->user, "finished");
    }
  }
}

TEST(ConfigTest, DoesNotRevisitEarlierVariationDeclarations) {
  auto config = Config::ParseYaml("prompt: {template_variations: {a: [finished], z: ['{a}']}}\n");
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ(VariationKeys(config->prompt), (std::vector<std::string>{"a", "z"}));
  EXPECT_EQ(VariationPrompt(config->prompt).status().code(), absl::StatusCode::kNotFound);
}

TEST(ConfigTest, CopiesAndAssignsOrderedVariationsAsIndependentValues) {
  auto parsed = Config::ParseYaml("prompt: {template_variations: {z: ['{a}'], a: [finished]}}\n");
  ASSERT_TRUE(parsed.ok()) << parsed.status();

  Config original = *parsed;
  Config copied = original;
  Config assigned;
  assigned.prompt.template_variations = {{"old", {"discarded"}}};
  assigned = original;

  PromptConfig assigned_prompt;
  assigned_prompt = original.prompt;

  Config moved;
  moved = std::move(assigned);

  original.prompt.template_variations.at("a") = {"changed"};
  for (const auto* prompt_config : {&copied.prompt, &moved.prompt, &assigned_prompt}) {
    EXPECT_EQ(VariationKeys(*prompt_config), (std::vector<std::string>{"z", "a"}));

    auto prompt = VariationPrompt(*prompt_config);
    ASSERT_TRUE(prompt.ok()) << prompt.status();
    EXPECT_EQ(prompt->user, "finished");
  }

  assigned_prompt.template_variations = {{"z", {"{a}"}}, {"a", {"replaced"}}};
  auto replaced = VariationPrompt(assigned_prompt);
  ASSERT_TRUE(replaced.ok()) << replaced.status();
  EXPECT_EQ(replaced->user, "replaced");
}

TEST(ConfigTest, PreservesMergedVariationOrderAndOverridePrecedence) {
  auto config = Config::ParseYaml(R"yaml(
first: &first {z: ['{a}'], a: [first]}
second: &second {z: [wrong], suffix: [last]}
prompt:
  template_variations:
    <<: [*first, *second]
    a: [finished]
    tail: [final]
)yaml");
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ(VariationKeys(config->prompt), (std::vector<std::string>{"z", "suffix", "a", "tail"}));

  auto prompt = VariationPrompt(config->prompt);
  ASSERT_TRUE(prompt.ok()) << prompt.status();
  EXPECT_EQ(prompt->user, "finished");

  EXPECT_FALSE(Config::ParseYaml("prompt: {template_variations: {z: [first], z: [second]}}").ok());
}

TEST(ConfigTest, HonorsExplicitYamlScalarTags) {
  EXPECT_FALSE(Config::ParseYaml("max_iterations: !!float 5").ok());
  EXPECT_FALSE(Config::ParseYaml("llm: {manual_mode: !!int true}").ok());

  auto config = Config::ParseYaml("log_level: !!str 123\nllm: {temperature: !!float 1}");
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ(config->log_level, "123");
  EXPECT_EQ(config->llm.temperature, 1.0);
}

TEST(ConfigTest, AcceptsExplicitYamlNullScalars) {
  for (const std::string value : {"!!null null", "!!null ~", "!!null ''", "!<tag:yaml.org,2002:null> null"}) {
    SCOPED_TRACE(value);
    auto config =
        Config::ParseYaml("language: " + value + "\nllm:\n  api_key: " + value + "\n  temperature: " + value + "\n");
    ASSERT_TRUE(config.ok()) << config.status();

    EXPECT_FALSE(config->language);
    EXPECT_FALSE(config->llm.api_key);
    EXPECT_EQ(config->llm.temperature, 0.7);
  }

  EXPECT_FALSE(Config::ParseYaml("max_iterations: !!null null").ok());
  EXPECT_FALSE(Config::ParseYaml("language: !!null []").ok());
  EXPECT_FALSE(Config::ParseYaml("language: !!null {}").ok());

  const auto quoted = Config::ParseYaml("language: !!str null");
  ASSERT_TRUE(quoted.ok()) << quoted.status();
  EXPECT_EQ(quoted->language, "null");
}

TEST(ConfigTest, SerializationRejectsInvalidProgrammaticValues) {
  Config config;
  config.llm.temperature = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(config.ToYaml().ok());

  config.llm.temperature = 0.7;
  config.diff_based_evolution = false;
  config.prompt.programs_as_changes_description = true;
  EXPECT_FALSE(config.Validate().ok());
  EXPECT_FALSE(config.ToYaml().ok());
}

TEST(ConfigTest, MatchesPyYamlImplicitScalarResolution) {
  for (const std::string text : {"08", "1e3", "_123", "TrUe"}) {
    auto config = Config::ParseYaml("log_level: " + text);
    ASSERT_TRUE(config.ok()) << config.status();
    EXPECT_EQ(config->log_level, text);
  }

  for (const auto& pair :
       std::map<std::string, int>{{"0b1010", 10}, {"1:30", 90}, {"010", 8}, {"0x10", 16}, {"1_000", 1000}}) {
    auto config = Config::ParseYaml("max_iterations: " + pair.first);
    ASSERT_TRUE(config.ok()) << config.status();
    EXPECT_EQ(config->max_iterations, pair.second);
  }

  auto scalar = Config::ParseYaml("convergence_threshold: 1:30.5");
  ASSERT_TRUE(scalar.ok()) << scalar.status();
  EXPECT_EQ(scalar->convergence_threshold, 90.5);
}

TEST(ConfigTest, SerializesScientificNumbersForPythonYamlReaders) {
  auto config = Config::ParseYaml("convergence_threshold: 1.0e-8");
  ASSERT_TRUE(config.ok());

  auto yaml = config->ToYaml();
  ASSERT_TRUE(yaml.ok());

  auto restored = Config::ParseYaml(*yaml);
  ASSERT_TRUE(restored.ok()) << restored.status();
  EXPECT_EQ(restored->convergence_threshold, 1e-8);
}

TEST(ConfigTest, RebuildCanClearDisabledLegacyModels) {
  auto config = Config::ParseYaml("llm: {primary_model: p}");
  ASSERT_TRUE(config.ok());

  config->llm.primary_model.reset();
  config->llm.secondary_model = "secondary";
  config->llm.secondary_model_weight = 0;

  ASSERT_TRUE(config->RebuildModels().ok());

  EXPECT_TRUE(config->llm.models.empty());
  EXPECT_TRUE(config->llm.evaluator_models.empty());
}

TEST(ConfigTest, HonorsStringTagsOnMergeKeysAndRequiresStringMappingKeys) {
  auto config = Config::ParseYaml("prompt: {template_variations: {!!str <<: [test]}}");
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ(config->prompt.template_variations.at("<<"), std::vector<std::string>{"test"});

  EXPECT_FALSE(Config::ParseYaml("database: {feature_bins: {12: 4}}").ok());
  EXPECT_TRUE(Config::ParseYaml("database: {feature_bins: {'12': 4}}").ok());
}

class ConfigFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("ievolve_config_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    std::filesystem::create_directories(dir_ / "templates");
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }
  std::filesystem::path dir_;
};

TEST_F(ConfigFileTest, ResolvesPathsAndBuildsPromptFromYaml) {
  std::ofstream(dir_ / "templates/system_message.txt") << "custom system";
  std::ofstream(dir_ / "config.yaml") << "prompt: {template_dir: templates}\nllm: {models: [{name: a}]}\n";

  auto config = Config::Load(dir_ / "config.yaml");
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ(config->prompt.template_dir, std::filesystem::canonical(dir_ / "templates").string());
  EXPECT_EQ(config->llm.models[0].system_message, "system_message");

  PromptSampler sampler(config->prompt, 42);
  auto prompt = sampler.BuildPrompt({});
  ASSERT_TRUE(prompt.ok()) << prompt.status();
  EXPECT_EQ(prompt->system, "custom system");

  ASSERT_TRUE(config->Save(dir_ / "saved.yaml").ok());

  auto restored = Config::Load(dir_ / "saved.yaml");
  ASSERT_TRUE(restored.ok());
  EXPECT_EQ(restored->ToJson(), config->ToJson());
}

TEST_F(ConfigFileTest, ResolvesRunPathsAndPreservesThemWhenSavedElsewhere) {
  const auto base = std::filesystem::canonical(dir_);
  const auto absolute_claude = (base / "claude").string();
  const Json run = {{"initial_program", "./programs/seed.py"},    {"evaluation_file", "scripts/evaluate.py"},
                    {"checkpoint", "previous/checkpoint_2"},      {"target_score", 0.9},
                    {"python_executable", "./bin tools/python3"}, {"codex_executable", "codex-custom"},
                    {"claude_executable", absolute_claude}};
  std::ofstream(dir_ / "config.yaml") << Json{{"run", run}}.dump();

  nlohmann::ordered_json source;
  auto config = Config::Load(dir_ / "config.yaml", &source);
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ(config->run.initial_program, (base / "programs/seed.py").string());
  EXPECT_EQ(config->run.evaluation_file, (base / "scripts/evaluate.py").string());
  EXPECT_EQ(config->run.output_directory, (base / "ievolve_output").string());
  EXPECT_EQ(config->run.checkpoint, (base / "previous/checkpoint_2").string());
  EXPECT_EQ(config->run.python_executable, (base / "bin tools/python3").string());
  EXPECT_EQ(config->run.codex_executable, "codex-custom");
  EXPECT_EQ(config->run.claude_executable, absolute_claude);
  EXPECT_EQ(Json(source["run"]), run);

  std::filesystem::create_directory(dir_ / "saved");
  ASSERT_TRUE(config->Save(dir_ / "saved/config.yaml").ok());

  auto restored = Config::Load(dir_ / "saved/config.yaml");
  ASSERT_TRUE(restored.ok()) << restored.status();
  EXPECT_EQ(restored->ToJson(), config->ToJson());
}

TEST_F(ConfigFileTest, ReportsMissingFilesAndWriteErrors) {
  EXPECT_EQ(Config::Load(dir_ / "absent.yaml").status().code(), absl::StatusCode::kNotFound);
  EXPECT_FALSE(Config::Load(dir_).ok());

  EXPECT_FALSE(Config{}.Save(dir_).ok());
  EXPECT_FALSE(Config{}.Save(dir_ / "missing/out.yaml").ok());

  EXPECT_TRUE(Config::Load().ok());
}

TEST_F(ConfigFileTest, ReturnsOriginalDocumentAfterSuccessfulLoad) {
  std::ofstream(dir_ / "config.yaml") << R"yaml(
file_suffix: .cc
llm:
  api_key: ${IEVOLVE_CONFIG_TEST_KEY}
  models: [{name: a}]
  evaluator_models: []
prompt:
  template_dir: templates
  system_message: explicit system
  template_variations: {z: ['{a}'], a: [finished]}
unknown: retained
)yaml";

  nlohmann::ordered_json source = {{"sentinel", true}};
  auto config = Config::Load(dir_ / "config.yaml", &source);
  ASSERT_TRUE(config.ok()) << config.status();

  const auto expected = nlohmann::ordered_json::parse(R"json({
    "file_suffix": ".cc",
    "llm": {"api_key": "${IEVOLVE_CONFIG_TEST_KEY}",
            "models": [{"name": "a"}], "evaluator_models": []},
    "prompt": {"template_dir": "templates", "system_message": "explicit system",
               "template_variations": {"z": ["{a}"], "a": ["finished"]}},
    "unknown": "retained"})json");
  EXPECT_EQ(source, expected);
  EXPECT_EQ(config->prompt.template_dir, std::filesystem::canonical(dir_ / "templates").string());
  EXPECT_EQ(config->llm.models[0].api_key, "fixture-key");
  EXPECT_EQ(config->llm.models[0].system_message, "explicit system");
  EXPECT_EQ(config->llm.evaluator_models.size(), 1);
  EXPECT_FALSE(config->ToJson().contains("unknown"));

  auto prompt = VariationPrompt(config->prompt);
  ASSERT_TRUE(prompt.ok()) << prompt.status();
  EXPECT_EQ(prompt->user, "finished");
}

TEST_F(ConfigFileTest, FailedLoadLeavesSourceDocumentUnchanged) {
  const nlohmann::ordered_json original = {{"sentinel", true}};
  auto source = original;

  EXPECT_FALSE(Config::Load(dir_ / "absent.yaml", &source).ok());
  EXPECT_EQ(source, original);

  for (const std::string yaml :
       {"prompt: [", "prompt: {num_top_programs: -1}", "llm: {api_key: '${IEVOLVE_MISSING_CONFIG_KEY_98173}'}"}) {
    std::ofstream(dir_ / "config.yaml") << yaml;
    EXPECT_FALSE(Config::Load(dir_ / "config.yaml", &source).ok());
    EXPECT_EQ(source, original);
  }
}

TEST(ConfigTest, DefaultLoadReturnsAnEmptySourceDocument) {
  nlohmann::ordered_json source = {{"sentinel", true}};
  auto config = Config::Load(std::nullopt, &source);
  ASSERT_TRUE(config.ok()) << config.status();

  EXPECT_EQ(source, nlohmann::ordered_json::object());
  EXPECT_EQ(config->max_iterations, 10000);
}

TEST(ConfigTest, MatchesPythonGoldenConfigurationsExceptCppRunSettingsAndRemovedFields) {
  std::ifstream input(std::string(IEVOLVE_CONFIG_TEST_DATA_DIR) + "/python_golden.json");
  ASSERT_TRUE(input.good());

  const auto fixtures = Json::parse(input);
  ASSERT_GE(fixtures.size(), 10);

  for (const auto& fixture : fixtures) {
    SCOPED_TRACE(fixture.at("name").get<std::string>());
    auto expected = fixture.at("expected");
    for (const auto* key : kRemovedDatabaseFields) ASSERT_EQ(expected.at("database").erase(key), 1u) << key;
    for (const auto* key : kRemovedEvaluatorFields) ASSERT_EQ(expected.at("evaluator").erase(key), 1u) << key;

    auto config = Config::FromJson(fixture.at("input"));
    ASSERT_TRUE(config.ok()) << config.status();
    auto serialized = Json(config->ToJson());
    ASSERT_EQ(serialized.erase("run"), 1u);
    EXPECT_EQ(serialized, expected);

    auto yaml_config = Config::ParseYaml(fixture.at("yaml").get<std::string>());
    ASSERT_TRUE(yaml_config.ok()) << yaml_config.status();
    serialized = Json(yaml_config->ToJson());
    ASSERT_EQ(serialized.erase("run"), 1u);
    EXPECT_EQ(serialized, expected);
  }
}
}  // namespace
}  // namespace ievolve
