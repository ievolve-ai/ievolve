#include "ievolve/llm/cli_common.h"

#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

#include "ievolve/utils/text.h"

namespace ievolve::llm_internal {
namespace {
using Json = nlohmann::json;

absl::Status Invalid(std::string_view field) {
  return absl::InvalidArgumentError("llm: invalid " + std::string(field));
}

// Adds the NUL rejection that argv and stdin payloads need on top of the shared
// UTF-8 check.
bool ValidText(const std::string& text, bool argument = false) {
  if (argument && text.find('\0') != std::string::npos) return false;

  return utils::IsValidUtf8(text);
}

absl::Status ValidateCall(const ResolvedCall& call, std::string_view provider) {
  if (call.timeout <= 0) return Invalid("timeout");
  if (call.retries < 0) return Invalid("retries");
  if (call.retry_delay < 0) return Invalid("retry_delay");

  if (call.reasoning_effort) {
    const auto& effort = *call.reasoning_effort;
    const bool common = effort == "low" || effort == "medium" || effort == "high" || effort == "xhigh";
    const bool specific = provider == "claude_code" ? effort == "max" : (effort == "none" || effort == "minimal");
    if (!common && !specific) return Invalid("reasoning_effort");
  }

  return absl::OkStatus();
}

ResolvedCall Defaults(const LLMModelConfig& config) {
  ResolvedCall call;
  call.system_message = config.system_message.value_or("");
  call.timeout = config.timeout.value_or(60);
  call.retries = config.retries.value_or(3);
  call.retry_delay = config.retry_delay.value_or(5);
  call.reasoning_effort = config.reasoning_effort;

  return call;
}

class TemporaryDirectory {
 public:
  absl::Status Create() {
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    if (error) return absl::InternalError("llm: cannot locate temporary directory");

    static std::atomic<std::uint64_t> sequence{0};
    for (int attempt = 0; attempt < 100; ++attempt) {
      const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
      auto candidate = base / ("ievolve_llm_" + std::to_string(id) + "_" + std::to_string(sequence++));
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = std::move(candidate);
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace,
                                     error);
        if (error) return absl::PermissionDeniedError("llm: cannot protect temporary directory");

        return absl::OkStatus();
      }

      if (error && error != std::errc::file_exists) {
        return absl::PermissionDeniedError("llm: cannot create temporary directory");
      }
    }

