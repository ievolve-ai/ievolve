#include "ievolve/evaluator/command_evaluator.h"

#include <cmath>
#include <limits>

namespace ievolve::evaluator {
namespace {
absl::Status Invalid(const std::string& message) { return absl::InvalidArgumentError("Command evaluator: " + message); }
absl::Status PhaseError(const char* phase, const absl::Status& status) {
  return absl::Status(status.code(), std::string(phase) + ": " + std::string(status.message()));
}
absl::Status ValidateLimit(std::int64_t bytes) {
  if (bytes <= 0 || static_cast<std::uint64_t>(bytes) > std::numeric_limits<std::size_t>::max()) {
    return Invalid("output limit must be positive and fit size_t");
  }
  return absl::OkStatus();
}
absl::Status ValidateBuild(const std::optional<BuildCommandConfig>& build, bool source_available) {
  if (!build) return absl::OkStatus();
  if (build->timeout <= 0) return Invalid("build timeout must be positive");
  auto status = ValidateLimit(build->max_output_bytes);
  if (!status.ok()) return status;
  return ValidateCommandTemplate(build->argv, source_available);
}

absl::StatusOr<process::ProcessResult> Build(const BuildCommandConfig& config, const CommandContext& context,
                                             const std::filesystem::path& working_directory,
                                             const process::ProcessRunner& runner) {
  auto argv = ExpandCommand(config.argv, context);
  if (!argv.ok()) return argv.status();
  process::ProcessRequest request;
  request.argv = std::move(*argv);
  request.working_directory = working_directory;
  request.timeout = std::chrono::seconds(config.timeout);
  request.max_output_bytes = static_cast<std::size_t>(config.max_output_bytes);
  request.cleanup_group_on_exit = true;
  return runner(request);
}

// Raw tool output may not be UTF-8. Keep it as bounded binary diagnostics and
// never replace an evaluator's own artifact, even with our reserved prefix.
void AddDiagnostic(EvaluationOutcome& outcome, const EvaluatorConfig& config, const std::string& key,
                   const std::string& bytes) {
  if (!config.enable_artifacts || bytes.empty() || outcome.result.artifacts.count(key)) return;
  const auto used = outcome.result.GetTotalArtifactSize();
  const auto limit = static_cast<std::size_t>(config.max_artifact_storage);
  if (used >= limit) return;
  const auto size = std::min(bytes.size(), limit - used);
  outcome.result.artifacts.emplace(key, ArtifactBytes(bytes.begin(), bytes.begin() + size));
}
EvaluationOutcome Reject(const std::string& reason) {
  EvaluationOutcome outcome;
  outcome.reason = reason;
  return outcome;
}
bool IsLimit(const absl::Status& status) {
  return absl::IsDeadlineExceeded(status) || absl::IsResourceExhausted(status);
}
std::string LimitReason(const char* phase, const absl::Status& status) {
  return std::string(phase) + (absl::IsDeadlineExceeded(status) ? "_timeout" : "_output_limit");
}
}  // namespace

CommandEvaluator::CommandEvaluator(CandidateConfig candidate, EvaluatorConfig evaluator,
                                   CommandEvaluatorOptions options, std::unique_ptr<EvaluationWorkspace> workspace)
    : candidate_(std::move(candidate)),
      evaluator_(std::move(evaluator)),
      options_(std::move(options)),
      workspace_(std::move(workspace)) {}

absl::StatusOr<std::shared_ptr<CommandEvaluator>> CommandEvaluator::Create(CandidateConfig candidate,
                                                                           EvaluatorConfig evaluator,
                                                                           CommandEvaluatorOptions options) {
  std::error_code error;
  if (!options.runner || !options.base_directory.is_absolute() ||
      options.base_directory.native().find('\0') != std::string::npos ||
      !std::filesystem::is_directory(options.base_directory, error)) {
    return Invalid("requires a runner and an absolute existing base_directory");
  }
  if (evaluator.timeout <= 0 || evaluator.max_artifact_storage < 0) return Invalid("invalid evaluation limits");
  for (const auto& status :
       {ValidateCommandTemplate(candidate.run.argv, true),
        ValidateCommandTemplate(evaluator.run.argv, evaluator.source.has_value()), ValidateBuild(candidate.build, true),
        ValidateBuild(evaluator.build, evaluator.source.has_value()), ValidateLimit(evaluator.max_output_bytes)}) {
    if (!status.ok()) return status;
  }
  if (evaluator.source) {
    if (evaluator.source->empty() || evaluator.source->find('\0') != std::string::npos)
      return Invalid("invalid source path");
    const auto source = (options.base_directory / *evaluator.source).lexically_normal();
    if (!std::filesystem::is_regular_file(source, error)) return Invalid("evaluator.source must be an existing file");
    evaluator.source = source.string();
  }
  auto workspace = EvaluationWorkspace::Create();
  if (!workspace.ok()) return workspace.status();
  auto backend = std::shared_ptr<CommandEvaluator>(
      new CommandEvaluator(std::move(candidate), std::move(evaluator), std::move(options), std::move(*workspace)));
  const CommandContext context{backend->options_.base_directory, backend->workspace_->path(),
                               backend->evaluator_.source};
  auto argv = ExpandCommand(backend->evaluator_.run.argv, context);
  if (!argv.ok()) return argv.status();
  backend->evaluator_argv_ = std::move(*argv);
  if (backend->evaluator_.build) {
    auto build = Build(*backend->evaluator_.build, context, context.workspace, backend->options_.runner);
    if (!build.ok()) return PhaseError("Evaluator build", build.status());
    if (build->exit_code != 0) {
      return absl::FailedPreconditionError("Evaluator build exited " + std::to_string(build->exit_code) + ": " +
                                           build->stderr_text);
    }
  }
  return backend;
}

absl::StatusOr<EvaluationOutcome> CommandEvaluator::Run(const std::filesystem::path& project) const {
  std::error_code error;
  if (!project.is_absolute() || !std::filesystem::is_directory(project, error)) {
    return Invalid("candidate must be an absolute existing project directory");
  }
  // Candidate commands run inside the project copy; {workspace} is the
  // per-evaluation directory holding it, which also hosts the evaluator.
  const CommandContext context{options_.base_directory, project.parent_path(), project};
  auto argv = ExpandCommand(candidate_.run.argv, context);
  if (!argv.ok()) return argv.status();
  process::ProcessResult build_result;
  double build_ms = 0;
  if (candidate_.build) {
    const auto start = std::chrono::steady_clock::now();
    auto build = Build(*candidate_.build, context, project, options_.runner);
    build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!build.ok()) {
      if (IsLimit(build.status())) return Reject(LimitReason("candidate_build", build.status()));
      return PhaseError("Candidate build", build.status());
    }
    build_result = std::move(*build);
    if (build_result.exit_code != 0) {
      auto outcome = Reject("candidate_build_failed");
      AddDiagnostic(outcome, evaluator_, "ievolve.build.stdout", build_result.stdout_text);
      AddDiagnostic(outcome, evaluator_, "ievolve.build.stderr", build_result.stderr_text);
      return outcome;
    }
  }
  const Metrics request_json = {{"protocol_version", 1},
                                {"candidate", {{"argv", *argv}, {"cwd", project.string()}}},
                                {"limits",
                                 {{"evaluation_timeout_ms", std::int64_t(evaluator_.timeout) * 1000},
                                  {"max_artifact_bytes", evaluator_.max_artifact_storage}}},
                                {"enable_artifacts", evaluator_.enable_artifacts}};
  process::ProcessRequest request;
  request.argv = evaluator_argv_;
  request.stdin_text = request_json.dump();
  request.working_directory = context.workspace;
  request.timeout = std::chrono::seconds(evaluator_.timeout);
  request.max_output_bytes = static_cast<std::size_t>(evaluator_.max_output_bytes);
  request.cleanup_group_on_exit = true;
  auto execution = options_.runner(request);
  if (!execution.ok()) {
    if (IsLimit(execution.status())) return Reject(LimitReason("evaluation", execution.status()));
    return PhaseError("Evaluator execution", execution.status());
  }
  if (execution->exit_code != 0) {
    return absl::FailedPreconditionError("Evaluator exited " + std::to_string(execution->exit_code) + ": " +
                                         execution->stderr_text);
  }
  auto outcome = EvaluationOutcome::Parse(execution->stdout_text, evaluator_.enable_artifacts,
                                          static_cast<std::size_t>(evaluator_.max_artifact_storage));
  if (!outcome.ok()) return PhaseError("Evaluator protocol", outcome.status());
  AddDiagnostic(*outcome, evaluator_, "ievolve.build.stdout", build_result.stdout_text);
  AddDiagnostic(*outcome, evaluator_, "ievolve.build.stderr", build_result.stderr_text);
  if (candidate_.build) AddDiagnostic(*outcome, evaluator_, "ievolve.build.duration_ms", std::to_string(build_ms));
  AddDiagnostic(*outcome, evaluator_, "ievolve.evaluator.stderr", execution->stderr_text);
  return outcome;
}
}  // namespace ievolve::evaluator
