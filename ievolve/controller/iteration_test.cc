#include "ievolve/controller/iteration.h"

#include <atomic>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

#include "gtest/gtest.h"
#include "ievolve/database/program_database.h"

namespace ievolve::controller {
namespace {

class FunctionLLM : public LLMInterface {
 public:
  using GenerateFunction = std::function<absl::StatusOr<LLMResponse>(const LLMRequest&)>;
  explicit FunctionLLM(GenerateFunction generate) : generate_(std::move(generate)) {}
  absl::StatusOr<LLMResponse> Generate(const LLMRequest& request) const override { return generate_(request); }

 private:
  GenerateFunction generate_;
};

std::string Diff(const std::string& old_text, const std::string& new_text) {
  return "<<<<<<< SEARCH\n" + old_text + "\n=======\n" + new_text + "\n>>>>>>> REPLACE";
}
Program MakeProgram(std::string id, std::string code, double score) {
  Program program;
  program.id = std::move(id);
  program.code = std::move(code);
  program.metrics = {{"combined_score", score}};

  return program;
}
IterationInput Input() {
  IterationInput input;
  input.parent = MakeProgram("parent", "score = 1", 0.3);
  input.parent.generation = 4;
  input.parent_island = 1;
  input.target_island = 2;
  input.iteration = 7;
  input.child_id = "child";

  return input;
}

class IterationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_.prompt.use_template_stochasticity = false;

    EvaluatorConfig settings;
    settings.cascade_evaluation = false;
    settings.max_retries = 0;
    settings.parallel_evaluations = 4;

    auto evaluator = evaluator::Evaluator::Create(
        settings, {{}, [this](const auto& path, int) -> absl::StatusOr<evaluator::EvaluationStageResult> {
                     ++evaluations_;
                     std::ifstream stream(path);
                     const std::string code(std::istreambuf_iterator<char>(stream), {});

                     return evaluator::EvaluationStageResult{{{{"combined_score", 0.8}}, {{"evaluated_code", code}}},
                                                             false};
                   }});
    ASSERT_TRUE(evaluator.ok()) << evaluator.status();
    evaluator_ = std::move(*evaluator);
  }
  absl::StatusOr<std::unique_ptr<IterationRunner>> Runner(std::string reply) {
    auto llm = std::make_shared<FunctionLLM>(
        [this, reply = std::move(reply)](const LLMRequest&) -> absl::StatusOr<LLMResponse> {
          ++generations_;
          return LLMResponse{reply, "offline", "fake", LLMUsage{12, 5, 2, 0.125}};
        });

    return IterationRunner::Create(config_, std::move(llm), evaluator_);
  }
  Config config_;
  std::shared_ptr<evaluator::Evaluator> evaluator_;
  std::atomic<int> generations_{0};
  std::atomic<int> evaluations_{0};
};

TEST_F(IterationTest, DiffCreatesEvaluatedChildAndCompleteRecords) {
  const auto input = Input();
  auto runner = Runner(Diff("score = 1", "score = 2"));
  ASSERT_TRUE(runner.ok()) << runner.status();

  auto result = (*runner)->Run(input);
  ASSERT_TRUE(result.ok()) << result.status();

  ASSERT_TRUE(result->child);
  EXPECT_TRUE(result->rejection_reason.empty());

  const auto& child = *result->child;
  EXPECT_EQ(child.id, "child");
  EXPECT_EQ(child.code, "score = 2");
  EXPECT_EQ(child.parent_id, "parent");
  EXPECT_EQ(child.generation, 5);
  EXPECT_EQ(child.iteration_found, 7);
  EXPECT_EQ(child.metrics, Metrics({{"combined_score", 0.8}}));
  EXPECT_EQ(child.metadata["parent_metrics"], input.parent.metrics);
  EXPECT_EQ(child.metadata["island"], 1);
  EXPECT_EQ(child.metadata["token_usage"]["input_tokens"], 12);
  EXPECT_EQ(child.metadata["token_usage"]["cost_usd"], 0.125);

  EXPECT_EQ(std::get<std::string>(result->artifacts.at("evaluated_code")), "score = 2");

  EXPECT_EQ(result->parent_id, "parent");
  EXPECT_EQ(result->iteration, 7);
  EXPECT_EQ(result->target_island, 2);
  EXPECT_EQ(result->response.model, "offline");
  ASSERT_TRUE(result->response.usage);
  EXPECT_EQ(result->response.usage->cached_input_tokens, 2);
  EXPECT_NE(result->prompt.user.find("score = 1"), std::string::npos);
  EXPECT_GE(result->elapsed_seconds, 0);

  EXPECT_EQ(input.parent.code, "score = 1");
  EXPECT_TRUE(child.Validate().ok());
}

