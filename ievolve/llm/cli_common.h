#ifndef IEVOLVE_LLM_CLI_COMMON_H_
#define IEVOLVE_LLM_CLI_COMMON_H_

#include "ievolve/llm/cli_client.h"
#include "nlohmann/json.hpp"

namespace ievolve::llm_internal {

struct ResolvedCall {
  std::string system_message;
  std::string prompt;
  int timeout = 60;
  int retries = 3;
  int retry_delay = 5;
  std::optional<std::string> reasoning_effort;
};

absl::Status ValidateModel(const LLMModelConfig& config, const CLIOptions& options, std::string_view provider);
absl::StatusOr<ResolvedCall> ResolveCall(const LLMModelConfig& config, const CLIOptions& options,
                                         const LLMRequest& request, std::string_view provider);
using ResponseParser = absl::StatusOr<LLMResponse> (*)(std::string_view);
absl::StatusOr<LLMResponse> RunCLI(const LLMModelConfig& config, const CLIOptions& options, const ResolvedCall& call,
                                   std::vector<std::string> argv, std::string_view provider, ResponseParser parser);
absl::Status ReadUsage(const nlohmann::json& input, bool claude, LLMResponse& response);
bool HasText(std::string_view text);

}  // namespace ievolve::llm_internal
#endif  // IEVOLVE_LLM_CLI_COMMON_H_
