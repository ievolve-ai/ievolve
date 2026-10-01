#include "ievolve/cli/cli.h"

#include <map>

#include "ievolve/cli/stop.h"
#include "ievolve/llm/no_generation.h"
#include "ievolve/utils/file.h"
#include "ievolve/utils/log.h"

namespace ievolve::cli {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

absl::Status Invalid(const std::string& message) { return absl::InvalidArgumentError(message); }

absl::Status CheckFile(const fs::path& path) {
  std::error_code error;
  const auto entry = fs::status(path, error);
  if (error == std::errc::no_such_file_or_directory || !fs::exists(entry)) {
    return absl::NotFoundError("File not found: " + path.string());
  }
  if (error || !fs::is_regular_file(entry)) {
    return absl::FailedPreconditionError("Not a readable file: " + path.string());
  }

  return absl::OkStatus();
}

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

  if (!run.evaluation_file) return Invalid("run.evaluation_file is required");
  if (!run.initial_program && !run.checkpoint) {
    return Invalid("run.initial_program is required unless run.checkpoint is set");
  }

  if (config.max_iterations < 0) return Invalid("max_iterations must be nonnegative");
  if (!utils::ParseLogLevel(config.log_level)) return Invalid("Unsupported log_level");

  auto status = CheckFile(*run.evaluation_file);
  if (!status.ok()) return status;

  std::string suffix;
  std::optional<std::string> checkpoint_language;
  if (run.checkpoint) {
    // These presentation fields are emitted by every controller checkpoint;
    // Controller subsequently validates/restores authoritative population
    // state.
    auto info = utils::ReadFile(fs::path(*run.checkpoint) / "best_program_info.json");
    if (!info.ok()) return info.status();

    try {
      const auto best = Json::parse(*info);
      checkpoint_language = best.at("language").get<std::string>();
    } catch (const Json::exception&) {
      return absl::DataLossError("Invalid checkpoint best-program metadata");
    }

    std::optional<std::string> checkpoint_suffix;
    for (const auto& entry : fs::directory_iterator(*run.checkpoint)) {
      const auto name = entry.path().filename().string();
      if (name == "best_program_info.json") continue;

      if (name.compare(0, 12, "best_program") == 0 && entry.is_regular_file()) {
        if (checkpoint_suffix) return absl::DataLossError("Ambiguous checkpoint program suffix");
        checkpoint_suffix = name.substr(12);
      }
    }
    if (!checkpoint_suffix) return absl::NotFoundError("Checkpoint best-program file not found");
    suffix = std::move(*checkpoint_suffix);
  } else {
    auto text = utils::ReadFile(*run.initial_program);
    if (!text.ok()) return text.status();
    prepared.initial.code = std::move(*text);
    suffix = fs::path(*run.initial_program).extension().string();
  }

  const auto inferred_language = Language(suffix);
  if (!inferred_language) return Invalid("Cannot infer program language from file suffix");
  if (checkpoint_language && *checkpoint_language != *inferred_language) {
    return absl::DataLossError("Checkpoint language does not match program file suffix");
  }
  if (config.language && *config.language != *inferred_language) {
    return Invalid("language must exactly match the language inferred from program file suffix");
  }
  if (!config.language) config.language = *inferred_language;
  if (!raw.contains("file_suffix")) config.file_suffix = suffix;

  const auto output_language = Language(config.file_suffix);
  if (!output_language || *output_language != *inferred_language) {
    return Invalid("file_suffix must identify the same language as the program file suffix");
  }

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
    if (config.llm.models.empty()) return Invalid("Configure llm.models for a nonzero max_iterations");

    auto ensemble = LLMEnsemble::Create(config.llm.models, factory);
    if (!ensemble.ok()) return ensemble.status();
    llm = std::move(*ensemble);
  }

  evaluator::EvaluatorOptions evaluation_options;
  evaluation_options.file_suffix = config.file_suffix;
  evaluation_options.python.python_executable = run.python_executable;

  evaluation_options.feedback.prompt = config.prompt;
  evaluation_options.feedback.models = config.llm.evaluator_models;
  evaluation_options.feedback.factory = factory;
  evaluation_options.feedback.feature_dimensions = config.database.feature_dimensions;
  evaluation_options.feedback.random_seed = static_cast<std::uint32_t>(config.random_seed.value_or(0));

  auto evaluator = evaluator::Evaluator::Create(config.evaluator, *run.evaluation_file, std::move(evaluation_options));
  if (!evaluator.ok()) return evaluator.status();

  controller::ControllerOptions options;
  options.output_directory = run.output_directory;
  options.logger = &logger;
  auto controller = controller::Controller::Create(config, std::move(llm), std::move(*evaluator), std::move(options));
  if (!controller.ok()) return controller.status();

  logger.Info(run.checkpoint ? "Resuming evolution" : "Starting evolution", " (iterations=", config.max_iterations,
              ", output=", run.output_directory, ')');

  StopMonitor monitor(runtime.stop_signal, **controller, &logger);
  controller::RunOptions run_options;
  run_options.target_score = run.target_score;
  if (run.checkpoint) run_options.checkpoint_path = *run.checkpoint;

  return (*controller)->Run(prepared.initial, run_options);
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
    if (text.find('\0') != std::string::npos) return Invalid("NUL in argument");

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
      return Invalid("Unexpected argument: " + flag + "; put runtime settings in YAML");
    }
    if (result.config_file) return Invalid("Repeated option: --config");

    std::string value;
    if (equal != std::string::npos) {
      value = text.substr(equal + 1);
    } else if (i + 1 < args.size() && !args[i + 1].empty() && args[i + 1].front() != '-') {
      value = args[++i];
    } else {
      return Invalid("Missing value for --config");
    }
    if (value.empty() || value.find('\0') != std::string::npos) return Invalid("Empty or invalid value for --config");
    result.config_file = value;
  }

  if (!result.help && !result.version && !result.config_file) return Invalid("--config FILE is required");

  return result;
}

std::string Help() {
  return "Usage: ievolve --config FILE\n"
         "\n"
         "  -c, --config FILE          YAML configuration (required to run)\n"
         "  -h, --help                 Show this help\n"
         "      --version              Show version\n"
         "\n"
         "All runtime settings come from YAML: run.initial_program, run.evaluation_file,\n"
         "run.output_directory, run.checkpoint, run.python_executable, run.target_score,\n"
         "run.codex_executable, run.claude_executable, max_iterations, log_level and llm.models.\n"
         "Relative run paths resolve against the YAML directory; bare executables use PATH.\n"
         "SIGINT/SIGTERM request a checkpoint after the current operation finishes.\n";
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
