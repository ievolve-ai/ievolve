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
    return LLMResponse{"print(" + std::to_string(++calls_) + ")", "offline", "fake", LLMUsage{8, 4, 0, 0.0}};
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

    project_ = root_ / "project";
    fs::create_directory(project_);
    initial_ = project_ / "initial program.py";
    evaluator_ = root_ / "evaluation.py";
    config_ = root_ / "config.yaml";

    Write(initial_, "print(0)\n");
    Write(evaluator_, R"py(
import json, subprocess, sys
request = json.load(sys.stdin)
try:
    run = subprocess.run(request['candidate']['argv'], cwd=request['candidate']['cwd'], capture_output=True, text=True, timeout=5)
except OSError as error:
    print(error, file=sys.stderr)
    sys.exit(2)
if run.returncode:
    json.dump({'protocol_version': 1, 'passed': False, 'reason': 'candidate_failed', 'metrics': {}}, sys.stdout)
else:
    score = float(run.stdout.strip())
    json.dump({'protocol_version': 1, 'passed': True, 'metrics': {'combined_score': score},
               'artifacts': {'result': str(score), 'binary': {'__bytes__': 'AP8='}}}, sys.stdout)
)py");

    WriteConfig(
        "max_iterations: 2\ncheckpoint_interval: 1\n"
        "diff_based_evolution: false\n"
        "evaluator: {}\n"
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
    (*document)["run"] = {
        {"project_dir", "project"}, {"evolve_files", {initial_.filename().string()}}, {"output_directory", "output"}};
    (*document)["candidate"]["run"]["argv"] = {IEVOLVE_PYTHON_EXECUTABLE, "{source}/" + initial_.filename().string()};
    if (!document->contains("evaluator") || (*document)["evaluator"].is_null())
      (*document)["evaluator"] = nlohmann::ordered_json::object();
    (*document)["evaluator"]["source"] = fs::relative(evaluator_, root_).string();
    (*document)["evaluator"]["run"]["argv"] = {IEVOLVE_PYTHON_EXECUTABLE, "{source}"};
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
  fs::path root_, project_, initial_, evaluator_, config_;
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
  // max_iterations is the total, so resuming at iteration 1 runs one more.
  Configure({{"max_iterations", 2},
             {"run", {{"checkpoint", "output/checkpoints/checkpoint_1"}, {"output_directory", "resumed"}}}});

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
      "evaluator:\n  use_llm_feedback: true\n"
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
      return LLMResponse{feedback ? "{\"quality\": 0.5}" : "print(1)", "offline", "fake", std::nullopt};
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
                "evaluator: {}\nllm:\n" +
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
      "evaluator: {}\n"
      "llm:\n  models:\n"
      "    - {provider: codex, name: a}\n"
      "    - {provider: codex, name: b}\n");

  std::string sequence;
  runtime_.llm_factory = [&](const LLMModelConfig& model) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
    return std::make_shared<CallbackLLM>([&, name = *model.name](const auto&) -> absl::StatusOr<LLMResponse> {
      sequence += name;
      return LLMResponse{"print(" + std::to_string(++calls_) + ")", name, "offline", std::nullopt};
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

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2);

  EXPECT_TRUE(selected_.empty());
  EXPECT_EQ(calls_, 0);
  EXPECT_NE(errors_.str().find("run.evolve_files"), std::string::npos) << errors_.str();
}

TEST_F(CLITest, GlobMatchingNothingIsAConfigurationError) {
  Configure({{"run", {{"evolve_files", {initial_.filename().string(), "src/**/*.cpp"}}}}});

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2);

  EXPECT_NE(errors_.str().find("run.evolve_files"), std::string::npos) << errors_.str();
  EXPECT_NE(errors_.str().find("[1]"), std::string::npos) << errors_.str();
  EXPECT_EQ(errors_.str().find("src/**/*.cpp"), std::string::npos) << errors_.str();
  EXPECT_TRUE(selected_.empty());
  EXPECT_FALSE(fs::exists(root_ / "output"));
}

