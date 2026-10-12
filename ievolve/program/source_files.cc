#include "ievolve/program/source_files.h"

#include <map>

#include "absl/strings/ascii.h"
#include "absl/strings/str_split.h"
#include "ievolve/utils/file.h"
#include "ievolve/utils/text.h"

namespace ievolve {

absl::Status ValidateSourcePath(std::string_view path) {
  const auto invalid = [] { return absl::InvalidArgumentError("source path must be a normalized relative path"); };
  if (path.empty() || path.front() == '/') return invalid();
  if (path.find('\\') != std::string_view::npos || path.find('\0') != std::string_view::npos) return invalid();
  for (const auto segment : absl::StrSplit(path, '/')) {
    if (segment.empty() || segment == "." || segment == "..") return invalid();
  }
  return absl::OkStatus();
}

absl::Status ValidateSourceFiles(const SourceFiles& files) {
  if (files.empty()) return absl::InvalidArgumentError("source files must not be empty");
  for (const auto& [path, content] : files) {
    auto status = ValidateSourcePath(path);
    if (!status.ok()) return status;
    if (!utils::IsValidUtf8(content)) return absl::InvalidArgumentError("source file " + path + " is not valid UTF-8");
  }
  return absl::OkStatus();
}

std::size_t TotalLength(const SourceFiles& files) {
  std::size_t total = 0;
  for (const auto& [path, content] : files) total += utils::Utf8Length(content);
  return total;
}

std::string LanguageOf(std::string_view path) {
  static const std::map<std::string, std::string> kLanguages = {
      {".py", "python"},     {".cc", "cpp"},        {".cpp", "cpp"}, {".cxx", "cpp"}, {".h", "cpp"},
      {".hh", "cpp"},        {".hpp", "cpp"},       {".c", "c"},     {".cu", "cuda"}, {".cuh", "cuda"},
      {".js", "javascript"}, {".ts", "typescript"}, {".rs", "rust"}, {".go", "go"},   {".java", "java"},
      {".jl", "julia"},      {".rb", "ruby"},       {".lua", "lua"}, {".r", "r"},     {".swift", "swift"}};

  const auto name = std::filesystem::path(std::string(path)).filename();
  const auto found = kLanguages.find(absl::AsciiStrToLower(name.extension().string()));
  return found == kLanguages.end() ? "text" : found->second;
}

absl::StatusOr<SourceFiles> ReadSourceFiles(const std::filesystem::path& root, const std::vector<std::string>& paths) {
  SourceFiles files;
  for (const auto& path : paths) {
    auto status = ValidateSourcePath(path);
    if (!status.ok()) return status;
    auto content = utils::ReadFile(root / path);
    if (!content.ok()) return content.status();
    files.emplace(path, std::move(*content));
  }
  auto status = ValidateSourceFiles(files);
  if (!status.ok()) return status;
  return files;
}

}  // namespace ievolve
