#include "ievolve/evaluator/evaluator.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>

#include "gtest/gtest.h"

namespace ievolve::evaluator {
namespace {
using namespace std::chrono_literals;

EvaluatorConfig DirectConfig() {
  EvaluatorConfig config;
  config.cascade_evaluation = false;
  return config;
}
EvaluatorOptions FastOptions() {
  EvaluatorOptions options;
  options.retry_delay = 0ms;
  return options;
}
EvaluationStageResult Score(double value) { return {{{{"combined_score", value}}, {}}, false}; }
EvaluationStageResult Failure() { return {{Metrics::object(), {{"stderr", std::string("failure")}}}, true}; }

TEST(EvaluatorTest, ExecutesPythonCascadeAndPreservesPartialFailures) {
  EvaluatorConfig config;
  config.timeout = 1;
  auto options = FastOptions();
  options.python.python_executable = IEVOLVE_PYTHON_EXECUTABLE;
  auto evaluator =
      Evaluator::Create(config, std::filesystem::path(IEVOLVE_EVALUATOR_TEST_DATA_DIR) / "python_cascade.py", options);
  ASSERT_TRUE(evaluator.ok()) << evaluator.status();

  auto result = (*evaluator)->Evaluate({"score = 0.25"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->metrics["combined_score"], 0.9);
  EXPECT_EQ(std::get<ArtifactBytes>(result->artifacts.at("binary")), (ArtifactBytes{0, 255}));
  EXPECT_FALSE(std::filesystem::exists(std::get<std::string>(result->artifacts.at("candidate_path"))));

  result = (*evaluator)->Evaluate({"def stage2_hook(): raise RuntimeError('stage2 failure')"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->metrics["combined_score"], 0.5);
  EXPECT_EQ(result->metrics["stage2_passed"], 0);
  EXPECT_FALSE(result->metrics.contains("timeout"));
  EXPECT_FALSE(std::filesystem::exists(std::get<std::string>(result->artifacts.at("candidate_path"))));

  result = (*evaluator)->Evaluate({"import time\ndef stage2_hook(): time.sleep(10)"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->metrics["combined_score"], 0.5);
  EXPECT_EQ(result->metrics["stage2_passed"], 0);
  EXPECT_EQ(result->metrics["timeout"], true);
  EXPECT_FALSE(std::filesystem::exists(std::get<std::string>(result->artifacts.at("candidate_path"))));
}

TEST(EvaluatorTest, PythonDirectUsesConfiguredTimeoutAndArtifactSettings) {
  auto config = DirectConfig();
  config.enable_artifacts = false;
  auto options = FastOptions();
  options.python.python_executable = IEVOLVE_PYTHON_EXECUTABLE;
  auto evaluator = Evaluator::Create(
      config, std::filesystem::path(IEVOLVE_EVALUATOR_TEST_DATA_DIR) / "python_evaluator.py", options);
  ASSERT_TRUE(evaluator.ok()) << evaluator.status();

  auto result = (*evaluator)->Evaluate({"def answer(): return 42"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->metrics["combined_score"], 42);
  EXPECT_TRUE(result->artifacts.empty());
}

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
  auto evaluator = Evaluator::Create(DirectConfig(),
                                     {{},
                                      [](const auto&, int) {
                                        auto result = Score(1);
                                        result.result.artifacts["report"] = std::string("present");
                                        return result;
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  ASSERT_EQ(setenv("ENABLE_ARTIFACTS", "TrUe", 1), 0);

  auto result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok());

  EXPECT_TRUE(result->artifacts.empty());
}

TEST(EvaluatorTest, DirectPreservesValuesAndCleansCandidate) {
  std::filesystem::path candidate;
  auto evaluator = Evaluator::Create(
      DirectConfig(),
      {{},
       [&](const auto& path, int stage) {
         candidate = path;
         EXPECT_EQ(stage, 0);

         std::ifstream stream(path);
         EXPECT_EQ(std::string(std::istreambuf_iterator<char>(stream), {}), "print(42)");
         EXPECT_EQ(path.extension(), ".py");

         return EvaluationStageResult{
             {{{"score", 42}, {"label", "ok"}, {"missing", nullptr}}, {{"blob", ArtifactBytes{0, 255}}}}, false};
       }},
      FastOptions());
  ASSERT_TRUE(evaluator.ok()) << evaluator.status();

  auto result = (*evaluator)->Evaluate({"print(42)", "../../metadata-only"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->metrics["label"], "ok");
  EXPECT_TRUE(result->metrics["missing"].is_null());
  EXPECT_EQ(std::get<ArtifactBytes>(result->artifacts.at("blob")), (ArtifactBytes{0, 255}));
  EXPECT_FALSE(std::filesystem::exists(candidate.parent_path()));
}

TEST(EvaluatorTest, DirectRetriesFailuresWithFreshFiles) {
  std::vector<std::filesystem::path> paths;
  auto config = DirectConfig();
  config.max_retries = 2;
  auto evaluator = Evaluator::Create(config,
                                     {{},
                                      [&](const auto& path, int) {
                                        paths.push_back(path);
                                        return paths.size() < 3 ? Failure() : Score(0.8);
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_DOUBLE_EQ(result->metrics["combined_score"], 0.8);
  ASSERT_EQ(paths.size(), 3);
  EXPECT_NE(paths[0], paths[1]);
  for (const auto& path : paths) EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(EvaluatorTest, ExhaustedExceptionsReturnDiagnosticsAndZeroError) {
  int calls = 0;
  auto config = DirectConfig();
  config.max_retries = 1;
  auto evaluator = Evaluator::Create(config,
                                     {{},
                                      [&](const auto&, int) {
                                        ++calls;
                                        return Failure();
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(calls, 2);
  EXPECT_EQ(result->metrics, Metrics({{"error", 0}}));
  EXPECT_EQ(std::get<std::string>(result->artifacts.at("stderr")), "failure");
}

TEST(EvaluatorTest, TimeoutNeverRetriesAndCleansCandidate) {
  int calls = 0;
  std::filesystem::path path;
  auto evaluator = Evaluator::Create(DirectConfig(),
                                     {{},
                                      [&](const auto& p, int) -> absl::StatusOr<EvaluationStageResult> {
                                        ++calls;
                                        path = p;
                                        return absl::DeadlineExceededError("deadline");
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(calls, 1);
  EXPECT_EQ(result->metrics, Metrics({{"error", 0}, {"timeout", true}}));
  EXPECT_FALSE(std::filesystem::exists(path.parent_path()));
}

TEST(EvaluatorTest, InfrastructureAndInvalidResultsAreErrorsWithoutRetry) {
  int calls = 0;
  auto evaluator = Evaluator::Create(DirectConfig(),
                                     {{},
                                      [&](const auto&, int) -> absl::StatusOr<EvaluationStageResult> {
                                        ++calls;
                                        return absl::DataLossError("broken protocol");
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  EXPECT_EQ((*evaluator)->Evaluate({"pass"}).status().code(), absl::StatusCode::kDataLoss);
  EXPECT_EQ(calls, 1);

  evaluator = Evaluator::Create(
      DirectConfig(),
      {{}, [](const auto&, int) { return EvaluationStageResult{{{{"nested", Metrics::array({1})}}, {}}, false}; }},
      FastOptions());
  ASSERT_TRUE(evaluator.ok());

  EXPECT_FALSE((*evaluator)->Evaluate({"pass"}).ok());
}

TEST(EvaluatorTest, CascadeMergesStagesOnSameFileAndStopsAtThreshold) {
  std::vector<int> calls;
  std::filesystem::path candidate;
  EvaluatorConfig config;
  config.cascade_thresholds = {0.5, 0.75};
  auto evaluator = Evaluator::Create(config,
                                     {{1, 2, 3},
                                      [&](const auto& path, int stage) {
                                        calls.push_back(stage);
                                        if (candidate.empty()) candidate = path;
                                        EXPECT_EQ(path, candidate);

                                        auto result = Score(stage == 1 ? 0.5 : 0.7);
                                        result.result.metrics["stage" + std::to_string(stage)] = true;
                                        result.result.metrics["ignored"] = "text";
                                        result.result.artifacts["report"] = std::to_string(stage);

                                        return result;
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(calls, (std::vector<int>{1, 2}));
  EXPECT_EQ(result->metrics["stage1"], 1.0);
  EXPECT_EQ(result->metrics["stage2"], 1.0);
  EXPECT_FALSE(result->metrics.contains("ignored"));
  EXPECT_EQ(std::get<std::string>(result->artifacts.at("report")), "2");
}

TEST(EvaluatorTest, CascadeStageThreeFailureRetainsEarlierScoresWithoutRetry) {
  int calls = 0;
  auto evaluator = Evaluator::Create({},
                                     {{1, 2, 3},
                                      [&](const auto&, int stage) {
                                        ++calls;
                                        return stage < 3 ? Score(0.9) : Failure();
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(calls, 3);
  EXPECT_EQ(result->metrics["combined_score"], 0.9);
  EXPECT_EQ(result->metrics["stage3_passed"], 0);
  EXPECT_FALSE(result->metrics.contains("error"));
  EXPECT_TRUE(result->artifacts.count("stderr"));
}

TEST(EvaluatorTest, CascadeTimeoutRetainsPartialResult) {
  auto evaluator = Evaluator::Create({},
                                     {{1, 2},
                                      [](const auto&, int stage) -> absl::StatusOr<EvaluationStageResult> {
                                        if (stage == 1) return Score(0.8);
                                        return absl::DeadlineExceededError("timeout");
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(result->metrics["combined_score"], 0.8);
  EXPECT_EQ(result->metrics["stage2_passed"], 0);
  EXPECT_EQ(result->metrics["timeout"], true);
}

TEST(EvaluatorTest, CascadeRunsThirdStageAtInclusiveThreshold) {
  EvaluatorConfig config;
  config.cascade_thresholds = {0.5, 0.75};
  std::vector<int> calls;
  auto evaluator = Evaluator::Create(config,
                                     {{1, 2, 3},
                                      [&](const auto&, int stage) {
                                        calls.push_back(stage);
                                        return Score(stage == 1 ? 0.5 : stage == 2 ? 0.75 : 0.9);
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(calls, (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(result->metrics["combined_score"], 0.9);
}

TEST(EvaluatorTest, MissingSecondStageStopsCascade) {
  auto evaluator = Evaluator::Create({},
                                     {{1, 3},
                                      [](const auto&, int stage) {
                                        EXPECT_EQ(stage, 1);
                                        return Score(1);
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  EXPECT_TRUE((*evaluator)->Evaluate({"pass"}).ok());
}

TEST(EvaluatorTest, InvalidInputDoesNotReachBackend) {
  int calls = 0;
  auto evaluator = Evaluator::Create(DirectConfig(),
                                     {{},
                                      [&](const auto&, int) {
                                        ++calls;
                                        return Score(1);
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  EXPECT_EQ((*evaluator)->Evaluate({std::string(1, '\xff')}).status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(calls, 0);
}

TEST(EvaluatorTest, StageOneFailureAndMissingStageFallback) {
  auto evaluator = Evaluator::Create({}, {{1}, [](const auto&, int) { return Failure(); }}, FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(result->metrics, Metrics({{"stage1_passed", 0}, {"error", 0}}));

  evaluator = Evaluator::Create({},
                                {{2, 3},
                                 [](const auto&, int stage) {
                                   EXPECT_EQ(stage, 0);
                                   return Score(0.8);
                                 }},
                                FastOptions());
  ASSERT_TRUE(evaluator.ok());

  EXPECT_TRUE((*evaluator)->Evaluate({"pass"}).ok());
}

TEST(EvaluatorTest, DisablingArtifactsIgnoresOversizedInvalidText) {
  auto config = DirectConfig();
  config.enable_artifacts = false;
  config.max_artifact_storage = 0;
  auto evaluator = Evaluator::Create(config,
                                     {{},
                                      [](const auto&, int) {
                                        auto result = Score(1);
                                        result.result.artifacts["text"] = std::string(100, '\xff');
                                        return result;
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_TRUE(result->artifacts.empty());
}

TEST(EvaluatorTest, ArtifactCapAppliesAfterMergingStages) {
  EvaluatorConfig config;
  config.max_artifact_storage = 5;
  auto evaluator = Evaluator::Create(config,
                                     {{1, 2},
                                      [](const auto&, int stage) {
                                        auto result = Score(1);
                                        result.result.artifacts[std::to_string(stage)] = std::string("abcd");
                                        return result;
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  EXPECT_EQ((*evaluator)->Evaluate({"pass"}).status().code(), absl::StatusCode::kResourceExhausted);
}

TEST(EvaluatorTest, RejectsInvalidAndUnsupportedSettingsBeforeCallingBackend) {
  const EvaluationBackend backend{{}, [](const auto&, int) { return Score(1); }};

  auto config = DirectConfig();
  config.parallel_evaluations = 0;
  EXPECT_FALSE(Evaluator::Create(config, backend).ok());

  config = DirectConfig();
  config.max_retries = -1;
  EXPECT_FALSE(Evaluator::Create(config, backend).ok());

  config = {};
  config.cascade_thresholds.clear();
  EXPECT_FALSE(Evaluator::Create(config, backend).ok());

  config = DirectConfig();
  config.memory_limit_mb = 100;
  EXPECT_EQ(Evaluator::Create(config, backend).status().code(), absl::StatusCode::kUnimplemented);

  config = DirectConfig();
  config.distributed = true;
  EXPECT_EQ(Evaluator::Create(config, backend).status().code(), absl::StatusCode::kUnimplemented);

  auto options = FastOptions();
  options.file_suffix = "/unsafe.py";
  EXPECT_FALSE(Evaluator::Create(DirectConfig(), backend, options).ok());

  EXPECT_FALSE(Evaluator::Create(DirectConfig(), EvaluationBackend{}).ok());
  EXPECT_FALSE(Evaluator::Create(DirectConfig(), {{1, 1}, backend.run}).ok());
}

TEST(EvaluatorTest, CallbackExceptionReleasesSlotAndRemovesFile) {
  int calls = 0;
  std::filesystem::path candidate;
  auto evaluator = Evaluator::Create(DirectConfig(),
                                     {{},
                                      [&](const auto& path, int) {
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

TEST(EvaluatorTest, BatchesAndConcurrentCallsShareLimitAndPreserveOrder) {
  std::atomic<int> active{0}, maximum{0};
  auto config = DirectConfig();
  config.parallel_evaluations = 2;
  auto evaluator = Evaluator::Create(config,
                                     {{},
                                      [&](const auto& path, int) {
                                        const int count = ++active;
                                        int observed = maximum.load();
                                        while (count > observed && !maximum.compare_exchange_weak(observed, count)) {
                                        }

                                        std::this_thread::sleep_for(10ms);
                                        std::ifstream stream(path);
                                        double value = 0;
                                        stream >> value;

                                        --active;
                                        return Score(value);
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto single = std::async(std::launch::async, [&] { return (*evaluator)->Evaluate({"9"}); });
  auto batch = (*evaluator)->EvaluateMultiple({{"1"}, {"2"}, {"3"}, {"4"}});
  ASSERT_TRUE(batch.ok()) << batch.status();

  EXPECT_TRUE(single.get().ok());
  ASSERT_EQ(batch->size(), 4);
  for (std::size_t i = 0; i < batch->size(); ++i) EXPECT_EQ((*batch)[i].metrics["combined_score"], i + 1);
  EXPECT_LE(maximum.load(), 2);
  EXPECT_EQ(active.load(), 0);

  EXPECT_TRUE((*evaluator)->EvaluateMultiple({})->empty());
}

TEST(EvaluatorTest, BatchJoinsWorkersAndReturnsFirstInputError) {
  std::atomic<int> calls{0};
  auto config = DirectConfig();
  config.parallel_evaluations = 2;
  auto evaluator = Evaluator::Create(config,
                                     {{},
                                      [&](const auto& path, int) -> absl::StatusOr<EvaluationStageResult> {
                                        ++calls;
                                        std::ifstream stream(path);
                                        int value;
                                        stream >> value;

                                        if (value == 1) {
                                          std::this_thread::sleep_for(20ms);
                                          return absl::DataLossError("first");
                                        }

                                        return absl::NotFoundError("second");
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->EvaluateMultiple({{"1"}, {"2"}});
  EXPECT_EQ(result.status().code(), absl::StatusCode::kDataLoss);
  EXPECT_EQ(calls.load(), 2);
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
  auto evaluator = Evaluator::Create(config, {{}, [](const auto&, int) { return Score(0.8); }}, options);
  ASSERT_TRUE(evaluator.ok()) << evaluator.status();

  auto result = (*evaluator)->Evaluate({"print(42)"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_NEAR(result->metrics["llm_quality"].get<double>(), 0.07, 1e-12);
  EXPECT_NEAR(result->metrics["llm_average"].get<double>(), 0.07, 1e-12);
  EXPECT_NEAR(result->metrics["combined_score"].get<double>(), 0.77, 1e-12);
  EXPECT_EQ(std::get<std::string>(result->artifacts.at("note")), "{\"ok\":true}");
}

TEST(EvaluatorTest, ZeroWeightFeedbackCannotChangeAverageOrInvalidateFeedback) {
  auto config = DirectConfig();
  config.use_llm_feedback = true;

  for (const std::string ignored : {"{\"unused\":123}", "not JSON"}) {
    auto options = FeedbackOptions({"{\"quality\":1}", ignored});
    options.feedback.models[0].weight = 1;
    options.feedback.models[1].weight = 0;
    auto evaluator = Evaluator::Create(config, {{}, [](const auto&, int) { return Score(0.8); }}, options);
    ASSERT_TRUE(evaluator.ok());

    auto result = (*evaluator)->Evaluate({"print(42)"});
    ASSERT_TRUE(result.ok());

    ASSERT_TRUE(result->metrics.contains("llm_average"));
    EXPECT_DOUBLE_EQ(result->metrics["llm_average"].get<double>(), 0.1);
    EXPECT_NEAR(result->metrics["combined_score"].get<double>(), 0.86, 1e-12);
    EXPECT_FALSE(result->metrics.contains("llm_unused"));
    EXPECT_FALSE(result->artifacts.count("llm_feedback_error"));
  }
}

TEST(EvaluatorTest, InvalidFeedbackLeavesScoresAndAddsDiagnostic) {
  auto config = DirectConfig();
  config.use_llm_feedback = true;
  auto evaluator =
      Evaluator::Create(config, {{}, [](const auto&, int) { return Score(0.8); }}, FeedbackOptions({"not JSON"}));
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"print(42)"});
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(result->metrics, Metrics({{"combined_score", 0.8}}));
  EXPECT_TRUE(result->artifacts.count("llm_feedback_error"));
}

TEST(EvaluatorTest, NonFiniteFeedbackMergeDegradesInsteadOfFailingTheRun) {
  // Feedback that cannot produce a finite score must keep the program score
  // and report a diagnostic. Returning a Status here would abort the whole
  // evolution run, unlike the documented artifact-cap error.
  auto config = DirectConfig();
  config.use_llm_feedback = true;
  const std::string huge = "1.7976931348623157e308";
  auto evaluator = Evaluator::Create(config, {{}, [](const auto&, int) { return Score(0.8); }},
                                     FeedbackOptions({"{\"a\":" + huge + ",\"b\":" + huge + ",\"c\":" + huge + "}"}));
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"print(42)"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->metrics, Metrics({{"combined_score", 0.8}}));
  EXPECT_TRUE(result->artifacts.count("llm_feedback_error"));
}

TEST(EvaluatorTest, InvalidFeedbackPreservesScoresWhenDiagnosticBudgetIsFull) {
  auto config = DirectConfig();
  config.use_llm_feedback = true;
  config.max_artifact_storage = 3;
  auto evaluator = Evaluator::Create(config,
                                     {{},
                                      [](const auto&, int) {
                                        auto result = Score(0.8);
                                        result.result.artifacts["report"] = std::string("abc");
                                        return result;
                                      }},
                                     FeedbackOptions({"not JSON"}));
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"print(42)"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->metrics, Metrics({{"combined_score", 0.8}}));
  EXPECT_EQ(std::get<std::string>(result->artifacts.at("report")), "abc");
  EXPECT_EQ(result->GetTotalArtifactSize(), 3);
}

TEST(EvaluatorTest, FailureDiagnosticsRespectRemainingBudget) {
  auto config = DirectConfig();
  config.max_artifact_storage = 0;
  auto evaluator = Evaluator::Create(config,
                                     {{},
                                      [](const auto&, int) -> absl::StatusOr<EvaluationStageResult> {
                                        return absl::DeadlineExceededError("timeout");
                                      }},
                                     FastOptions());
  ASSERT_TRUE(evaluator.ok());

  auto result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->metrics["timeout"], true);
  EXPECT_TRUE(result->artifacts.empty());

  config = {};
  config.max_artifact_storage = 3;
  evaluator =
      Evaluator::Create(config,
                        {{1, 2},
                         [](const auto&, int stage) {
                           if (stage == 2) {
                             return EvaluationStageResult{{Metrics::object(), {{"error", std::string("bad")}}}, true};
                           }

                           auto score = Score(1);
                           score.result.artifacts["report"] = std::string("abc");
                           return score;
                         }},
                        FastOptions());
  ASSERT_TRUE(evaluator.ok());

  result = (*evaluator)->Evaluate({"pass"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->metrics["combined_score"], 1);
  EXPECT_EQ(result->metrics["stage2_passed"], 0);
  EXPECT_EQ(std::get<std::string>(result->artifacts.at("report")), "abc");
  EXPECT_EQ(result->GetTotalArtifactSize(), 3);
}

TEST(EvaluatorTest, FeedbackRequiresModelsAndHonorsFinalArtifactCap) {
  auto config = DirectConfig();
  config.use_llm_feedback = true;
  EvaluationBackend backend{{}, [](const auto&, int) { return Score(0.8); }};
  EXPECT_FALSE(Evaluator::Create(config, backend).ok());

  config.max_artifact_storage = 3;
  auto evaluator = Evaluator::Create(config, backend, FeedbackOptions({"{\"note\":\"too long\"}"}));
  ASSERT_TRUE(evaluator.ok());

  EXPECT_EQ((*evaluator)->Evaluate({"print(42)"}).status().code(), absl::StatusCode::kResourceExhausted);
}

}  // namespace
}  // namespace ievolve::evaluator
