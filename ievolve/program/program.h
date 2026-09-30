#ifndef IEVOLVE_PROGRAM_PROGRAM_H_
#define IEVOLVE_PROGRAM_PROGRAM_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "ievolve/program/metrics.h"

namespace ievolve {

class Program {
 public:
  Program();
  static absl::StatusOr<Program> FromJson(const Metrics& value);
  absl::StatusOr<Metrics> ToJson() const;
  absl::Status Validate() const;

  std::string id;
  std::string code;
  std::string changes_description;
  std::string language = "python";
  std::optional<std::string> parent_id;
  std::int64_t generation = 0;
  double timestamp;
  std::int64_t iteration_found = 0;
  Metrics metrics = Metrics::object();
  double complexity = 0.0;
  double diversity = 0.0;
  Metrics metadata = Metrics::object();
  std::optional<Metrics> prompts;
  std::optional<std::string> artifacts_json;
  std::optional<std::string> artifact_dir;
  std::optional<std::vector<double>> embedding;
};

}  // namespace ievolve

#endif  // IEVOLVE_PROGRAM_PROGRAM_H_
