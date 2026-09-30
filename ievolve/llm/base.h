#ifndef IEVOLVE_LLM_BASE_H_
#define IEVOLVE_LLM_BASE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "ievolve/config/types.h"

namespace ievolve {

struct LLMMessage {
  std::string role;
  std::string content;
};

struct GenerationOptions {
  std::optional<int> timeout;
  std::optional<int> retries;
  std::optional<int> retry_delay;
  std::optional<std::string> reasoning_effort;
};

struct LLMRequest {
  // Missing uses the model default; an explicit empty string clears it.
  std::optional<std::string> system_message;
  std::vector<LLMMessage> messages;
  GenerationOptions options;
};

struct LLMUsage {
  std::optional<std::int64_t> input_tokens;
  std::optional<std::int64_t> output_tokens;
  std::optional<std::int64_t> cached_input_tokens;
  std::optional<double> cost_usd;
};

struct LLMResponse {
  std::string text;
  std::string model;
  std::string provider;
  std::optional<LLMUsage> usage;
};

class LLMInterface {
 public:
  virtual ~LLMInterface() = default;
  // Implementations must support concurrent calls; each result owns its usage.
  virtual absl::StatusOr<LLMResponse> Generate(const LLMRequest& request) const = 0;
  absl::StatusOr<LLMResponse> Generate(std::string_view prompt, const GenerationOptions& options = {}) const;
  absl::StatusOr<LLMResponse> GenerateWithContext(std::string_view system_message,
                                                  const std::vector<LLMMessage>& messages,
                                                  const GenerationOptions& options = {}) const;
};

using LLMFactory = std::function<absl::StatusOr<std::shared_ptr<LLMInterface>>(const LLMModelConfig&)>;

}  // namespace ievolve
#endif  // IEVOLVE_LLM_BASE_H_
