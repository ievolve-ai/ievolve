#include "ievolve/controller/controller.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include "absl/strings/ascii.h"
#include "ievolve/controller/checkpoint_file.h"
#include "ievolve/controller/staging_directory.h"
#include "ievolve/utils/file.h"
#include "ievolve/utils/text.h"

namespace ievolve::controller {
namespace {
namespace fs = std::filesystem;

bool ReachedTarget(const Metrics& metrics, double target) {
  const auto score = metrics.find("combined_score");
  return score != metrics.end() && IsNumeric(*score) && CompareMetricNumbers(*score, Metrics(target)) >= 0;
}

absl::Status WriteFile(const fs::path& path, const std::string& content) {
  const auto status = utils::WriteFile(path, content);
  return status.ok() ? status : absl::FailedPreconditionError("Cannot write controller output");
}

bool InvalidPath(const fs::path& path) { return path.empty() || path.string().find('\0') != std::string::npos; }

// Settings the controller relies on beyond Config::Validate.
absl::Status CheckControllerConfig(const Config& config, const fs::path& output_directory) {
  const auto invalid = [](const std::string& message) {
    return absl::InvalidArgumentError("Invalid controller configuration: " + message);
  };
  if (InvalidPath(output_directory)) return invalid("output_directory must be a nonempty path");
  if (config.max_iterations < 0) return invalid("max_iterations must be nonnegative");
  if (config.checkpoint_interval <= 0) return invalid("checkpoint_interval must be positive");
  if (!std::isfinite(config.convergence_threshold)) return invalid("convergence_threshold must be finite");
  if (config.early_stopping_patience && config.early_stopping_metric.empty()) {
    return invalid("early_stopping_metric must be set when early_stopping_patience is");
  }

  // The best program file must stay inside the output directory and must not
  // collide with best_program_info.json.
  const auto& suffix = config.file_suffix;
  if (suffix.find_first_of("/\\") != std::string::npos || suffix.find('\0') != std::string::npos ||
      absl::AsciiStrToLower(suffix) == "_info.json") {
    return invalid("file_suffix must be a plain file extension");
  }

  return absl::OkStatus();
}

// Rejects a restored population that the checkpoint's own progress could not
// have produced: counts, best program and generations must agree, and every
// program must carry valid evaluated metrics in the checkpoint's language.
absl::Status ValidateRestoredPopulation(const PopulationSnapshot& snapshot, const CheckpointFile& file) {
  const auto& progress = file.progress;
  if (snapshot.programs.empty() || snapshot.last_iteration > progress.iteration() ||
      ((snapshot.last_iteration == 0) != (progress.accepted() == 0))) {
    return absl::DataLossError("Checkpoint population progress differs");
  }

  if (snapshot.best_program_id != file.best_id) return absl::DataLossError("Checkpoint best program differs");
  for (const auto& [id, program] : snapshot.programs) {
    evaluator::EvaluationOutcome outcome{true, {program.metrics, {}}, {}};
    if (program.language != file.language || !outcome.Validate().ok()) {
      return absl::DataLossError("Checkpoint contains invalid evaluated program");
    }
  }

  auto remaining = progress.accepted();
  for (const auto generation : snapshot.generations) {
    if (generation > remaining) return absl::DataLossError("Checkpoint generation counts differ");
    remaining -= generation;
  }
  if (remaining != 0) return absl::DataLossError("Checkpoint generation counts differ");

  return absl::OkStatus();
}

// Rejected candidates never enter the database, so the debug log is the only
// place their diagnostics (build and evaluator stderr, evaluator artifacts)
// appear. Each one is cut to this many bytes to keep the log readable.
constexpr std::size_t kMaxLoggedDiagnosticBytes = 4096;

// Rounds to tenths so progress lines stay short.
double Seconds(double seconds) { return std::round(seconds * 10) / 10; }
}  // namespace

Controller::Controller(Config config, ControllerOptions options, std::shared_ptr<evaluator::Evaluator> evaluator,
                       std::unique_ptr<IterationRunner> runner, ProgramDatabase database)
    : config_(std::move(config)),
      options_(std::move(options)),
      evaluator_(std::move(evaluator)),
      runner_(std::move(runner)),
      database_(std::move(database)),
      progress_(StoppingPolicy{config_.early_stopping_patience, config_.early_stopping_metric,
                               config_.convergence_threshold}) {}

absl::StatusOr<std::unique_ptr<Controller>> Controller::Create(const Config& config,
                                                               std::shared_ptr<const LLMInterface> llm,
                                                               std::shared_ptr<evaluator::Evaluator> evaluator,
                                                               ControllerOptions options) {
  const auto status = config.Validate();
  if (!status.ok()) return status;

  if (!config.language || config.language->empty()) {
    return absl::InvalidArgumentError("language must be set before creating a controller");
  }

  const auto invalid = CheckControllerConfig(config, options.output_directory);
  if (!invalid.ok()) return invalid;

  if (config.evolution_trace.enabled || config.max_tasks_per_child) {
    return absl::UnimplementedError("Controller tracing and worker recycling are not migrated");
  }

  auto runner = IterationRunner::Create(config, std::move(llm), evaluator);
  if (!runner.ok()) return runner.status();
  auto database = ProgramDatabase::Create(config.database, options.population_strategy);
  if (!database.ok()) return database.status();

  return std::unique_ptr<Controller>(
      new Controller(config, std::move(options), std::move(evaluator), std::move(*runner), std::move(*database)));
}

absl::StatusOr<RunResult> Controller::Run(const InitialProgram& initial, const RunOptions& options) {
  // Single-use even when it fails: a second Run would resume from partially
  // advanced progress that no checkpoint describes.
  if (started_.exchange(true)) return absl::FailedPreconditionError("Controller Run is single-use");

  try {
    return RunImpl(initial, options);
  } catch (const fs::filesystem_error&) {
    return absl::FailedPreconditionError("Controller filesystem operation failed");
  } catch (...) {
    return absl::InternalError("Controller execution failed");
  }
}

// Evaluates the seed program once and records it as iteration 0. Like Step,
// the database is copied, mutated and only swapped back on success.
absl::Status Controller::Initialize(const InitialProgram& initial) {
  if (database_.size() != 0) {
    return absl::FailedPreconditionError("Populated database requires an explicit controller checkpoint");
  }

  Program seed;
  seed.id = "initial";
  seed.code = initial.code;
  seed.language = *config_.language;
  seed.changes_description = config_.prompt.initial_changes_description;

  auto status = seed.Validate();
  if (!status.ok()) return status;
  if (seed.language.empty() || utils::Trim(seed.code).empty() ||
      utils::Utf8Prefix(seed.code, config_.max_code_length).size() != seed.code.size()) {
    return absl::InvalidArgumentError("Invalid initial program text or length");
  }

  Log(utils::LogLevel::kInfo, "Evaluating initial program");
  auto evaluation = evaluator_->Evaluate({seed.code, seed.id, seed.language});
  if (!evaluation.ok()) return evaluation.status();
  if (!evaluation->passed) {
    Log(utils::LogLevel::kInfo, "Initial program rejected: ", evaluation->reason);
    LogDiagnostics("Initial program", evaluation->result.artifacts);
    return absl::FailedPreconditionError("Initial program rejected: " + evaluation->reason);
  }
  seed.metrics = std::move(evaluation->result.metrics);

  auto staged = database_;
  auto admitted = staged.Add(seed, {{0}, {0}});
  if (!admitted.ok()) return admitted.status();
  if (!*admitted) return absl::FailedPreconditionError("Initial program was not admitted");

  status = staged.StoreArtifacts(seed.id, evaluation->result.artifacts);
  if (!status.ok()) return status;

  database_ = std::move(staged);
  Log(utils::LogLevel::kInfo, "Initial program ", ScoreText(seed.metrics));
  return absl::OkStatus();
}

// Runs one evolution attempt: select an island, sample a parent, delegate the
// prompt/model/edit/evaluate cycle to IterationRunner, then admit the result.
//
// Every mutation lands on copies of the database and progress, which replace
// the members only at the very end. Any failure in between therefore leaves no
// half-admitted child behind. Returns the child's metrics when one was
// accepted, nullopt when the attempt was rejected but still consumed a budget
// slot, and a Status only for genuine failures.
absl::StatusOr<std::optional<Metrics>> Controller::Step() {
  auto staged = database_;
  auto progress = progress_;
  const auto iteration = progress.iteration() + 1;

  auto context = staged.SelectionContext(progress.iteration(), std::vector<int>(config_.database.num_islands, 0));
  if (!context.ok()) return context.status();
  auto target = IslandSelection::Select(*context, options_.island_selector);
  if (!target.ok()) return target.status();
  auto input = BuildInput(staged, *target, iteration);
  if (!input.ok()) return input.status();

  const auto label = "Iteration " + std::to_string(iteration) + "/" + std::to_string(final_iteration_);
  Log(utils::LogLevel::kDebug, label, ": parent ", input->parent.id, " from island ", input->parent_island,
      ", target island ", *target);

  auto result = runner_->Run(*input);
  if (!result.ok()) return result.status();
  auto admission = Admit(staged, *result, *target, iteration, label);
  if (!admission.ok()) return admission.status();
  LogAttempt(label, *result, *admission, staged);

  std::optional<Metrics> metrics;
  if (*admission == Admission::kAccepted) {
    metrics = result->child->metrics;
    progress.RecordAccepted(*metrics);
  } else {
    progress.RecordRejected();
  }

  // Commit point: both members are replaced together, so the database and the
  // progress counters can never disagree about this attempt.
  database_ = std::move(staged);
  progress_ = std::move(progress);

  return metrics;
}

// Samples a parent and inspirations from the target island and gathers the
// parent island's programs as prompt context.
absl::StatusOr<IterationInput> Controller::BuildInput(ProgramDatabase& staged, int target,
                                                      std::int64_t iteration) const {
  auto sample = staged.SampleFromIsland(target, config_.prompt.num_diverse_programs);
  if (!sample.ok()) return sample.status();
  auto snapshot = staged.Snapshot();
  if (!snapshot.ok()) return snapshot.status();

  IterationInput input;
  input.parent = std::move(sample->parent);
  input.parent_island = input.parent.metadata.at("island").get<int>();
  input.target_island = target;
  input.iteration = iteration;
  input.child_id = "iteration-" + std::to_string(iteration);
  if (snapshot->programs.count(input.child_id)) return absl::AlreadyExistsError("Iteration ID already exists");

  for (const auto& id : snapshot->islands[input.parent_island])
    input.island_programs.push_back(snapshot->programs.at(id));
  input.inspirations = std::move(sample->inspirations);
  if (config_.prompt.include_artifacts) {
    auto artifacts = staged.GetArtifacts(input.parent.id);
    if (!artifacts.ok()) return artifacts.status();
    input.parent_artifacts = std::move(*artifacts);
  }

  return input;
}

// Offers the child to the population. An admitted child also gets its
// artifacts and prompt stored, advances the island generation and may
// trigger migration.
absl::StatusOr<Controller::Admission> Controller::Admit(ProgramDatabase& staged, const IterationResult& result,
                                                        int target, std::int64_t iteration,
                                                        const std::string& label) const {
  if (!result.child) return Admission::kRejected;

  const auto& child = *result.child;
  auto admitted = staged.Add(child, {{target}, {iteration}});
  if (!admitted.ok()) return admitted.status();
  if (!*admitted) return Admission::kDiscarded;

  auto status = staged.StoreArtifacts(child.id, result.artifacts);
  if (!status.ok()) return status;

  const auto& usage = child.metadata.at("token_usage");
  status = staged.LogPrompt(child.id, config_.diff_based_evolution ? "diff_user" : "full_rewrite_user",
                            {{"system", result.prompt.system}, {"user", result.prompt.user}}, {result.response.text},
                            usage.is_null() ? std::nullopt : std::optional<Metrics>(usage));
  if (!status.ok()) return status;

  status = staged.IncrementGeneration(target);
  if (!status.ok()) return status;
  auto migrate = staged.ShouldMigrate();
  if (!migrate.ok()) return migrate.status();
  if (*migrate) {
    Log(utils::LogLevel::kDebug, label, ": migrating programs between islands");
    status = staged.Migrate();
    if (!status.ok()) return status;
  }

  return Admission::kAccepted;
}

void Controller::LogAttempt(const std::string& label, const IterationResult& result, Admission admission,
                            const ProgramDatabase& staged) const {
  const auto elapsed = Seconds(result.elapsed_seconds);
  switch (admission) {
    case Admission::kRejected:
      Log(utils::LogLevel::kInfo, label, " rejected: ", result.rejection_reason, " (", elapsed, "s)");
      LogDiagnostics(label, result.artifacts);
      return;
    case Admission::kDiscarded:
      Log(utils::LogLevel::kInfo, label, " discarded by the database: ", ScoreText(result.child->metrics), " (",
          elapsed, "s)");
      return;
    case Admission::kAccepted:
      Log(utils::LogLevel::kInfo, label, " accepted: ", ScoreText(result.child->metrics), " (", elapsed, "s)");
      const auto best = staged.GetBestProgram();
      if (best.ok() && best->id == result.child->id) {
        Log(utils::LogLevel::kInfo, "New best program ", best->id, " with ", ScoreText(best->metrics));
      }
      return;
  }
}

void Controller::LogDiagnostics(const std::string& label, const ArtifactMap& diagnostics) const {
  if (!options_.logger || !options_.logger->IsEnabled(utils::LogLevel::kDebug)) return;
  for (const auto& [key, value] : diagnostics) {
    const auto* text = std::get_if<std::string>(&value);
    const auto* bytes = std::get_if<ArtifactBytes>(&value);
    const auto size = text ? text->size() : bytes->size();
    const auto shown = std::min(size, kMaxLoggedDiagnosticBytes);
    const auto prefix = text ? text->substr(0, shown) : std::string(bytes->begin(), bytes->begin() + shown);
    Log(utils::LogLevel::kDebug, label, " ", key, ":\n", prefix,
        shown < size ? "\n... (" + std::to_string(size - shown) + " more bytes)" : std::string());
  }
}

// JSON serialization prints the shortest round-trip form, as Python repr does.
std::string Controller::ScoreText(const Metrics& metrics) const {
  auto text = "score " + Metrics(GetFitnessScore(metrics, config_.database.feature_dimensions)).dump();
  if (target_score_) text += ", target " + Metrics(*target_score_).dump();
  return text;
}

// Prepares both files before replacing either. The checkpoint is the
// authoritative atomic record; replacement of the best output pair itself is
// not atomic.
absl::Status Controller::PublishBest() const {
  auto best = database_.GetBestProgram();
  if (!best.ok()) return best.status();

  StagingDirectory staged(options_.output_directory);
  auto status = WriteFile(staged.path() / ("best_program" + config_.file_suffix), best->code);
  if (!status.ok()) return status;

  Metrics info = {{"id", best->id},
                  {"generation", best->generation},
                  {"iteration_found", best->iteration_found},
                  {"current_iteration", progress_.iteration()},
                  {"metrics", best->metrics},
                  {"language", best->language},
                  {"timestamp", best->timestamp},
                  {"parent_id", best->parent_id ? Metrics(*best->parent_id) : Metrics(nullptr)}};
  status = WriteFile(staged.path() / "best_program_info.json", info.dump(2));
  if (!status.ok()) return status;

  staged.PublishContents(options_.output_directory / "best");
  return absl::OkStatus();
}

absl::Status Controller::SaveCheckpoint() {
  if (checkpoint_iteration_ == progress_.iteration()) return absl::OkStatus();

  const auto parent = options_.output_directory / "checkpoints";
  const auto destination = parent / ("checkpoint_" + std::to_string(progress_.iteration()));
  if (fs::exists(fs::symlink_status(destination))) {
    return absl::AlreadyExistsError("Controller checkpoint already exists");
  }

  StagingDirectory staged(parent);
  auto status = database_.Save(staged.path() / "database");
  if (!status.ok()) return status;

  const auto best = database_.GetBestProgram();
  if (!best.ok()) return best.status();
  status = CheckpointFile{*config_.language, config_.file_suffix, best->id, progress_}.Write(staged.path());
  if (!status.ok()) return status;

  staged.Publish(destination);
  checkpoint_path_ = destination;
  checkpoint_iteration_ = progress_.iteration();
  Log(utils::LogLevel::kInfo, "Saved checkpoint ", destination.string());

  return absl::OkStatus();
}

// Restores a controller checkpoint. Beyond reading the counters, this rejects
// any state that a real run could not have produced: counters that disagree
// with each other or with the restored population, and early-stopping state
// that does not match the configured policy. Refusing a mismatched policy is
// deliberate, since resuming under a different one would silently change when
// the run stops. Nothing is committed until every check has passed.
absl::Status Controller::LoadCheckpoint(const fs::path& path) {
  if (InvalidPath(path)) return absl::InvalidArgumentError("Invalid checkpoint path");

  auto state = CheckpointFile::Read(path);
  if (!state.ok()) return state.status();
  if (state->language != *config_.language || state->file_suffix != config_.file_suffix) {
    return absl::FailedPreconditionError("Checkpoint program language or file suffix differs");
  }
  if (state->progress.policy() != progress_.policy()) {
    return absl::FailedPreconditionError("Checkpoint early-stopping policy differs");
  }
  try {
    auto progress = state->progress;
    auto staged = database_;
    auto status = staged.Load(path / "database");
    if (!status.ok()) return status;
    auto snapshot = staged.Snapshot();
    if (!snapshot.ok()) return snapshot.status();
    status = ValidateRestoredPopulation(*snapshot, *state);
    if (!status.ok()) return status;

    database_ = std::move(staged);
    progress_ = std::move(progress);
    checkpoint_path_ = path;
    checkpoint_iteration_ = progress_.iteration();

    return absl::OkStatus();
  } catch (const Metrics::exception&) {
    return absl::DataLossError("Malformed controller checkpoint JSON");
  } catch (const std::invalid_argument&) {
    return absl::DataLossError("Invalid controller checkpoint counter");
  }
}

absl::StatusOr<RunResult> Controller::RunImpl(const InitialProgram& initial, const RunOptions& options) {
  const int iterations = options.iterations.value_or(config_.max_iterations);
  if (iterations < 0 || (options.target_score && !std::isfinite(*options.target_score))) {
    return absl::InvalidArgumentError("Invalid run budget or target score");
  }
  if (stop_requested_.load()) return absl::CancelledError("Stopped before initialization");

  target_score_ = options.target_score;
  auto status = options.checkpoint_path ? LoadCheckpoint(*options.checkpoint_path) : Initialize(initial);
  if (!status.ok()) return status;
  if (progress_.iteration() > std::numeric_limits<std::int64_t>::max() - iterations) {
    return absl::OutOfRangeError("Iteration budget overflows checkpoint counter");
  }
  if (options.checkpoint_path) {
    Log(utils::LogLevel::kInfo, "Resumed at iteration ", progress_.iteration(), " (accepted ", progress_.accepted(),
        ", rejected ", progress_.rejected(), ")");
  }

  const auto start = progress_.iteration();
  final_iteration_ = start + iterations;
  auto reached_target = PopulationReachedTarget();
  if (!reached_target.ok()) return reached_target.status();

  StopReason reason = StopReason::kIterationLimit;
  for (;;) {
    if (const auto stop = CheckStop(*reached_target)) {
      reason = *stop;
      break;
    }
    if (progress_.iteration() == final_iteration_) break;

    auto metrics = Step();
    if (!metrics.ok()) return metrics.status();
    if (target_score_ && *metrics) *reached_target = ReachedTarget(**metrics, *target_score_);

    if (progress_.iteration() % config_.checkpoint_interval == 0) {
      status = SaveCheckpoint();
      if (!status.ok()) return status;
    }
  }

  status = SaveCheckpoint();
  if (!status.ok()) return status;
  status = PublishBest();
  if (!status.ok()) return status;

  auto program = database_.GetBestProgram();
  if (!program.ok()) return program.status();
  Log(utils::LogLevel::kInfo, "Evolution finished after ", progress_.iteration() - start,
      " iteration(s); best program ", program->id, " with ", ScoreText(program->metrics));

  return RunResult{
      std::move(*program),  reason,          progress_.iteration() - start, progress_.iteration(), progress_.accepted(),
      progress_.rejected(), checkpoint_path_};
}

// The whole population is scanned, so a restored checkpoint that already
// meets the target stops immediately instead of evolving once more first.
absl::StatusOr<bool> Controller::PopulationReachedTarget() const {
  if (!target_score_) return false;

  auto snapshot = database_.Snapshot();
  if (!snapshot.ok()) return snapshot.status();
  for (const auto& [id, program] : snapshot->programs) {
    if (ReachedTarget(program.metrics, *target_score_)) return true;
  }
  return false;
}

// Stop conditions are tested before the budget, so a resumed run that already
// satisfies one exits without spending an iteration or a model call.
std::optional<StopReason> Controller::CheckStop(bool reached_target) const {
  if (reached_target) {
    Log(utils::LogLevel::kInfo, "Target score ", *target_score_, " reached");
    return StopReason::kTargetScore;
  }
  if (progress_.early_stopped()) {
    Log(utils::LogLevel::kInfo, "Early stopping: ", config_.early_stopping_metric, " stopped improving");
    return StopReason::kEarlyStopping;
  }
  if (stop_requested_.load()) {
    Log(utils::LogLevel::kInfo, "Stop requested; finishing the run");
    return StopReason::kRequested;
  }
  return std::nullopt;
}
}  // namespace ievolve::controller
