#include "ievolve/evaluator/evaluator.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

#include "gtest/gtest.h"

namespace ievolve::evaluator {
namespace {
using namespace std::chrono_literals;

EvaluatorConfig DirectConfig() {
  EvaluatorConfig config;
  return config;
}
EvaluatorOptions FastOptions() {
  EvaluatorOptions options;
  return options;
}
EvaluationOutcome Score(double value) { return {true, {{{"combined_score", value}}, {}}, {}}; }
EvaluationOutcome Failure() { return {false, {Metrics::object(), {{"stderr", "failure"}}}, "wrong_answer"}; }

TEST(EvaluatorTest, SnapshotsArtifactEnvironmentAtCreation) {
  struct RestoreEnvironment {
    std::optional<std::string> original;
    RestoreEnvironment() {
      if (const char* value = std::getenv("ENABLE_ARTIFACTS")) original = value;
    }
    ~RestoreEnvironment() {
      if (original) {
        setenv("ENABLE_ARTIFACTS", original->c_str(), 1);
      } else {
        unsetenv("ENABLE_ARTIFACTS");
      }
    }
  } restore;

  ASSERT_EQ(setenv("ENABLE_ARTIFACTS", "false", 1), 0);
  auto evaluator = Evaluator::Create(DirectConfig(), {[](const auto&) {
                                       auto result = Score(1);
                                       result.result.artifacts["report"] = std::string("present");
                                       return result;
                                     }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  CandidateConfig candidate;
  candidate.run.argv = {"candidate"};
  EvaluatorConfig command_config;
  command_config.run.argv = {"judge"};
  EvaluatorOptions command_options;
  command_options.runner = [](const process::ProcessRequest& request) -> absl::StatusOr<process::ProcessResult> {
    EXPECT_FALSE(Metrics::parse(request.stdin_text).at("enable_artifacts").get<bool>());
    return process::ProcessResult{
        0, R"({"protocol_version":1,"passed":true,"metrics":{"combined_score":1},"artifacts":{"ignored":123}})", ""};
  };
  auto production = Evaluator::Create(candidate, command_config, std::filesystem::current_path(), command_options);
  ASSERT_TRUE(production.ok()) << production.status();

  ASSERT_EQ(setenv("ENABLE_ARTIFACTS", "TrUe", 1), 0);
  auto command_result = (*production)->Evaluate({"print(42)"});
  ASSERT_TRUE(command_result.ok()) << command_result.status();
  EXPECT_TRUE(command_result->result.artifacts.empty());

  auto result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok());

  EXPECT_TRUE(result->result.artifacts.empty());
}

TEST(EvaluatorTest, DirectPreservesValuesAndCleansCandidate) {
  std::filesystem::path candidate;
  auto evaluator = Evaluator::Create(
      DirectConfig(), {[&](const auto& path) {
        candidate = path;

        std::ifstream stream(path);
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(stream), {}), "print(42)");
        EXPECT_EQ(path.extension(), ".py");

        return EvaluationOutcome{
            true,
            {{{"combined_score", 42}, {"label", "ok"}, {"missing", nullptr}}, {{"blob", ArtifactBytes{0, 255}}}},
            {}};
      }},
      FastOptions());
  ASSERT_TRUE(evaluator.ok()) << evaluator.status();