TEST_F(IterationTest, RejectedEditsRetainUsageAndNeverEvaluate) {
  for (const auto& reply : {std::string("nothing to edit"), Diff("missing", "x"), Diff("score = 1", "score = 1"),
                            std::string("<<<<<<< SEARCH\nbroken")}) {
    auto runner = Runner(reply);
    ASSERT_TRUE(runner.ok());

    auto result = (*runner)->Run(Input());
    ASSERT_TRUE(result.ok()) << result.status();

    EXPECT_FALSE(result->child);
    EXPECT_FALSE(result->rejection_reason.empty());
    EXPECT_EQ(result->response.text, reply);
    ASSERT_TRUE(result->response.usage);
    EXPECT_EQ(result->response.usage->output_tokens, 5);
    EXPECT_FALSE(result->prompt.user.empty());
    EXPECT_TRUE(result->artifacts.empty());
  }

  EXPECT_EQ(evaluations_.load(), 0);
}

TEST_F(IterationTest, RejectionReasonNamesTheFailedEditStep) {
  struct Case {
    bool diff_based;
    bool description_mode;
    std::string reply;
    std::string reason;
  };
  config_.prompt.initial_changes_description = "initial";

  for (const auto& test : std::vector<Case>{
           {true, false, "nothing to edit", "No valid diffs found in response"},
           {true, false, Diff("missing", "x"), "No SEARCH block matched the parent program"},
           {true, false, Diff("score = 1", "score = 1"), "Diff did not change the parent program"},
           {true, true, Diff("score = 1", "score = 2"), "Changes description was not updated or is empty"},
           {false, false, "score = 1", "Rewrite is identical to the parent program"},
       }) {
    config_.diff_based_evolution = test.diff_based;
    config_.prompt.programs_as_changes_description = test.description_mode;
    auto runner = Runner(test.reply);
    ASSERT_TRUE(runner.ok()) << runner.status();

    auto result = (*runner)->Run(Input());
    ASSERT_TRUE(result.ok()) << result.status();

    EXPECT_FALSE(result->child) << test.reply;
    EXPECT_EQ(result->rejection_reason, test.reason) << test.reply;
  }

  EXPECT_EQ(evaluations_.load(), 0);
}

TEST_F(IterationTest, PartialMatchesAreAllowedWhenCodeChanges) {
  auto runner = Runner(Diff("score = 1", "score = 2") + "\n" + Diff("missing", "ignored"));
  ASSERT_TRUE(runner.ok());

  auto result = (*runner)->Run(Input());
  ASSERT_TRUE(result.ok());

  ASSERT_TRUE(result->child);
  EXPECT_EQ(result->child->code, "score = 2");
  EXPECT_EQ(evaluations_.load(), 1);
}

TEST_F(IterationTest, FullRewriteUsesConfiguredLanguage) {
  config_.diff_based_evolution = false;
  config_.language = "cpp";
  auto runner = Runner("```cpp\nint answer() { return 42; }\n```");
  ASSERT_TRUE(runner.ok());

  auto result = (*runner)->Run(Input());
  ASSERT_TRUE(result.ok());

  ASSERT_TRUE(result->child);
  EXPECT_EQ(result->child->code, "int answer() { return 42; }");
  EXPECT_EQ(result->child->language, "cpp");
  EXPECT_EQ(result->child->metadata["changes"], "Full rewrite");
  EXPECT_TRUE(result->child->changes_description.empty());
}

