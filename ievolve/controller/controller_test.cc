#include "ievolve/controller/controller.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "gtest/gtest.h"
#include "ievolve/database/checkpoint.h"

namespace ievolve::controller {
namespace {
namespace fs = std::filesystem;

class FunctionLLM : public LLMInterface {
 public:
  using GenerateFunction = std::function<absl::StatusOr<LLMResponse>(const LLMRequest&)>;
  explicit FunctionLLM(GenerateFunction function) : function_(std::move(function)) {}
  absl::StatusOr<LLMResponse> Generate(const LLMRequest& request) const override { return function_(request); }

 private:
  GenerateFunction function_;
};

class ControllerTest : public testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> sequence{0};
    root_ = fs::temp_directory_path() /
            ("ievolve-controller-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence++));
    fs::create_directory(root_);

    config_.run.output_directory = (root_ / "output").string();
    config_.language = "python";
    config_.prompt.use_template_stochasticity = false;
    config_.diff_based_evolution = false;
    config_.checkpoint_interval = 2;
    config_.database.artifacts_base_path = (root_ / "artifacts").string();

    EvaluatorConfig settings;
    auto evaluator = evaluator::Evaluator::Create(
        settings, {[this](const auto& path) -> absl::StatusOr<evaluator::EvaluationOutcome> {
          ++evaluations_;
          const auto code = Read(path);
          auto metrics = score_ ? score_(code) : Metrics{{"combined_score", std::stod(code)}};

          return evaluator::EvaluationOutcome{true, {metrics, {{"code", code}, {"binary", ArtifactBytes{0, 255}}}}, {}};
        }});
    ASSERT_TRUE(evaluator.ok()) << evaluator.status();
    evaluator_ = std::move(*evaluator);
  }
  void TearDown() override {
    std::error_code error;
    fs::remove_all(root_, error);
  }
  absl::StatusOr<std::unique_ptr<Controller>> Make() {
    auto llm = std::make_shared<FunctionLLM>([this](const auto& request) -> absl::StatusOr<LLMResponse> {
      ++calls_;
      if (generate_) return generate_(request);
      return LLMResponse{std::to_string(calls_), "offline", "fake", LLMUsage{12, 5, 2, 0.125}};
    });

    return Controller::Create(config_, llm, evaluator_, options_);
  }
  static std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  static void Write(const fs::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    stream.close();

    ASSERT_TRUE(stream.good());
  }
  static Metrics ReadJson(const fs::path& path) { return Metrics::parse(Read(path)); }
  fs::path Output() const { return config_.run.output_directory; }
  fs::path root_;
  Config config_;
  ControllerOptions options_;
  std::shared_ptr<evaluator::Evaluator> evaluator_;
  FunctionLLM::GenerateFunction generate_;
  std::function<Metrics(const std::string&)> score_;
  int calls_ = 0;
  int evaluations_ = 0;
};

TEST_F(ControllerTest, WrongFastCandidateCannotReplaceNegativeValidSeed) {
  EvaluatorConfig settings;
  auto evaluator = evaluator::Evaluator::Create(
      settings, {[](const auto& path) {
        if (Read(path) == "-10") return evaluator::EvaluationOutcome{true, {{{"combined_score", -10}}, {}}, {}};
        return evaluator::EvaluationOutcome{false, {Metrics::object(), {{"case", "wrong answer"}}}, "wrong_answer"};
      }});
  ASSERT_TRUE(evaluator.ok());
  evaluator_ = std::move(*evaluator);
  config_.max_iterations = 1;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());
  auto result = (*controller)->Run({"-10"});
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(result->best.id, "initial");
  EXPECT_EQ(result->best.metrics.at("combined_score"), -10);
  EXPECT_EQ(result->rejected_iterations, 1);
  EXPECT_FALSE((*controller)->database().Get("iteration-1").ok());
}

TEST_F(ControllerTest, RejectsSeedBeforePopulationOrModelCalls) {
  auto evaluator = evaluator::Evaluator::Create(
      {}, {[](const auto&) {
        return evaluator::EvaluationOutcome{false, {Metrics::object(), {{"case", "seed case"}}}, "wrong_answer"};
      }});
  ASSERT_TRUE(evaluator.ok());
  evaluator_ = std::move(*evaluator);
  std::ostringstream log;
  utils::Logger logger(log, utils::LogLevel::kDebug);
  options_.logger = &logger;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());
  EXPECT_EQ((*controller)->Run({"0"}).status().code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_EQ(calls_, 0);
  EXPECT_EQ((*controller)->database().size(), 0);
  EXPECT_NE(log.str().find("[INFO] Initial program rejected: wrong_answer\n"), std::string::npos) << log.str();
  EXPECT_NE(log.str().find("[DEBUG] Initial program case:\nseed case\n"), std::string::npos) << log.str();
  EXPECT_FALSE(fs::exists(Output() / "evaluations"));
}

