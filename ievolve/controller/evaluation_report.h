#ifndef IEVOLVE_CONTROLLER_EVALUATION_REPORT_H_
#define IEVOLVE_CONTROLLER_EVALUATION_REPORT_H_

#include <filesystem>

#include "ievolve/controller/iteration.h"

namespace ievolve::controller {
absl::Status WriteEvaluationReport(const std::filesystem::path& output_directory, const RejectedEvaluation& evaluation);
}  // namespace ievolve::controller
#endif  // IEVOLVE_CONTROLLER_EVALUATION_REPORT_H_
