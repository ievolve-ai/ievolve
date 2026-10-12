#include "ievolve/utils/glob.h"

#include <algorithm>
#include <set>
#include <system_error>

namespace ievolve::utils {
namespace {
namespace fs = std::filesystem;

constexpr std::string_view kAnyDirectories = "**";

std::vector<std::string_view> Segments(std::string_view text) {
  std::vector<std::string_view> segments;
  std::size_t start = 0;
  while (true) {
    const auto slash = text.find('/', start);
    segments.push_back(text.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
    if (slash == std::string_view::npos) return segments;
    start = slash + 1;
  }
}

// '*' matches any run and '?' any single character within one segment. The
// backtracking point is the most recent '*', which keeps this linear-ish.
bool MatchSegment(std::string_view pattern, std::string_view text) {
  std::size_t p = 0, t = 0;
  auto star = std::string_view::npos;
  std::size_t resume = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      resume = t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++resume;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

bool MatchSegments(const std::vector<std::string_view>& pattern, std::size_t p,
                   const std::vector<std::string_view>& path, std::size_t t) {
  if (p == pattern.size()) return t == path.size();
  if (pattern[p] == kAnyDirectories) {
    for (auto next = t; next <= path.size(); ++next) {
      if (MatchSegments(pattern, p + 1, path, next)) return true;
    }
    return false;
  }
  return t < path.size() && MatchSegment(pattern[p], path[t]) && MatchSegments(pattern, p + 1, path, t + 1);
}

}  // namespace

absl::Status ValidateGlob(std::string_view pattern) {
  const auto invalid = [](const char* reason) { return absl::InvalidArgumentError(std::string("glob ") + reason); };
  if (pattern.empty()) return invalid("must not be empty");
  if (pattern.front() == '/') return invalid("must be relative");
  if (pattern.find('\\') != std::string_view::npos) return invalid("must use '/' separators");
  for (const auto segment : Segments(pattern)) {
    if (segment.empty() || segment == "." || segment == "..") return invalid("has an empty, '.' or '..' segment");
    if (segment != kAnyDirectories && segment.find(kAnyDirectories) != std::string_view::npos) {
      return invalid("uses '**' inside a segment");
    }
  }
  return absl::OkStatus();
}

bool MatchGlob(std::string_view pattern, std::string_view path) {
  return MatchSegments(Segments(pattern), 0, Segments(path), 0);
}

absl::StatusOr<std::vector<std::string>> ExpandGlobs(const fs::path& root, const std::vector<std::string>& patterns) {
  if (patterns.empty()) return absl::InvalidArgumentError("glob list must not be empty");
  for (std::size_t i = 0; i < patterns.size(); ++i) {
    const auto status = ValidateGlob(patterns[i]);
    if (!status.ok()) return absl::InvalidArgumentError("[" + std::to_string(i) + "] " + std::string(status.message()));
  }

  std::error_code error;
  if (!fs::is_directory(root, error)) return absl::NotFoundError("glob root is not a directory");

  std::set<std::string> files;
  std::vector<bool> matched(patterns.size(), false);
  fs::recursive_directory_iterator it(root, error), end;
  if (error) return absl::FailedPreconditionError("glob root cannot be listed");
  for (; it != end; it.increment(error)) {
    if (error) return absl::FailedPreconditionError("glob tree cannot be listed");
    const auto status = it->symlink_status(error);
    if (error) return absl::FailedPreconditionError("glob entry cannot be inspected");
    if (fs::is_directory(status)) {
      if (it->path().filename() == ".git") it.disable_recursion_pending();
      continue;
    }
    if (!fs::is_regular_file(status)) continue;

    const auto relative = it->path().lexically_relative(root).generic_string();
    for (std::size_t i = 0; i < patterns.size(); ++i) {
      if (MatchGlob(patterns[i], relative)) {
        matched[i] = true;
        files.insert(relative);
      }
    }
  }
  if (error) return absl::FailedPreconditionError("glob tree cannot be listed");

  const auto missing = std::find(matched.begin(), matched.end(), false);
  if (missing != matched.end()) {
    return absl::InvalidArgumentError("glob [" + std::to_string(missing - matched.begin()) + "] matched no files");
  }
  return std::vector<std::string>(files.begin(), files.end());
}

}  // namespace ievolve::utils