TEST_F(ControllerTest, RejectionDiagnosticsAreLoggedTruncatedAtDebugLevel) {
  const std::string long_output(5000, 'x');
  auto evaluator = evaluator::Evaluator::Create(
      {}, {[&](const auto& path) {
        if (Read(path) == "0") return evaluator::EvaluationOutcome{true, {{{"combined_score", -10}}, {}}, {}};
        return evaluator::EvaluationOutcome{
            false,
            {Metrics::object(),
             {{"case", "failed case"}, {"stderr", ArtifactBytes(long_output.begin(), long_output.end())}}},
            "wrong_answer"};
      }});
  ASSERT_TRUE(evaluator.ok());
  evaluator_ = std::move(*evaluator);

  for (const auto level : {utils::LogLevel::kDebug, utils::LogLevel::kInfo}) {
    std::ostringstream log;
    utils::Logger logger(log, level);
    options_.logger = &logger;
    config_.run.output_directory = (root_ / (level == utils::LogLevel::kDebug ? "debug" : "info")).string();
    config_.max_iterations = 1;
    auto controller = Make();
    ASSERT_TRUE(controller.ok());
    auto result = (*controller)->Run({"0"});
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->rejected_iterations, 1);

    const auto text = log.str();
    EXPECT_NE(text.find("[INFO] Iteration 1/1 rejected: wrong_answer ("), std::string::npos) << text;
    const bool debug = level == utils::LogLevel::kDebug;
    EXPECT_EQ(text.find("[DEBUG] Iteration 1/1 case:\nfailed case\n") != std::string::npos, debug) << text;
    EXPECT_EQ(text.find("[DEBUG] Iteration 1/1 stderr:\n" + std::string(4096, 'x') + "\n... (904 more bytes)\n") !=
                  std::string::npos,
              debug)
        << text;
    // The rejected source is never logged.
    EXPECT_EQ(text.find("\n1\n"), std::string::npos);
    EXPECT_FALSE(fs::exists(Output() / "evaluations"));
  }
}

TEST_F(ControllerTest, CheckpointHasCurrentManifestAndNeedsNoBestExport) {
  config_.max_iterations = 0;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());
  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_TRUE(fs::exists(result->checkpoint_path / "checkpoint.json"));
  EXPECT_FALSE(fs::exists(result->checkpoint_path / "best_program.py"));
  EXPECT_FALSE(fs::exists(result->checkpoint_path / "controller.json"));
  const auto manifest = ReadJson(result->checkpoint_path / "checkpoint.json");
  EXPECT_EQ(manifest["program"]["best_id"], "initial");
  EXPECT_EQ(manifest["evaluation_protocol_version"], 1);
}

TEST_F(ControllerTest, ZeroIterationsEvaluatesSeedAndExportsBest) {
  config_.max_iterations = 0;
  auto controller = Make();
  ASSERT_TRUE(controller.ok()) << controller.status();

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(evaluations_, 1);
  EXPECT_EQ(calls_, 0);
  EXPECT_EQ(result->best.code, "0");
  EXPECT_EQ(result->last_iteration, 0);
  EXPECT_EQ(result->iterations_executed, 0);
  EXPECT_EQ(result->stop_reason, StopReason::kIterationLimit);
  EXPECT_EQ(Read(Output() / "best/best_program.py"), "0");
  EXPECT_EQ(ReadJson(result->checkpoint_path / "checkpoint.json")["progress"]["iteration"], 0);
  EXPECT_TRUE(fs::exists(result->checkpoint_path / "database/CURRENT"));

  EXPECT_EQ((*controller)->Run({"0"}).status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST_F(ControllerTest, SerialLoopStoresRecordsAndPlacesChildrenOnTargetIslands) {
  config_.database.num_islands = 3;
  config_.max_iterations = 3;
  auto controller = Make();
  ASSERT_TRUE(controller.ok()) << controller.status();

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->best.code, "3");
  EXPECT_EQ(result->accepted_iterations, 3);
  EXPECT_EQ(result->rejected_iterations, 0);
  EXPECT_EQ(result->iterations_executed, 3);
  EXPECT_EQ(evaluations_, 4);

  auto snapshot = (*controller)->database().Snapshot();
  ASSERT_TRUE(snapshot.ok());
  EXPECT_EQ(snapshot->generations, (std::vector<std::int64_t>{1, 1, 1}));

  for (int i = 1; i <= 3; ++i) {
    auto child = (*controller)->database().Get("iteration-" + std::to_string(i));
    ASSERT_TRUE(child.ok()) << child.status();
    EXPECT_EQ(child->metadata["island"], i - 1);
    ASSERT_TRUE(child->prompts);

    const auto& prompt = child->prompts->at("full_rewrite_user");
    EXPECT_FALSE(prompt.at("user").get<std::string>().empty());
    EXPECT_EQ(prompt.at("responses")[0], std::to_string(i));
    EXPECT_EQ(prompt.at("token_usage")["input_tokens"], 12);

    auto artifacts = (*controller)->database().GetArtifacts(child->id);
    ASSERT_TRUE(artifacts.ok());
    EXPECT_EQ(std::get<ArtifactBytes>(artifacts->at("binary")), (ArtifactBytes{0, 255}));
  }

  EXPECT_TRUE(fs::exists(Output() / "checkpoints/checkpoint_2"));
  EXPECT_EQ(result->checkpoint_path.filename(), "checkpoint_3");
}

TEST_F(ControllerTest, RejectedAttemptsAreCheckpointedAndResumeAfterLastAttempt) {
  config_.diff_based_evolution = true;
  generate_ = [](const auto&) -> absl::StatusOr<LLMResponse> {
    return LLMResponse{"no edit", "offline", "fake", std::nullopt};
  };

  config_.max_iterations = 3;
  auto controller = Make();
  ASSERT_TRUE(controller.ok()) << controller.status();

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->last_iteration, 3);
  EXPECT_EQ(result->rejected_iterations, 3);
  EXPECT_EQ(evaluations_, 1);
  EXPECT_EQ((*controller)->database().Snapshot()->last_iteration, 0);

  config_.run.output_directory = (root_ / "resumed").string();
  config_.max_iterations = 1;
  config_.run.checkpoint = result->checkpoint_path.string();
  auto resumed = Make();
  ASSERT_TRUE(resumed.ok());

  auto continued = (*resumed)->Run({});
  ASSERT_TRUE(continued.ok()) << continued.status();

  EXPECT_EQ(continued->last_iteration, 4);
  EXPECT_EQ(continued->rejected_iterations, 4);
  EXPECT_EQ(continued->iterations_executed, 1);
  EXPECT_EQ(evaluations_, 1);
  EXPECT_EQ(continued->checkpoint_path.filename(), "checkpoint_4");
}

