#include "ievolve/evaluator/python_evaluator.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#ifndef IEVOLVE_PYTHON_EXECUTABLE
#define IEVOLVE_PYTHON_EXECUTABLE "python3"
#endif

namespace ievolve::evaluator {
namespace {
namespace fs = std::filesystem;

const fs::path kData(IEVOLVE_EVALUATOR_TEST_DATA_DIR);

class PythonEvaluatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern = (fs::temp_directory_path() / "ievolve_python_test.XXXXXX").string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
    directory_ = pattern;
    options_.python_executable = IEVOLVE_PYTHON_EXECUTABLE;
  }

  void TearDown() override {
    std::error_code error;
    fs::remove_all(directory_, error);
  }

  fs::path Script(const std::string& source) {
    // A new filename avoids importlib's timestamp/size bytecode cache.
    const auto path = directory_ / (std::to_string(next_script_++) + ".py");
    std::ofstream stream(path);
    stream << source;

    return path;
  }

  PythonEvaluatorOptions options_;
  fs::path directory_;
  int next_script_ = 0;
};

TEST_F(PythonEvaluatorTest, ExecutesCandidateWithImportsAndProtectedStdout) {
  auto backend = PythonEvaluator::Create(kData / "python_evaluator.py", options_);
  ASSERT_TRUE(backend.ok()) << backend.status();
  EXPECT_EQ(backend->stages, (std::vector<int>{1, 3}));

  const auto result = backend->run(kData / "python_candidate.py", 0);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_FALSE(result->failed);
  EXPECT_EQ(result->result.metrics, (Metrics{{"combined_score", 42}, {"calls", 1}, {"ok", true}}));
  EXPECT_EQ(std::get<std::string>(result->result.artifacts.at("text")), "λ");
  EXPECT_EQ(std::get<ArtifactBytes>(result->result.artifacts.at("binary")), (ArtifactBytes{0, 255, 128}));
  EXPECT_TRUE(fs::equivalent(std::get<std::string>(result->result.artifacts.at("cwd")), kData));
}

