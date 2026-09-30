#include "ievolve/cli/cli.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <sstream>

#include "gtest/gtest.h"
#include "ievolve/utils/yaml.h"

namespace ievolve::cli {
namespace {
namespace fs = std::filesystem;

class OfflineLLM : public LLMInterface {
 public:
  explicit OfflineLLM(int& calls) : calls_(calls) {}
  absl::StatusOr<LLMResponse> Generate(const LLMRequest&) const override {
    return LLMResponse{"score = " + std::to_string(++calls_), "offline", "fake", LLMUsage{8, 4, 0, 0.0}};
  }

 private:
  int& calls_;
};

class CallbackLLM : public LLMInterface {
 public:
  explicit CallbackLLM(std::function<absl::StatusOr<LLMResponse>(const LLMRequest&)> callback)
      : callback_(std::move(callback)) {}
  absl::StatusOr<LLMResponse> Generate(const LLMRequest& request) const override { return callback_(request); }

 private:
  std::function<absl::StatusOr<LLMResponse>(const LLMRequest&)> callback_;
};

TEST(CLIArgumentsTest, ParsesConfigurationAliasesEqualsAndPathsLiterally) {
  for (const auto& args : std::vector<std::vector<std::string>>{
           {"-c", "config file.yaml"}, {"--config", "config file.yaml"}, {"--config=config file.yaml"}}) {
    auto parsed = ParseArguments(args);
    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ(parsed->config_file, fs::path("config file.yaml"));
  }
}

TEST(CLIArgumentsTest, RejectsMissingMalformedDuplicateAndUnknownOptions) {
  for (const auto& args : std::vector<std::vector<std::string>>{{},
                                                                {"--unknown"},
                                                                {"--config"},
                                                                {"--config="},
                                                                {"--config", ""},
                                                                {"--config", "--help"},
                                                                {"-c", "one.yaml", "--config=two.yaml"},
                                                                {"initial.py", "evaluator.py"},
                                                                {"--", "file.yaml"},
                                                                {"--config", std::string("nul\0.yaml", 9)}}) {
    EXPECT_EQ(ParseArguments(args).status().code(), absl::StatusCode::kInvalidArgument);
  }
}

TEST(CLIArgumentsTest, HelpAndVersionNeedNoFiles) {
  auto help = ParseArguments({"--help"});
  ASSERT_TRUE(help.ok());
  EXPECT_TRUE(help->help);

  auto version = ParseArguments({"--version"});
  ASSERT_TRUE(version.ok());
  EXPECT_TRUE(version->version);
}

TEST(CLIArgumentsTest, RejectsEveryFormerRuntimeOverride) {
  for (const std::string flag :
       {"--output", "--iterations", "--target-score", "--log-level", "--checkpoint", "--python", "--provider",
        "--model", "--codex-executable", "--claude-executable", "-o", "-i", "-t", "-l"}) {
    SCOPED_TRACE(flag);
    EXPECT_FALSE(ParseArguments({"--config", "file.yaml", flag, "1"}).ok());
  }
}

class CLITest : public testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> sequence{0};
    root_ = fs::temp_directory_path() /
            ("ievolve-cli-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(sequence++));
    fs::create_directory(root_);

    initial_ = root_ / "initial program.py";
    evaluator_ = root_ / "evaluation.py";
    config_ = root_ / "config.yaml";

    Write(initial_, "score = 0\n");
    Write(evaluator_,
          "import runpy\nfrom types import SimpleNamespace\n"
          "def evaluate(path):\n"
          "    score = runpy.run_path(path)['score']\n"
          "    return SimpleNamespace(metrics={'combined_score': score}, "
          "artifacts={'result': str(score), 'binary': b'\\x00\\xff'})\n");

    WriteConfig(
        "max_iterations: 2\ncheckpoint_interval: 1\n"
        "diff_based_evolution: false\n"
        "evaluator:\n  cascade_evaluation: false\n  max_retries: 0\n"
        "llm:\n  models:\n    - provider: codex\n      name: offline\n");

    runtime_.llm_factory = [this](const LLMModelConfig& model) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
      selected_.push_back(model);
      return std::make_shared<OfflineLLM>(calls_);
    };
  }
  void TearDown() override {
    std::error_code error;
    fs::remove_all(root_, error);
  }
  std::vector<std::string> Args() const { return {"--config", config_.string()}; }
  void SaveConfig(const nlohmann::ordered_json& document) {
    auto yaml = utils::EmitYaml(document);
    ASSERT_TRUE(yaml.ok()) << yaml.status();
    Write(config_, *yaml);
  }
  void WriteConfig(const std::string& yaml) {
    auto document = utils::ParseYaml(yaml);
    ASSERT_TRUE(document.ok()) << document.status();
    (*document)["run"] = {{"initial_program", initial_.filename().string()},
                          {"evaluation_file", fs::relative(evaluator_, root_).string()},
                          {"output_directory", "output"},
                          {"python_executable", IEVOLVE_PYTHON_EXECUTABLE}};
    if (!document->contains("max_iterations")) (*document)["max_iterations"] = 1;
    SaveConfig(*document);
  }
  void Configure(const nlohmann::ordered_json& patch) {
    auto document = utils::ReadYaml(config_);
    ASSERT_TRUE(document.ok()) << document.status();
    document->merge_patch(patch);
    SaveConfig(*document);
  }
  static void Write(const fs::path& path, const std::string& content) {
    std::ofstream stream(path);
    stream << content;
    stream.close();
    ASSERT_TRUE(stream.good());
  }
  static Metrics Json(const fs::path& path) {
    std::ifstream stream(path);
    return Metrics::parse(stream);
  }
  fs::path root_, initial_, evaluator_, config_;
  RuntimeOptions runtime_;
  std::vector<LLMModelConfig> selected_;
  int calls_ = 0;
  std::ostringstream output_, errors_;
};