TEST_F(IterationTest, RejectsEmptyWhitespaceAndUnchangedRewrites) {
  config_.diff_based_evolution = false;

  for (const auto& reply : {std::string(""), std::string(" \n\t\u3000"), std::string("score = 1")}) {
    auto runner = Runner(reply);
    ASSERT_TRUE(runner.ok());

    auto result = (*runner)->Run(Input());
    ASSERT_TRUE(result.ok());

    EXPECT_FALSE(result->child);
    EXPECT_FALSE(result->rejection_reason.empty());
  }

  EXPECT_EQ(evaluations_.load(), 0);
}

TEST_F(IterationTest, ChangesDescriptionModeUpdatesBothTargets) {
  config_.prompt.programs_as_changes_description = true;
  auto input = Input();
  input.parent.changes_description = "original method";
  auto runner = Runner(Diff("score = 1", "score = 2") + "\n" + Diff("original method", "improved method"));
  ASSERT_TRUE(runner.ok());

  auto result = (*runner)->Run(input);
  ASSERT_TRUE(result.ok()) << result.status();

  ASSERT_TRUE(result->child) << result->rejection_reason;
  EXPECT_EQ(result->child->code, "score = 2");
  EXPECT_EQ(result->child->changes_description, "improved method");
  EXPECT_NE(result->prompt.user.find("original method"), std::string::npos);
}

TEST_F(IterationTest, DescriptionOnlyEditUsesInitialFallback) {
  config_.prompt.programs_as_changes_description = true;
  config_.prompt.initial_changes_description = "initial description";
  auto runner = Runner(Diff("initial description", "a better description"));
  ASSERT_TRUE(runner.ok());

  auto result = (*runner)->Run(Input());
  ASSERT_TRUE(result.ok());

  ASSERT_TRUE(result->child) << result->rejection_reason;
  EXPECT_EQ(result->child->code, "score = 1");
  EXPECT_EQ(result->child->changes_description, "a better description");
}

TEST_F(IterationTest, DescriptionMustChangeAndRemainNonempty) {
  config_.prompt.programs_as_changes_description = true;
  config_.prompt.initial_changes_description = "initial";

  for (const auto& reply :
       {Diff("score = 1", "score = 2"), Diff("initial", " \u3000"), Diff("initial", "\u3000initial\u3000")}) {
    auto runner = Runner(reply);
    ASSERT_TRUE(runner.ok());

    auto result = (*runner)->Run(Input());
    ASSERT_TRUE(result.ok());

    EXPECT_FALSE(result->child);
    EXPECT_FALSE(result->rejection_reason.empty());
  }

  EXPECT_EQ(evaluations_.load(), 0);
}

TEST_F(IterationTest, RestoresOutsideRegionsAndRejectsOutsideOnlyEdits) {
  config_.enforce_evolve_blocks = true;
  config_.diff_based_evolution = false;
  auto input = Input();
  input.parent.code = "fixed = 1\n# EVOLVE-BLOCK-START\nscore = 1\n# EVOLVE-BLOCK-END\n";

  for (const bool inside : {false, true}) {
    auto runner = Runner("fixed = 2\n# EVOLVE-BLOCK-START\nscore = " + std::string(inside ? "2" : "1") +
                         "\n# EVOLVE-BLOCK-END\n");
    ASSERT_TRUE(runner.ok());

    auto result = (*runner)->Run(input);
    ASSERT_TRUE(result.ok());

    if (inside) {
      ASSERT_TRUE(result->child);
      EXPECT_EQ(result->child->code, "fixed = 1\n# EVOLVE-BLOCK-START\nscore = 2\n# EVOLVE-BLOCK-END\n");
    } else {
      EXPECT_FALSE(result->child);
    }
  }

  EXPECT_EQ(evaluations_.load(), 1);
}

TEST_F(IterationTest, CodeLimitCountsUnicodeCodePointsAfterRestoration) {
  config_.diff_based_evolution = false;
  config_.max_code_length = 2;
  auto runner = Runner("λ中");
  ASSERT_TRUE(runner.ok());

  auto result = (*runner)->Run(Input());
  ASSERT_TRUE(result.ok());

  ASSERT_TRUE(result->child) << result->rejection_reason;

  runner = Runner("λ中🙂");
  ASSERT_TRUE(runner.ok());

  result = (*runner)->Run(Input());
  ASSERT_TRUE(result.ok());

  EXPECT_FALSE(result->child);
  EXPECT_EQ(evaluations_.load(), 1);
}

