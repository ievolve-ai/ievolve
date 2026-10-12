#ifndef IEVOLVE_PROGRAM_SOURCE_FILES_H_
#define IEVOLVE_PROGRAM_SOURCE_FILES_H_

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace ievolve {

// The evolvable files of a project: '/'-separated paths relative to the
// project directory, mapped to UTF-8 content. Ordered by path, so iteration and
// serialization are deterministic.
using SourceFiles = std::map<std::string, std::string>;

// InvalidArgument unless the path is nonempty, relative, '/'-separated, free
// of NUL and backslashes, and has no empty, "." or ".." segment.
absl::Status ValidateSourcePath(std::string_view path);

// InvalidArgument unless there is at least one file, every path is valid and
// every content is UTF-8. Messages name the path, never the content.
absl::Status ValidateSourceFiles(const SourceFiles& files);

// UTF-8 code points across all contents.
std::size_t TotalLength(const SourceFiles& files);

// Code-fence language for a path's extension, compared case-insensitively;
// "text" when the extension is unknown.
std::string LanguageOf(std::string_view path);

// Reads each path under root into a validated SourceFiles. Read failures keep
// utils::ReadFile's codes; invalid paths and non-UTF-8 content are
// InvalidArgument.
absl::StatusOr<SourceFiles> ReadSourceFiles(const std::filesystem::path& root, const std::vector<std::string>& paths);

}  // namespace ievolve

#endif  // IEVOLVE_PROGRAM_SOURCE_FILES_H_