TEST_F(CLITest, RunsConfiguredPipelineAndPrintsActualResult) {
  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  EXPECT_EQ(calls_, 2);

  const auto info = Json(root_ / "output/best/best_program_info.json");
  EXPECT_EQ(info["metrics"]["combined_score"], 2);
  EXPECT_EQ(info["current_iteration"], 2);
  EXPECT_NE(output_.str().find("checkpoint_2"), std::string::npos);
  EXPECT_NE(output_.str().find("combined_score"), std::string::npos);
}

TEST_F(CLITest, YamlAloneConfiguresRunFromAnotherDirectory) {
  Configure({{"run", {{"output_directory", "yaml-output"}, {"target_score", 1}}}});

  ASSERT_EQ(ievolve::cli::Run({"--config", config_.string()}, output_, errors_, runtime_), 0) << errors_.str();

  EXPECT_EQ(calls_, 1);
  EXPECT_EQ(Json(root_ / "yaml-output/best/best_program_info.json")["current_iteration"], 1);
}

TEST(CLIArgumentsTest, RejectsRuntimeOverridesWhenConfigurationIsSelected) {
  EXPECT_FALSE(ParseArguments({"initial.py", "evaluator.py", "--config", "config.yaml", "--iterations", "1"}).ok());
}

TEST_F(CLITest, BaselineNeedsNoConfiguredModelOrAuthentication) {
  Configure({{"max_iterations", 0}, {"llm", nullptr}, {"run", {{"output_directory", "baseline"}}}});

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_), 0) << errors_.str();

  EXPECT_EQ(Json(root_ / "baseline/best/best_program_info.json")["metrics"]["combined_score"], 0);
}

TEST_F(CLITest, ResumeDoesNotReadOrEvaluateInitialProgram) {
  Configure({{"max_iterations", 1}});
  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  fs::remove(initial_);
  Configure({{"run", {{"checkpoint", "output/checkpoints/checkpoint_1"}, {"output_directory", "resumed"}}}});

  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  EXPECT_EQ(calls_, 2);
  EXPECT_EQ(Json(root_ / "resumed/best/best_program_info.json")["current_iteration"], 2);
}

