#ifndef IEVOLVE_PROMPT_PROMPT_PROGRAM_H_
#define IEVOLVE_PROMPT_PROMPT_PROGRAM_H_

#include "ievolve/program/program.h"

namespace ievolve {

// The prompt-facing snapshot. Database identity and storage stay outside this
// component. Metadata fields are typed to avoid unchecked dynamic lookups.
struct PromptProgram {
  static absl::StatusOr<PromptProgram> FromProgram(const Program& program);
  std::optional<std::string> id;
  std::string code;
  Metrics metrics = Metrics::object();
  std::optional<std::string> changes_description;
  std::optional<std::string> changes;
  Metrics parent_metrics = Metrics::object();
  std::vector<std::string> key_features;
  bool diverse = false;
  bool migrant = false;
  bool random = false;
};

}  // namespace ievolve

#endif  // IEVOLVE_PROMPT_PROMPT_PROGRAM_H_