TEST_F(ControllerTest, LogsSeedAcceptedIterationsCheckpointsAndSummary) {
  std::ostringstream log;
  utils::Logger logger(log);
  options_.logger = &logger;
  config_.max_iterations = 2;
  config_.run.target_score = 5;
  auto controller = Make();
  ASSERT_TRUE(controller.ok()) << controller.status();

  ASSERT_TRUE((*controller)->Run({"0"}).ok());

  const auto text = log.str();
  for (const auto* line :
       {"[INFO] Evaluating initial program\n", "[INFO] Initial program score 0.0, target 5.0\n",
        "[INFO] Iteration 1/2 accepted: score 1.0, target 5.0 (",
        "[INFO] New best program iteration-1 with score 1.0, target 5.0\n",
        "[INFO] Iteration 2/2 accepted: score 2.0, target 5.0 (", "[INFO] Saved checkpoint ",
        "[INFO] Evolution finished after 2 iteration(s); best program iteration-2 with score 2.0, target 5.0\n"}) {
    EXPECT_NE(text.find(line), std::string::npos) << line << "\n" << text;
  }
  EXPECT_EQ(text.find("[DEBUG]"), std::string::npos);
}

TEST_F(ControllerTest, LogsRejectionsResumeAndDebugDetails) {
  config_.diff_based_evolution = true;
  generate_ = [](const auto&) -> absl::StatusOr<LLMResponse> {
    return LLMResponse{"no edit", "offline", "fake", std::nullopt};
  };
  config_.max_iterations = 1;
  auto controller = Make();
  ASSERT_TRUE(controller.ok()) << controller.status();

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  std::ostringstream log;
  utils::Logger logger(log, utils::LogLevel::kDebug);
  options_.logger = &logger;
  config_.run.output_directory = (root_ / "resumed").string();
  config_.run.checkpoint = result->checkpoint_path.string();
  auto resumed = Make();
  ASSERT_TRUE(resumed.ok()) << resumed.status();

  ASSERT_TRUE((*resumed)->Run({}).ok());

  const auto text = log.str();
  for (const auto* line :
       {"[INFO] Resumed at iteration 1 (accepted 0, rejected 1)\n",
        "[DEBUG] Iteration 2/2: parent initial from island 0, target island ",
        "[INFO] Iteration 2/2 rejected: No valid diffs found in response (", "best program initial with score 0.0\n"}) {
    EXPECT_NE(text.find(line), std::string::npos) << line << "\n" << text;
  }
  EXPECT_EQ(text.find("Evaluating initial program"), std::string::npos);
}

TEST_F(ControllerTest, EarlyStoppingPreservesPatienceAcrossResume) {
  config_.early_stopping_patience = 2;
  score_ = [](const auto&) { return Metrics{{"combined_score", 1.0}}; };
  config_.max_iterations = 2;
  auto controller = Make();
  ASSERT_TRUE(controller.ok()) << controller.status();

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  config_.run.output_directory = (root_ / "resumed").string();
  config_.max_iterations = 5;
  config_.run.checkpoint = result->checkpoint_path.string();
  auto resumed = Make();
  ASSERT_TRUE(resumed.ok());

  auto continued = (*resumed)->Run({});
  ASSERT_TRUE(continued.ok()) << continued.status();

  EXPECT_EQ(continued->last_iteration, 3);
  EXPECT_EQ(continued->iterations_executed, 1);
  EXPECT_EQ(continued->stop_reason, StopReason::kEarlyStopping);
}

TEST_F(ControllerTest, TargetAlreadyReachedBySeedNeedsNoModelCall) {
  config_.max_iterations = 10;
  config_.run.target_score = 0;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->stop_reason, StopReason::kTargetScore);
  EXPECT_EQ(result->last_iteration, 0);
  EXPECT_EQ(calls_, 0);
}

TEST_F(ControllerTest, TargetStopsAtActualIterationAndSavesThatCheckpoint) {
  config_.max_iterations = 10;
  config_.run.target_score = 3;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->stop_reason, StopReason::kTargetScore);
  EXPECT_EQ(result->last_iteration, 3);
  EXPECT_EQ(result->checkpoint_path.filename(), "checkpoint_3");
  EXPECT_EQ(calls_, 3);
  EXPECT_FALSE(fs::exists(Output() / "checkpoints/checkpoint_10"));
}