TEST_F(IterationTest, InvalidUtf8ModelCodeNeverExecutes) {
  config_.diff_based_evolution = false;
  auto runner = Runner(std::string(1, '\xff'));
  ASSERT_TRUE(runner.ok());

  auto result = (*runner)->Run(Input());
  ASSERT_TRUE(result.ok());

  EXPECT_FALSE(result->child);
  EXPECT_EQ(evaluations_.load(), 0);
}

TEST_F(IterationTest, InvalidUtf8DescriptionIsRejectedBeforeEvaluation) {
  config_.prompt.programs_as_changes_description = true;
  config_.prompt.initial_changes_description = "initial";
  config_.diff_pattern = "(initial):(.{1})";
  auto runner = Runner("initial:é");
  ASSERT_TRUE(runner.ok());

  auto result = (*runner)->Run(Input());
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_FALSE(result->child);
  EXPECT_FALSE(result->rejection_reason.empty());
  EXPECT_EQ(result->response.text, "initial:é");
  EXPECT_EQ(evaluations_.load(), 0);
}

TEST_F(IterationTest, InvalidUtf8SummaryIsRejectedBeforeEvaluation) {
  config_.diff_pattern = "(.{1})=(.{1})";
  auto runner = Runner("x=y é=q");
  ASSERT_TRUE(runner.ok());

  auto input = Input();
  input.parent.code = "x";

  auto result = (*runner)->Run(input);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_FALSE(result->child);
  EXPECT_FALSE(result->rejection_reason.empty());
  EXPECT_EQ(evaluations_.load(), 0);
}

TEST_F(IterationTest, InvalidInputIsRejectedBeforeModelCalls) {
  auto runner = Runner(Diff("score = 1", "score = 2"));
  ASSERT_TRUE(runner.ok());

  std::vector<IterationInput> invalid(6, Input());
  invalid[0].child_id.clear();
  invalid[1].child_id = "parent";
  invalid[2].iteration = -1;
  invalid[3].parent.generation = std::numeric_limits<std::int64_t>::max();
  invalid[4].parent_island = config_.database.num_islands;
  invalid[5].target_island = -1;

  for (const auto& input : invalid) EXPECT_FALSE((*runner)->Run(input).ok());

  auto input = Input();
  input.island_programs.push_back(MakeProgram("child", "context", 0.1));
  EXPECT_FALSE((*runner)->Run(input).ok());

  input = Input();
  input.inspirations.push_back(MakeProgram("bad", "context", 0.1));
  input.inspirations.back().metadata["key_features"] = 3;
  EXPECT_FALSE((*runner)->Run(input).ok());

  EXPECT_EQ(generations_.load(), 0);
  EXPECT_EQ(evaluations_.load(), 0);
}

TEST_F(IterationTest, RejectsInvalidSettingsAndNullDependencies) {
  auto llm = std::make_shared<FunctionLLM>([](const auto&) { return LLMResponse{}; });

  EXPECT_FALSE(IterationRunner::Create(config_, nullptr, evaluator_).ok());
  EXPECT_FALSE(IterationRunner::Create(config_, llm, nullptr).ok());

  config_.max_code_length = -1;
  EXPECT_FALSE(IterationRunner::Create(config_, llm, evaluator_).ok());

  config_.max_code_length = 10;
  config_.prompt.programs_as_changes_description = true;
  config_.diff_based_evolution = false;
  EXPECT_FALSE(IterationRunner::Create(config_, llm, evaluator_).ok());

  config_.diff_based_evolution = true;
  config_.diff_pattern = "no captures";
  EXPECT_FALSE(IterationRunner::Create(config_, llm, evaluator_).ok());
}