TEST_F(CLITest, YamlSelectsProviderModelAndSeed) {
  Configure({{"random_seed", 123},
             {"max_iterations", 1},
             {"llm", {{"models", {{{"provider", "claude_code"}, {"name", "chosen"}}}}}}});

  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  ASSERT_EQ(selected_.size(), 1u);
  EXPECT_EQ(selected_[0].provider, "claude_code");
  EXPECT_EQ(selected_[0].name, "chosen");
  EXPECT_EQ(selected_[0].random_seed, 123);
}

TEST_F(CLITest, GlobalSeedFillsGenerationAndFeedbackModelsWithoutOverwriting) {
  WriteConfig(
      "random_seed: 123\ndiff_based_evolution: false\n"
      "evaluator:\n  cascade_evaluation: false\n  use_llm_feedback: true\n"
      "llm:\n  models:\n"
      "    - {provider: codex, name: generator}\n"
      "    - {provider: codex, name: seeded_generator, random_seed: 0}\n"
      "  evaluator_models:\n"
      "    - {provider: codex, name: reviewer}\n"
      "    - {provider: codex, name: seeded_reviewer, random_seed: 9}\n");

  runtime_.llm_factory = [&](const LLMModelConfig& model) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
    selected_.push_back(model);
    const bool feedback = model.name->find("reviewer") != std::string::npos;
    return std::make_shared<CallbackLLM>([feedback](const auto&) -> absl::StatusOr<LLMResponse> {
      return LLMResponse{feedback ? "{\"quality\": 0.5}" : "score = 1", "offline", "fake", std::nullopt};
    });
  };

  auto args = Args();
  Configure({{"max_iterations", 1}});

  ASSERT_EQ(ievolve::cli::Run(args, output_, errors_, runtime_), 0) << errors_.str();

  ASSERT_EQ(selected_.size(), 4u);
  EXPECT_EQ(selected_[0].random_seed, 123);
  EXPECT_EQ(selected_[1].random_seed, 0);
  EXPECT_EQ(selected_[2].random_seed, 123);
  EXPECT_EQ(selected_[3].random_seed, 9);
}

TEST_F(CLITest, SharedModelSeedAndNullGlobalSeedRemainEffective) {
  for (const bool shared_seed : {false, true}) {
    SCOPED_TRACE(shared_seed);
    WriteConfig(std::string("random_seed: ") + (shared_seed ? "123" : "null") +
                "\ndiff_based_evolution: false\n"
                "evaluator: {cascade_evaluation: false}\nllm:\n" +
                (shared_seed ? "  random_seed: 17\n" : "") + "  models: [{provider: codex}]\n");

    selected_.clear();
    auto args = Args();
    Configure({{"run", {{"output_directory", shared_seed ? "shared" : "unseeded"}}}});
    Configure({{"max_iterations", 1}});

    ASSERT_EQ(ievolve::cli::Run(args, output_, errors_, runtime_), 0) << errors_.str();

    ASSERT_EQ(selected_.size(), 1u);
    EXPECT_EQ(selected_[0].random_seed, shared_seed ? std::optional<std::int64_t>(17) : std::nullopt);
  }
}

TEST_F(CLITest, GlobalSeedMakesMultiModelSelectionRepeatable) {
  WriteConfig(
      "random_seed: 123\nmax_iterations: 12\ndiff_based_evolution: false\n"
      "evaluator: {cascade_evaluation: false}\n"
      "llm:\n  models:\n"
      "    - {provider: codex, name: a}\n"
      "    - {provider: codex, name: b}\n");

  std::string sequence;
  runtime_.llm_factory = [&](const LLMModelConfig& model) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
    return std::make_shared<CallbackLLM>([&, name = *model.name](const auto&) -> absl::StatusOr<LLMResponse> {
      sequence += name;
      return LLMResponse{"score = " + std::to_string(++calls_), name, "offline", std::nullopt};
    });
  };

  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  const auto first = sequence;
  ASSERT_EQ(first.size(), 12u);

  sequence.clear();
  calls_ = 0;
  auto args = Args();
  Configure({{"run", {{"output_directory", "repeat"}}}});

  ASSERT_EQ(ievolve::cli::Run(args, output_, errors_, runtime_), 0) << errors_.str();

  EXPECT_EQ(sequence, first);
}

