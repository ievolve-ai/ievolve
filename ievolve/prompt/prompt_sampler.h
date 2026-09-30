#ifndef IEVOLVE_PROMPT_PROMPT_SAMPLER_H_
#define IEVOLVE_PROMPT_PROMPT_SAMPLER_H_

#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "ievolve/config/types.h"
#include "ievolve/prompt/artifact_renderer.h"
#include "ievolve/prompt/metrics.h"
#include "ievolve/prompt/prompt_program.h"
#include "ievolve/prompt/template_manager.h"

namespace ievolve {

struct PromptRequest {
  std::string current_program;
  Metrics program_metrics = Metrics::object();
  std::vector<PromptProgram> previous_programs;
  std::vector<PromptProgram> top_programs;
  std::vector<PromptProgram> inspirations;
  std::string language = "python";
  bool diff_based_evolution = true;
  std::optional<std::string> template_key;
  Artifacts program_artifacts;
  std::vector<std::string> feature_dimensions;
  std::string current_changes_description;
  Metrics extra_values = Metrics::object();
};

struct Prompt {
  std::string system;
  std::string user;
};

// Owns mutable RNG state; use a separate sampler per worker. Fixed seeds are
// reproducible within a C++ standard-library implementation, not across Python.
class PromptSampler {
 public:
  explicit PromptSampler(PromptConfig config = {}, std::uint32_t random_seed = std::random_device{}());
  void SetTemplates(std::optional<std::string> system_template = std::nullopt,
                    std::optional<std::string> user_template = std::nullopt);
  TemplateManager& template_manager() { return template_manager_; }
  const TemplateManager& template_manager() const { return template_manager_; }
  absl::StatusOr<Prompt> BuildPrompt(const PromptRequest& request);

 private:
  absl::StatusOr<std::string> FormatHistory(const PromptRequest& request);
  absl::StatusOr<std::string> FormatProgram(const PromptProgram& program, const PromptRequest& request,
                                            const std::string& number, bool diverse) const;
  absl::StatusOr<std::string> FormatInspirations(const std::vector<const PromptProgram*>& programs,
                                                 const PromptRequest& request) const;
  std::string IdentifyImprovementAreas(const PromptRequest& request) const;
  std::string DetermineProgramType(const PromptProgram& program,
                                   const std::vector<std::string>& feature_dimensions = {}) const;
  std::string ExtractUniqueFeatures(const PromptProgram& program) const;
  std::string ApplyVariations(std::string text);

  PromptConfig config_;
  TemplateManager template_manager_;
  absl::Status initialization_status_;
  std::optional<std::string> system_template_override_;
  std::optional<std::string> user_template_override_;
  std::mt19937 random_;
};

}  // namespace ievolve

#endif  // IEVOLVE_PROMPT_PROMPT_SAMPLER_H_
