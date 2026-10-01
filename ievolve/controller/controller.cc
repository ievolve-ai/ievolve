#include "ievolve/controller/controller.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

#include "ievolve/utils/file.h"
#include "ievolve/utils/text.h"

namespace ievolve::controller {
namespace {
namespace fs = std::filesystem;

bool Improved(const Metrics& current, const Metrics& previous, double threshold) {
  if (current.is_number_float() || previous.is_number_float()) {
    return NumberAs<long double>(current) - NumberAs<long double>(previous) >= threshold;
  }

  // Integer deltas need up to 65 magnitude bits (uint64 - negative int64).
  // Subtract before converting: long double has only 53 bits on some targets.
  const auto magnitude = [](const Metrics& value) {
    const bool negative = !value.is_number_unsigned() && !value.is_boolean() && value.get<std::int64_t>() < 0;
    const auto raw = value.is_boolean() ? std::uint64_t(value.get<bool>()) : value.get<std::uint64_t>();
    return std::make_pair(negative, negative ? std::uint64_t{0} - raw : raw);
  };

  const auto [left_negative, left] = magnitude(current);
  const auto [right_negative, right] = magnitude(previous);
  bool negative = left_negative;
  bool carry = false;
  std::uint64_t delta;
  if (left_negative != right_negative) {
    delta = left + right;
    carry = delta < left;
  } else if (left >= right) {
    delta = left - right;
  } else {
    delta = right - left;
    negative = !negative;
  }

  if (!carry && delta == 0) negative = false;
  if (negative != (threshold < 0)) return !negative;

  const double required = std::abs(threshold);
  // Subtraction from 2^64 is exact for a double between 2^64 and 2^65.
  const int comparison =
      carry ? (required < 0x1p64 ? 1 : CompareMetricNumbers(Metrics(delta), Metrics(required - 0x1p64)))
            : CompareMetricNumbers(Metrics(delta), Metrics(required));

  return negative ? comparison <= 0 : comparison >= 0;
}

bool ReachedTarget(const Metrics& metrics, double target) {
  const auto score = metrics.find("combined_score");
  return score != metrics.end() && IsNumeric(*score) && CompareMetricNumbers(*score, Metrics(target)) >= 0;
}

std::optional<Metrics> StoppingScore(const Metrics& metrics, const std::string& name) {
  if (metrics.empty()) return std::nullopt;

  const auto score = metrics.find(name);
  if (score != metrics.end()) return IsNumeric(*score) ? std::optional<Metrics>(*score) : std::nullopt;

  // Divide first to retain small residuals after cancellation of large terms.
  // If rounding still overflows (e.g. three DBL_MAX/3), scale and clamp
  // instead.
  std::size_t count = 0;
  long double scale = 0;
  for (const auto& value : metrics)
    if (value.is_number()) {
      ++count;
      scale = std::max(scale, std::abs(NumberAs<long double>(value)));
    }

  // A mean over zero numeric metrics does not exist; reporting 0.0 would be a
  // fabricated score. EvaluationResult::PassesThreshold refuses this the same
  // way.
  if (count == 0) return std::nullopt;

  long double average = 0;
  for (const auto& value : metrics)
    if (value.is_number()) average += NumberAs<long double>(value) / count;
  if (std::isfinite(average)) return Metrics(static_cast<double>(std::clamp(average, -scale, scale)));

  average = 0;
  if (scale > 0) {
    for (const auto& value : metrics)
      if (value.is_number()) average += (NumberAs<long double>(value) / scale) / count;
  }

  return Metrics(static_cast<double>(std::clamp(average, -1.0L, 1.0L) * scale));
}

absl::Status WriteFile(const fs::path& path, const std::string& content) {
  const auto status = utils::WriteFile(path, content);
  return status.ok() ? status : absl::FailedPreconditionError("Cannot write controller output");
}

// Private staging directories reside on the destination filesystem.
class StagingDirectory {
 public:
  explicit StagingDirectory(const fs::path& parent) {
    fs::create_directories(parent);
    std::random_device random;
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = parent / (".controller-" + std::to_string(random()) + "-" + std::to_string(random()));
      if (fs::create_directory(path_)) return;
    }