TEST_F(CLITest, MissingFilesFailBeforeConstructingModel) {
  fs::remove(initial_);

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 1);

  EXPECT_TRUE(selected_.empty());
  EXPECT_EQ(calls_, 0);
  EXPECT_FALSE(errors_.str().empty());
}

TEST_F(CLITest, ExecutableHelpWorksWithoutRuntimeDependencies) {
  auto result = process::RunProcess({{IEVOLVE_COMMAND_PATH, "--help"}});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->exit_code, 0);
  EXPECT_NE(result->stdout_text.find("--config"), std::string::npos);
  EXPECT_NE(result->stdout_text.find("run.python_executable"), std::string::npos);
}

TEST_F(CLITest, RelativeTemplatesResolveAgainstConfigurationDirectory) {
  const auto templates = root_ / "custom_templates";
  fs::create_directory(templates);
  Write(templates / "system_message.txt", "CUSTOM SYSTEM FROM CONFIG DIRECTORY");
  std::ofstream(config_, std::ios::app) << "prompt:\n  template_dir: custom_templates\n";

  runtime_.llm_factory = [&](const auto&) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
    return std::make_shared<CallbackLLM>([&](const auto& request) -> absl::StatusOr<LLMResponse> {
      EXPECT_EQ(request.system_message, "CUSTOM SYSTEM FROM CONFIG DIRECTORY");

      return LLMResponse{"score = 1", "offline", "fake", std::nullopt};
    });
  };

  auto args = Args();
  Configure({{"max_iterations", 1}});

  EXPECT_EQ(ievolve::cli::Run(args, output_, errors_, runtime_), 0) << errors_.str();
}

TEST_F(CLITest, FeedbackModelsInheritYamlGenerationModels) {
  WriteConfig(
      "evaluator:\n  cascade_evaluation: false\n  use_llm_feedback: true\n"
      "llm:\n  models:\n    - provider: codex\n      name: old\n");

  runtime_.llm_factory = [&](const LLMModelConfig& model) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
    selected_.push_back(model);
    return std::make_shared<CallbackLLM>([](const auto&) -> absl::StatusOr<LLMResponse> {
      return LLMResponse{"{\"quality\": 0.5}", "offline", "fake", std::nullopt};
    });
  };

  auto args = Args();
  Configure({{"max_iterations", 0}, {"llm", {{"models", {{{"provider", "claude_code"}, {"name", "chosen"}}}}}}});

  ASSERT_EQ(ievolve::cli::Run(args, output_, errors_, runtime_), 0) << errors_.str();

  ASSERT_EQ(selected_.size(), 1u);
  EXPECT_EQ(selected_[0].provider, "claude_code");
  EXPECT_EQ(selected_[0].name, "chosen");

  EXPECT_TRUE(Json(root_ / "output/best/best_program_info.json")["metrics"].contains("llm_quality"));
}

TEST_F(CLITest, ExplicitYamlFeedbackModelsRemainIndependent) {
  WriteConfig(
      "evaluator:\n  cascade_evaluation: false\n  use_llm_feedback: true\n"
      "llm:\n  models:\n    - provider: codex\n      name: old\n"
      "  evaluator_models:\n    - provider: codex\n      name: reviewer\n");

  auto args = Args();
  Configure({{"max_iterations", 0}, {"llm", {{"models", {{{"provider", "claude_code"}, {"name", "chosen"}}}}}}});

  ASSERT_EQ(ievolve::cli::Run(args, output_, errors_, runtime_), 0) << errors_.str();

  ASSERT_EQ(selected_.size(), 1u);
  EXPECT_EQ(selected_[0].provider, "codex");
  EXPECT_EQ(selected_[0].name, "reviewer");
}

TEST_F(CLITest, MissingModelsAreConfigurationErrorsBeforeGeneration) {
  Configure({{"llm", nullptr}});

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2);
  EXPECT_TRUE(selected_.empty());
  EXPECT_EQ(calls_, 0);
}