TEST_F(IterationTest, ContextRankingAndArtifactRenderingReachPrompt) {
  config_.prompt.num_top_programs = 1;
  config_.prompt.num_diverse_programs = 0;

  auto input = Input();
  input.island_programs = {MakeProgram("low", "LOW-CONTEXT", 0.1), MakeProgram("high", "HIGH-CONTEXT", 0.9)};
  input.inspirations = {MakeProgram("inspiration", "INSPIRATION", 0.7)};
  input.parent_artifacts = {{"report", std::string("artifact text")}, {"binary", ArtifactBytes{'O', 'K', 255}}};

  auto runner = Runner(Diff("score = 1", "score = 2"));
  ASSERT_TRUE(runner.ok());

  auto result = (*runner)->Run(input);
  ASSERT_TRUE(result.ok());

  ASSERT_TRUE(result->child);
  EXPECT_NE(result->prompt.user.find("HIGH-CONTEXT"), std::string::npos);
  EXPECT_EQ(result->prompt.user.find("LOW-CONTEXT"), std::string::npos);
  EXPECT_NE(result->prompt.user.find("INSPIRATION"), std::string::npos);
  EXPECT_NE(result->prompt.user.find("artifact text"), std::string::npos);
  EXPECT_NE(result->prompt.user.find("OK�"), std::string::npos);
}

TEST_F(IterationTest, LargeIntegerContextScoresKeepExactOrder) {
  config_.prompt.num_top_programs = 1;
  config_.prompt.num_diverse_programs = 0;

  auto input = Input();
  input.island_programs = {MakeProgram("low", "LOW-CONTEXT", 0), MakeProgram("high", "HIGH-CONTEXT", 0)};
  input.island_programs[0].metrics["combined_score"] = UINT64_C(9007199254740992);
  input.island_programs[1].metrics["combined_score"] = UINT64_C(9007199254740993);

  auto runner = Runner(Diff("score = 1", "score = 2"));
  ASSERT_TRUE(runner.ok());

  auto result = (*runner)->Run(input);
  ASSERT_TRUE(result.ok());

  EXPECT_NE(result->prompt.user.find("HIGH-CONTEXT"), std::string::npos);
  EXPECT_EQ(result->prompt.user.find("LOW-CONTEXT"), std::string::npos);
}

TEST_F(IterationTest, DependencyErrorsAndExceptionsReturnStatuses) {
  auto llm = std::make_shared<FunctionLLM>(
      [](const auto&) -> absl::StatusOr<LLMResponse> { return absl::UnavailableError("offline"); });
  auto runner = IterationRunner::Create(config_, llm, evaluator_);
  ASSERT_TRUE(runner.ok());

  EXPECT_EQ((*runner)->Run(Input()).status().code(), absl::StatusCode::kUnavailable);

  llm = std::make_shared<FunctionLLM>(
      [](const auto&) -> absl::StatusOr<LLMResponse> { throw std::runtime_error("private model input"); });
  runner = IterationRunner::Create(config_, llm, evaluator_);
  ASSERT_TRUE(runner.ok());

  const auto failed = (*runner)->Run(Input());
  EXPECT_EQ(failed.status().code(), absl::StatusCode::kInternal);
  EXPECT_EQ(std::string(failed.status().message()).find("private model input"), std::string::npos);
  EXPECT_EQ(evaluations_.load(), 0);
}

TEST_F(IterationTest, InvalidUsageFailsBeforeEvaluation) {
  auto llm = std::make_shared<FunctionLLM>([](const auto&) {
    return LLMResponse{Diff("score = 1", "score = 2"), "offline", "fake", LLMUsage{-1, 0, 0, 0.0}};
  });
  auto runner = IterationRunner::Create(config_, llm, evaluator_);
  ASSERT_TRUE(runner.ok());

  EXPECT_EQ((*runner)->Run(Input()).status().code(), absl::StatusCode::kDataLoss);
  EXPECT_EQ(evaluations_.load(), 0);
}

TEST_F(IterationTest, ConcurrentCallsOwnIndependentResults) {
  auto runner = Runner(Diff("score = 1", "score = 2"));
  ASSERT_TRUE(runner.ok());

  std::vector<std::future<absl::StatusOr<IterationResult>>> calls;
  for (int i = 0; i < 8; ++i)
    calls.push_back(std::async(std::launch::async, [&, i] {
      auto input = Input();
      input.child_id = "child-" + std::to_string(i);
      input.iteration = i;
      return (*runner)->Run(input);
    }));

  for (int i = 0; i < 8; ++i) {
    auto result = calls[i].get();
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->child);
    EXPECT_EQ(result->child->id, "child-" + std::to_string(i));
    EXPECT_EQ(result->child->iteration_found, i);
    EXPECT_EQ(result->response.usage->input_tokens, 12);
    EXPECT_EQ(std::get<std::string>(result->artifacts.at("evaluated_code")), "score = 2");
  }

  EXPECT_EQ(evaluations_.load(), 8);
}

