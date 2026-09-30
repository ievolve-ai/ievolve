#include "ievolve/prompt/metrics.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace ievolve {
namespace {}  // namespace

// Renders a metric the way Python would, because these strings go into prompts
// that are compared against reference output: null prints as "None" and
// booleans as "True"/"False". A requested precision applies to numbers and
// booleans, so
// an explicit format wins over the bool spelling below.
std::string FormatValue(const Metrics& value, int precision) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_null()) return "None";

  if (precision >= 0 && (value.is_number() || value.is_boolean())) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(precision) << Number(value);
    return stream.str();
  }

  if (value.is_boolean()) return value.get<bool>() ? "True" : "False";
  if (value.is_number_float()) {
    const double number = value.get<double>();
    if (std::isnan(number)) return "nan";
    if (std::isinf(number)) return number < 0 ? "-inf" : "inf";
  }

  return value.dump(-1, ' ', false, Metrics::error_handler_t::replace);
}

std::string FormatFeatureCoordinates(const Metrics& metrics, const std::vector<std::string>& feature_dimensions) {
  std::string result;
  if (!metrics.is_object()) return result;

  for (const auto& name : feature_dimensions) {
    const auto value = metrics.find(name);
    if (value == metrics.end()) continue;
    if (value->is_number_float() && std::isnan(value->get<double>())) continue;

    if (!result.empty()) result += ", ";
    result += name + "=" + FormatValue(*value, 2);
  }

  return result;
}

std::string FormatMetrics(const Metrics& metrics, bool bullet_list) {
  std::string result;
  if (!metrics.is_object()) return result;

  for (const auto& item : metrics.items()) {
    if (!result.empty()) result += bullet_list ? "\n" : ", ";
    result += (bullet_list ? "- " : "") + item.key() + ": " + FormatValue(item.value(), 4);
  }

  return result;
}

}  // namespace ievolve