TEST_F(CLITest, InvalidRunSettingsFailBeforeConstructingDependencies) {
  const auto original = utils::ReadYaml(config_);
  ASSERT_TRUE(original.ok()) << original.status();

  for (const auto& run : std::vector<nlohmann::ordered_json>{
           nullptr,
           nlohmann::ordered_json::array(),
           {{"initial_program", "initial program.py"}},
           {{"evaluation_file", "evaluation.py"}},
           {{"initial_program", "initial program.py"}, {"evaluation_file", "evaluation.py"}, {"iterations", 2}}}) {
    auto document = *original;
    document["run"] = run;
    SaveConfig(document);
    EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2) << errors_.str();
  }

  for (const auto& patch : std::vector<nlohmann::ordered_json>{{{"initial_program", ""}},
                                                               {{"evaluation_file", 42}},
                                                               {{"output_directory", false}},
                                                               {{"checkpoint", ""}},
                                                               {{"python_executable", ""}},
                                                               {{"codex_executable", nullptr}},
                                                               {{"claude_executable", std::string("nul\0path", 8)}},
                                                               {{"target_score", "nan"}},
                                                               {{"target_score", true}},
                                                               {{"target_score", nlohmann::ordered_json::array()}}}) {
    auto document = *original;
    document["run"].update(patch);
    SaveConfig(document);
    EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2) << errors_.str();
  }

  for (const auto& patch : std::vector<nlohmann::ordered_json>{{{"max_iterations", -1}},
                                                               {{"max_iterations", 1.5}},
                                                               {{"max_iterations", 2147483648LL}},
                                                               {{"log_level", "verbose"}}}) {
    auto document = *original;
    document.update(patch);
    SaveConfig(document);
    EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2) << errors_.str();
  }

  EXPECT_TRUE(selected_.empty());
  EXPECT_FALSE(fs::exists(root_ / "output"));
}

TEST_F(CLITest, YamlLogLevelSuppressesProgressMessages) {
  Configure({{"log_level", "ERROR"}});

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();
  EXPECT_TRUE(errors_.str().empty());
}

TEST_F(CLITest, ProgressMessagesUseLogLevelsFromYaml) {
  Configure({{"max_iterations", 0}});

  for (const std::string level : {"DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"}) {
    SCOPED_TRACE(level);
    Configure({{"log_level", level}, {"run", {{"output_directory", "output-" + level}}}});
    errors_.str("");

    ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

    if (level == "DEBUG" || level == "INFO") {
      EXPECT_NE(errors_.str().find("[INFO] Starting evolution (iterations=0,"), std::string::npos);
      EXPECT_NE(errors_.str().find("[INFO] Initial program score "), std::string::npos);
    } else {
      EXPECT_TRUE(errors_.str().empty());
    }
  }
}

TEST_F(CLITest, CriticalLogThresholdStillReportsRunFailures) {
  Configure({{"log_level", "CRITICAL"}, {"llm", nullptr}});

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2);
  EXPECT_NE(errors_.str().find("Error:"), std::string::npos);
  EXPECT_NE(errors_.str().find("Configure llm.models"), std::string::npos);
}

TEST_F(CLITest, DefaultOutputDirectoryIsRelativeToConfiguration) {
  Configure({{"max_iterations", 0}, {"run", {{"output_directory", nullptr}}}});

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_), 0) << errors_.str();
  EXPECT_TRUE(fs::exists(root_ / "ievolve_output/best/best_program.py"));
}

TEST_F(CLITest, LanguageAndSuffixAreInferredAndPreservedOnResume) {
  const auto cpp = root_ / "initial.cc";
  fs::rename(initial_, cpp);
  initial_ = cpp;
  Configure({{"run", {{"initial_program", "initial.cc"}}}});
  auto args = Args();
  Configure({{"max_iterations", 1}});

  ASSERT_EQ(ievolve::cli::Run(args, output_, errors_, runtime_), 0) << errors_.str();

  EXPECT_TRUE(fs::exists(root_ / "output/best/best_program.cc"));
  EXPECT_EQ(Json(root_ / "output/best/best_program_info.json")["language"], "cpp");

  Configure({{"language", "cpp"},
             {"run",
              {{"initial_program", nullptr},
               {"checkpoint", "output/checkpoints/checkpoint_1"},
               {"output_directory", "resumed"}}}});

  ASSERT_EQ(ievolve::cli::Run(args, output_, errors_, runtime_), 0) << errors_.str();

  EXPECT_TRUE(fs::exists(root_ / "resumed/best/best_program.cc"));
  EXPECT_EQ(Json(root_ / "resumed/best/best_program_info.json")["language"], "cpp");
}