    throw std::runtime_error("Cannot allocate controller staging directory");
  }
  ~StagingDirectory() {
    if (path_.empty()) return;

    std::error_code error;
    fs::remove_all(path_, error);
  }
  const fs::path& path() const { return path_; }
  // Publishing is a single rename, so a reader never observes a half-written
  // checkpoint. Clearing the path transfers ownership: the destructor must not
  // delete what has already been handed over.
  void Publish(const fs::path& destination) {
    fs::rename(path_, destination);
    path_.clear();
  }

 private:
  fs::path path_;
};

// Throws rather than returning a status because every caller sits inside
// LoadCheckpoint's try block, which turns a malformed counter into one DataLoss
// error. The message is not observable: that handler supplies its own.
std::int64_t Counter(const Metrics& value) {
  const auto number = AsCounter(value);
  if (!number) throw std::invalid_argument("Invalid counter");

  return *number;
}

bool InvalidPath(const fs::path& path) { return path.empty() || path.string().find('\0') != std::string::npos; }

// Rounds to tenths so progress lines stay short.
double Seconds(double seconds) { return std::round(seconds * 10) / 10; }
}  // namespace

Controller::Controller(Config config, ControllerOptions options, std::shared_ptr<evaluator::Evaluator> evaluator,
                       std::unique_ptr<IterationRunner> runner, ProgramDatabase database)
    : config_(std::move(config)),
      options_(std::move(options)),
      evaluator_(std::move(evaluator)),
      runner_(std::move(runner)),
      database_(std::move(database)) {}

absl::StatusOr<std::unique_ptr<Controller>> Controller::Create(const Config& config,
                                                               std::shared_ptr<const LLMInterface> llm,
                                                               std::shared_ptr<evaluator::Evaluator> evaluator,
                                                               ControllerOptions options) {
  const auto status = config.Validate();
  if (!status.ok()) return status;

  if (!config.language || config.language->empty()) {
    return absl::InvalidArgumentError("language must be set before creating a controller");
  }

  auto suffix = config.file_suffix;
  for (char& c : suffix)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';

  // file_suffix is rejected when it would make the best program file collide
  // with best_program_info.json, or escape the output directory.
  if (InvalidPath(options.output_directory) || config.max_iterations < 0 || config.checkpoint_interval <= 0 ||
      !std::isfinite(config.convergence_threshold) ||
      (config.early_stopping_patience && config.early_stopping_metric.empty()) ||
      config.file_suffix.find_first_of("/\\") != std::string::npos ||
      config.file_suffix.find('\0') != std::string::npos || suffix == "_info.json") {
    return absl::InvalidArgumentError("Invalid controller configuration");
  }

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
  seed.metrics = std::move(evaluation->metrics);

  auto staged = database_;
  auto admitted = staged.Add(seed, {{0}, {0}});
  if (!admitted.ok()) return admitted.status();
  if (!*admitted) return absl::FailedPreconditionError("Initial program was not admitted");

  status = staged.StoreArtifacts(seed.id, evaluation->artifacts);
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
  const auto iteration = progress.iteration + 1;

  auto context = staged.SelectionContext(progress.iteration, std::vector<int>(config_.database.num_islands, 0));
  if (!context.ok()) return context.status();
  auto target = IslandSelection::Select(*context, options_.island_selector);
  if (!target.ok()) return target.status();

  auto sample = staged.SampleFromIsland(*target, config_.prompt.num_diverse_programs);
  if (!sample.ok()) return sample.status();
  auto snapshot = staged.Snapshot();
  if (!snapshot.ok()) return snapshot.status();

  IterationInput input;
  input.parent = std::move(sample->parent);
  input.parent_island = input.parent.metadata.at("island").get<int>();
  input.target_island = *target;
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

  const auto label = "Iteration " + std::to_string(iteration) + "/" + std::to_string(final_iteration_);
  Log(utils::LogLevel::kDebug, label, ": parent ", input.parent.id, " from island ", input.parent_island,
      ", target island ", *target);

  auto result = runner_->Run(input);
  if (!result.ok()) return result.status();
  const auto elapsed = Seconds(result->elapsed_seconds);

  bool accepted = false;
  std::optional<Metrics> metrics;
  if (result->child) {
    auto admitted = staged.Add(*result->child, {{*target}, {iteration}});
    if (!admitted.ok()) return admitted.status();
    accepted = *admitted;
    if (accepted) {
      auto status = staged.StoreArtifacts(result->child->id, result->artifacts);
      if (!status.ok()) return status;

      const auto& usage = result->child->metadata.at("token_usage");
      status =
          staged.LogPrompt(result->child->id, config_.diff_based_evolution ? "diff_user" : "full_rewrite_user",
                           {{"system", result->prompt.system}, {"user", result->prompt.user}}, {result->response.text},
                           usage.is_null() ? std::nullopt : std::optional<Metrics>(usage));
      if (!status.ok()) return status;

      status = staged.IncrementGeneration(*target);
      if (!status.ok()) return status;
      auto migrate = staged.ShouldMigrate();
      if (!migrate.ok()) return migrate.status();
      if (*migrate) {
        Log(utils::LogLevel::kDebug, label, ": migrating programs between islands");
        status = staged.Migrate();
        if (!status.ok()) return status;
      }

      TrackScore(*result->child, progress);
      metrics = result->child->metrics;
    }
  }

  if (!result->child) {
    Log(utils::LogLevel::kInfo, label, " rejected: ", result->rejection_reason, " (", elapsed, "s)");
  } else if (!accepted) {
    Log(utils::LogLevel::kInfo, label, " discarded by the database: ", ScoreText(result->child->metrics), " (", elapsed,
        "s)");
  } else {
    Log(utils::LogLevel::kInfo, label, " accepted: ", ScoreText(result->child->metrics), " (", elapsed, "s)");
    const auto best = staged.GetBestProgram();
    if (best.ok() && best->id == result->child->id) {
      Log(utils::LogLevel::kInfo, "New best program ", best->id, " with ", ScoreText(best->metrics));
    }
  }

  progress.iteration = iteration;
  if (accepted) {
    ++progress.accepted;
  } else {
    ++progress.rejected;
  }

  // Commit point: both members are replaced together, so the database and the
  // progress counters can never disagree about this attempt.
  database_ = std::move(staged);
  progress_ = std::move(progress);

  return metrics;
}

