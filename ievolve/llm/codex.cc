#include "ievolve/llm/codex.h"

#include <sstream>
#include <utility>

#include "ievolve/llm/cli_common.h"

namespace ievolve {
namespace {
std::string TomlString(const std::string& value) {
  auto escaped = nlohmann::json(value).dump();
  // JSON permits literal DEL, while TOML basic strings require an escape.
  for (std::size_t offset = 0; (offset = escaped.find('\x7f', offset)) != std::string::npos; offset += 6)
    escaped.replace(offset, 1, "\\u007f");

  return escaped;
}

// Codex streams newline-delimited events rather than one envelope, so the reply
// has to be reassembled. Both a start and a completion event are required: a
// truncated stream that merely stops arriving must not be mistaken for a short
// but successful answer.
absl::StatusOr<LLMResponse> ParseCodex(std::string_view output) {
  using Json = nlohmann::json;
  std::istringstream stream{std::string(output)};
  std::string line;
  LLMResponse response;
  bool completed = false;
  bool started = false;

  try {
    while (std::getline(stream, line)) {
      if (!llm_internal::HasText(line)) continue;

      const auto event = Json::parse(line);
      if (!event.is_object() || !event.contains("type") || !event["type"].is_string()) {
        return absl::DataLossError("llm: invalid Codex event");
      }

      const auto type = event["type"].get<std::string>();
      if (completed && (type.compare(0, 5, "turn.") == 0 || type.compare(0, 5, "item.") == 0 ||
                        type == "thread.started" || type == "error")) {
        return absl::DataLossError("llm: events after Codex turn completion");
      }

      if (type == "turn.started") {
        if (started || !response.text.empty()) return absl::DataLossError("llm: unexpected Codex turn start");
        started = true;
      }
      if (type == "turn.failed") return absl::UnavailableError("llm: Codex turn failed");

      if (type == "item.completed") {
        if (!event.contains("item") || !event["item"].is_object()) {
          return absl::DataLossError("llm: invalid Codex item");
        }
        const auto& item = event["item"];
        if (item.value("type", "") != "agent_message") continue;

        if (item.contains("phase") && !item["phase"].is_null()) {
          if (!item["phase"].is_string()) return absl::DataLossError("llm: invalid Codex message phase");
          if (item["phase"] != "final_answer") continue;
        }

        if (!item.contains("text") || !item["text"].is_string()) {
          return absl::DataLossError("llm: invalid Codex response text");
        }
        response.text = item["text"].get<std::string>();
      } else if (type == "turn.completed") {
        if (!llm_internal::HasText(response.text)) {
          return absl::DataLossError("llm: Codex completion without a response");
        }

        completed = true;
        const auto status = llm_internal::ReadUsage(event, false, response);
        if (!status.ok()) return status;
      }
    }
  } catch (const Json::exception&) {
    return absl::DataLossError("llm: malformed Codex response");
  }

  if (!completed || !llm_internal::HasText(response.text)) return absl::DataLossError("llm: incomplete Codex response");

  return response;
}
}  // namespace

CodexCLILLM::CodexCLILLM(LLMModelConfig config, CLIOptions options)
    : config_(std::move(config)), options_(std::move(options)) {}

absl::StatusOr<LLMResponse> CodexCLILLM::Generate(const LLMRequest& request) const {
  const auto call = llm_internal::ResolveCall(config_, options_, request, "codex");
  if (!call.ok()) return call.status();

  std::vector<std::string> argv = {options_.codex_executable,
                                   "exec",
                                   "--json",
                                   "--ephemeral",
                                   "--skip-git-repo-check",
                                   "--ignore-user-config",
                                   "--sandbox",
                                   "read-only",
                                   "--color",
                                   "never",
                                   "-c",
                                   "approval_policy=\"never\"",
                                   "-c",
                                   "web_search=\"disabled\"",
                                   "-c",
                                   "project_doc_max_bytes=0",
                                   "-c",
                                   "mcp_servers={}"};
  for (const char* feature : {"shell_tool", "unified_exec", "apps", "multi_agent", "hooks", "plugins", "browser_use",
                              "in_app_browser", "shell_snapshot"})
    argv.insert(argv.end(), {"--disable", feature});

  if (config_.name) argv.insert(argv.end(), {"--model", *config_.name});
  if (call->reasoning_effort) {
    argv.insert(argv.end(), {"-c", "model_reasoning_effort=" + TomlString(*call->reasoning_effort)});
  }

  // This remains one argv entry; no shell parses the instructions or config.
  argv.insert(argv.end(), {"-c", "developer_instructions=" + TomlString(call->system_message), "-"});

  return llm_internal::RunCLI(config_, options_, *call, std::move(argv), "codex", ParseCodex);
}
}  // namespace ievolve
