#include "ievolve/cli/cli.h"

#include <map>

#include "ievolve/cli/stop.h"
#include "ievolve/controller/checkpoint_file.h"
#include "ievolve/llm/no_generation.h"
#include "ievolve/utils/file.h"
#include "ievolve/utils/log.h"

namespace ievolve::cli {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

// Infers the program language from a file extension. Every key is ASCII, so
// lowercasing stays ASCII. Configured language names are compared literally.
std::optional<std::string> Language(std::string extension) {
  for (auto& c : extension)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';

  static const std::map<std::string, std::string> languages = {
      {".py", "python"},     {".cc", "cpp"},        {".cpp", "cpp"}, {".cxx", "cpp"}, {".c", "c"},
      {".js", "javascript"}, {".ts", "typescript"}, {".rs", "rust"}, {".go", "go"},   {".java", "java"},
      {".jl", "julia"},      {".rb", "ruby"},       {".lua", "lua"}, {".r", "r"},     {".swift", "swift"}};

  const auto found = languages.find(extension);
  return found == languages.end() ? std::nullopt : std::optional<std::string>(found->second);
}

struct Prepared {
  Config config;
  controller::InitialProgram initial;
};

// Loads one document and prepares the seed. The source suffix always determines
// language; checkpoint metadata is only checked for consistency on resume.
absl::StatusOr<Prepared> Prepare(const fs::path& config_file) {
  Json raw;
  auto loaded = Config::Load(config_file, &raw);
  if (!loaded.ok()) return loaded.status();

  Prepared prepared;
  prepared.config = std::move(*loaded);
  auto& config = prepared.config;
  const auto& run = config.run;

  if (!run.initial_program && !run.checkpoint) {
    return absl::InvalidArgumentError("run.initial_program is required unless run.checkpoint is set");
  }

  if (config.max_iterations < 0) return absl::InvalidArgumentError("max_iterations must be nonnegative");
  if (!utils::ParseLogLevel(config.log_level)) return absl::InvalidArgumentError("Unsupported log_level");

  absl::Status status;

  // The checkpoint fixes language and suffix on resume; otherwise the seed
  // file's suffix determines the language and, unless configured, the suffix.
  std::string language;
  if (run.checkpoint) {
    auto state = controller::CheckpointFile::Read(*run.checkpoint);
    if (!state.ok()) return state.status();
    if (raw.contains("file_suffix") && config.file_suffix != state->file_suffix) {
      return absl::InvalidArgumentError("file_suffix must match checkpoint program metadata");
    }
    const auto inferred = Language(state->file_suffix);
    if (!inferred) return absl::InvalidArgumentError("Cannot infer program language from file suffix");
    if (*inferred != state->language) {
      return absl::DataLossError("Checkpoint language does not match program file suffix");
    }
    language = state->language;
    config.file_suffix = state->file_suffix;
  } else {
    auto text = utils::ReadFile(*run.initial_program);
    if (!text.ok()) return text.status();
    // The seed is a one-file project named after the program file.
    prepared.initial.files = {{fs::path(*run.initial_program).filename().string(), std::move(*text)}};
    const auto suffix = fs::path(*run.initial_program).extension().string();
    const auto inferred = Language(suffix);
    if (!inferred) return absl::InvalidArgumentError("Cannot infer program language from file suffix");
    if (!raw.contains("file_suffix")) {
      config.file_suffix = suffix;
    } else if (Language(config.file_suffix) != inferred) {
      return absl::InvalidArgumentError("file_suffix must identify the same language as the program file suffix");
    }
    language = *inferred;
  }

  if (config.language && *config.language != language) {
    return absl::InvalidArgumentError("language must exactly match the language inferred from program file suffix");
  }
  config.language = language;

  if (!config.database.artifacts_base_path) {
    config.database.artifacts_base_path = (fs::path(run.output_directory) / "artifacts").string();
  }

  if (config.random_seed) {
    // Shared/model-specific seeds were already normalized by Config::Load.
    // Fill only missing seeds from the run-wide seed.
    status = config.UpdateModelParams({{"random_seed", *config.random_seed}});
    if (!status.ok()) return status;
  }

  status = config.Validate();
  if (!status.ok()) return status;

  return prepared;
}

// Assembles the model client, evaluator and controller from the prepared
// configuration, then runs one evolution. StopMonitor is declared after the
// controller so it is destroyed, and its thread joined, before the controller
// it borrows goes away.
absl::StatusOr<controller::RunResult> Execute(const Prepared& prepared, const RuntimeOptions& runtime,
                                              std::ostream& errors) {
  const auto& config = prepared.config;
  const auto& run = config.run;
  // Prepare has already validated the configured level. Declared first so it
  // outlives the controller and monitor that borrow it.
  utils::Logger logger(errors, *utils::ParseLogLevel(config.log_level));

  auto factory = runtime.llm_factory;
  if (!factory) {
    CLIOptions clients;
    clients.codex_executable = run.codex_executable;
    clients.claude_executable = run.claude_executable;
    factory = [clients = std::move(clients)](const LLMModelConfig& model) { return CreateLLM(model, clients); };
  }

  std::shared_ptr<const LLMInterface> llm;
  if (config.max_iterations == 0) {
    // A zero-iteration run still evaluates the seed, so no model client is
    // started and no credentials are needed. NoGeneration fails loudly if
    // something does try to generate.
    llm = std::make_shared<NoGeneration>();
  } else {
    if (config.llm.models.empty())
      return absl::InvalidArgumentError("Configure llm.models for a nonzero max_iterations");

    auto ensemble = LLMEnsemble::Create(config.llm.models, factory);
    if (!ensemble.ok()) return ensemble.status();
    llm = std::move(*ensemble);
  }

  evaluator::EvaluatorOptions evaluation_options;
  evaluation_options.file_suffix = config.file_suffix;

  evaluation_options.feedback.prompt = config.prompt;
  evaluation_options.feedback.models = config.llm.evaluator_models;
  evaluation_options.feedback.factory = factory;
  evaluation_options.feedback.feature_dimensions = config.database.feature_dimensions;
  evaluation_options.feedback.random_seed = static_cast<std::uint32_t>(config.random_seed.value_or(0));

  auto evaluator = evaluator::Evaluator::Create(config.candidate, config.evaluator, *run.base_directory,
                                                std::move(evaluation_options));
  if (!evaluator.ok()) return evaluator.status();

  controller::ControllerOptions options;
  options.logger = &logger;
  auto controller = controller::Controller::Create(config, std::move(llm), std::move(*evaluator), std::move(options));
  if (!controller.ok()) return controller.status();

  logger.Info(run.checkpoint ? "Resuming evolution" : "Starting evolution", " (iterations=", config.max_iterations,
              ", output=", run.output_directory, ')');

  StopMonitor monitor(runtime.stop_signal, **controller, &logger);
  return (*controller)->Run(prepared.initial);
}

// Invalid usage and invalid configuration share exit code 2; every other
// status is reported as a runtime failure.
int ReportError(const absl::Status& status, std::ostream& errors) {
  errors << "Error: " << status << '\n';
  return status.code() == absl::StatusCode::kInvalidArgument ? 2 : 1;
}
}  // namespace