TEST_F(PythonEvaluatorTest, ImportsFreshStateForEveryConcurrentStage) {
  auto backend = PythonEvaluator::Create(kData / "python_evaluator.py", options_);
  ASSERT_TRUE(backend.ok()) << backend.status();

  std::vector<std::future<absl::StatusOr<EvaluationStageResult>>> calls;
  for (int index = 0; index < 4; ++index) {
    calls.push_back(
        std::async(std::launch::async, [&, index] { return backend->run(kData / "python_candidate.py", index % 2); }));
  }

  for (auto& call : calls) {
    const auto result = call.get();
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->result.metrics.at("calls"), 1);
  }

  const auto stage3 = backend->run(kData / "python_candidate.py", 3);
  ASSERT_TRUE(stage3.ok()) << stage3.status();
  EXPECT_EQ(stage3->result.metrics.at("combined_score"), 3);

  EXPECT_EQ(backend->run(kData / "python_candidate.py", 2).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(PythonEvaluatorTest, ReturnsScriptExceptionsWithBoundedDiagnostics) {
  options_.max_artifact_bytes = 120;
  auto backend =
      PythonEvaluator::Create(Script("def evaluate(path):\n    raise ValueError('intentional failure')\n"), options_);
  ASSERT_TRUE(backend.ok()) << backend.status();

  auto result = backend->run(kData / "python_candidate.py", 0);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_TRUE(result->failed);
  EXPECT_EQ(result->result.metrics.at("error"), 0);
  EXPECT_NE(std::get<std::string>(result->result.artifacts.at("error")).find("intentional failure"), std::string::npos);
  EXPECT_LE(result->result.GetTotalArtifactSize(), 120);
}

TEST_F(PythonEvaluatorTest, HandlesExceptionsWhoseMessageCannotBeFormatted) {
  auto backend = PythonEvaluator::Create(Script("class UnprintableError(Exception):\n"
                                                "    def __str__(self): raise RuntimeError('broken formatter')\n"
                                                "def evaluate(path): raise UnprintableError()\n"),
                                         options_);
  ASSERT_TRUE(backend.ok()) << backend.status();

  const auto result = backend->run(kData / "python_candidate.py", 0);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_TRUE(result->failed);
  EXPECT_EQ(result->result.metrics.at("error"), 0);
  EXPECT_NE(std::get<std::string>(result->result.artifacts.at("error")).find("UnprintableError"), std::string::npos);
}

TEST_F(PythonEvaluatorTest, RevalidatesModuleAndEntryPointsAtEachStage) {
  for (const auto* replacement : {"raise RuntimeError('import changed')\n", "evaluate = 3\n",
                                  "def evaluate(path): return {}\nevaluate_stage1 = None\n", ""}) {
    SCOPED_TRACE(replacement);
    const auto script = Script(
        "def evaluate(path): return {}\n"
        "def evaluate_stage1(path): return {}\n");
    auto backend = PythonEvaluator::Create(script, options_);
    ASSERT_TRUE(backend.ok()) << backend.status();

    if (*replacement == '\0') {
      ASSERT_TRUE(fs::remove(script));
    } else {
      std::ofstream stream(script);
      stream << replacement;
    }

    EXPECT_EQ(backend->run(kData / "python_candidate.py", 1).status().code(), absl::StatusCode::kFailedPrecondition);
  }
}

TEST_F(PythonEvaluatorTest, AcceptsResultObjectsWithMetricsAndArtifacts) {
  auto backend = PythonEvaluator::Create(Script("from types import SimpleNamespace\n"
                                                "def evaluate(path):\n"
                                                "    return SimpleNamespace(metrics={'score': 0.75}, "
                                                "artifacts={'text': 'diagnostic', 'bytes': b'\\x00\\xff'})\n"),
                                         options_);
  ASSERT_TRUE(backend.ok()) << backend.status();

  const auto result = backend->run(kData / "python_candidate.py", 0);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_FALSE(result->failed);
  EXPECT_EQ(result->result.metrics.at("score"), 0.75);
  EXPECT_EQ(std::get<std::string>(result->result.artifacts.at("text")), "diagnostic");
  EXPECT_EQ(std::get<ArtifactBytes>(result->result.artifacts.at("bytes")), (ArtifactBytes{0, 255}));

  options_.enable_artifacts = false;
  backend = PythonEvaluator::Create(Script("class Result:\n"
                                           "    metrics = {'score': 1}\n"
                                           "    @property\n"
                                           "    def artifacts(self): raise AssertionError('do not inspect')\n"
                                           "def evaluate(path): return Result()\n"),
                                    options_);
  ASSERT_TRUE(backend.ok()) << backend.status();

  const auto disabled = backend->run(kData / "python_candidate.py", 0);
  ASSERT_TRUE(disabled.ok()) << disabled.status();

  EXPECT_TRUE(disabled->result.artifacts.empty());
  EXPECT_EQ(disabled->result.metrics.at("score"), 1);
}

TEST_F(PythonEvaluatorTest, RejectsInvalidMetricsInsteadOfReturningScriptFailure) {
  for (const auto* expression :
       {"[]", "{'nested': []}", "{'nan': float('nan')}", "{1: 2}", "{'huge': 2**80}", "{'bad': '\\ud800'}",
        "{'combined_score': '0.5'}", "{'combined_score': True}", "{'combined_score': None}"}) {
    SCOPED_TRACE(expression);
    auto backend =
        PythonEvaluator::Create(Script("def evaluate(path):\n    return " + std::string(expression) + "\n"), options_);
    ASSERT_TRUE(backend.ok()) << backend.status();

    EXPECT_EQ(backend->run(kData / "python_candidate.py", 0).status().code(), absl::StatusCode::kDataLoss);
  }
}

TEST_F(PythonEvaluatorTest, ValidatesImportsAndEntryPointsDuringCreation) {
  for (const auto* source : {"raise RuntimeError('import failure')\n", "evaluate = 2\n",
                             "def evaluate(path): return {}\nevaluate_stage1 = 2\n"}) {
    SCOPED_TRACE(source);
    EXPECT_EQ(PythonEvaluator::Create(Script(source), options_).status().code(), absl::StatusCode::kFailedPrecondition);
  }
}

TEST_F(PythonEvaluatorTest, DisablesArtifactsWithoutInspectingTheirTypes) {
  options_.enable_artifacts = false;
  options_.max_artifact_bytes = 0;
  auto backend = PythonEvaluator::Create(Script("from openevolve.evaluation_result import EvaluationResult\n"
                                                "def evaluate(path): return EvaluationResult({'score': 1}, "
                                                "object())\n"),
                                         options_);
  ASSERT_TRUE(backend.ok()) << backend.status();

  auto result = backend->run(kData / "python_candidate.py", 0);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_TRUE(result->result.artifacts.empty());
}

TEST_F(PythonEvaluatorTest, RejectsInvalidArtifactsAndEnforcesDecodedPayloadCap) {
  auto backend = PythonEvaluator::Create(Script("from openevolve.evaluation_result import EvaluationResult\n"
                                                "def evaluate(path): return EvaluationResult({}, {'bad': 42})\n"),
                                         options_);
  ASSERT_TRUE(backend.ok()) << backend.status();

  EXPECT_EQ(backend->run(kData / "python_candidate.py", 0).status().code(), absl::StatusCode::kDataLoss);

  options_.max_artifact_bytes = 1;
  backend = PythonEvaluator::Create(kData / "python_evaluator.py", options_);
  ASSERT_TRUE(backend.ok()) << backend.status();

  EXPECT_EQ(backend->run(kData / "python_candidate.py", 0).status().code(), absl::StatusCode::kResourceExhausted);
}

TEST_F(PythonEvaluatorTest, EnforcesTimeoutOnInspectionAndEvaluation) {
  options_.timeout = std::chrono::milliseconds(250);
  EXPECT_EQ(PythonEvaluator::Create(Script("import time\ntime.sleep(5)\ndef evaluate(path): return {}\n"), options_)
                .status()
                .code(),
            absl::StatusCode::kDeadlineExceeded);

  auto backend = PythonEvaluator::Create(Script("import time\ndef evaluate(path):\n    "
                                                "time.sleep(5)\n    return {}\n"),
                                         options_);
  ASSERT_TRUE(backend.ok()) << backend.status();

  EXPECT_EQ(backend->run(kData / "python_candidate.py", 0).status().code(), absl::StatusCode::kDeadlineExceeded);
}

TEST_F(PythonEvaluatorTest, EnforcesCombinedOutputCapForEvaluatorLogging) {
  options_.max_output_bytes = 4096;
  auto backend = PythonEvaluator::Create(Script("import os\ndef evaluate(path):\n    os.write(1, b'x' * 100000)\n"
                                                "    return {}\n"),
                                         options_);
  ASSERT_TRUE(backend.ok()) << backend.status();

  EXPECT_EQ(backend->run(kData / "python_candidate.py", 0).status().code(), absl::StatusCode::kResourceExhausted);
}

TEST_F(PythonEvaluatorTest, RejectsMalformedProtocolAndPreservesRunnerFailures) {
  const std::vector<std::string> malformed = {"not json",
                                              "{}",
                                              "{\"kind\":\"inspect\",\"stages\":[0]}",
                                              "{\"kind\":\"inspect\",\"stages\":[1,1]}",
                                              "{\"kind\":\"inspect\",\"stages\":[true]}",
                                              "{\"kind\":\"inspect\",\"stages\":[],\"kind\":\"inspect\"}",
                                              "{\"kind\":\"inspect\",\"stages\":[]} trailing"};

  for (const auto& output : malformed) {
    SCOPED_TRACE(output);
    options_.runner = [output](const process::ProcessRequest&) -> absl::StatusOr<process::ProcessResult> {
      return process::ProcessResult{0, output, ""};
    };

    EXPECT_EQ(PythonEvaluator::Create(kData / "python_evaluator.py", options_).status().code(),
              absl::StatusCode::kDataLoss);
  }

  options_.runner = [](const process::ProcessRequest&) -> absl::StatusOr<process::ProcessResult> {
    return absl::PermissionDeniedError("test denied launch");
  };

  EXPECT_EQ(PythonEvaluator::Create(kData / "python_evaluator.py", options_).status().code(),
            absl::StatusCode::kPermissionDenied);
}

TEST_F(PythonEvaluatorTest, RejectsMalformedResultProtocol) {
  const std::vector<std::string> malformed = {"{\"kind\":\"result\",\"failed\":0,\"result\":{\"metrics\":{}}}",
                                              "{\"kind\":\"result\",\"failed\":false,\"result\":{\"metrics\":[]}}",
                                              "{\"kind\":\"result\",\"failed\":false,\"result\":{\"metrics\":{\"s\":1,"
                                              "\"s\":2}}}",
                                              "{\"kind\":\"result\",\"failed\":false,\"result\":{\"metrics\":{},"
                                              "\"artifacts\":{\"b\":{\"__bytes__\":\"x\"}}}}"};

  for (const auto& output : malformed) {
    SCOPED_TRACE(output);
    options_.runner = [output](const process::ProcessRequest& request) -> absl::StatusOr<process::ProcessResult> {
      const auto input = Metrics::parse(request.stdin_text);
      return process::ProcessResult{
          0, input.at("action") == "inspect" ? "{\"kind\":\"inspect\",\"stages\":[]}" : output, ""};
    };

    auto backend = PythonEvaluator::Create(kData / "python_evaluator.py", options_);
    ASSERT_TRUE(backend.ok()) << backend.status();

    EXPECT_EQ(backend->run(kData / "python_candidate.py", 0).status().code(), absl::StatusCode::kDataLoss);
  }
}

TEST_F(PythonEvaluatorTest, RejectsInvalidOptionsAndMissingFiles) {
  auto invalid = options_;
  invalid.timeout = std::chrono::milliseconds(0);
  EXPECT_EQ(PythonEvaluator::Create(kData / "python_evaluator.py", invalid).status().code(),
            absl::StatusCode::kInvalidArgument);

  invalid = options_;
  invalid.max_output_bytes = 0;
  EXPECT_EQ(PythonEvaluator::Create(kData / "python_evaluator.py", invalid).status().code(),
            absl::StatusCode::kInvalidArgument);

  invalid = options_;
  invalid.python_executable.clear();
  EXPECT_EQ(PythonEvaluator::Create(kData / "python_evaluator.py", invalid).status().code(),
            absl::StatusCode::kInvalidArgument);

  invalid = options_;
  invalid.runner = {};
  EXPECT_EQ(PythonEvaluator::Create(kData / "python_evaluator.py", invalid).status().code(),
            absl::StatusCode::kInvalidArgument);

  EXPECT_EQ(PythonEvaluator::Create(directory_ / "absent.py", options_).status().code(), absl::StatusCode::kNotFound);
}

}  // namespace
}  // namespace ievolve::evaluator