TEST_F(ControllerTest, DeclinedAdmissionDoesNotAdvanceGenerationOrStopping) {
  config_.early_stopping_patience = 0;
  config_.convergence_threshold = 1;
  options_.population_strategy.admit = [](const auto& snapshot, const auto&, int) -> absl::StatusOr<bool> {
    return snapshot.programs.empty();
  };

  config_.max_iterations = 3;
  config_.run.target_score = 1;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->stop_reason, StopReason::kIterationLimit);
  EXPECT_EQ(result->last_iteration, 3);
  EXPECT_EQ(result->accepted_iterations, 0);
  EXPECT_EQ(result->rejected_iterations, 3);
  EXPECT_EQ(evaluations_, 4);
  EXPECT_EQ((*controller)->database().size(), 1u);
  EXPECT_EQ((*controller)->database().Snapshot()->generations,
            std::vector<std::int64_t>(config_.database.num_islands, 0));
}

TEST_F(ControllerTest, CustomSelectorUsesZeroBasedIterationsAndFallbackParentIsland) {
  config_.database.num_islands = 3;
  config_.database.migration_interval = 1;
  int selections = 0;
  int migrations = 0;
  options_.island_selector = [&](const auto& context) -> absl::StatusOr<int> {
    EXPECT_EQ(context.iteration, selections++);
    EXPECT_EQ(context.pending_counts, (std::vector<int>{0, 0, 0}));
    return 2;
  };
  options_.population_strategy.migrate = [&](const auto& snapshot) -> absl::StatusOr<std::vector<MigrationMove>> {
    ++migrations;
    EXPECT_EQ(snapshot.generations[2], migrations);
    return std::vector<MigrationMove>{{"initial", 1}};
  };

  config_.max_iterations = 2;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(selections, 2);
  EXPECT_EQ(migrations, 2);

  const auto snapshot = (*controller)->database().Snapshot();
  ASSERT_TRUE(snapshot.ok());
  EXPECT_EQ(snapshot->generations, (std::vector<std::int64_t>{0, 0, 2}));
  EXPECT_FALSE(snapshot->islands[1].empty());
  EXPECT_EQ(snapshot->programs.at("iteration-1").parent_id, "initial");
  EXPECT_EQ(snapshot->programs.at("iteration-1").metadata["island"], 2);
  EXPECT_EQ(snapshot->last_migration_generation, 2);
}

TEST_F(ControllerTest, MigrationErrorRollsBackEntireAttempt) {
  options_.population_strategy.migration_due = [](const auto&) -> absl::StatusOr<bool> { return true; };
  options_.population_strategy.migrate = [](const auto& snapshot) -> absl::StatusOr<std::vector<MigrationMove>> {
    EXPECT_TRUE(snapshot.programs.count("iteration-1"));
    return absl::UnavailableError("injected migration failure");
  };

  config_.max_iterations = 3;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  EXPECT_EQ(result.status().code(), absl::StatusCode::kUnavailable);

  EXPECT_EQ(calls_, 1);
  EXPECT_EQ((*controller)->database().size(), 1u);
  EXPECT_EQ((*controller)->database().Snapshot()->last_iteration, 0);
  EXPECT_EQ((*controller)->database().Snapshot()->generations,
            std::vector<std::int64_t>(config_.database.num_islands, 0));
  EXPECT_FALSE(fs::exists(Output() / "best"));
}

TEST_F(ControllerTest, ModelFailureStopsWithoutRetryOrPartialChild) {
  generate_ = [](const auto&) -> absl::StatusOr<LLMResponse> { return absl::UnavailableError("offline failure"); };

  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  EXPECT_EQ(result.status().code(), absl::StatusCode::kUnavailable);

  EXPECT_EQ(calls_, 1);
  EXPECT_EQ(evaluations_, 1);
  EXPECT_EQ((*controller)->database().size(), 1u);
}

TEST_F(ControllerTest, EventStoppingUsesExactEqualityAndMissingMetricNumericAverage) {
  config_.early_stopping_patience = -1;
  config_.early_stopping_metric = "missing";
  config_.convergence_threshold = 2;
  score_ = [](const auto& code) {
    return Metrics{{"combined_score", std::stod(code)}, {"flag", true}, {"label", "text"}};
  };

  config_.max_iterations = 10;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->stop_reason, StopReason::kEarlyStopping);
  EXPECT_EQ(result->last_iteration, 2);

  config_.run.output_directory = (root_ / "resumed").string();
  config_.run.checkpoint = result->checkpoint_path.string();
  auto resumed = Make();
  ASSERT_TRUE(resumed.ok());

  auto continued = (*resumed)->Run({});
  ASSERT_TRUE(continued.ok()) << continued.status();

  EXPECT_EQ(continued->stop_reason, StopReason::kEarlyStopping);
  EXPECT_EQ(continued->iterations_executed, 0);
  EXPECT_EQ(calls_, 2);
}

TEST_F(ControllerTest, NonnumericPresentStoppingMetricIsIgnored) {
  config_.early_stopping_patience = 1;
  config_.early_stopping_metric = "label";
  score_ = [](const auto& code) { return Metrics{{"combined_score", std::stod(code)}, {"label", "fixed"}}; };

  config_.max_iterations = 3;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->stop_reason, StopReason::kIterationLimit);
}