TEST_F(CLITest, MatchingLanguagePreservesExplicitFileSuffix) {
  fs::rename(initial_, root_ / "initial.cc");
  Configure({{"language", "cpp"},
             {"file_suffix", ".cpp"},
             {"max_iterations", 0},
             {"run", {{"initial_program", "initial.cc"}}}});

  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  EXPECT_TRUE(fs::exists(root_ / "output/best/best_program.cpp"));
  EXPECT_EQ(Json(root_ / "output/best/best_program_info.json")["language"], "cpp");

  Configure({{"language", nullptr},
             {"file_suffix", ".cxx"},
             {"run",
              {{"initial_program", nullptr},
               {"checkpoint", "output/checkpoints/checkpoint_0"},
               {"output_directory", "resumed"}}}});

  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();
  EXPECT_TRUE(fs::exists(root_ / "resumed/best/best_program.cxx"));
  EXPECT_EQ(Json(root_ / "resumed/best/best_program_info.json")["language"], "cpp");
}

TEST_F(CLITest, OutputSuffixMustMatchInferredLanguageBeforeStarting) {
  for (const std::string suffix : {".code", ".rs", "", "py"}) {
    SCOPED_TRACE(suffix);
    Configure({{"file_suffix", suffix}});
    errors_.str("");

    EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2) << errors_.str();
    EXPECT_NE(errors_.str().find("file_suffix"), std::string::npos);
    EXPECT_TRUE(selected_.empty());
    EXPECT_FALSE(fs::exists(root_ / "output"));
  }
}

TEST_F(CLITest, CheckpointMetadataMustMatchInferredSuffixLanguage) {
  Configure({{"max_iterations", 0}});
  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  const auto info = root_ / "output/checkpoints/checkpoint_0/best_program_info.json";
  auto metadata = Json(info);
  metadata["language"] = "cpp";
  Write(info, metadata.dump());
  Configure({{"max_iterations", 1},
             {"run", {{"checkpoint", "output/checkpoints/checkpoint_0"}, {"output_directory", "resumed"}}}});
  errors_.str("");

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 1) << errors_.str();
  EXPECT_NE(errors_.str().find("language"), std::string::npos);
  EXPECT_TRUE(selected_.empty());
  EXPECT_FALSE(fs::exists(root_ / "resumed"));
}

TEST_F(CLITest, CheckpointSuffixMustIdentifyLanguage) {
  Configure({{"max_iterations", 0}});
  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  const auto checkpoint = root_ / "output/checkpoints/checkpoint_0";
  auto program = checkpoint / "best_program.py";
  Configure({{"max_iterations", 1},
             {"file_suffix", ".py"},
             {"run", {{"checkpoint", checkpoint.string()}, {"output_directory", "resumed"}}}});

  for (const std::string suffix : {".code", ""}) {
    SCOPED_TRACE(suffix);
    const auto renamed = checkpoint / ("best_program" + suffix);
    fs::rename(program, renamed);
    program = renamed;
    errors_.str("");

    EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2) << errors_.str();
    EXPECT_NE(errors_.str().find("language"), std::string::npos);
    EXPECT_TRUE(selected_.empty());
    EXPECT_FALSE(fs::exists(root_ / "resumed"));
  }
}

