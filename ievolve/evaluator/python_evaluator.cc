#include "ievolve/evaluator/python_evaluator.h"

#include <algorithm>
#include <set>
#include <string_view>
#include <utility>

namespace ievolve::evaluator {
namespace {
namespace fs = std::filesystem;

#include "python_worker.inc"

absl::Status ProtocolError() { return absl::DataLossError("evaluator: invalid Python worker protocol"); }

bool ValidArgument(const std::string& value) {
  if (value.empty() || value.find('\0') != std::string::npos) return false;

  try {
    (void)Metrics(value).dump();
    return true;
  } catch (const Metrics::exception&) {
    return false;
  }
}

absl::StatusOr<fs::path> AbsoluteFile(const fs::path& path) {
  if (!ValidArgument(path.string())) return absl::InvalidArgumentError("evaluator: invalid file path");

  std::error_code error;
  const auto absolute = fs::absolute(path, error);
  if (error) return absl::InvalidArgumentError("evaluator: cannot resolve file path");

  const auto status = fs::status(absolute, error);
  if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status))) {
    return absl::NotFoundError("evaluator: file does not exist");
  }
  if (error) return absl::PermissionDeniedError("evaluator: cannot inspect file");
  if (!fs::is_regular_file(status)) return absl::InvalidArgumentError("evaluator: path is not a regular file");

  return absolute;
}

// Parses one worker protocol message. The callback enforces what the JSON
// grammar cannot: a depth cap, so a deeply nested reply cannot exhaust the
// stack, and rejection of duplicate keys, which the parser would otherwise
// silently collapse to the last occurrence. Everything the worker sends is
// untrusted input: it is whatever the evaluation script produced.
absl::StatusOr<Metrics> ParseProtocol(const std::string& text) {
  struct ExcessiveDepth {};
  bool duplicate = false;
  std::vector<std::set<std::string>> objects;
  const auto callback = [&](int depth, Metrics::parse_event_t event, Metrics& value) {
    if (depth > 32) throw ExcessiveDepth{};

    if (event == Metrics::parse_event_t::object_start) {
      objects.emplace_back();
    } else if (event == Metrics::parse_event_t::object_end) {
      objects.pop_back();
    } else if (event == Metrics::parse_event_t::key) {
      duplicate |= !objects.back().insert(value.get<std::string>()).second;
    }

    return true;
  };

  try {
    auto value = Metrics::parse(text, callback);
    if (duplicate || !value.is_object()) return ProtocolError();

    const auto kind = value.find("kind");
    if (kind == value.end() || !kind->is_string()) return ProtocolError();

    if (*kind == "error") {
      const auto code = value.find("code");
      const auto message = value.find("message");
      if (value.size() != 3 || code == value.end() || !code->is_string() || message == value.end() ||
          !message->is_string()) {
        return ProtocolError();
      }

      const auto diagnostic = "evaluator: " + message->get<std::string>();
      if (*code == "failed_precondition") return absl::FailedPreconditionError(diagnostic);
      if (*code == "resource_exhausted") return absl::ResourceExhaustedError(diagnostic);
      if (*code == "data_loss") return absl::DataLossError(diagnostic);
      return ProtocolError();
    }

    return value;
  } catch (const Metrics::exception&) {
    return ProtocolError();
  } catch (const ExcessiveDepth&) {
    return ProtocolError();
  }
}

