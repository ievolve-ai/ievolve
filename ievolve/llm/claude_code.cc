#include "ievolve/llm/claude_code.h"

#include <utility>

#include "ievolve/llm/cli_common.h"

namespace ievolve {
namespace {
// Claude Code emits one JSON result envelope. A nonzero exit is not the only
// failure signal: the envelope carries its own is_error and subtype, and a
// budget or turn-limit subtype maps to ResourceExhausted so the caller does not
// retry a request that cannot succeed.
absl::StatusOr<LLMResponse> ParseClaude(std::string_view output) {
  using Json = nlohmann::json;
  try {
    const auto result = Json::parse(output);
    if (!result.is_object() || result.value("type", "") != "result" || !result.contains("is_error") ||
        !result["is_error"].is_boolean() || !result.contains("subtype") || !result["subtype"].is_string()) {
      return absl::DataLossError("llm: invalid Claude result envelope");
    }

    if (result["is_error"].get<bool>() || result["subtype"] != "success") {
      if (result["subtype"] == "error_max_budget_usd" || result["subtype"] == "error_max_turns" ||
          result["subtype"] == "error_max_structured_output_retries") {
        return absl::ResourceExhaustedError("llm: Claude generation limit exceeded");
      }
      if (result["subtype"] == "error_during_execution") return absl::UnavailableError("llm: Claude execution failed");
      return absl::FailedPreconditionError("llm: Claude generation failed");
    }

    if (!result.contains("result") || !result["result"].is_string()) {
      return absl::DataLossError("llm: missing Claude response text");
    }
    LLMResponse response;
    response.text = result["result"].get<std::string>();
    if (!llm_internal::HasText(response.text)) return absl::DataLossError("llm: empty Claude response");

    const auto status = llm_internal::ReadUsage(result, true, response);
    if (!status.ok()) return status;

    return response;
  } catch (const Json::exception&) {
    return absl::DataLossError("llm: malformed Claude response");
  }
}
}  // namespace

ClaudeCodeLLM::ClaudeCodeLLM(LLMModelConfig config, CLIOptions options)
    : config_(std::move(config)), options_(std::move(options)) {}
absl::StatusOr<LLMResponse> ClaudeCodeLLM::Generate(const LLMRequest& request) const {
  const auto call = llm_internal::ResolveCall(config_, options_, request, "claude_code");
  if (!call.ok()) return call.status();

  std::vector<std::string> argv = {options_.claude_executable, "--print",     "--output-format",   "json",
                                   "--no-session-persistence", "--safe-mode", "--tools",           "",
                                   "--disallowedTools",        "mcp__*",      "--permission-mode", "dontAsk"};
  if (config_.name) argv.insert(argv.end(), {"--model", *config_.name});
  if (!call->system_message.empty()) argv.insert(argv.end(), {"--system-prompt", call->system_message});
  if (call->reasoning_effort) argv.insert(argv.end(), {"--effort", *call->reasoning_effort});

  return llm_internal::RunCLI(config_, options_, *call, std::move(argv), "claude_code", ParseClaude);
}
}  // namespace ievolve