TEST_F(ControllerTest, MetricsWithoutCombinedScoreFailBeforePopulationInitialization) {
  score_ = [](const auto&) { return Metrics{{"flag", true}, {"label", "text"}}; };
  auto controller = Make();
  ASSERT_TRUE(controller.ok());
  EXPECT_FALSE((*controller)->Run({"0"}).ok());
  EXPECT_EQ(calls_, 0);
  EXPECT_EQ((*controller)->database().size(), 0);
}

TEST_F(ControllerTest, IntegerImprovementsKeepPrecisionBeyondDoubleRange) {
  config_.early_stopping_patience = 1;
  config_.convergence_threshold = 1;
  score_ = [](const auto& code) {
    return Metrics{{"combined_score", std::uint64_t{9007199254740991} + std::stoull(code)}};
  };

  config_.max_iterations = 3;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->stop_reason, StopReason::kIterationLimit);
  EXPECT_EQ(result->last_iteration, 3);
}

TEST_F(ControllerTest, StopRequestFinishesCurrentAttemptAndCheckpoints) {
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  generate_ = [&](const auto&) -> absl::StatusOr<LLMResponse> {
    (*controller)->RequestStop();
    return LLMResponse{"1", "offline", "fake", std::nullopt};
  };

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->stop_reason, StopReason::kRequested);
  EXPECT_EQ(result->last_iteration, 1);
  EXPECT_EQ(evaluations_, 2);
  EXPECT_TRUE(fs::exists(result->checkpoint_path / "checkpoint.json"));
}

TEST_F(ControllerTest, IntegerStoppingHandlesSignsFlagsAndSixtyFiveBitDeltas) {
  struct Case {
    Metrics previous;
    Metrics current;
    double threshold;
    bool improved;
  };
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const std::vector<Case> cases = {{minimum, minimum + 1, 1, true},
                                   {minimum + 1, minimum, 1, false},
                                   {maximum - 1, maximum, 1, true},
                                   {maximum, maximum - 1, 0, false},
                                   {false, true, 1, true},
                                   {true, false, -1, true},
                                   {std::int64_t{2}, std::uint64_t{3}, 1, true},
                                   {minimum, maximum, 0x1p64, true},
                                   {maximum, minimum, -0x1p64, false},
                                   {minimum, maximum, 0x1p65, false},
                                   {maximum, minimum, -0x1p65, true}};

  config_.early_stopping_patience = 1;
  config_.early_stopping_metric = "progress";
  int index = 0;

  for (const auto& test : cases) {
    SCOPED_TRACE(index);
    calls_ = 0;
    config_.run.output_directory = (root_ / std::to_string(index++)).string();
    config_.convergence_threshold = test.threshold;
    score_ = [&](const auto& code) {
      return Metrics{{"combined_score", 1}, {"progress", code == "2" ? test.current : test.previous}};
    };

    config_.max_iterations = 2;
    auto controller = Make();
    ASSERT_TRUE(controller.ok());

    auto result = (*controller)->Run({"0"});
    ASSERT_TRUE(result.ok()) << result.status();

    EXPECT_EQ(result->stop_reason, test.improved ? StopReason::kIterationLimit : StopReason::kEarlyStopping);
  }
}

TEST_F(ControllerTest, UsesConfiguredBudgetLanguageAndOutputSuffix) {
  config_.max_iterations = 1;
  config_.language = "cpp";
  config_.file_suffix = ".cc";

  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->last_iteration, 1);
  EXPECT_EQ(result->best.language, "cpp");
  EXPECT_EQ(Read(Output() / "best/best_program.cc"), "1");

  const auto initial = (*controller)->database().Get("initial");
  ASSERT_TRUE(initial.ok()) << initial.status();
  EXPECT_EQ(initial->language, "cpp");
}

TEST_F(ControllerTest, RequiresConfiguredLanguageBeforeCreatingController) {
  for (const auto& language : std::vector<std::optional<std::string>>{std::nullopt, ""}) {
    config_.language = language;

    auto controller = Make();
    EXPECT_EQ(controller.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_NE(std::string(controller.status().message()).find("language"), std::string::npos);
  }

  EXPECT_EQ(calls_, 0);
  EXPECT_EQ(evaluations_, 0);
  EXPECT_FALSE(fs::exists(Output()));
}

TEST_F(ControllerTest, FailedFinalExportStillLeavesAUsableCheckpoint) {
  fs::create_directories(Output());
  Write(Output() / "best", "occupied");

  config_.max_iterations = 1;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition);

  config_.run.checkpoint = (Output() / "checkpoints/checkpoint_1").string();
  config_.run.output_directory = (root_ / "resumed").string();
  config_.max_iterations = 0;
  auto resumed = Make();
  ASSERT_TRUE(resumed.ok());

  auto recovered = (*resumed)->Run({});
  ASSERT_TRUE(recovered.ok()) << recovered.status();

  EXPECT_EQ(recovered->best.code, "1");
  EXPECT_EQ(recovered->last_iteration, 1);
  EXPECT_EQ(evaluations_, 2);
}