absl::StatusOr<Metrics> RunWorker(const fs::path& file, const PythonEvaluatorOptions& options, Metrics input) {
  process::ProcessRequest request;
  // -B keeps user module directories free of worker-created __pycache__ files.
  request.argv = {options.python_executable, "-B", "-c", kPythonEvaluatorWorker};
  request.working_directory = file.parent_path();
  request.timeout = options.timeout;
  request.max_output_bytes = options.max_output_bytes;

  input["evaluation_file"] = file.string();
  input["enable_artifacts"] = options.enable_artifacts;
  input["max_artifact_bytes"] = options.max_artifact_bytes;
  request.stdin_text = input.dump();

  try {
    auto output = options.runner(request);
    if (!output.ok()) return output.status();

    if (output->stdout_text.size() > options.max_output_bytes ||
        output->stderr_text.size() > options.max_output_bytes - output->stdout_text.size()) {
      return absl::ResourceExhaustedError("evaluator: process output limit exceeded");
    }

    if (output->exit_code != 0) {
      auto code = absl::StatusCode::kUnavailable;
      if (output->exit_code == 126) code = absl::StatusCode::kPermissionDenied;
      if (output->exit_code == 127) code = absl::StatusCode::kNotFound;
      return absl::Status(code, "evaluator: Python worker exited with code " + std::to_string(output->exit_code));
    }

    return ParseProtocol(output->stdout_text);
  } catch (...) {
    return absl::InternalError("evaluator: process runner threw an exception");
  }
}

absl::StatusOr<std::vector<int>> ParseStages(const Metrics& value) {
  const auto stages = value.find("stages");
  if (value.at("kind") != "inspect" || value.size() != 2 || stages == value.end() || !stages->is_array()) {
    return ProtocolError();
  }

  std::vector<int> result;
  for (const auto& stage : *stages) {
    if (!stage.is_number_integer() || stage < 1 || stage > 3) return ProtocolError();

    const auto number = stage.get<int>();
    if (std::find(result.begin(), result.end(), number) != result.end()) return ProtocolError();
    result.push_back(number);
  }

  std::sort(result.begin(), result.end());
  return result;
}

absl::StatusOr<EvaluationStageResult> RunStage(const fs::path& file, const PythonEvaluatorOptions& options,
                                               const fs::path& candidate, int stage) {
  auto path = AbsoluteFile(candidate);
  if (!path.ok()) return path.status();

  auto output = RunWorker(file, options, {{"action", "run"}, {"candidate_file", path->string()}, {"stage", stage}});
  if (!output.ok()) return output.status();

  const auto failed = output->find("failed");
  const auto result = output->find("result");
  if (output->at("kind") != "result" || output->size() != 3 || failed == output->end() || !failed->is_boolean() ||
      result == output->end() || !result->is_object()) {
    return ProtocolError();
  }

  if (!options.enable_artifacts) (*result)["artifacts"] = Metrics::object();
  auto decoded = EvaluationResult::FromJson(*result, options.max_artifact_bytes);
  if (!decoded.ok()) {
    if (decoded.status().code() == absl::StatusCode::kResourceExhausted) return decoded.status();
    return ProtocolError();
  }

  return EvaluationStageResult{std::move(*decoded), failed->get<bool>()};
}
}  // namespace

absl::StatusOr<EvaluationBackend> PythonEvaluator::Create(const fs::path& evaluation_file,
                                                          PythonEvaluatorOptions options) {
  if (!ValidArgument(options.python_executable) || !options.runner ||
      options.timeout <= std::chrono::milliseconds::zero() || options.max_output_bytes == 0) {
    return absl::InvalidArgumentError("evaluator: invalid Python evaluator options");
  }

  auto file = AbsoluteFile(evaluation_file);
  if (!file.ok()) return file.status();

  auto response = RunWorker(*file, options, {{"action", "inspect"}});
  if (!response.ok()) return response.status();
  auto stages = ParseStages(*response);
  if (!stages.ok()) return stages.status();

  EvaluationBackend backend;
  backend.stages = *stages;
  backend.run = [file = std::move(*file), options = std::move(options), stages = std::move(*stages)](
                    const fs::path& candidate, int stage) {
    if (stage != 0 && std::find(stages.begin(), stages.end(), stage) == stages.end()) {
      return absl::StatusOr<EvaluationStageResult>(
          absl::InvalidArgumentError("evaluator: requested stage is unavailable"));
    }

    return RunStage(file, options, candidate, stage);
  };

  return backend;
}
}  // namespace ievolve::evaluator
