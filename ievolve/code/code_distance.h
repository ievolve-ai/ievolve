#ifndef IEVOLVE_CODE_CODE_DISTANCE_H_
#define IEVOLVE_CODE_CODE_DISTANCE_H_

#include <cstddef>
#include <string_view>

#include "absl/status/statusor.h"
#include "ievolve/program/source_files.h"

namespace ievolve {

class CodeDistance {
 public:
  // Computes edit distance on Unicode code points. Both inputs must be UTF-8.
  static absl::StatusOr<std::size_t> Levenshtein(std::string_view left, std::string_view right);
  // Sums Levenshtein over the union of paths; a path missing on one side
  // counts as empty there.
  static absl::StatusOr<std::size_t> FilesDistance(const SourceFiles& left, const SourceFiles& right);
};

}  // namespace ievolve

#endif  // IEVOLVE_CODE_CODE_DISTANCE_H_
