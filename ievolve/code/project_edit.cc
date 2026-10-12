#include "ievolve/code/project_edit.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"

namespace ievolve {
namespace {

constexpr std::string_view kFence = "```";

bool IsFence(std::string_view line) { return absl::StartsWith(absl::StripLeadingAsciiWhitespace(line), kFence); }

// Strips the decorations models put around a file name, e.g.
// "### File: `src/a.py`" or "**src/a.py**:".
std::string CleanPath(std::string_view line) {
  auto text = absl::StripAsciiWhitespace(line);
  while (!text.empty() && text.front() == '#') text.remove_prefix(1);
  text = absl::StripAsciiWhitespace(text);
  if (absl::StartsWithIgnoreCase(text, "file:")) text = absl::StripAsciiWhitespace(text.substr(5));

  const auto decoration = [](char c) { return c == '*' || c == '`' || c == ':'; };
  while (!text.empty() && decoration(text.front())) text.remove_prefix(1);
  while (!text.empty() && decoration(text.back())) text.remove_suffix(1);
  return std::string(absl::StripAsciiWhitespace(text));
}

// The last nonempty line of text that is not a code fence.
std::string_view LastTextLine(std::string_view text) {
  while (!text.empty()) {
    const auto newline = text.find_last_of('\n', text.size() - 1);
    const auto start = newline == std::string_view::npos ? 0 : newline + 1;
    const auto line = text.substr(start);
    if (!absl::StripAsciiWhitespace(line).empty() && !IsFence(line)) return line;
    if (newline == std::string_view::npos) break;
    text = text.substr(0, newline);
  }
  return {};
}

bool IsBlank(std::string_view text) { return absl::StripAsciiWhitespace(text).empty(); }

absl::Status Unnamed(const char* kind, std::size_t index) {
  return absl::InvalidArgumentError(std::string(kind) + " [" + std::to_string(index) +
                                    "] does not name a project file");
}

// Unnamed blocks of a single-file project: they target the file, or are routed
// by content when a changes description is also editable.
absl::Status RouteUnnamed(std::vector<DiffBlock> unnamed, const std::string& path, const std::string& content,
                          std::optional<std::string_view> description, FileDiffs& result) {
  auto& file = result.files[path];
  if (!description) {
    std::move(unnamed.begin(), unnamed.end(), std::back_inserter(file));
    return absl::OkStatus();
  }

  auto targets = CodeParser::SplitDiffsByTarget(unnamed, content, *description);
  if (!targets.ok()) return targets.status();
  std::move(targets->code.begin(), targets->code.end(), std::back_inserter(file));
  std::move(targets->changes_description.begin(), targets->changes_description.end(),
            std::back_inserter(result.changes_description));
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<FileDiffs> GroupDiffsByFile(const std::vector<DiffBlock>& blocks, std::string_view response,
                                           const SourceFiles& files, std::optional<std::string_view> description) {
  FileDiffs result;
  std::vector<DiffBlock> unnamed;
  std::optional<std::string> previous;
  std::size_t previous_end = 0;

  for (std::size_t i = 0; i < blocks.size(); ++i) {
    const auto& block = blocks[i];
    const auto begin = std::min(block.begin, response.size());
    const bool adjacent =
        previous && previous_end <= begin && IsBlank(response.substr(previous_end, begin - previous_end));
    const auto path = adjacent ? *previous : CleanPath(LastTextLine(response.substr(0, begin)));
    previous_end = std::min(block.end, response.size());

    if (description && path == kChangesDescriptionPath) {
      result.changes_description.push_back(block);
      previous = path;
    } else if (files.count(path)) {
      result.files[path].push_back(block);
      previous = path;
    } else if (files.size() == 1) {
      unnamed.push_back(block);
      previous.reset();
    } else {
      return Unnamed("SEARCH block", i);
    }
  }

  if (!unnamed.empty()) {
    const auto& [path, content] = *files.begin();
    auto status = RouteUnnamed(std::move(unnamed), path, content, description, result);
    if (!status.ok()) return status;
  }
  return result;
}

FilesApplication ApplyFileDiffs(const SourceFiles& files, const FileDiffs& diffs) {
  FilesApplication result{files, 0};
  for (const auto& [path, blocks] : diffs.files) {
    const auto found = result.files.find(path);
    if (found == result.files.end()) continue;

    auto applied = CodeParser::ApplyDiffBlocks(found->second, blocks);
    if (applied.applied_count == 0) continue;
    found->second = std::move(applied.text);
    result.applied_count += applied.applied_count;
  }
  return result;
}

absl::StatusOr<SourceFiles> ParseFileRewrite(std::string_view response, const SourceFiles& files) {
  if (files.size() == 1) {
    const auto& path = files.begin()->first;
    return SourceFiles{{path, CodeParser::ParseFullRewrite(response, LanguageOf(path))}};
  }

  SourceFiles result = files;
  std::set<std::string> rewritten;
  std::string_view last_text;
  std::optional<std::string> open_path;
  std::size_t content_start = 0;
  std::size_t index = 0;

  for (std::size_t start = 0; start < response.size();) {
    const auto newline = response.find('\n', start);
    const auto next = newline == std::string_view::npos ? response.size() : newline + 1;
    const auto line = response.substr(start, next - start);

    if (open_path) {
      if (absl::StripAsciiWhitespace(line) == kFence) {
        result[*open_path] = std::string(response.substr(content_start, start - content_start));
        open_path.reset();
        last_text = {};
      }
    } else if (IsFence(line)) {
      auto path = CleanPath(last_text);
      if (!files.count(path)) return Unnamed("Rewrite block", index);
      if (!rewritten.insert(path).second) {
        return absl::InvalidArgumentError("Rewrite block [" + std::to_string(index) + "] repeats a file");
      }
      open_path = std::move(path);
      content_start = next;
      ++index;
    } else if (!IsBlank(line)) {
      last_text = line;
    }
    start = next;
  }

  if (open_path) return absl::InvalidArgumentError("Rewrite block is not closed");
  if (rewritten.empty()) return absl::InvalidArgumentError("Rewrite contains no file blocks");
  return result;
}

absl::StatusOr<SourceFiles> EnforceEditableFiles(const SourceFiles& original, const SourceFiles& proposed) {
  const auto same_paths = std::equal(original.begin(), original.end(), proposed.begin(), proposed.end(),
                                     [](const auto& left, const auto& right) { return left.first == right.first; });
  if (!same_paths) return absl::InvalidArgumentError("Edited files differ from the original files");

  SourceFiles result;
  for (const auto& [path, content] : proposed) {
    auto enforced = CodeParser::EnforceEditableBlocks(original.at(path), content);
    if (!enforced.ok()) {
      return absl::Status(enforced.status().code(), path + ": " + std::string(enforced.status().message()));
    }
    result.emplace(path, std::move(*enforced));
  }
  return result;
}

}  // namespace ievolve
