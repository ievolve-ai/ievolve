#ifndef IEVOLVE_CODE_CODE_DISTANCE_H_
#define IEVOLVE_CODE_CODE_DISTANCE_H_

#include <cstddef>
#include <string_view>

#include "absl/status/statusor.h"

namespace ievolve {

class CodeDistance {
 public:
  // Computes edit distance on Unicode code points. Both inputs must be UTF-8.
  static absl::StatusOr<std::size_t> Levenshtein(std::string_view left, std::string_view right);
};

}  // namespace ievolve

#endif  // IEVOLVE_CODE_CODE_DISTANCE_H_
