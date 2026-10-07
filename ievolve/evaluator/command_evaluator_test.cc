#include "ievolve/evaluator/command_evaluator.h"

#include <fstream>
#include <future>
#include <thread>

#ifndef _WIN32
#include <unistd.h>

#include <csignal>
#endif

#include "gtest/gtest.h"
#include "ievolve/utils/file.h"

namespace ievolve::evaluator {
namespace {
std::filesystem::path Data(const char* file) { return std::filesystem::path(IEVOLVE_EVALUATOR_TEST_DATA_DIR) / file; }
EvaluatorConfig PythonJudge() {
  EvaluatorConfig config;
  config.source = Data("cli_evaluator.py").string();
  config.run.argv = {IEVOLVE_PYTHON_EXECUTABLE, "{source}"};
  return config;
}
CandidateConfig PythonCandidate() {
  CandidateConfig config;
  config.run.argv = {IEVOLVE_PYTHON_EXECUTABLE, "{source}"};
  return config;
}
CommandEvaluatorOptions Options() { return {std::filesystem::current_path(), process::RunProcess}; }
Metrics Passing() { return {{"protocol_version", 1}, {"passed", true}, {"metrics", {{"combined_score", -1}}}}; }

TEST(CommandEvaluatorTest, BuildsFixedEvaluatorOnceAndEachCandidateWithScopedCommands) {
  auto workspace = EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  auto source = (*workspace)->WriteSource("code", ".cc");
  ASSERT_TRUE(source.ok());
  CandidateConfig candidate;
  candidate.build = BuildCommandConfig{{"compile", "{source}", "{workspace}/candidate"}};
  candidate.run.argv = {"{workspace}/candidate", "arg with spaces"};
  EvaluatorConfig evaluator;
  evaluator.source = Data("cli_evaluator.py").string();
  evaluator.build = BuildCommandConfig{{"compile", "{source}", "{workspace}/judge"}};
  evaluator.run.argv = {"{workspace}/judge"};
  std::vector<process::ProcessRequest> requests;
  auto options = Options();
  options.runner = [&](const process::ProcessRequest& request) -> absl::StatusOr<process::ProcessResult> {
    requests.push_back(request);
    return process::ProcessResult{0, request.stdin_text.empty() ? "" : Passing().dump(), "diagnostic"};
  };
  std::filesystem::path fixed_workspace;
  {
    auto backend = CommandEvaluator::Create(candidate, evaluator, options);
    ASSERT_TRUE(backend.ok()) << backend.status();
    ASSERT_EQ(requests.size(), 1u);
    fixed_workspace = *requests[0].working_directory;
    EXPECT_NE(fixed_workspace, (*workspace)->path());
    EXPECT_EQ(requests[0].argv[1], *evaluator.source);
    for (int i = 0; i < 2; ++i) {
      auto result = (*backend)->Run(*source);
      ASSERT_TRUE(result.ok()) << result.status();
      EXPECT_TRUE(result->passed);
    }
    ASSERT_EQ(requests.size(), 5u);
    for (const auto& request : requests) EXPECT_TRUE(request.cleanup_group_on_exit);
    EXPECT_EQ(requests[1].argv[1], source->string());
    EXPECT_EQ(requests[2].argv[0], (fixed_workspace / "judge").string());
    EXPECT_EQ(requests[2].working_directory, (*workspace)->path());
    const auto request = Metrics::parse(requests[2].stdin_text);
    EXPECT_EQ(request["candidate"]["argv"],
              (std::vector<std::string>{((*workspace)->path() / "candidate").string(), "arg with spaces"}));
    EXPECT_EQ(request["candidate"]["cwd"], (*workspace)->path().string());
    EXPECT_EQ(request["protocol_version"], 1);
    EXPECT_EQ(request["limits"]["evaluation_timeout_ms"], 300000);
  }
  EXPECT_FALSE(std::filesystem::exists(fixed_workspace));
}

TEST(CommandEvaluatorTest, ValidatesAllTemplatesBeforeAnyBuildAndAllowsPrebuiltJudge) {
  auto candidate = PythonCandidate();
  EvaluatorConfig evaluator;
  evaluator.run.argv = {"prebuilt"};
  auto options = Options();
  int calls = 0;
  options.runner = [&](const auto&) -> absl::StatusOr<process::ProcessResult> {
    ++calls;
    return process::ProcessResult{};
  };
  EXPECT_TRUE(CommandEvaluator::Create(candidate, evaluator, options).ok());
  evaluator.build = BuildCommandConfig{{"compile"}};
  evaluator.run.argv.push_back("{source}");
  EXPECT_FALSE(CommandEvaluator::Create(candidate, evaluator, options).ok());
  evaluator.run.argv = {"prebuilt"};
  candidate.run.argv = {"{bad}"};
  EXPECT_FALSE(CommandEvaluator::Create(candidate, evaluator, options).ok());
  EXPECT_EQ(calls, 0);
}

TEST(CommandEvaluatorTest, DistinguishesCandidateRejectionsFromInfrastructureErrors) {
  auto workspace = EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  auto source = (*workspace)->WriteSource("code", ".cpp");
  ASSERT_TRUE(source.ok());
  for (int mode = 0; mode < 7; ++mode) {
    auto candidate = PythonCandidate();
    if (mode < 4) candidate.build = BuildCommandConfig{{"compile"}};
    auto options = Options();
    options.runner = [mode](const auto&) -> absl::StatusOr<process::ProcessResult> {
      if (mode == 1 || mode == 4) return absl::DeadlineExceededError("limit");
      if (mode == 2 || mode == 5) return absl::ResourceExhaustedError("limit");
      if (mode == 3) return absl::NotFoundError("missing compiler");
      return process::ProcessResult{3, "", "failure"};
    };
    auto backend = CommandEvaluator::Create(candidate, PythonJudge(), options);
    ASSERT_TRUE(backend.ok());
    const auto result = (*backend)->Run(*source);
    if (mode == 3 || mode == 6)
      EXPECT_FALSE(result.ok());
    else {
      ASSERT_TRUE(result.ok()) << result.status();
      EXPECT_FALSE(result->passed);
      EXPECT_FALSE(result->reason.empty());
    }
  }
  auto evaluator = PythonJudge();
  evaluator.build = BuildCommandConfig{{"compile"}};
  auto options = Options();
  options.runner = [](const auto&) -> absl::StatusOr<process::ProcessResult> {
    return process::ProcessResult{1, "", "fixed failure"};
  };
  EXPECT_FALSE(CommandEvaluator::Create(PythonCandidate(), evaluator, options).ok());
}

TEST(CommandEvaluatorTest, RunsRealPythonCandidateAndReportsCrashOrSpawnError) {
  auto workspace = EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  auto source = (*workspace)->WriteSource("print(sum(map(int,input().split())))", ".py");
  ASSERT_TRUE(source.ok());
  auto backend = CommandEvaluator::Create(PythonCandidate(), PythonJudge(), Options());
  ASSERT_TRUE(backend.ok()) << backend.status();
  auto result = (*backend)->Run(*source);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_TRUE(result->passed);
  ASSERT_TRUE(utils::WriteFile(*source, "raise RuntimeError('candidate crash')").ok());
  result = (*backend)->Run(*source);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_FALSE(result->passed);
  auto candidate = PythonCandidate();
  candidate.run.argv[0] = "/missing-ievolve-interpreter";
  backend = CommandEvaluator::Create(candidate, PythonJudge(), Options());
  ASSERT_TRUE(backend.ok());
  EXPECT_FALSE((*backend)->Run(*source).ok());
}

TEST(CommandEvaluatorTest, CompilesRealCcAndCppCandidatesAndRejectsInvalidSource) {
  for (const char* suffix : {".cc", ".cpp"}) {
    auto workspace = EvaluationWorkspace::Create();
    ASSERT_TRUE(workspace.ok());
    auto source =
        (*workspace)->WriteSource("#include <iostream>\nint main(){int a,b;std::cin>>a>>b;std::cout<<a+b;}", suffix);
    ASSERT_TRUE(source.ok());
    CandidateConfig candidate;
    candidate.build =
        BuildCommandConfig{{IEVOLVE_CXX_EXECUTABLE, "-std=c++17", "{source}", "-o", "{workspace}/candidate"}};
    candidate.run.argv = {"{workspace}/candidate"};
    auto backend = CommandEvaluator::Create(candidate, PythonJudge(), Options());
    ASSERT_TRUE(backend.ok()) << backend.status();
    auto result = (*backend)->Run(*source);
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->passed);
    ASSERT_TRUE(utils::WriteFile(*source, "invalid C++").ok());
    result = (*backend)->Run(*source);
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_FALSE(result->passed);
    EXPECT_EQ(result->reason, "candidate_build_failed");
    EXPECT_FALSE(result->result.artifacts.empty());
  }
}

TEST(CommandEvaluatorTest, ConcurrentRunsUseIndependentCandidateDirectories) {
  auto backend = CommandEvaluator::Create(PythonCandidate(), PythonJudge(), Options());
  ASSERT_TRUE(backend.ok());
  std::vector<std::future<void>> calls;
  for (int i = 0; i < 4; ++i) {
    calls.push_back(std::async(std::launch::async, [&] {
      auto workspace = EvaluationWorkspace::Create();
      ASSERT_TRUE(workspace.ok());
      auto source =
          (*workspace)->WriteSource("from pathlib import Path\nPath('marker').touch(exist_ok=False)\nprint(5)", ".py");
      ASSERT_TRUE(source.ok());
      auto outcome = (*backend)->Run(*source);
      ASSERT_TRUE(outcome.ok()) << outcome.status();
      EXPECT_TRUE(outcome->passed);
    }));
  }
  for (auto& call : calls) call.get();
}

TEST(CommandEvaluatorTest, FailedFixedBuildCleansWorkspace) {
  auto options = Options();
  std::filesystem::path workspace;
  options.runner = [&](const auto& request) -> absl::StatusOr<process::ProcessResult> {
    workspace = *request.working_directory;
    EXPECT_TRUE(std::filesystem::exists(workspace));
    return absl::DeadlineExceededError("fixed build timeout");
  };
  auto evaluator = PythonJudge();
  evaluator.build = BuildCommandConfig{{"compiler"}};
  EXPECT_FALSE(CommandEvaluator::Create(PythonCandidate(), evaluator, options).ok());
  EXPECT_FALSE(workspace.empty());
  EXPECT_FALSE(std::filesystem::exists(workspace));
}

TEST(CommandEvaluatorTest, BoundsDiagnosticsAndHonorsArtifactDisable) {
  auto workspace = EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  auto source = (*workspace)->WriteSource("code", ".cpp");
  ASSERT_TRUE(source.ok());
  auto options = Options();
  options.runner = [](const auto&) -> absl::StatusOr<process::ProcessResult> {
    return process::ProcessResult{0, Passing().dump(), std::string(200, '\xff')};
  };
  auto evaluator = PythonJudge();
  evaluator.max_artifact_storage = 10;
  for (bool enabled : {true, false}) {
    evaluator.enable_artifacts = enabled;
    auto backend = CommandEvaluator::Create(PythonCandidate(), evaluator, options);
    ASSERT_TRUE(backend.ok());
    const auto outcome = (*backend)->Run(*source);
    ASSERT_TRUE(outcome.ok()) << outcome.status();
    EXPECT_EQ(outcome->result.GetTotalArtifactSize(), enabled ? 10u : 0u);
    EXPECT_TRUE(outcome->Validate(10).ok());
  }
}

#ifndef _WIN32
bool Running(pid_t pid) {
  if (kill(pid, 0) != 0 && errno == ESRCH) return false;
#ifdef __linux__
  std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
  std::string stat;
  std::getline(file, stat);
  const auto end = stat.rfind(')');
  if (end != std::string::npos && end + 2 < stat.size() && stat[end + 2] == 'Z') return false;
#endif
  return true;
}

TEST(CommandEvaluatorTest, TerminatesNestedCandidateAfterTimeoutAndNormalEvaluatorExit) {
  for (bool timeout : {false, true}) {
    auto workspace = EvaluationWorkspace::Create();
    auto fixed = EvaluationWorkspace::Create();
    ASSERT_TRUE(workspace.ok());
    ASSERT_TRUE(fixed.ok());
    auto source =
        (*workspace)
            ->WriteSource(
                "import os,time\nfrom pathlib import Path\nPath('pid').write_text(str(os.getpid()))\ntime.sleep(30)",
                ".py");
    ASSERT_TRUE(source.ok());
    auto script = (*fixed)->WriteSource(R"py(
import json, subprocess, sys, time
from pathlib import Path
r = json.load(sys.stdin)
p = subprocess.Popen(r['candidate']['argv'], cwd=r['candidate']['cwd'],
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
while not Path('pid').exists():
    time.sleep(0.001)
if sys.argv[1] == 'timeout':
    time.sleep(30)
json.dump({'protocol_version': 1, 'passed': False, 'reason': 'wrong_answer', 'metrics': {}}, sys.stdout)
)py",
                                        ".py");
    ASSERT_TRUE(script.ok());
    auto evaluator = PythonJudge();
    evaluator.source = script->string();
    evaluator.run.argv.push_back(timeout ? "timeout" : "exit");
    evaluator.timeout = 2;
    auto backend = CommandEvaluator::Create(PythonCandidate(), evaluator, Options());
    ASSERT_TRUE(backend.ok());
    auto result = (*backend)->Run(*source);
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_FALSE(result->passed);
    EXPECT_EQ(result->reason, timeout ? "evaluation_timeout" : "wrong_answer");
    const auto pid_text = utils::ReadFile((*workspace)->path() / "pid");
    ASSERT_TRUE(pid_text.ok());
    const pid_t pid = std::stoi(*pid_text);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (Running(pid) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(Running(pid));
    if (Running(pid)) kill(pid, SIGKILL);
  }
}
#endif

TEST(CommandEvaluatorTest, MalformedEvaluatorOutputIsInfrastructureError) {
  auto workspace = EvaluationWorkspace::Create();
  ASSERT_TRUE(workspace.ok());
  auto source = (*workspace)->WriteSource("code", ".py");
  auto options = Options();
  options.runner = [](const auto&) -> absl::StatusOr<process::ProcessResult> {
    return process::ProcessResult{0, "noise{}", ""};
  };
  auto backend = CommandEvaluator::Create(PythonCandidate(), PythonJudge(), options);
  ASSERT_TRUE(backend.ok());
  EXPECT_FALSE((*backend)->Run(*source).ok());
}
}  // namespace
}  // namespace ievolve::evaluator