void Controller::TrackScore(const Program& child, Progress& progress) const {
  if (!config_.early_stopping_patience) return;

  const auto score = StoppingScore(child.metrics, config_.early_stopping_metric);
  if (!score) return;

  if (*config_.early_stopping_patience <= 0) {
    if (CompareMetricNumbers(*score, Metrics(config_.convergence_threshold)) == 0) {
      progress.best_score = *score;
      progress.early_stopped = true;
    }
    return;
  }

  if (!progress.best_score || Improved(*score, *progress.best_score, config_.convergence_threshold)) {
    progress.best_score = *score;
    progress.without_improvement = 0;
  } else {
    ++progress.without_improvement;
  }

  progress.early_stopped = progress.without_improvement >= *config_.early_stopping_patience;
}

double Controller::Fitness(const Metrics& metrics) const {
  return GetFitnessScore(metrics, config_.database.feature_dimensions);
}

// JSON serialization prints the shortest round-trip form, as Python repr does.
std::string Controller::ScoreText(const Metrics& metrics) const {
  auto text = "score " + Metrics(Fitness(metrics)).dump();
  if (target_score_) text += ", target " + Metrics(*target_score_).dump();
  return text;
}

Metrics Controller::StoppingPolicy() const {
  return {{"patience", config_.early_stopping_patience ? Metrics(*config_.early_stopping_patience) : Metrics(nullptr)},
          {"metric", config_.early_stopping_metric},
          {"threshold", config_.convergence_threshold}};
}

