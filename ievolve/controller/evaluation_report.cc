#include "ievolve/controller/evaluation_report.h"

#include <regex>

#include "ievolve/utils/file.h"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace ievolve::controller {
absl::Status WriteEvaluationReport(const std::filesystem::path& output_directory,
                                   const RejectedEvaluation& evaluation) {
  if (evaluation.outcome.passed || !std::regex_match(evaluation.candidate.id, std::regex("initial|iteration-[0-9]+"))) {
    return absl::InvalidArgumentError("Rejection report requires a rejected outcome and controller candidate ID");
  }
  auto candidate = evaluation.candidate.ToJson();
  if (!candidate.ok()) return candidate.status();
  auto outcome = evaluation.outcome.ToJson();
  if (!outcome.ok()) return outcome.status();
  const Metrics report = {{"candidate", *candidate}, {"outcome", *outcome}};
  const auto directory = output_directory / "evaluations";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return absl::UnavailableError("Cannot create evaluation report directory: " + error.message());
#ifdef _WIN32
  return absl::UnimplementedError("Atomic evaluation reports require POSIX");
#else
  std::string name = (directory / ".report-XXXXXX").string();
  const int descriptor = mkstemp(name.data());
  if (descriptor < 0) return absl::UnavailableError("Cannot create temporary evaluation report");
  close(descriptor);
  struct TemporaryFile {
    std::filesystem::path path;
    ~TemporaryFile() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } temporary{name};
  auto status = utils::WriteFile(temporary.path, report.dump(2));
  if (!status.ok()) return absl::UnavailableError("Cannot write evaluation report: " + std::string(status.message()));
  std::filesystem::rename(temporary.path, directory / (evaluation.candidate.id + ".json"), error);
  if (error) return absl::UnavailableError("Cannot publish evaluation report: " + error.message());
  return absl::OkStatus();
#endif
}
}  // namespace ievolve::controller