TEST_F(ControllerTest, ResumeBudgetOverflowFailsWithoutModelCalls) {
  config_.max_iterations = 0;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok());

  const auto state_path = result->checkpoint_path / "checkpoint.json";
  auto state = ReadJson(state_path);
  state["progress"]["iteration"] = std::numeric_limits<std::int64_t>::max();
  state["progress"]["rejected"] = std::numeric_limits<std::int64_t>::max();
  Write(state_path, state.dump());

  config_.max_iterations = 1;
  config_.run.checkpoint = result->checkpoint_path.string();
  auto resumed = Make();
  ASSERT_TRUE(resumed.ok());

  EXPECT_EQ((*resumed)->Run({}).status().code(), absl::StatusCode::kOutOfRange);
  EXPECT_EQ(calls_, 0);
  EXPECT_EQ(evaluations_, 1);
}

TEST_F(ControllerTest, ExtremeFiniteAverageProducesResumableCheckpoint) {
  config_.early_stopping_patience = 3;
  config_.early_stopping_metric = "missing";
  score_ = [](const auto&) {
    const auto maximum = std::numeric_limits<double>::max();
    return Metrics{{"combined_score", maximum}, {"a", maximum}, {"b", maximum}, {"c", maximum}};
  };

  config_.max_iterations = 2;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  const auto state = ReadJson(result->checkpoint_path / "checkpoint.json");
  EXPECT_TRUE(state["progress"]["best_score"].is_number());
  EXPECT_EQ(state["progress"]["best_score"], std::numeric_limits<double>::max());

  config_.max_iterations = 0;
  config_.run.checkpoint = result->checkpoint_path.string();
  auto resumed = Make();
  ASSERT_TRUE(resumed.ok());

  auto continued = (*resumed)->Run({});
  EXPECT_TRUE(continued.ok()) << continued.status();
}

TEST_F(ControllerTest, SourceFilenameCannotCollideWithBestMetadata) {
  for (const auto& suffix : {"_info.json", "_INFO.JSON"}) {
    config_.file_suffix = suffix;
    EXPECT_EQ(Make().status().code(), absl::StatusCode::kInvalidArgument);
  }
}

TEST_F(ControllerTest, FiniteAverageRetainsSmallResidualAfterLargeCancellation) {
  config_.early_stopping_patience = 0;
  config_.early_stopping_metric = "missing";
  config_.convergence_threshold = 0;
  score_ = [](const auto&) {
    const auto maximum = std::numeric_limits<double>::max();
    return Metrics{{"combined_score", 0}, {"a", maximum}, {"b", -maximum}, {"c", 1e-100}};
  };

  config_.max_iterations = 2;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->stop_reason, StopReason::kIterationLimit);
  EXPECT_EQ(result->last_iteration, 2);
}

TEST_F(ControllerTest, StopBeforeRunAvoidsAllDependencies) {
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  (*controller)->RequestStop();

  EXPECT_EQ((*controller)->Run({"0"}).status().code(), absl::StatusCode::kCancelled);
  EXPECT_EQ(calls_, 0);
  EXPECT_EQ(evaluations_, 0);
  EXPECT_FALSE(fs::exists(Output()));
}

TEST_F(ControllerTest, PortableCheckpointRestoresArtifactsAfterOriginalStorageRemoved) {
  config_.database.artifact_size_threshold = 0;
  config_.max_iterations = 2;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  const auto moved = root_ / "moved";
  fs::rename(result->checkpoint_path, moved);
  fs::remove_all(root_ / "artifacts");

  config_.run.output_directory = (root_ / "resumed").string();
  config_.run.checkpoint = moved.string();
  config_.max_iterations = 1;
  auto resumed = Make();
  ASSERT_TRUE(resumed.ok());

  auto continued = (*resumed)->Run({});
  ASSERT_TRUE(continued.ok()) << continued.status();

  EXPECT_EQ(continued->last_iteration, 3);
  EXPECT_EQ(evaluations_, 4);

  auto artifacts = (*resumed)->database().GetArtifacts("initial");
  ASSERT_TRUE(artifacts.ok()) << artifacts.status();
  EXPECT_EQ(std::get<ArtifactBytes>(artifacts->at("binary")), (ArtifactBytes{0, 255}));
  EXPECT_TRUE(fs::exists(continued->checkpoint_path / "database/CURRENT"));
}

TEST_F(ControllerTest, ResumeRestoresDatabaseSamplingSequence) {
  config_.database.num_islands = 1;
  config_.database.exploration_ratio = 1;
  config_.database.exploitation_ratio = 0;
  config_.max_iterations = 4;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  auto expected = (*controller)->database().Get("iteration-4");
  ASSERT_TRUE(expected.ok());

  config_.run.checkpoint = (Output() / "checkpoints/checkpoint_2").string();
  config_.run.output_directory = (root_ / "resumed").string();
  calls_ = 2;
  config_.max_iterations = 2;
  auto resumed = Make();
  ASSERT_TRUE(resumed.ok());

  auto continued = (*resumed)->Run({});
  ASSERT_TRUE(continued.ok()) << continued.status();

  auto actual = (*resumed)->database().Get("iteration-4");
  ASSERT_TRUE(actual.ok());
  EXPECT_EQ(actual->parent_id, expected->parent_id);
  EXPECT_EQ(actual->generation, expected->generation);
  EXPECT_EQ(actual->prompts, expected->prompts);
  EXPECT_EQ(actual->metrics, expected->metrics);
}

