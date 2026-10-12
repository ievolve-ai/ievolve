#ifndef IEVOLVE_PROGRAM_UTEST_HELPERS_PYTHON_PROGRAM_H_
#define IEVOLVE_PROGRAM_UTEST_HELPERS_PYTHON_PROGRAM_H_

#include <map>
#include <string>

#include "ievolve/program/metrics.h"

namespace ievolve::program_test {

// The single file a Python program's code becomes: "main" plus the extension
// whose LanguageOf is the Python language. Throws on languages without one, so
// a fixture that cannot round-trip fails loudly.
inline std::string PythonFileName(const std::string& language) {
  static const std::map<std::string, std::string> kExtensions = {
      {"python", ".py"},     {"cpp", ".cpp"}, {"c", ".c"},   {"cuda", ".cu"},    {"javascript", ".js"},
      {"typescript", ".ts"}, {"rust", ".rs"}, {"go", ".go"}, {"java", ".java"},  {"julia", ".jl"},
      {"ruby", ".rb"},       {"lua", ".lua"}, {"r", ".r"},   {"swift", ".swift"}};
  return "main" + kExtensions.at(language);
}

// Python records carry one "code" string and a "language" (default python);
// the port stores files. Replaces "code" in place, so key order is kept, and
// drops "language".
inline Metrics FromPythonProgram(const Metrics& python) {
  if (!python.is_object()) return python;
  const auto language = python.value("language", std::string("python"));
  Metrics result = Metrics::object();
  for (const auto& [key, value] : python.items()) {
    if (key == "language") continue;
    if (key == "code") {
      result["files"] = {{PythonFileName(language), value}};
    } else {
      result[key] = value;
    }
  }
  return result;
}

}  // namespace ievolve::program_test

#endif  // IEVOLVE_PROGRAM_UTEST_HELPERS_PYTHON_PROGRAM_H_