// Only the configuration file is selected on the command line. Runtime values
// belong to YAML, so positional arguments and former override flags are errors.
absl::StatusOr<Arguments> ParseArguments(const std::vector<std::string>& args) {
  Arguments result;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto& text = args[i];
    if (text.find('\0') != std::string::npos) return absl::InvalidArgumentError("NUL in argument");

    if (text == "--help" || text == "-h") {
      result.help = true;
      continue;
    }
    if (text == "--version") {
      result.version = true;
      continue;
    }

    const auto equal = text.find('=');
    const auto flag = text.substr(0, equal);
    if (flag != "--config" && flag != "-c") {
      return absl::InvalidArgumentError("Unexpected argument: " + flag + "; put runtime settings in YAML");
    }
    if (result.config_file) return absl::InvalidArgumentError("Repeated option: --config");

    std::string value;
    if (equal != std::string::npos) {
      value = text.substr(equal + 1);
    } else if (i + 1 < args.size() && !args[i + 1].empty() && args[i + 1].front() != '-') {
      value = args[++i];
    } else {
      return absl::InvalidArgumentError("Missing value for --config");
    }
    if (value.empty() || value.find('\0') != std::string::npos)
      return absl::InvalidArgumentError("Empty or invalid value for --config");
    result.config_file = value;
  }

  if (!result.help && !result.version && !result.config_file)
    return absl::InvalidArgumentError("--config FILE is required");

  return result;
}

