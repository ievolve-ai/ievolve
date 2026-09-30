#ifndef IEVOLVE_PROMPT_METRICS_H_
#define IEVOLVE_PROMPT_METRICS_H_

#include <string>
#include <vector>

#include "ievolve/program/metrics.h"

namespace ievolve {

std::string FormatFeatureCoordinates(const Metrics& metrics, const std::vector<std::string>& feature_dimensions);
std::string FormatMetrics(const Metrics& metrics, bool bullet_list = true);
std::string FormatValue(const Metrics& value, int precision = -1);

}  // namespace ievolve

#endif  // IEVOLVE_PROMPT_METRICS_H_
