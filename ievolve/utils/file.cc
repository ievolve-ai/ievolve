#include "ievolve/utils/file.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace ievolve::utils {
namespace fs = std::filesystem;

absl::StatusOr<std::string> ReadFile(const fs::path& path) {
  std::error_code error;
  const auto entry = fs::status(path, error);
  if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(entry))) {
    return absl::NotFoundError("File not found: " + path.string());
  }
  if (error || !fs::is_regular_file(entry)) {
    return absl::FailedPreconditionError("Not a readable file: " + path.string());
  }

  std::ifstream stream(path, std::ios::binary);
  if (!stream) return absl::PermissionDeniedError("Cannot read: " + path.string());

  std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  if (stream.bad()) return absl::DataLossError("Cannot finish reading: " + path.string());

  return text;
}

absl::Status WriteFile(const fs::path& path, std::string_view content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) return absl::PermissionDeniedError("Cannot open for writing: " + path.string());

  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  stream.close();
  if (!stream) return absl::DataLossError("Cannot finish writing: " + path.string());

  return absl::OkStatus();
}

}  // namespace ievolve::utils