TEST_F(CLITest, ProjectDirectoryIsRequiredAndMustExist) {
  for (const auto& project : {nlohmann::ordered_json(nullptr), nlohmann::ordered_json("absent")}) {
    SCOPED_TRACE(project.dump());
    Configure({{"run", {{"project_dir", project}}}});
    errors_.str("");

    EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2);
    EXPECT_NE(errors_.str().find("run.project_dir"), std::string::npos) << errors_.str();
  }
  EXPECT_TRUE(selected_.empty());
  EXPECT_FALSE(fs::exists(root_ / "output"));
}

// Every evaluation copies project_dir, so output written inside it would be
// copied into later evaluations.
TEST_F(CLITest, OutputDirectoryMustNotBeInsideProjectDirectory) {
  for (const std::string output : {"project", "project/output", "./project/../project/nested/output"}) {
    SCOPED_TRACE(output);
    Configure({{"run", {{"output_directory", output}}}});
    errors_.str("");

    EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2);
    EXPECT_NE(errors_.str().find("run.output_directory"), std::string::npos) << errors_.str();
  }
  EXPECT_TRUE(selected_.empty());
  EXPECT_EQ(std::distance(fs::directory_iterator(project_), fs::directory_iterator()), 1);

  Configure({{"max_iterations", 0}, {"run", {{"output_directory", "project-output"}}}});
  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();
}

TEST_F(CLITest, ExecutableHelpWorksWithoutRuntimeDependencies) {
  auto result = process::RunProcess({{IEVOLVE_COMMAND_PATH, "--help"}});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->exit_code, 0);
  EXPECT_NE(result->stdout_text.find("--config"), std::string::npos);
  for (const auto* key : {"run:", "candidate:", "evaluator:", "llm:", "prompt:", "database:", "[required]"}) {
    EXPECT_NE(result->stdout_text.find(key), std::string::npos) << key;
  }
}

TEST_F(CLITest, RelativeTemplatesResolveAgainstConfigurationDirectory) {
  const auto templates = root_ / "custom_templates";
  fs::create_directory(templates);
  Write(templates / "system_message.txt", "CUSTOM SYSTEM FROM CONFIG DIRECTORY");
  std::ofstream(config_, std::ios::app) << "prompt:\n  template_dir: custom_templates\n";

  runtime_.llm_factory = [&](const auto&) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
    return std::make_shared<CallbackLLM>([&](const auto& request) -> absl::StatusOr<LLMResponse> {
      EXPECT_EQ(request.system_message, "CUSTOM SYSTEM FROM CONFIG DIRECTORY");

      return LLMResponse{"print(1)", "offline", "fake", std::nullopt};
    });
  };

  auto args = Args();
  Configure({{"max_iterations", 1}});

  EXPECT_EQ(ievolve::cli::Run(args, output_, errors_, runtime_), 0) << errors_.str();
}