absl::Status Controller::WriteBest(const fs::path& directory) const {
  auto best = database_.GetBestProgram();
  if (!best.ok()) return best.status();

  fs::create_directories(directory);
  auto status = WriteFile(directory / ("best_program" + config_.file_suffix), best->code);
  if (!status.ok()) return status;

  Metrics info = {{"id", best->id},
                  {"generation", best->generation},
                  {"iteration_found", best->iteration_found},
                  {"current_iteration", progress_.iteration},
                  {"metrics", best->metrics},
                  {"language", best->language},
                  {"timestamp", best->timestamp},
                  {"parent_id", best->parent_id ? Metrics(*best->parent_id) : Metrics(nullptr)}};
  return WriteFile(directory / "best_program_info.json", info.dump(2));
}

absl::Status Controller::SaveCheckpoint() {
  if (checkpoint_iteration_ == progress_.iteration) return absl::OkStatus();

  const auto parent = options_.output_directory / "checkpoints";
  const auto destination = parent / ("checkpoint_" + std::to_string(progress_.iteration));
  if (fs::exists(fs::symlink_status(destination))) {
    return absl::AlreadyExistsError("Controller checkpoint already exists");
  }

  StagingDirectory staged(parent);
  auto status = database_.Save(staged.path() / "database");
  if (!status.ok()) return status;

  const Metrics state = {{"format", "ievolve.controller"},
                         {"version", 1},
                         {"iteration", progress_.iteration},
                         {"accepted", progress_.accepted},
                         {"rejected", progress_.rejected},
                         {"without_improvement", progress_.without_improvement},
                         {"best_score", progress_.best_score.value_or(Metrics(nullptr))},
                         {"early_stopped", progress_.early_stopped},
                         {"stopping_policy", StoppingPolicy()}};
  status = WriteFile(staged.path() / "controller.json", state.dump(2));
  if (!status.ok()) return status;

  status = WriteBest(staged.path());
  if (!status.ok()) return status;

  staged.Publish(destination);
  checkpoint_path_ = destination;
  checkpoint_iteration_ = progress_.iteration;
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

  const auto text = utils::ReadFile(path / "controller.json");
  if (text.status().code() == absl::StatusCode::kDataLoss) return text.status();
  if (!text.ok()) return absl::NotFoundError("Controller checkpoint state not found");

  try {
    const auto state = Metrics::parse(*text);
    if (state.at("format") != "ievolve.controller" || Counter(state.at("version")) != 1) {
      return absl::DataLossError("Unsupported controller checkpoint format");
    }
    if (nlohmann::json(state.at("stopping_policy")) != nlohmann::json(StoppingPolicy())) {
      return absl::FailedPreconditionError("Checkpoint early-stopping policy differs");
    }

    Progress progress;
    progress.iteration = Counter(state.at("iteration"));
    progress.accepted = Counter(state.at("accepted"));
    progress.rejected = Counter(state.at("rejected"));
    progress.without_improvement = Counter(state.at("without_improvement"));
    progress.early_stopped = state.at("early_stopped").get<bool>();
    if (!state.at("best_score").is_null()) {
      if (!IsNumeric(state.at("best_score")) || !std::isfinite(NumberAs<long double>(state.at("best_score")))) {
        return absl::DataLossError("Invalid checkpoint stopping score");
      }
      progress.best_score = state.at("best_score");
    }

    if (progress.accepted > progress.iteration || progress.rejected != progress.iteration - progress.accepted ||
        progress.without_improvement > progress.accepted || (progress.best_score && progress.accepted == 0) ||
        (progress.without_improvement > 0 && !progress.best_score)) {
      return absl::DataLossError("Inconsistent controller progress");
    }

    if (!config_.early_stopping_patience) {
      if (progress.best_score || progress.without_improvement || progress.early_stopped) {
        return absl::DataLossError("Unexpected early-stopping state");
      }
    } else if (*config_.early_stopping_patience > 0) {
      if (progress.early_stopped != (progress.without_improvement >= *config_.early_stopping_patience)) {
        return absl::DataLossError("Inconsistent checkpoint patience");
      }
    } else if (progress.without_improvement || progress.early_stopped != progress.best_score.has_value() ||
               (progress.best_score &&
                CompareMetricNumbers(*progress.best_score, Metrics(config_.convergence_threshold)) != 0)) {
      return absl::DataLossError("Inconsistent checkpoint stopping event");
    }

    auto staged = database_;
    auto status = staged.Load(path / "database");
    if (!status.ok()) return status;
    auto snapshot = staged.Snapshot();
    if (!snapshot.ok()) return snapshot.status();
    if (snapshot->programs.empty() || snapshot->last_iteration > progress.iteration ||
        ((snapshot->last_iteration == 0) != (progress.accepted == 0))) {
      return absl::DataLossError("Checkpoint population progress differs");
    }

    auto remaining = progress.accepted;
    for (const auto generation : snapshot->generations) {
      if (generation > remaining) return absl::DataLossError("Checkpoint generation counts differ");
      remaining -= generation;
    }
    if (remaining != 0) return absl::DataLossError("Checkpoint generation counts differ");

    database_ = std::move(staged);
    progress_ = std::move(progress);
    checkpoint_path_ = path;
    checkpoint_iteration_ = progress_.iteration;

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
  if (progress_.iteration > std::numeric_limits<std::int64_t>::max() - iterations) {
    return absl::OutOfRangeError("Iteration budget overflows checkpoint counter");
  }
  if (options.checkpoint_path) {
    Log(utils::LogLevel::kInfo, "Resumed at iteration ", progress_.iteration, " (accepted ", progress_.accepted,
        ", rejected ", progress_.rejected, ")");
  }

  const auto start = progress_.iteration;
  final_iteration_ = start + iterations;
  // The whole restored population is scanned, so a checkpoint that already
  // meets the target stops immediately instead of evolving once more first.
  bool reached_target = false;
  if (options.target_score) {
    auto snapshot = database_.Snapshot();
    if (!snapshot.ok()) return snapshot.status();
    for (const auto& [id, program] : snapshot->programs)
      reached_target |= ReachedTarget(program.metrics, *options.target_score);
  }

  StopReason reason = StopReason::kIterationLimit;
  // Stop conditions are tested before the budget check, so a resumed run that
  // already satisfies one exits without spending an iteration or a model call.
  for (;;) {
    if (reached_target) {
      Log(utils::LogLevel::kInfo, "Target score ", *options.target_score, " reached");
      reason = StopReason::kTargetScore;
      break;
    }
    if (progress_.early_stopped) {
      Log(utils::LogLevel::kInfo, "Early stopping: ", config_.early_stopping_metric, " stopped improving");
      reason = StopReason::kEarlyStopping;
      break;
    }
    if (stop_requested_.load()) {
      Log(utils::LogLevel::kInfo, "Stop requested; finishing the run");
      reason = StopReason::kRequested;
      break;
    }
    if (progress_.iteration - start == iterations) break;

    auto metrics = Step();
    if (!metrics.ok()) return metrics.status();
    if (options.target_score && *metrics) reached_target = ReachedTarget(**metrics, *options.target_score);

    if (progress_.iteration % config_.checkpoint_interval == 0) {
      status = SaveCheckpoint();
      if (!status.ok()) return status;
    }
  }

  status = SaveCheckpoint();
  if (!status.ok()) return status;

  // Prepare both files first. The checkpoint is the authoritative atomic
  // record; replacement of the best output pair itself is not atomic.
  StagingDirectory best(options_.output_directory);
  status = WriteBest(best.path());
  if (!status.ok()) return status;
  const auto destination = options_.output_directory / "best";
  fs::create_directories(destination);
  for (const auto& entry : fs::directory_iterator(best.path()))
    fs::rename(entry.path(), destination / entry.path().filename());

  auto program = database_.GetBestProgram();
  if (!program.ok()) return program.status();
  Log(utils::LogLevel::kInfo, "Evolution finished after ", progress_.iteration - start, " iteration(s); best program ",
      program->id, " with ", ScoreText(program->metrics));

  return RunResult{
      std::move(*program), reason,          progress_.iteration - start, progress_.iteration, progress_.accepted,
      progress_.rejected,  checkpoint_path_};
}
}  // namespace ievolve::controller