std::string Help() {
  return R"(Usage: ievolve --config FILE

  -c, --config FILE          YAML configuration (required to run)
  -h, --help                 Show this help
      --version              Show version

YAML configuration. Values shown are defaults; [required] keys must be set.

run:
  initial_program: null            # [required] seed source unless run.checkpoint is set;
                                   # its suffix selects the language
  checkpoint: null                 # resume from a checkpoints/checkpoint_N directory
  base_directory: .                # relative to the YAML file
  output_directory: ievolve_output # best/ and checkpoints/; relative to base_directory
  target_score: null               # stop once combined_score >= this value
  codex_executable: codex
  claude_executable: claude

max_iterations: 10000              # total attempts, resumed ones included;
                                   # 0 evaluates the seed only, without model calls
checkpoint_interval: 100           # iterations between checkpoints
log_level: INFO                    # DEBUG (adds rejection diagnostics), INFO, WARNING,
                                   # ERROR or CRITICAL
random_seed: 42
language: null                     # inferred from the seed file suffix
file_suffix: .py                   # inferred from the seed file
diff_based_evolution: true         # false asks for a full rewrite each iteration
diff_pattern: '<<<<<<< SEARCH\n(.*?)=======\n(.*?)>>>>>>> REPLACE'
max_code_length: 10000             # characters
enforce_evolve_blocks: false       # revert edits outside EVOLVE-BLOCK regions
early_stopping_patience: null      # accepted children without improvement before
                                   # stopping; null disables early stopping
convergence_threshold: 0.001       # improvement that resets the patience count
early_stopping_metric: combined_score

candidate:
  run:
    argv: []                       # [required] e.g. [python3, '{source}']
  build:                           # optional, e.g. to compile C++ candidates
    argv: []                       # e.g. [c++, -O2, '{source}', -o, '{workspace}/candidate']
    timeout: 60                    # seconds
    max_output_bytes: 8388608

evaluator:
  run:
    argv: []                       # [required] e.g. [python3, '{source}']
  source: null                     # evaluator source substituted for '{source}'
  build: null                      # same fields as candidate.build
  timeout: 300                     # seconds for the whole evaluation, excluding builds
  max_output_bytes: 8388608        # stdout + stderr
  enable_artifacts: true           # keep diagnostics; ENABLE_ARTIFACTS=false also disables
  max_artifact_storage: 104857600  # decoded artifact bytes per result
  use_llm_feedback: false          # let llm.evaluator_models score passing code
  llm_feedback_weight: 0.1

llm:
  models: []                       # [required when max_iterations > 0], e.g.
                                   # - provider: claude_code   (or codex)
                                   #   name: sonnet            (omit for the CLI default)
                                   #   weight: 1.0             (relative share)
  evaluator_models: []             # judges for use_llm_feedback; defaults to models
  timeout: 60                      # seconds per call; models entries may override
  retries: 3                       # timeout, retries, retry_delay and reasoning_effort
  retry_delay: 5
  reasoning_effort: null
  random_seed: null                # seed for weighted model selection
  primary_model: null              # legacy alternative to models, with
  primary_model_weight: null       # secondary_model and secondary_model_weight

prompt:
  template_dir: null               # custom templates, relative to the YAML file
  system_message: system_message   # template name or literal text
  evaluator_system_message: evaluator_system_message
  num_top_programs: 3
  num_diverse_programs: 2
  include_artifacts: true          # show the parent's artifacts in the prompt
  max_artifact_bytes: 20480
  artifact_security_filter: true
  programs_as_changes_description: false
  system_message_changes_description: null
  initial_changes_description: ""
  use_template_stochasticity: true
  template_variations: {}
  suggest_simplification_after_chars: 500
  include_changes_under_chars: 100
  concise_implementation_max_lines: 10
  comprehensive_implementation_min_lines: 50
  code_length_threshold: null
  diff_summary_max_line_len: 100
  diff_summary_max_lines: 30

database:
  population_size: 1000
  archive_size: 100
  num_islands: 5
  elite_selection_ratio: 0.1
  exploration_ratio: 0.2           # exploration + exploitation must not exceed 1.0
  exploitation_ratio: 0.7
  random_seed: 42
  migration_interval: 50
  migration_rate: 0.1
  feature_dimensions: [complexity, diversity]
  feature_bins: 10                 # an integer, or a per-dimension map
  diversity_reference_size: 20
  log_prompts: true
  artifacts_base_path: null        # where artifacts too large to inline are written
  artifact_size_threshold: 32768   # bytes; larger artifacts go to files
  cleanup_old_artifacts: true
  artifact_retention_days: 30

Accepted but ignored: log_dir, prompt.use_meta_prompting, prompt.meta_prompt_weight, and
llm (or model) api_base, api_key, init_client, temperature, top_p, max_tokens,
max_ai_credits and system_message.
Rejected when enabled: evolution_trace.enabled, max_tasks_per_child, llm manual_mode and
allow_all_tools.

Commands are argv arrays; '{source}', '{workspace}' and '{base_dir}' expand in place.
Relative paths resolve against the YAML file's directory; bare executables use PATH.
SIGINT/SIGTERM save a checkpoint after the current operation finishes.
)";
}