TEST_F(CLITest, CheckpointRequiresExactlyOneBestProgramFile) {
  Configure({{"max_iterations", 0}});
  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  const auto checkpoint = root_ / "output/checkpoints/checkpoint_0";
  fs::remove(checkpoint / "best_program.py");
  Configure({{"max_iterations", 1}, {"run", {{"checkpoint", checkpoint.string()}, {"output_directory", "resumed"}}}});

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 1) << errors_.str();
  EXPECT_TRUE(selected_.empty());
  EXPECT_FALSE(fs::exists(root_ / "resumed"));

  Write(checkpoint / "best_program", "score = 0\n");
  Write(checkpoint / "best_program.py", "score = 0\n");
  errors_.str("");

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 1) << errors_.str();
  EXPECT_NE(errors_.str().find("Ambiguous"), std::string::npos);
  EXPECT_TRUE(selected_.empty());
  EXPECT_FALSE(fs::exists(root_ / "resumed"));
}

TEST_F(CLITest, ConfiguredLanguageMustMatchInferenceExactlyBeforeStarting) {
  for (const std::string language : {"cpp", "Python", "python ", ""}) {
    SCOPED_TRACE(language);
    Configure({{"language", language}});
    errors_.str("");

    EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2) << errors_.str();
    EXPECT_NE(errors_.str().find("language"), std::string::npos);
    EXPECT_TRUE(selected_.empty());
    EXPECT_FALSE(fs::exists(root_ / "output"));
  }
}

TEST_F(CLITest, UnknownExtensionCannotBypassLanguageInference) {
  const auto unknown = root_ / "initial.unknown";
  fs::rename(initial_, unknown);
  Configure({{"language", "python"}, {"run", {{"initial_program", "initial.unknown"}}}});

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2) << errors_.str();
  EXPECT_NE(errors_.str().find("language"), std::string::npos);
  EXPECT_TRUE(selected_.empty());
  EXPECT_FALSE(fs::exists(root_ / "output"));
}

TEST_F(CLITest, ConfiguredLanguageMustMatchCheckpointBeforeStarting) {
  Configure({{"max_iterations", 0}});
  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  fs::remove(initial_);
  Configure({{"language", "cpp"},
             {"max_iterations", 1},
             {"run", {{"checkpoint", "output/checkpoints/checkpoint_0"}, {"output_directory", "resumed"}}}});
  errors_.str("");

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2) << errors_.str();
  EXPECT_NE(errors_.str().find("language"), std::string::npos);
  EXPECT_TRUE(selected_.empty());
  EXPECT_FALSE(fs::exists(root_ / "resumed"));
}

TEST_F(CLITest, InvalidYamlAndInvalidEvaluatorFailBeforeGeneration) {
  WriteConfig("llm: []\n");
  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2);
  EXPECT_TRUE(selected_.empty());

  WriteConfig(
      "evaluator:\n  cascade_evaluation: false\nllm:\n  models:\n    - "
      "provider: codex\n");
  Write(evaluator_, "raise ImportError('bad evaluator')\n");

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 1);
  EXPECT_EQ(calls_, 0);
}

TEST_F(CLITest, RuntimeModelFailureIsReportedWithoutSuccessOutput) {
  runtime_.llm_factory = [](const auto&) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
    return std::make_shared<CallbackLLM>(
        [](const auto&) -> absl::StatusOr<LLMResponse> { return absl::UnavailableError("injected model failure"); });
  };

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 1);

  EXPECT_EQ(output_.str(), "");
  EXPECT_NE(errors_.str().find("injected model failure"), std::string::npos);
}

TEST_F(CLITest, StopFlagBeforeRunMakesNoDependencyCalls) {
  std::atomic<int> stopped{SIGINT};
  runtime_.stop_signal = &stopped;

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 130);

  EXPECT_TRUE(selected_.empty());
  EXPECT_FALSE(fs::exists(root_ / "output"));
}