TEST_F(IterationTest, MatchesPythonCandidateTransformationFixtures) {
  std::ifstream stream(std::filesystem::path(IEVOLVE_CONTROLLER_TEST_DATA_DIR) / "iteration.json");
  ASSERT_TRUE(stream);
  const auto fixtures = Metrics::parse(stream);

  for (const auto& item : fixtures.at("cases")) {
    SCOPED_TRACE(item.at("name").get<std::string>());
    config_ = Config{};
    config_.prompt.use_template_stochasticity = false;
    const auto& overrides = item.at("config");
    config_.diff_based_evolution = overrides.value("diff_based_evolution", true);
    config_.max_code_length = overrides.value("max_code_length", 10000);
    config_.enforce_evolve_blocks = overrides.value("enforce_evolve_blocks", false);
    config_.diff_pattern = overrides.value("diff_pattern", config_.diff_pattern);
    config_.prompt.programs_as_changes_description = overrides.value("programs_as_changes_description", false);
    config_.prompt.initial_changes_description = overrides.value("initial_changes_description", "");
    config_.prompt.diff_summary_max_line_len = overrides.value("diff_summary_max_line_len", 100);
    config_.prompt.diff_summary_max_lines = overrides.value("diff_summary_max_lines", 30);

    auto input = Input();
    input.parent.code = item.at("parent_code");
    input.parent.changes_description = item.at("parent_changes_description");
    auto runner = Runner(item.at("response").get<std::string>());
    ASSERT_TRUE(runner.ok()) << runner.status();

    const auto evaluations_before = evaluations_.load();
    auto result = (*runner)->Run(input);
    ASSERT_TRUE(result.ok()) << result.status();

    const auto& expected = item.at("expected");
    ASSERT_EQ(result->child.has_value(), expected.at("accepted").get<bool>()) << result->rejection_reason;
    if (result->child) {
      EXPECT_EQ(result->child->code, expected.at("code"));
      EXPECT_EQ(result->child->changes_description, expected.at("changes_description"));
      EXPECT_EQ(result->child->metadata["changes"], expected.at("changes_summary"));
      EXPECT_EQ(evaluations_.load(), evaluations_before + 1);
    } else {
      EXPECT_FALSE(result->rejection_reason.empty());
      EXPECT_EQ(evaluations_.load(), evaluations_before);
    }
  }
}