TEST_F(ControllerTest, InvalidCheckpointStateNeverMutatesDatabaseOrCallsDependencies) {
  config_.max_iterations = 1;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  const auto state_path = result->checkpoint_path / "checkpoint.json";
  const auto state = ReadJson(state_path);
  const std::vector<std::pair<std::string, Metrics>> corruptions = {
      {"schema_version", 1.0},
      {"schema_version", 2},
      {"iteration", -1},
      {"iteration", std::numeric_limits<std::uint64_t>::max()},
      {"accepted", 2},
      {"accepted", 0},
      {"rejected", 2},
      {"early_stopped", "false"},
      {"without_improvement", 1},
      {"best_score", "1.0"}};

  for (const auto& [key, value] : corruptions) {
    SCOPED_TRACE(key + ": " + value.dump());
    auto corrupt = state;
    if (key == "schema_version")
      corrupt[key] = value;
    else
      corrupt["progress"][key] = value;
    Write(state_path, corrupt.dump());

    config_.run.checkpoint = result->checkpoint_path.string();
    auto resumed = Make();
    ASSERT_TRUE(resumed.ok());

    auto continued = (*resumed)->Run({});
    EXPECT_EQ(continued.status().code(), absl::StatusCode::kDataLoss);
    EXPECT_EQ((*resumed)->database().size(), 0u);
  }

  EXPECT_EQ(calls_, 1);
  EXPECT_EQ(evaluations_, 2);
}

TEST_F(ControllerTest, RejectsMismatchedBestAndInvalidEvaluatedPrograms) {
  config_.max_iterations = 0;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());
  auto saved = (*controller)->Run({"0"});
  ASSERT_TRUE(saved.ok());
  const auto manifest_path = saved->checkpoint_path / "checkpoint.json";
  const auto original = ReadJson(manifest_path);
  auto metadata = original;
  metadata["program"]["best_id"] = "missing";
  Write(manifest_path, metadata.dump());
  config_.run.checkpoint = saved->checkpoint_path.string();
  auto resumed = Make();
  ASSERT_TRUE(resumed.ok());
  EXPECT_EQ((*resumed)->Run({}).status().code(), absl::StatusCode::kDataLoss);
  EXPECT_EQ((*resumed)->database().size(), 0);
  Write(manifest_path, original.dump());
  auto data = Checkpoint::Load(saved->checkpoint_path / "database");
  ASSERT_TRUE(data.ok());
  for (int mode = 0; mode < 3; ++mode) {
    auto corrupted = *data;
    if (mode == 0) corrupted.programs[0].metrics["combined_score"] = false;
    if (mode == 1) corrupted.programs[0].metrics.erase("combined_score");
    if (mode == 2) corrupted.programs[0].language = "cpp";
    ASSERT_TRUE(Checkpoint::Save(saved->checkpoint_path / "database", corrupted, config_.database).ok());
    resumed = Make();
    ASSERT_TRUE(resumed.ok());
    EXPECT_EQ((*resumed)->Run({}).status().code(), absl::StatusCode::kDataLoss);
    EXPECT_EQ((*resumed)->database().size(), 0);
  }
  EXPECT_EQ(calls_, 0);
  EXPECT_EQ(evaluations_, 1);
}

TEST_F(ControllerTest, FailedPublicationPreservesPreviousCheckpoint) {
  config_.database.artifact_size_threshold = 0;
  config_.checkpoint_interval = 1;
  config_.max_iterations = 2;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());
  generate_ = [&](const auto&) -> absl::StatusOr<LLMResponse> {
    if (calls_ == 2) {
      const auto seed = (*controller)->database().Get("initial");
      if (!seed.ok() || !seed->artifact_dir) return absl::InternalError("test seed artifact missing");
      fs::remove_all(*seed->artifact_dir);
    }
    return LLMResponse{std::to_string(calls_), "offline", "fake", std::nullopt};
  };
  EXPECT_FALSE((*controller)->Run({"0"}).ok());
  const auto complete = Output() / "checkpoints/checkpoint_1";
  ASSERT_TRUE(fs::exists(complete / "checkpoint.json"));
  EXPECT_FALSE(fs::exists(Output() / "checkpoints/checkpoint_2"));
  for (const auto& entry : fs::directory_iterator(Output() / "checkpoints")) {
    EXPECT_EQ(entry.path(), complete);
  }
  config_.run.output_directory = (root_ / "resumed").string();
  config_.run.checkpoint = complete.string();
  config_.max_iterations = 0;
  auto resumed = Make();
  ASSERT_TRUE(resumed.ok());
  auto result = (*resumed)->Run({});
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(result->last_iteration, 1);
  EXPECT_EQ(result->best.metrics["combined_score"], 1);
}