    return absl::AlreadyExistsError("llm: cannot allocate temporary directory");
  }
  ~TemporaryDirectory() {
    if (path_.empty()) return;

    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

absl::Status ReadCount(const Json& usage, const char* key, std::optional<std::int64_t>& output) {
  const auto item = usage.find(key);
  if (item == usage.end()) return absl::OkStatus();

  if (!item->is_number_integer() ||
      (item->is_number_unsigned() &&
       item->get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) ||
      (!item->is_number_unsigned() && item->get<std::int64_t>() < 0)) {
    return absl::DataLossError("llm: invalid token usage");
  }

  output = item->get<std::int64_t>();
  return absl::OkStatus();
}
}  // namespace

bool HasText(std::string_view text) { return text.find_first_not_of(" \t\r\n") != std::string_view::npos; }

absl::Status ValidateModel(const LLMModelConfig& config, const CLIOptions& options, std::string_view provider) {
  if (config.provider && *config.provider != provider) return Invalid("provider");
  if (config.name && (!HasText(*config.name) || !ValidText(*config.name, true))) return Invalid("model name");
  if (config.system_message && !ValidText(*config.system_message, true)) return Invalid("system_message");
  if (config.manual_mode.value_or(false)) return Invalid("manual_mode for CLI clients");
  if (config.allow_all_tools.value_or(false)) return Invalid("allow_all_tools for text generation");

  if (!options.runner) return Invalid("process runner");
  const auto& executable = provider == "claude_code" ? options.claude_executable : options.codex_executable;
  if (executable.empty() || !ValidText(executable, true)) return Invalid("CLI executable");
  if (options.max_output_bytes == 0) return Invalid("max_output_bytes");

  return ValidateCall(Defaults(config), provider);
}

// Folds the model configuration and the per-request overrides into one resolved
// call, validating everything before a process is started.
//
// A single message is sent as raw text; two or more become a JSON
// transcript, so a multi-turn conversation survives a CLI that accepts only
// one prompt. At
// least one nonempty user message is required, because a transcript of nothing
// but assistant turns would silently produce an unusable request.
absl::StatusOr<ResolvedCall> ResolveCall(const LLMModelConfig& config, const CLIOptions& options,
                                         const LLMRequest& request, std::string_view provider) {
  auto status = ValidateModel(config, options, provider);
  if (!status.ok()) return status;

  auto call = Defaults(config);
  if (request.system_message) call.system_message = *request.system_message;
  if (!ValidText(call.system_message, true)) return Invalid("system_message");

  if (request.options.timeout) call.timeout = *request.options.timeout;
  if (request.options.retries) call.retries = *request.options.retries;
  if (request.options.retry_delay) call.retry_delay = *request.options.retry_delay;
  if (request.options.reasoning_effort) call.reasoning_effort = request.options.reasoning_effort;
  status = ValidateCall(call, provider);
  if (!status.ok()) return status;

  bool user = false;
  Json messages = Json::array();
  for (const auto& message : request.messages) {
    if (message.role != "user" && message.role != "assistant") return Invalid("message role");
    if (!ValidText(message.content)) return Invalid("message content encoding");

    user |= message.role == "user" && HasText(message.content);
    messages.push_back({{"role", message.role}, {"content", message.content}});
  }
  if (!user) return Invalid("messages: a nonempty user message is required");

  call.prompt =
      request.messages.size() == 1 ? request.messages[0].content : Json{{"messages", std::move(messages)}}.dump();

  return call;
}

// Runs the model CLI with bounded retries and parses its reply.
//
// Unless the caller pins a working directory, each call gets a fresh empty
// temporary one, so the model cannot see or disturb the project tree. The
// prompt travels on stdin rather than argv, keeping it out of the process
// table.
//
// Retries are deliberately selective: timeouts and execution failures are worth
// repeating, while known resource limits, argument and startup errors (exit
// codes 2, 126, 127), invalid output and output overruns are not. Any other
// nonzero
// exit cannot be classified reliably, so it gets the configured bounded retry.
absl::StatusOr<LLMResponse> RunCLI(const LLMModelConfig& config, const CLIOptions& options, const ResolvedCall& call,
                                   std::vector<std::string> argv, std::string_view provider, ResponseParser parser) {
  TemporaryDirectory temporary;
  process::ProcessRequest process;
  process.argv = std::move(argv);
  process.stdin_text = call.prompt;
  process.timeout = std::chrono::seconds(call.timeout);
  process.max_output_bytes = options.max_output_bytes;
  process.working_directory = options.working_directory;
  if (!process.working_directory) {
    const auto status = temporary.Create();
    if (!status.ok()) return status;
    process.working_directory = temporary.path();
  }

  for (int attempt = 0;; ++attempt) {
    absl::StatusOr<LLMResponse> response = absl::InternalError("llm: CLI call failed");
    try {
      auto result = options.runner(process);
      if (!result.ok()) {
        // Preserve the code but not arbitrary diagnostics from injected
        // runners.
        response = absl::Status(result.status().code(), "llm: CLI process failed");
      } else if (result->stdout_text.size() > options.max_output_bytes ||
                 result->stderr_text.size() > options.max_output_bytes - result->stdout_text.size()) {
        response = absl::ResourceExhaustedError("llm: CLI output limit exceeded");
      } else if (result->exit_code != 0) {
        auto code = absl::StatusCode::kUnavailable;
        if (result->exit_code == 2) code = absl::StatusCode::kInvalidArgument;
        if (result->exit_code == 126) code = absl::StatusCode::kPermissionDenied;
        if (result->exit_code == 127) code = absl::StatusCode::kNotFound;
        response = absl::Status(code, "llm: CLI exited with code " + std::to_string(result->exit_code));

        // Known structured limits remain nonretryable on unsuccessful exits.
        // A success envelope never overrides a nonzero exit status.
        const auto parsed = parser(result->stdout_text);
        if (!parsed.ok() && (parsed.status().code() == absl::StatusCode::kResourceExhausted ||
                             parsed.status().code() == absl::StatusCode::kFailedPrecondition)) {
          response = parsed.status();
        }
      } else {
        response = parser(result->stdout_text);
      }
    } catch (...) {
      return absl::InternalError("llm: CLI runner or parser threw an exception");
    }

    if (response.ok()) {
      response->model = config.name.value_or("");
      response->provider = provider;
      return response;
    }

    if (attempt >= call.retries || (response.status().code() != absl::StatusCode::kUnavailable &&
                                    response.status().code() != absl::StatusCode::kDeadlineExceeded)) {
      return response.status();
    }

    std::this_thread::sleep_for(std::chrono::seconds(call.retry_delay));
  }
}

absl::Status ReadUsage(const Json& input, bool claude, LLMResponse& response) {
  const auto usage = input.find("usage");
  const auto cost = input.find("total_cost_usd");
  if (usage == input.end() && (!claude || cost == input.end())) return absl::OkStatus();

  LLMUsage value;
  if (usage != input.end()) {
    if (!usage->is_object()) return absl::DataLossError("llm: invalid usage object");

    auto status = ReadCount(*usage, "input_tokens", value.input_tokens);
    if (!status.ok()) return status;
    status = ReadCount(*usage, "output_tokens", value.output_tokens);
    if (!status.ok()) return status;
    status = ReadCount(*usage, claude ? "cache_read_input_tokens" : "cached_input_tokens", value.cached_input_tokens);
    if (!status.ok()) return status;
  }

  if (claude && cost != input.end()) {
    if (!cost->is_number() || !std::isfinite(cost->get<double>()) || cost->get<double>() < 0) {
      return absl::DataLossError("llm: invalid usage cost");
    }
    value.cost_usd = cost->get<double>();
  }

  response.usage = value;
  return absl::OkStatus();
}
}  // namespace ievolve::llm_internal