TEST_F(CLITest, FeedbackModelsInheritYamlGenerationModels) {
  WriteConfig(
      "evaluator:\n  use_llm_feedback: true\n"
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
      "evaluator:\n  use_llm_feedback: true\n"
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
           {{"evaluation_file", "evaluation.py"}},
           {{"initial_program", "initial program.py"}, {"evaluation_file", "evaluation.py"}, {"iterations", 2}}}) {
    auto document = *original;
    document["run"] = run;
    SaveConfig(document);
    EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2) << errors_.str();
  }

  for (const auto& patch : std::vector<nlohmann::ordered_json>{{{"initial_program", "initial program.py"}},
                                                               {{"project_dir", ""}},
                                                               {{"project_dir", nullptr}},
                                                               {{"evolve_files", nlohmann::ordered_json::array()}},
                                                               {{"evolve_files", {"../initial program.py"}}},
                                                               {{"evolve_files", "initial program.py"}},
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
  EXPECT_TRUE(fs::exists(root_ / "ievolve_output/best/best_program/initial program.py"));
}

// language and file_suffix are gone: each file's language follows its suffix.
TEST_F(CLITest, RemovedLanguageSettingsAreIgnored) {
  Configure({{"max_iterations", 0}, {"language", "cpp"}, {"file_suffix", ".cc"}});

  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  EXPECT_TRUE(fs::exists(root_ / "output/best/best_program/initial program.py"));
  EXPECT_FALSE(Json(root_ / "output/best/best_program_info.json").contains("language"));
}

TEST_F(CLITest, ResumeRequiresTheCheckpointEvolveFiles) {
  Configure({{"max_iterations", 0}});
  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  // The same file, named by a different pattern, is still a different set.
  Configure({{"max_iterations", 1},
             {"run",
              {{"evolve_files", {"*.py"}},
               {"checkpoint", "output/checkpoints/checkpoint_0"},
               {"output_directory", "resumed"}}}});
  errors_.str("");

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2) << errors_.str();
  EXPECT_NE(errors_.str().find("run.evolve_files"), std::string::npos) << errors_.str();
  EXPECT_TRUE(selected_.empty());
  EXPECT_FALSE(fs::exists(root_ / "resumed"));
}

TEST_F(CLITest, SingleFileVersionOneCheckpointCannotBeResumed) {
  Configure({{"max_iterations", 0}});
  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  const auto manifest = root_ / "output/checkpoints/checkpoint_0/checkpoint.json";
  auto state = Json(manifest);
  state["schema_version"] = 1;
  state["program"] = {{"language", "python"}, {"file_suffix", ".py"}, {"best_id", "initial"}};
  Write(manifest, state.dump());
  Configure({{"max_iterations", 1},
             {"run", {{"checkpoint", "output/checkpoints/checkpoint_0"}, {"output_directory", "resumed"}}}});
  errors_.str("");

  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 1) << errors_.str();
  EXPECT_NE(errors_.str().find("schema_version"), std::string::npos) << errors_.str();
  EXPECT_TRUE(selected_.empty());
  EXPECT_FALSE(fs::exists(root_ / "resumed"));
}

// Two evolved files and a helper that is copied but never shown to or edited
// by the model. One diff response edits both evolved files.
TEST_F(CLITest, EvolvesAMultiFileProjectWithACrossFileDiff) {
  fs::remove(initial_);
  fs::create_directory(project_ / "solver");
  Write(project_ / "main.py", "from solver import a, b\nprint(a.value() + b.value())\n");
  Write(project_ / "solver/a.py", "def value():\n    return 1\n");
  Write(project_ / "solver/b.py", "def value():\n    return 2\n");
  Configure({{"max_iterations", 1},
             {"diff_based_evolution", true},
             {"run", {{"evolve_files", {"solver/*.py"}}}},
             {"candidate", {{"run", {{"argv", {IEVOLVE_PYTHON_EXECUTABLE, "{source}/main.py"}}}}}}});

  std::string prompt;
  runtime_.llm_factory = [&](const auto&) -> absl::StatusOr<std::shared_ptr<LLMInterface>> {
    return std::make_shared<CallbackLLM>([&](const LLMRequest& request) -> absl::StatusOr<LLMResponse> {
      ++calls_;
      for (const auto& message : request.messages) prompt += message.content;
      return LLMResponse{
          "solver/a.py\n<<<<<<< SEARCH\n    return 1\n=======\n    return 10\n>>>>>>> REPLACE\n\n"
          "solver/b.py\n<<<<<<< SEARCH\n    return 2\n=======\n    return 20\n>>>>>>> REPLACE\n",
          "offline", "fake", std::nullopt};
    });
  };

  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();

  EXPECT_EQ(calls_, 1);
  EXPECT_NE(prompt.find("solver/a.py"), std::string::npos);
  EXPECT_NE(prompt.find("solver/b.py"), std::string::npos);
  EXPECT_EQ(prompt.find("main.py"), std::string::npos);
  const auto info = Json(root_ / "output/best/best_program_info.json");
  EXPECT_EQ(info["id"], "iteration-1");
  EXPECT_EQ(info["metrics"]["combined_score"], 30);
  const auto best = root_ / "output/best/best_program";
  std::ifstream a(best / "solver/a.py"), b(best / "solver/b.py");
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(a), {}), "def value():\n    return 10\n");
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(b), {}), "def value():\n    return 20\n");
  EXPECT_FALSE(fs::exists(best / "main.py"));
  // The base project itself is never modified.
  std::ifstream original(project_ / "solver/a.py");
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(original), {}), "def value():\n    return 1\n");
}

