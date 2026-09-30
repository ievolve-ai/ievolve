#include "ievolve/prompt/prompt_sampler.h"

#include <fstream>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace ievolve {
namespace {

PromptProgram MakeProgram(std::string id, std::string code, double score) {
  PromptProgram program;
  program.id = std::move(id);
  program.code = std::move(code);
  program.metrics = {{"score", score}};

  return program;
}

TEST(PromptSamplerTest, BuildsDiffAndRewritePromptsWithLiteralCode) {
  PromptSampler sampler;
  PromptRequest request;
  request.current_program = "int main() { return 0; }";
  request.language = "cpp";
  request.program_metrics = {{"score", 0.5}, {"size", 100}};
  request.feature_dimensions = {"size"};

  auto result = sampler.BuildPrompt(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_NE(result->user.find("<<<<<<< SEARCH"), std::string::npos);
  EXPECT_NE(result->user.find(request.current_program), std::string::npos);
  EXPECT_NE(result->user.find("Fitness: 0.5000"), std::string::npos);
  EXPECT_NE(result->user.find("size=100.00"), std::string::npos);

  request.diff_based_evolution = false;
  result = sampler.BuildPrompt(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_NE(result->user.find("Rewrite the program"), std::string::npos);
  EXPECT_EQ(result->user.find("<<<<<<< SEARCH"), std::string::npos);
}

TEST(PromptSamplerTest, TemplatePrecedenceAndExtraValues) {
  PromptConfig config;
  config.system_message = "Literal system";

  PromptSampler sampler(config);
  sampler.template_manager().AddTemplate("custom", "{extra}: {current_program}");
  sampler.template_manager().AddTemplate("explicit", "{fitness_score}");
  sampler.SetTemplates("evaluator_system_message", "custom");

  PromptRequest request;
  request.extra_values = {{"extra", "hello"}};
  request.current_program = "{extra}";

  auto result = sampler.BuildPrompt(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->user, "hello: {extra}");
  EXPECT_EQ(result->system, *sampler.template_manager().GetTemplate("evaluator_system_message"));

  request.template_key = "explicit";
  result = sampler.BuildPrompt(request);
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(result->user, "0.0000");

  sampler.SetTemplates();
  result = sampler.BuildPrompt(request);
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(result->system, "Literal system");

  request.extra_values["fitness_score"] = "clobber";
  EXPECT_FALSE(sampler.BuildPrompt(request).ok());
}

TEST(PromptSamplerTest, ExpandsVariationsInInitializerDeclarationOrder) {
  PromptConfig config;
  config.template_variations = {{"z", {"{a}"}}, {"a", {"finished"}}};

  PromptSampler sampler(config, 42);
  sampler.template_manager().AddTemplate("chained", "{z} {z}");

  PromptRequest request;
  request.template_key = "chained";

  auto result = sampler.BuildPrompt(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->user, "finished finished");
}

TEST(PromptSamplerTest, UsesRecentThreeAttemptsInReverseOrder) {
  PromptSampler sampler;
  PromptRequest request;
  for (int i = 1; i <= 4; ++i) {
    auto program = MakeProgram(std::to_string(i), "", i * 0.1);
    program.changes = "change_" + std::to_string(i);
    program.parent_metrics = {{"score", 0.0}};
    request.previous_programs.push_back(program);
  }
  request.program_metrics = {{"score", 0.5}};

  auto result = sampler.BuildPrompt(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->user.find("change_1"), std::string::npos);
  EXPECT_LT(result->user.find("change_4"), result->user.find("change_3"));
  EXPECT_LT(result->user.find("change_3"), result->user.find("change_2"));
  EXPECT_NE(result->user.find("Improvement in all metrics"), std::string::npos);
  EXPECT_NE(result->user.find("Fitness improved: 0.4000 → 0.5000"), std::string::npos);
}

TEST(PromptSamplerTest, ComparesLargeHistoryCountersWithoutLosingPrecision) {
  PromptSampler sampler;
  PromptRequest request;
  PromptProgram program;
  program.metrics = {{"counter", 9007199254740993LL}};
  program.parent_metrics = {{"counter", 9007199254740992LL}};
  request.previous_programs = {program};

  auto result = sampler.BuildPrompt(request);
  ASSERT_TRUE(result.ok());

  EXPECT_NE(result->user.find("Improvement in all metrics"), std::string::npos);

  // Mixed integer/float comparison must also preserve the integer's low bit.
  request.previous_programs[0].parent_metrics["counter"] = 9007199254740992.0;
  result = sampler.BuildPrompt(request);
  ASSERT_TRUE(result.ok());

  EXPECT_NE(result->user.find("Improvement in all metrics"), std::string::npos);
}

TEST(PromptSamplerTest, OrdersNumericHistoryAcrossSignedUnsignedAndFloatBounds) {
  const std::vector<std::tuple<Metrics, Metrics, std::string>> cases = {
      {-1, std::uint64_t{0}, "Regression in all metrics"},
      {-1, -2, "Improvement in all metrics"},
      {true, false, "Improvement in all metrics"},
      {1, 1.5, "Regression in all metrics"},
      {-1.5, -1, "Regression in all metrics"},
      {std::numeric_limits<std::uint64_t>::max(), 0x1p64, "Regression in all metrics"},
      {std::numeric_limits<std::int64_t>::min(), -0x1p64, "Improvement in all metrics"},
      {std::numeric_limits<std::int64_t>::min(), -0x1p63, "Mixed results"},
      {0, std::numeric_limits<double>::quiet_NaN(), "Mixed results"}};

  PromptSampler sampler;
  for (const auto& test_case : cases) {
    PromptProgram program;
    program.metrics = {{"counter", std::get<0>(test_case)}};
    program.parent_metrics = {{"counter", std::get<1>(test_case)}};
    PromptRequest request;
    request.previous_programs = {program};

    auto result = sampler.BuildPrompt(request);
    ASSERT_TRUE(result.ok());

    EXPECT_NE(result->user.find(std::get<2>(test_case)), std::string::npos);
  }
}

TEST(PromptSamplerTest, DeduplicatesTopAndDiverseButPreservesMissingIds) {
  PromptConfig config;
  config.num_top_programs = 1;
  config.num_diverse_programs = 1;

  PromptSampler sampler(config, 7);
  PromptRequest request;
  request.top_programs = {MakeProgram("A", "program_A", 0.9), MakeProgram("B", "program_B", 0.8)};
  request.inspirations = request.top_programs;

  auto no_id = MakeProgram("", "program_noid", 0.3);
  no_id.id.reset();
  request.inspirations.push_back(no_id);

  auto result = sampler.BuildPrompt(request);
  ASSERT_TRUE(result.ok()) << result.status();

  for (const auto* marker : {"program_A", "program_B", "program_noid"}) {
    const auto first = result->user.find(marker);
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(result->user.find(marker, first + 1), std::string::npos);
  }
  EXPECT_NE(result->user.find("Program D1"), std::string::npos);
}

TEST(PromptSamplerTest, SupportsChangesDescriptionsIncludingMissingOnes) {
  PromptConfig config;
  config.programs_as_changes_description = true;
  config.system_message_changes_description = "  Custom instructions  ";

  PromptSampler sampler(config);
  PromptRequest request;

  auto program = MakeProgram("A", "hidden code", 0.8);
  program.changes_description = "Optimized loops";
  request.top_programs = {program, MakeProgram("B", "also hidden", 0.5)};
  request.current_changes_description = "Current changes  \n";

  auto result = sampler.BuildPrompt(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_NE(result->system.find("Custom instructions"), std::string::npos);
  EXPECT_NE(result->user.find("Optimized loops"), std::string::npos);
  EXPECT_NE(result->user.find("<missing changes_description>"), std::string::npos);
  EXPECT_NE(result->user.find("Current changes\n```"), std::string::npos);
  EXPECT_EQ(result->user.find("hidden"), std::string::npos);

  request.current_changes_description.clear();
  EXPECT_TRUE(sampler.BuildPrompt(request).ok());
}

TEST(PromptSamplerTest, SeedControlsVariationsAndDiverseSelection) {
  PromptConfig config;
  config.num_top_programs = 1;
  config.num_diverse_programs = 2;
  config.template_variations = {{"variation", {"first", "second", "third"}}};

  PromptSampler first(config, 17);
  PromptSampler second(config, 17);
  first.template_manager().AddTemplate("custom", "{variation}\n{evolution_history}");
  second.template_manager().AddTemplate("custom", "{variation}\n{evolution_history}");

  PromptRequest request;
  request.template_key = "custom";
  for (int i = 0; i < 8; ++i) {
    request.top_programs.push_back(MakeProgram(std::to_string(i), "code" + std::to_string(i), 0.5));
  }

  for (int i = 0; i < 5; ++i) {
    auto lhs = first.BuildPrompt(request);
    auto rhs = second.BuildPrompt(request);
    ASSERT_TRUE(lhs.ok()) << lhs.status();
    ASSERT_TRUE(rhs.ok()) << rhs.status();

    EXPECT_EQ(lhs->user, rhs->user);
    EXPECT_EQ(lhs->user.find("{variation}"), std::string::npos);
    EXPECT_NE(lhs->user.find("Program D2"), std::string::npos);
    EXPECT_EQ(lhs->user.find("Program D3"), std::string::npos);
  }
}

TEST(PromptSamplerTest, ReturnsErrorsForInvalidInputs) {
  PromptConfig config;
  config.num_top_programs = -1;
  EXPECT_FALSE(PromptSampler(config).BuildPrompt({}).ok());

  config.num_top_programs = 0;
  config.num_diverse_programs = 0;
  EXPECT_TRUE(PromptSampler(config).BuildPrompt({}).ok());

  PromptRequest request;
  request.template_key = "missing";
  EXPECT_FALSE(PromptSampler().BuildPrompt(request).ok());

  request.template_key.reset();
  request.program_metrics = Metrics::array();
  EXPECT_FALSE(PromptSampler().BuildPrompt(request).ok());
}

TEST(PromptSamplerTest, HonorsArtifactEnableSwitch) {
  PromptRequest request;
  request.program_artifacts = {{"log", "artifact content"}};

  auto result = PromptSampler().BuildPrompt(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_NE(result->user.find("artifact content"), std::string::npos);

  PromptConfig config;
  config.include_artifacts = false;
  result = PromptSampler(config).BuildPrompt(request);
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(result->user.find("artifact content"), std::string::npos);
}

PromptProgram ReadProgram(const Metrics& data) {
  PromptProgram program;
  if (data.contains("id") && !data["id"].is_null()) {
    program.id = data["id"].get<std::string>();
  }

  program.code = data.value("code", "");
  program.metrics = data.value("metrics", Metrics::object());
  if (data.contains("changes_description")) {
    program.changes_description = data["changes_description"].get<std::string>();
  }
  program.key_features = data.value("key_features", std::vector<std::string>{});

  const auto metadata = data.value("metadata", Metrics::object());
  if (metadata.contains("changes")) {
    program.changes = metadata["changes"].get<std::string>();
  }
  program.parent_metrics = metadata.value("parent_metrics", Metrics::object());
  program.diverse = metadata.value("diverse", false);
  program.migrant = metadata.value("migrant", false);
  program.random = metadata.value("random", false);

  return program;
}

TEST(PromptSamplerTest, MatchesFullPythonGoldenPrompts) {
  std::ifstream input(std::string(IEVOLVE_TEST_DATA_DIR) + "/python_golden.json");
  ASSERT_TRUE(input.good());

  const auto cases = Metrics::parse(input, nullptr, false);
  ASSERT_TRUE(cases.is_array());
  ASSERT_FALSE(cases.empty());

  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case["name"].get<std::string>());
    const auto settings = test_case.value("config", Metrics::object());
    PromptConfig config;
    config.num_top_programs = settings.value("num_top_programs", 3);
    config.num_diverse_programs = settings.value("num_diverse_programs", 2);
    config.programs_as_changes_description = settings.value("programs_as_changes_description", false);
    config.max_artifact_bytes = settings.value("max_artifact_bytes", 20 * 1024);
    config.artifact_security_filter = settings.value("artifact_security_filter", true);
    config.suggest_simplification_after_chars = settings.value("suggest_simplification_after_chars", 500);
    if (settings.contains("system_message_changes_description")) {
      config.system_message_changes_description = settings["system_message_changes_description"].get<std::string>();
    }
    if (settings.contains("template_variations")) {
      for (const auto& variation : settings["template_variations"].items())
        config.template_variations.emplace(variation.key(), variation.value().get<std::vector<std::string>>());
    }

    PromptSampler sampler(config, 42);
    const auto templates = test_case.value("templates", Metrics::object());
    for (const auto& item : templates.items()) {
      sampler.template_manager().AddTemplate(item.key(), item.value().get<std::string>());
    }

    const auto fragments = test_case.value("fragments", Metrics::object());
    for (const auto& item : fragments.items()) {
      sampler.template_manager().AddFragment(item.key(), item.value().get<std::string>());
    }

    const auto overrides = test_case.value("set_templates", Metrics::object());
    sampler.SetTemplates(overrides.value("system_template", ""), overrides.value("user_template", ""));

    const auto data = test_case["request"];
    PromptRequest request;
    request.current_program = data.value("current_program", "");
    request.current_changes_description = data.value("current_changes_description", "");
    request.language = data.value("language", "python");
    request.program_metrics = data.value("program_metrics", Metrics::object());
    request.feature_dimensions = data.value("feature_dimensions", std::vector<std::string>{});
    request.diff_based_evolution = data.value("diff_based_evolution", true);
    request.template_key = data.value("template_key", "");

    for (const auto& field : {"previous_programs", "top_programs", "inspirations"}) {
      auto* programs = std::string(field) == "previous_programs" ? &request.previous_programs
                       : std::string(field) == "top_programs"    ? &request.top_programs
                                                                 : &request.inspirations;
      for (const auto& item : data.value(field, Metrics::array())) {
        programs->push_back(ReadProgram(item));
      }
    }

    const auto artifacts = data.value("program_artifacts", Metrics::object());
    for (const auto& item : artifacts.items()) {
      request.program_artifacts.emplace_back(item.key(), item.value().get<std::string>());
    }
    if (data.contains("extra")) request.extra_values["extra"] = data["extra"];

    auto result = sampler.BuildPrompt(request);
    ASSERT_TRUE(result.ok()) << result.status();

    EXPECT_EQ(result->system, test_case["expected"]["system"].get<std::string>());
    EXPECT_EQ(result->user, test_case["expected"]["user"].get<std::string>());
  }
}

}  // namespace
}  // namespace ievolve
