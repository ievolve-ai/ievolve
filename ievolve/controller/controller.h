#ifndef IEVOLVE_CONTROLLER_CONTROLLER_H_
#define IEVOLVE_CONTROLLER_CONTROLLER_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "ievolve/controller/iteration.h"
#include "ievolve/controller/run_progress.h"
#include "ievolve/database/program_database.h"
#include "ievolve/utils/log.h"

namespace ievolve::controller {

// Runtime hooks that YAML cannot express. Every other setting, including the
// iteration budget, target score, checkpoint to resume and output directory,
// comes from Config.
struct ControllerOptions {
  PopulationStrategy population_strategy;
  IslandSelector island_selector;
  // Borrowed progress logger; must outlive the controller. Null is silent.
  utils::Logger* logger = nullptr;
};

struct InitialProgram {
  // The evolved files read from run.project_dir; every path must match one of
  // run.evolve_files.
  SourceFiles files;
};

enum class StopReason { kIterationLimit, kTargetScore, kEarlyStopping, kRequested };

struct RunResult {
  Program best;
  StopReason stop_reason = StopReason::kIterationLimit;
  std::int64_t iterations_executed = 0;  // This Run call only.
  std::int64_t last_iteration = 0;
  std::int64_t accepted_iterations = 0;  // Totals, including resumed history.
  std::int64_t rejected_iterations = 0;
  std::filesystem::path checkpoint_path;
};

class Controller {
 public:
  // Dependencies are already configured. This serial controller owns
  // persistence through checkpoint bundles.
  // config.run.evolve_files must be set before creation.
  static absl::StatusOr<std::unique_ptr<Controller>> Create(const Config& config,
                                                            std::shared_ptr<const LLMInterface> llm,
                                                            std::shared_ptr<evaluator::Evaluator> evaluator,
                                                            ControllerOptions options);

  // Runs attempts until iteration config.max_iterations, counting those
  // already in the resumed checkpoint (config.run.checkpoint, which ignores
  // initial and must have been saved with the same run.evolve_files); a
  // checkpoint at or past the budget runs none. Single-use,
  // including failed calls. Rejections consume attempts; dependency errors stop
  // immediately. A failed attempt does not commit database changes.
  absl::StatusOr<RunResult> Run(const InitialProgram& initial);
  // Cooperative: an in-flight evaluation finishes before stopping. Can be
  // called concurrently with Run; cancellation before initialization is Status.
  void RequestStop() { stop_requested_.store(true); }
  // Inspect only before or after Run, never concurrently with it.
  const ProgramDatabase& database() const { return database_; }

 private:
  Controller(Config config, ControllerOptions options, std::shared_ptr<evaluator::Evaluator> evaluator,
             std::unique_ptr<IterationRunner> runner, ProgramDatabase database);
  absl::StatusOr<RunResult> RunImpl(const InitialProgram& initial);
  std::filesystem::path OutputDirectory() const { return config_.run.output_directory; }
  absl::Status Initialize(const InitialProgram& initial);
  // Outcome of offering an iteration's result to the population.
  enum class Admission { kRejected, kDiscarded, kAccepted };

  absl::StatusOr<std::optional<Metrics>> Step();
  absl::StatusOr<IterationInput> BuildInput(ProgramDatabase& staged, int target, std::int64_t iteration) const;
  absl::StatusOr<Admission> Admit(ProgramDatabase& staged, const IterationResult& result, int target,
                                  std::int64_t iteration, const std::string& label) const;
  void LogAttempt(const std::string& label, const IterationResult& result, Admission admission,
                  const ProgramDatabase& staged) const;
  absl::StatusOr<bool> PopulationReachedTarget() const;
  std::optional<StopReason> CheckStop(bool reached_target) const;
  absl::Status SaveCheckpoint();
  absl::Status LoadCheckpoint(const std::filesystem::path& path);
  absl::Status PublishBest() const;
  // "score X", plus ", target T" when the run has a target score. Values keep
  // full double precision.
  std::string ScoreText(const Metrics& metrics) const;
  // DEBUG lines with each diagnostic of a rejected evaluation, truncated.
  void LogDiagnostics(const std::string& label, const ArtifactMap& diagnostics) const;
  template <typename... Args>
  void Log(utils::LogLevel level, const Args&... args) const {
    if (options_.logger) options_.logger->Log(level, args...);
  }

  Config config_;
  ControllerOptions options_;
  std::shared_ptr<evaluator::Evaluator> evaluator_;
  std::unique_ptr<IterationRunner> runner_;
  ProgramDatabase database_;
  RunProgress progress_;
  std::filesystem::path checkpoint_path_;
  std::int64_t checkpoint_iteration_ = -1;
  std::int64_t final_iteration_ = 0;
  std::atomic<bool> started_{false};
  std::atomic<bool> stop_requested_{false};
};

}  // namespace ievolve::controller
#endif  // IEVOLVE_CONTROLLER_CONTROLLER_H_