  auto result = (*evaluator)->Evaluate({"print(42)", "../../metadata-only"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->result.metrics["label"], "ok");
  EXPECT_TRUE(result->result.metrics["missing"].is_null());
  EXPECT_EQ(std::get<ArtifactBytes>(result->result.artifacts.at("blob")), (ArtifactBytes{0, 255}));
  EXPECT_FALSE(std::filesystem::exists(candidate.parent_path()));
}

TEST(EvaluatorTest, InfrastructureAndInvalidResultsAreErrorsWithoutRetry) {
  int calls = 0;
  auto evaluator = Evaluator::Create(DirectConfig(), {[&](const auto&) -> absl::StatusOr<EvaluationOutcome> {
                                       ++calls;
                                       return absl::DataLossError("broken protocol");
                                     }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  EXPECT_EQ((*evaluator)->Evaluate({"pass"}).status().code(), absl::StatusCode::kDataLoss);
  EXPECT_EQ(calls, 1);

  evaluator = Evaluator::Create(DirectConfig(), {[](const auto&) {
                                  return EvaluationOutcome{true, {{{"nested", Metrics::array({1})}}, {}}, {}};
                                }},
                                FastOptions());
  ASSERT_TRUE(evaluator.ok());

  EXPECT_FALSE((*evaluator)->Evaluate({"pass"}).ok());
}

TEST(EvaluatorTest, InvalidInputDoesNotReachBackend) {
  int calls = 0;
  auto evaluator = Evaluator::Create(DirectConfig(), {[&](const auto&) {
                                       ++calls;
                                       return Score(1);
                                     }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  EXPECT_EQ((*evaluator)->Evaluate({std::string(1, '\xff')}).status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(calls, 0);
}

TEST(EvaluatorTest, DisablingArtifactsIgnoresOversizedInvalidText) {
  auto config = DirectConfig();
  config.enable_artifacts = false;
  config.max_artifact_storage = 0;
  auto evaluator = Evaluator::Create(config, {[](const auto&) {
                                       auto result = Score(1);
                                       result.result.artifacts["text"] = std::string(100, '\xff');
                                       return result;
                                     }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_TRUE(result->result.artifacts.empty());
}

TEST(EvaluatorTest, RejectsInvalidSettingsBeforeCallingBackend) {
  const EvaluationBackend backend{[](const auto&) { return Score(1); }};

  auto config = DirectConfig();
  config.timeout = 0;
  EXPECT_FALSE(Evaluator::Create(config, backend).ok());

  config = {};
  config.max_artifact_storage = -1;
  EXPECT_FALSE(Evaluator::Create(config, backend).ok());

  auto options = FastOptions();
  options.file_suffix = "/unsafe.py";
  EXPECT_FALSE(Evaluator::Create(DirectConfig(), backend, options).ok());

  EXPECT_FALSE(Evaluator::Create(DirectConfig(), EvaluationBackend{}).ok());
}

TEST(EvaluatorTest, CallbackExceptionRemovesFile) {
  int calls = 0;
  std::filesystem::path candidate;
  auto evaluator = Evaluator::Create(DirectConfig(), {[&](const auto& path) {
                                       candidate = path;
                                       if (++calls == 1) throw std::runtime_error("private code");
                                       return Score(1);
                                     }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto first = (*evaluator)->Evaluate({"pass"});
  EXPECT_EQ(first.status().code(), absl::StatusCode::kInternal);
  EXPECT_FALSE(std::filesystem::exists(candidate.parent_path()));

  EXPECT_TRUE((*evaluator)->Evaluate({"pass"}).ok());
}

TEST(EvaluatorTest, RejectionsAreTerminalAndKeepDiagnostics) {
  int calls = 0;
  std::filesystem::path candidate;
  auto evaluator = Evaluator::Create({}, {[&](const auto& path) {
                                       ++calls;
                                       candidate = path;
                                       return Failure();
                                     }});
  ASSERT_TRUE(evaluator.ok());
  auto result = (*evaluator)->Evaluate({"print(42)"});
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result->passed);
  EXPECT_EQ(result->reason, "wrong_answer");
  EXPECT_FALSE(result->result.metrics.contains("combined_score"));
  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(std::filesystem::exists(candidate.parent_path()));
}

TEST(EvaluatorTest, MissingScoreAndBooleanScoreAreRejectedBeforeFeedback) {
  for (const Metrics score : {Metrics(true), Metrics(nullptr)}) {
    auto evaluator = Evaluator::Create(
        {}, {[score](const auto&) { return EvaluationOutcome{true, {{{"combined_score", score}}, {}}, {}}; }});
    ASSERT_TRUE(evaluator.ok());
    EXPECT_FALSE((*evaluator)->Evaluate({"print(42)"}).ok());
  }
}

class CountingLLM : public LLMInterface {
 public:
  explicit CountingLLM(int& calls) : calls_(calls) {}
  absl::StatusOr<LLMResponse> Generate(const LLMRequest&) const override {
    ++calls_;
    return LLMResponse{"{\"quality\":100}", "fake", "fake", std::nullopt};
  }

 private:
  int& calls_;
};
TEST(EvaluatorTest, RejectedCandidateNeverRequestsFeedback) {
  EvaluatorConfig config;
  config.use_llm_feedback = true;
  EvaluatorOptions options;
  int calls = 0;
  LLMModelConfig model;
  model.name = "fake";
  options.feedback.models = {model};
  options.feedback.factory = [&](const auto&) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
    return std::make_shared<CountingLLM>(calls);
  };
  auto evaluator = Evaluator::Create(config, {[](const auto&) { return Failure(); }}, options);
  ASSERT_TRUE(evaluator.ok());
  auto result = (*evaluator)->Evaluate({"print(42)"});
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result->passed);
  EXPECT_EQ(calls, 0);
}

class FixedLLM : public LLMInterface {
 public:
  explicit FixedLLM(std::string response) : response_(std::move(response)) {}
  absl::StatusOr<LLMResponse> Generate(const LLMRequest& request) const override {
    EXPECT_TRUE(request.system_message.has_value());
    EXPECT_NE(request.messages.at(0).content.find("print(42)"), std::string::npos);

    return LLMResponse{response_, "fake", "fake", std::nullopt};
  }

 private:
  std::string response_;
};
EvaluatorOptions FeedbackOptions(std::vector<std::string> responses) {
  auto options = FastOptions();
  for (std::size_t i = 0; i < responses.size(); ++i) {
    LLMModelConfig model;
    model.name = std::to_string(i);
    model.weight = i + 1;
    options.feedback.models.push_back(model);
  }

  options.feedback.factory = [responses](const LLMModelConfig& model) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
    return std::make_shared<FixedLLM>(responses.at(std::stoi(*model.name)));
  };

  return options;
}

TEST(EvaluatorTest, FeedbackUsesNormalizedWeightsAndKeepsTypedArtifacts) {
  auto config = DirectConfig();
  config.use_llm_feedback = true;
  auto options = FeedbackOptions(
      {"```json\n{\"quality\":0.3,\"note\":\"first\"}\n```", "Answer: {\"quality\":0.9,\"note\":{\"ok\":true}}"});
  auto evaluator = Evaluator::Create(config, {[](const auto&) { return Score(0.8); }}, options);
  ASSERT_TRUE(evaluator.ok()) << evaluator.status();

  auto result = (*evaluator)->Evaluate({"print(42)"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_NEAR(result->result.metrics["llm_quality"].get<double>(), 0.07, 1e-12);
  EXPECT_NEAR(result->result.metrics["llm_average"].get<double>(), 0.07, 1e-12);
  EXPECT_NEAR(result->result.metrics["combined_score"].get<double>(), 0.77, 1e-12);
  EXPECT_EQ(std::get<std::string>(result->result.artifacts.at("note")), "{\"ok\":true}");
}

TEST(EvaluatorTest, ZeroWeightFeedbackCannotChangeAverageOrInvalidateFeedback) {
  auto config = DirectConfig();
  config.use_llm_feedback = true;

  for (const std::string ignored : {"{\"unused\":123}", "not JSON"}) {
    auto options = FeedbackOptions({"{\"quality\":1}", ignored});
    options.feedback.models[0].weight = 1;
    options.feedback.models[1].weight = 0;
    auto evaluator = Evaluator::Create(config, {[](const auto&) { return Score(0.8); }}, options);
    ASSERT_TRUE(evaluator.ok());

    auto result = (*evaluator)->Evaluate({"print(42)"});
    ASSERT_TRUE(result.ok());

    ASSERT_TRUE(result->result.metrics.contains("llm_average"));
    EXPECT_DOUBLE_EQ(result->result.metrics["llm_average"].get<double>(), 0.1);
    EXPECT_NEAR(result->result.metrics["combined_score"].get<double>(), 0.86, 1e-12);
    EXPECT_FALSE(result->result.metrics.contains("llm_unused"));
    EXPECT_FALSE(result->result.artifacts.count("llm_feedback_error"));
  }
}

TEST(EvaluatorTest, InvalidFeedbackLeavesScoresAndAddsDiagnostic) {
  auto config = DirectConfig();
  config.use_llm_feedback = true;
  auto evaluator = Evaluator::Create(config, {[](const auto&) { return Score(0.8); }}, FeedbackOptions({"not JSON"}));
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"print(42)"});
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(result->result.metrics, Metrics({{"combined_score", 0.8}}));
  EXPECT_TRUE(result->result.artifacts.count("llm_feedback_error"));
}

TEST(EvaluatorTest, NonFiniteFeedbackMergeDegradesInsteadOfFailingTheRun) {
  // Feedback that cannot produce a finite score must keep the program score
  // and report a diagnostic. Returning a Status here would abort the whole
  // evolution run, unlike the documented artifact-cap error.
  auto config = DirectConfig();
  config.use_llm_feedback = true;
  const std::string huge = "1.7976931348623157e308";
  auto evaluator = Evaluator::Create(config, {[](const auto&) { return Score(0.8); }},
                                     FeedbackOptions({"{\"a\":" + huge + ",\"b\":" + huge + ",\"c\":" + huge + "}"}));
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"print(42)"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->result.metrics, Metrics({{"combined_score", 0.8}}));
  EXPECT_TRUE(result->result.artifacts.count("llm_feedback_error"));
}

TEST(EvaluatorTest, InvalidFeedbackPreservesScoresWhenDiagnosticBudgetIsFull) {
  auto config = DirectConfig();
  config.use_llm_feedback = true;
  config.max_artifact_storage = 3;
  auto evaluator = Evaluator::Create(config, {[](const auto&) {
                                       auto result = Score(0.8);
                                       result.result.artifacts["report"] = std::string("abc");
                                       return result;
                                     }},
                                     FeedbackOptions({"not JSON"}));
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"print(42)"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->result.metrics, Metrics({{"combined_score", 0.8}}));
  EXPECT_EQ(std::get<std::string>(result->result.artifacts.at("report")), "abc");
  EXPECT_EQ(result->result.GetTotalArtifactSize(), 3);
}

TEST(EvaluatorTest, FeedbackRequiresModelsAndHonorsFinalArtifactCap) {
  auto config = DirectConfig();
  config.use_llm_feedback = true;
  EvaluationBackend backend{[](const auto&) { return Score(0.8); }};
  EXPECT_FALSE(Evaluator::Create(config, backend).ok());

  config.max_artifact_storage = 3;
  auto evaluator = Evaluator::Create(config, backend, FeedbackOptions({"{\"note\":\"too long\"}"}));
  ASSERT_TRUE(evaluator.ok());

  EXPECT_EQ((*evaluator)->Evaluate({"print(42)"}).status().code(), absl::StatusCode::kResourceExhausted);
}

}  // namespace
}  // namespace ievolve::evaluator