TEST_F(CLITest, RestoresWithoutInitialOrBestExport) {
  Configure({{"max_iterations", 0}});
  ASSERT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();
  const auto checkpoint = root_ / "output/checkpoints/checkpoint_0";
  EXPECT_FALSE(fs::exists(checkpoint / "best_program"));
  fs::remove_all(root_ / "output/best");
  fs::remove(initial_);
  Configure({{"max_iterations", 1}, {"run", {{"checkpoint", checkpoint.string()}, {"output_directory", "resumed"}}}});
  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 0) << errors_.str();
  EXPECT_EQ(calls_, 1);
  EXPECT_EQ(Json(root_ / "resumed/best/best_program_info.json")["metrics"]["combined_score"], 1);
}

TEST_F(CLITest, InvalidYamlAndInvalidEvaluatorFailBeforeGeneration) {
  WriteConfig("llm: []\n");
  EXPECT_EQ(ievolve::cli::Run(Args(), output_, errors_, runtime_), 2);
  EXPECT_TRUE(selected_.empty());

  WriteConfig(
      "evaluator:\nllm:\n  models:\n    - "
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
               {"run", {{"output_directory", output.string()}}},
               {"candidate", {{"run", {{"argv", {executables[i], "{source}/initial program.py"}}}}}},
               {"evaluator", {{"source", "scripts/evaluate.py"}, {"run", {{"argv", {executables[i], "{source}"}}}}}}});
    request.argv = {"/usr/bin/env", "PATH=" + binaries.string(), IEVOLVE_COMMAND_PATH, "--config", config_.string()};
    request.working_directory = fs::temp_directory_path();
    request.timeout = std::chrono::seconds(15);

    auto result = process::RunProcess(request);
    ASSERT_TRUE(result.ok()) << result.status();

    EXPECT_EQ(result->exit_code, 0) << result->stderr_text;
    EXPECT_TRUE(fs::exists(output / "checkpoints/checkpoint_0/checkpoint.json"));
  }
}

TEST_F(CLITest, RelativeModelExecutablesResolveAgainstYamlDirectory) {
  const auto binaries = root_ / "bin tools";
  fs::create_directory(binaries);

  for (const std::string provider : {"codex", "claude_code"}) {
    SCOPED_TRACE(provider);
    const std::string reply = provider == "codex" ? "{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_"
                                                    "message\","
                                                    "\"text\":\"print(1)\"}}\n{\"type\":\"turn.completed\"}"
                                                  : "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
                                                    "\"result\":\"print(1)\"}";

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
        "import os, signal, time, json, sys\n"
        "json.load(sys.stdin)\nos.kill(os.getppid(), signal.SIGINT)\ntime.sleep(0.1)\n"
        "json.dump({'protocol_version':1,'passed':True,'metrics':{'combined_score':0}}, sys.stdout)\n");

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
  EXPECT_TRUE(fs::exists(root_ / "output/checkpoints/checkpoint_0/checkpoint.json"));
}
#endif

}  // namespace
}  // namespace ievolve::cli