TEST_F(IterationTest, RunsEnsembleAndPythonEvaluatorBeforeExplicitDatabaseWrite) {
  auto options = evaluator::EvaluatorOptions{};
  options.python.python_executable = IEVOLVE_PYTHON_EXECUTABLE;
  auto evaluation = evaluator::Evaluator::Create(
      config_.evaluator, std::filesystem::path(IEVOLVE_CONTROLLER_TEST_DATA_DIR) / "evaluate_score.py", options);
  ASSERT_TRUE(evaluation.ok()) << evaluation.status();
  std::shared_ptr<evaluator::Evaluator> evaluator = std::move(*evaluation);

  LLMModelConfig model;
  model.name = "offline";
  LLMRequest sent;
  auto ensemble =
      LLMEnsemble::Create({model}, [&](const LLMModelConfig&) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
        return std::make_shared<FunctionLLM>([&](const LLMRequest& request) {
          sent = request;
          return LLMResponse{Diff("score = 1", "score = 2"), "offline", "fake", std::nullopt};
        });
      });
  ASSERT_TRUE(ensemble.ok()) << ensemble.status();

  auto runner = IterationRunner::Create(config_, std::move(*ensemble), evaluator);
  ASSERT_TRUE(runner.ok()) << runner.status();

  ProgramDatabase database;
  auto input = Input();
  ASSERT_TRUE(database.Add(input.parent).ok());

  auto result = (*runner)->Run(input);
  ASSERT_TRUE(result.ok()) << result.status();

  ASSERT_TRUE(result->child) << result->rejection_reason;
  EXPECT_EQ(result->child->metrics["combined_score"], 2);
  EXPECT_EQ(std::get<std::string>(result->artifacts.at("source")), "score = 2");
  EXPECT_EQ(std::get<ArtifactBytes>(result->artifacts.at("binary")), (ArtifactBytes{0, 255}));
  EXPECT_TRUE(result->child->metadata["token_usage"].is_null());

  EXPECT_EQ(sent.system_message, result->prompt.system);
  ASSERT_EQ(sent.messages.size(), 1);
  EXPECT_EQ(sent.messages.front().role, "user");
  EXPECT_EQ(sent.messages.front().content, result->prompt.user);

  EXPECT_EQ(database.size(), 1);
  EXPECT_EQ(database.Get("child").status().code(), absl::StatusCode::kNotFound);

  ASSERT_TRUE(database.Add(*result->child).ok());
  ASSERT_TRUE(database.StoreArtifacts("child", result->artifacts).ok());
  ASSERT_TRUE(database
                  .LogPrompt("child", "diff_user", {{"system", result->prompt.system}, {"user", result->prompt.user}},
                             {result->response.text})
                  .ok());

  auto stored = database.Get("child");
  ASSERT_TRUE(stored.ok());
  EXPECT_TRUE(stored->prompts.has_value());

  auto artifacts = database.GetArtifacts("child");
  ASSERT_TRUE(artifacts.ok());
  EXPECT_EQ(*artifacts, result->artifacts);

  auto best = database.GetBestProgram();
  ASSERT_TRUE(best.ok());
  EXPECT_EQ(best->id, "child");
}

TEST_F(IterationTest, EvaluationFailurePropagatesAndNextRunCanProceed) {
  std::atomic<int> calls{0};
  auto evaluator =
      evaluator::Evaluator::Create({}, {{}, [&](const auto&, int) -> absl::StatusOr<evaluator::EvaluationStageResult> {
                                          if (++calls == 1) return absl::DataLossError("invalid evaluator output");
                                          return evaluator::EvaluationStageResult{{{{"combined_score", 1}}, {}}, false};
                                        }});
  ASSERT_TRUE(evaluator.ok());
  evaluator_ = std::move(*evaluator);
  auto runner = Runner(Diff("score = 1", "score = 2"));
  ASSERT_TRUE(runner.ok());

  EXPECT_EQ((*runner)->Run(Input()).status().code(), absl::StatusCode::kDataLoss);

  auto result = (*runner)->Run(Input());
  ASSERT_TRUE(result.ok());

  ASSERT_TRUE(result->child);
  EXPECT_EQ(result->child->metrics["combined_score"], 1);
}

TEST_F(IterationTest, ContextTiesStayStableAndFallbackIgnoresFlags) {
  config_.prompt.num_top_programs = 1;
  config_.prompt.num_diverse_programs = 0;

  auto input = Input();
  auto first = MakeProgram("first", "FIRST-CONTEXT", 0);
  first.metrics = {{"accuracy", 0.5}, {"timeout", true}, {"note", "ok"}};
  auto second = MakeProgram("second", "SECOND-CONTEXT", 0.5);
  input.island_programs = {first, second};

  auto runner = Runner(Diff("score = 1", "score = 2"));
  ASSERT_TRUE(runner.ok());

  auto result = (*runner)->Run(input);
  ASSERT_TRUE(result.ok());

  EXPECT_NE(result->prompt.user.find("FIRST-CONTEXT"), std::string::npos);
  EXPECT_EQ(result->prompt.user.find("SECOND-CONTEXT"), std::string::npos);

  input.island_programs[0].metrics["accuracy"] = 0.4;
  result = (*runner)->Run(input);
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(result->prompt.user.find("FIRST-CONTEXT"), std::string::npos);
  EXPECT_NE(result->prompt.user.find("SECOND-CONTEXT"), std::string::npos);
}

}  // namespace
}  // namespace ievolve::controller