#ifndef _WIN32
TEST_F(CLITest, PythonExecutablePathsResolveAgainstYamlAndBareNamesUsePath) {
  const auto binaries = root_ / "bin tools";
  fs::create_directory(binaries);
  fs::create_symlink(IEVOLVE_PYTHON_EXECUTABLE, binaries / "python-fixture");

  fs::create_directory(root_ / "scripts");
  fs::rename(evaluator_, root_ / "scripts/evaluate.py");
  evaluator_ = root_ / "scripts/evaluate.py";

  const std::vector<std::string> executables = {"./bin tools/python-fixture", "bin tools/python-fixture",
                                                (binaries / "python-fixture").string(), "python-fixture"};

  for (std::size_t i = 0; i < executables.size(); ++i) {
    SCOPED_TRACE(executables[i]);
    const auto output = root_ / ("python-" + std::to_string(i));
    process::ProcessRequest request;
    Configure({{"max_iterations", 0},
               {"run",
                {{"evaluation_file", "scripts/evaluate.py"},
                 {"python_executable", executables[i]},
                 {"output_directory", output.string()}}}});
    request.argv = {"/usr/bin/env", "PATH=" + binaries.string(), IEVOLVE_COMMAND_PATH, "--config", config_.string()};
    request.working_directory = fs::temp_directory_path();
    request.timeout = std::chrono::seconds(15);

    auto result = process::RunProcess(request);
    ASSERT_TRUE(result.ok()) << result.status();

    EXPECT_EQ(result->exit_code, 0) << result->stderr_text;
    EXPECT_TRUE(fs::exists(output / "checkpoints/checkpoint_0/controller.json"));
  }
}

TEST_F(CLITest, RelativeModelExecutablesResolveAgainstYamlDirectory) {
  const auto binaries = root_ / "bin tools";
  fs::create_directory(binaries);

  for (const std::string provider : {"codex", "claude_code"}) {
    SCOPED_TRACE(provider);
    const std::string reply = provider == "codex" ? "{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_"
                                                    "message\","
                                                    "\"text\":\"score = 1\"}}\n{\"type\":\"turn.completed\"}"
                                                  : "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
                                                    "\"result\":\"score = 1\"}";

    const auto executable = binaries / provider;
    Write(executable, "#!/bin/sh\n/bin/cat >/dev/null\nprintf '%s\\n' '" + reply + "'\n");
    fs::permissions(executable, fs::perms::owner_all);

    const auto output = root_ / provider;
    process::ProcessRequest request;
    Configure({{"max_iterations", 1},
               {"llm", {{"models", {{{"provider", provider}}}}}},
               {"run",
                {{"output_directory", output.string()},
                 {provider == "codex" ? "codex_executable" : "claude_executable", "./bin tools/" + provider}}}});
    request.argv = {IEVOLVE_COMMAND_PATH, "--config", config_.string()};
    request.working_directory = fs::temp_directory_path();
    request.timeout = std::chrono::seconds(15);

    auto result = process::RunProcess(request);
    ASSERT_TRUE(result.ok()) << result.status();

    ASSERT_EQ(result->exit_code, 0) << result->stderr_text;
    EXPECT_EQ(Json(output / "best/best_program_info.json")["metrics"]["combined_score"], 1);
  }
}

TEST_F(CLITest, ExecutableInterruptSavesSeedCheckpointAndReturnsSignalExitCode) {
  // Signal this test's separate CLI subprocess from its evaluator child.
  Write(evaluator_,
        "import os, signal, time\ndef evaluate(path):\n"
        "    os.kill(os.getppid(), signal.SIGINT)\n"
        "    time.sleep(0.1)\n"
        "    return {'combined_score': 0}\n");

  process::ProcessRequest request;
  request.argv = Args();
  // A signal regression must fail offline, never invoke an installed model.
  Configure({{"run", {{"codex_executable", "./no-codex"}}}});
  request.argv.insert(request.argv.begin(), IEVOLVE_COMMAND_PATH);
  request.timeout = std::chrono::seconds(10);

  auto result = process::RunProcess(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->exit_code, 130) << result->stderr_text;
  EXPECT_NE(result->stdout_text.find("Stop reason: requested"), std::string::npos);
  EXPECT_NE(result->stderr_text.find("[WARNING] Received SIGINT; stopping after the current step"), std::string::npos)
      << result->stderr_text;
  EXPECT_NE(result->stderr_text.find("[INFO] Stop requested; finishing the run"), std::string::npos);
  EXPECT_TRUE(fs::exists(root_ / "output/checkpoints/checkpoint_0/controller.json"));
}
#endif

}  // namespace
}  // namespace ievolve::cli
