#ifndef IEVOLVE_UTILS_FILE_H_
#define IEVOLVE_UTILS_FILE_H_

#include <filesystem>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

// Whole-file helpers shared across components. They only move bytes and report
// a status code per failure kind; policy stays with the caller. Components that
// must refuse symlinks check before reading, and components with their own
// error vocabulary (corrupt checkpoint, artifact I/O, ...) map these codes.
namespace ievolve::utils {

// Reads a regular file in binary mode, following symlinks.
//   NotFound           the path does not exist
//   FailedPrecondition the path cannot be inspected or is not a regular file
//   PermissionDenied   the file cannot be opened
//   DataLoss           reading stopped before end of file
absl::StatusOr<std::string> ReadFile(const std::filesystem::path& path);

// Creates or truncates a file and writes content in binary mode.
//   PermissionDenied   the file cannot be opened for writing
//   DataLoss           writing or closing failed
absl::Status WriteFile(const std::filesystem::path& path, std::string_view content);

}  // namespace ievolve::utils

#endif  // IEVOLVE_UTILS_FILE_H_