int Run(const std::vector<std::string>& args, std::ostream& output, std::ostream& errors,
        const RuntimeOptions& runtime) {
  try {
    auto parsed = ParseArguments(args);
    if (!parsed.ok()) {
      ReportError(parsed.status(), errors);
      errors << "Use --help for usage.\n";
      return 2;
    }

    if (parsed->help) {
      output << Help();
      return 0;
    }
    if (parsed->version) {
      output << "ievolve " IEVOLVE_VERSION "\n";
      return 0;
    }

    if (const int signal = StopSignal(runtime.stop_signal)) return 128 + signal;

    auto prepared = Prepare(*parsed->config_file);
    if (!prepared.ok()) return ReportError(prepared.status(), errors);

    auto result = Execute(*prepared, runtime, errors);
    if (!result.ok()) {
      // A signal that arrives before the controller produces a result surfaces
      // as Cancelled. Report it as the signal exit code, not a generic failure.
      if (result.status().code() == absl::StatusCode::kCancelled && StopSignal(runtime.stop_signal)) {
        return 128 + StopSignal(runtime.stop_signal);
      }
      return ReportError(result.status(), errors);
    }

    output << "Stop reason: " << StopName(result->stop_reason)
           << "\nIterations executed: " << result->iterations_executed << "\nLast iteration: " << result->last_iteration
           << "\nAccepted/rejected (total): " << result->accepted_iterations << '/' << result->rejected_iterations
           << "\nBest program metrics: " << result->best.metrics.dump(2)
           << "\nCheckpoint: " << result->checkpoint_path.string()
           << "\nBest program: " << (fs::path(prepared->config.run.output_directory) / "best").string() << '\n';

    return result->stop_reason == controller::StopReason::kRequested && StopSignal(runtime.stop_signal)
               ? 128 + StopSignal(runtime.stop_signal)
               : 0;
    // Diagnostics stay generic on purpose: the caught exceptions can carry
    // paths and configuration values that the CLI does not echo back.
  } catch (const fs::filesystem_error&) {
    errors << "Error: CLI filesystem operation failed\n";
  } catch (...) {
    errors << "Error: CLI execution failed\n";
  }

  return 1;
}
}  // namespace ievolve::cli