TEST_F(ControllerTest, RejectsMissingArtifactsWithoutPartiallyRestoringDatabase) {
  config_.database.artifact_size_threshold = 0;
  config_.max_iterations = 0;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());
  auto saved = (*controller)->Run({"0"});
  ASSERT_TRUE(saved.ok());
  auto data = Checkpoint::Load(saved->checkpoint_path / "database");
  ASSERT_TRUE(data.ok());
  ASSERT_TRUE(data->programs[0].artifact_dir);
  fs::remove_all(*data->programs[0].artifact_dir);
  config_.run.checkpoint = saved->checkpoint_path.string();
  auto resumed = Make();
  ASSERT_TRUE(resumed.ok());
  EXPECT_EQ((*resumed)->Run({}).status().code(), absl::StatusCode::kDataLoss);
  EXPECT_EQ((*resumed)->database().size(), 0);
  EXPECT_EQ(evaluations_, 1);
  EXPECT_EQ(calls_, 0);
}

TEST_F(ControllerTest, ChangedStoppingPolicyCannotSilentlyResetResumePatience) {
  config_.max_iterations = 0;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok());

  config_.early_stopping_patience = 3;
  config_.run.checkpoint = result->checkpoint_path.string();
  auto resumed = Make();
  ASSERT_TRUE(resumed.ok());

  EXPECT_EQ((*resumed)->Run({}).status().code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_EQ(evaluations_, 1);
}

TEST_F(ControllerTest, CheckpointCollisionPreservesPreviouslyPublishedState) {
  config_.max_iterations = 2;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok());

  const auto before = Read(result->checkpoint_path / "checkpoint.json");
  auto conflicting = Make();
  ASSERT_TRUE(conflicting.ok());

  auto failure = (*conflicting)->Run({"0"});
  EXPECT_EQ(failure.status().code(), absl::StatusCode::kAlreadyExists);
  EXPECT_EQ(Read(result->checkpoint_path / "checkpoint.json"), before);
}

TEST_F(ControllerTest, InvalidOutputPathReturnsErrorInsteadOfSuccessfulRun) {
  fs::create_directories(Output());
  Write(Output() / "checkpoints", "occupied");
  config_.max_iterations = 0;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition);

  EXPECT_FALSE(fs::exists(Output() / "best"));
  EXPECT_EQ(Read(Output() / "checkpoints"), "occupied");
}

TEST_F(ControllerTest, DisablingPromptLoggingLeavesProgramPromptsEmpty) {
  config_.database.log_prompts = false;
  config_.max_iterations = 1;
  auto controller = Make();
  ASSERT_TRUE(controller.ok());

  auto result = (*controller)->Run({"0"});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_FALSE(result->best.prompts);
}

TEST_F(ControllerTest, InvalidSettingsAndUnsupportedModesFailBeforeRun) {
  config_.checkpoint_interval = 0;
  EXPECT_EQ(Make().status().code(), absl::StatusCode::kInvalidArgument);

  config_.checkpoint_interval = 1;
  config_.max_iterations = -1;
  EXPECT_EQ(Make().status().code(), absl::StatusCode::kInvalidArgument);

  config_.max_iterations = 1;
  config_.file_suffix = "../escape";
  EXPECT_EQ(Make().status().code(), absl::StatusCode::kInvalidArgument);

  config_.file_suffix = ".py";
  config_.evolution_trace.enabled = true;
  EXPECT_EQ(Make().status().code(), absl::StatusCode::kUnimplemented);

  EXPECT_EQ(calls_, 0);
  EXPECT_EQ(evaluations_, 0);
}

TEST_F(ControllerTest, InvalidSettingsNameTheOffendingField) {
  const auto original = config_;
  const std::vector<std::pair<std::string, std::function<void(Config&)>>> cases = {
      {"convergence_threshold", [](Config& c) { c.convergence_threshold = std::numeric_limits<double>::infinity(); }},
      {"early_stopping_metric",
       [](Config& c) {
         c.early_stopping_patience = 1;
         c.early_stopping_metric = "";
       }},
      {"file_suffix", [](Config& c) { c.file_suffix = "../escape"; }},
      {"file_suffix", [](Config& c) { c.file_suffix = "_INFO.JSON"; }}};
  for (const auto& [field, mutate] : cases) {
    SCOPED_TRACE(field);
    config_ = original;
    mutate(config_);
    const auto controller = Make();
    EXPECT_EQ(controller.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_NE(std::string(controller.status().message()).find(field), std::string::npos) << controller.status();
  }

  config_ = original;
  config_.run.output_directory.clear();
  const auto controller = Make();
  EXPECT_EQ(controller.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(std::string(controller.status().message()).find("output_directory"), std::string::npos);
}

TEST_F(ControllerTest, InvalidRunInputsFailBeforeEvaluation) {
  for (const auto& initial : std::vector<InitialProgram>{{""}, {" \u3000"}, {std::string(1, '\xff')}}) {
    auto controller = Make();
    ASSERT_TRUE(controller.ok());

    EXPECT_EQ((*controller)->Run(initial).status().code(), absl::StatusCode::kInvalidArgument);
  }

  // An invalid budget or target score is a configuration error caught at creation.
  const auto original = config_;
  config_.max_iterations = -1;
  EXPECT_EQ(Make().status().code(), absl::StatusCode::kInvalidArgument);
  config_ = original;
  config_.run.target_score = std::numeric_limits<double>::infinity();
  const auto controller = Make();
  EXPECT_EQ(controller.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(std::string(controller.status().message()).find("run.target_score"), std::string::npos);

  EXPECT_EQ(evaluations_, 0);
}

}  // namespace
}  // namespace ievolve::controller
