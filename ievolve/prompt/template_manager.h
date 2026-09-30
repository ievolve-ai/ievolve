#ifndef IEVOLVE_PROMPT_TEMPLATE_MANAGER_H_
#define IEVOLVE_PROMPT_TEMPLATE_MANAGER_H_

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "ievolve/prompt/metrics.h"

namespace ievolve {

class TemplateManager {
 public:
  TemplateManager();

  // A missing directory leaves defaults in place. Invalid files return an error
  // without changing any existing templates or fragments.
  absl::Status LoadDirectory(const std::filesystem::path& directory);
  bool HasTemplate(const std::string& name) const;
  absl::StatusOr<std::string> GetTemplate(const std::string& name) const;
  std::string GetFragment(const std::string& name, const Metrics& values = Metrics::object()) const;
  void AddTemplate(const std::string& name, std::string text);
  void AddFragment(const std::string& name, std::string text);

  // Supports {name}, {{/}}, and numeric {name:.Nf}. Inserted values are
  // literal.
  static absl::StatusOr<std::string> Format(std::string_view text, const Metrics& values);

 private:
  std::map<std::string, std::string> templates_;
  std::map<std::string, std::string> fragments_;
};

}  // namespace ievolve

#endif  // IEVOLVE_PROMPT_TEMPLATE_MANAGER_H_
