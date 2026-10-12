#ifndef IEVOLVE_UTILS_GLOB_H_
#define IEVOLVE_UTILS_GLOB_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

// Path globs relative to a project root. Paths are '/'-separated. '*' and '?'
// never cross '/', and '**' as a whole segment matches zero or more
// directories. There are no character classes, braces or negations.
namespace ievolve::utils {

// InvalidArgument unless the pattern is a nonempty relative path whose
// segments are neither empty, "." nor "..", has no backslash, and uses '**'
// only as a whole segment.
absl::Status ValidateGlob(std::string_view pattern);

// Matches a valid pattern against a '/'-separated relative path.
bool MatchGlob(std::string_view pattern, std::string_view path);

// Regular files under root matching any pattern, as sorted unique relative
// paths. Symlinks are neither followed nor matched, and .git directories are
// skipped.
//   InvalidArgument    no patterns, an invalid pattern, or a pattern that
//                      matched nothing (named by its index)
//   NotFound           root is not a directory
//   FailedPrecondition the tree cannot be listed
absl::StatusOr<std::vector<std::string>> ExpandGlobs(const std::filesystem::path& root,
                                                     const std::vector<std::string>& patterns);

}  // namespace ievolve::utils

#endif  // IEVOLVE_UTILS_GLOB_H_
