#include "ievolve/code/code_parser.h"

#include <algorithm>
#include <optional>
#include <regex>
#include <utility>

#include "ievolve/utils/text.h"

namespace ievolve {
namespace {
using utils::JoinLines;
using utils::SplitLines;
using utils::TrimRight;

constexpr std::string_view kSearch = "<<<<<<< SEARCH\n";
constexpr std::string_view kSeparator = "=======\n";
constexpr std::string_view kReplace = ">>>>>>> REPLACE";

std::vector<int> Delimiters(std::string_view text) {
  std::vector<int> markers;
  for (auto line : SplitLines(text)) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    const auto start = line.find_first_not_of(" \t");
    if (start == std::string_view::npos) continue;
    const auto end = line.find_last_not_of(" \t");
    line = line.substr(start, end - start + 1);

    if (line == "<<<<<<< SEARCH") {
      markers.push_back(0);
    } else if (line == "=======") {
      markers.push_back(1);
    } else if (line == ">>>>>>> REPLACE") {
      markers.push_back(2);
    }
  }

  return markers;
}

absl::StatusOr<std::vector<DiffBlock>> ExtractStandard(std::string_view text) {
  std::vector<DiffBlock> result;
  std::size_t cursor = 0;

  while (true) {
    const auto start = text.find(kSearch, cursor);
    if (start == std::string_view::npos) break;

    const auto search_start = start + kSearch.size();
    const auto separator = text.find(kSeparator, search_start);
    if (separator == std::string_view::npos) break;

    const auto replacement_start = separator + kSeparator.size();
    const auto end = text.find(kReplace, replacement_start);
    if (end == std::string_view::npos) break;

    if (!Delimiters(text.substr(cursor, start - cursor)).empty()) {
      return absl::InvalidArgumentError("Unmatched SEARCH/REPLACE delimiter outside a diff block");
    }
    const auto block_end = end + kReplace.size();
    if (Delimiters(text.substr(start, block_end - start)) != std::vector<int>{0, 1, 2}) {
      return absl::InvalidArgumentError("Malformed SEARCH/REPLACE delimiter sequence");
    }

    result.push_back({std::string(TrimRight(text.substr(search_start, separator - search_start))),
                      std::string(TrimRight(text.substr(replacement_start, end - replacement_start)))});
    cursor = block_end;
  }

  if (!Delimiters(text.substr(cursor)).empty()) {
    return absl::InvalidArgumentError("Unmatched SEARCH/REPLACE delimiter outside a diff block");
  }

  return result;
}

// std::regex lacks a C++17 DOTALL flag. Preserve escapes/classes while
// replacing each wildcard with a class that also accepts line breaks.
std::string DotAll(std::string_view pattern) {
  std::string result;
  bool in_class = false;

  for (std::size_t i = 0; i < pattern.size(); ++i) {
    const char ch = pattern[i];
    if (ch == '\\') {
      result += ch;
      if (i + 1 < pattern.size()) result += pattern[++i];
    } else {
      // A POSIX class, collating symbol, or equivalence class has its own
      // closing bracket inside the outer character class. Copy it verbatim.
      if (in_class && ch == '[' && i + 1 < pattern.size() &&
          (pattern[i + 1] == ':' || pattern[i + 1] == '.' || pattern[i + 1] == '=')) {
        const std::string closing = std::string(1, pattern[i + 1]) + "]";
        const auto end = pattern.find(closing, i + 2);
        if (end != std::string_view::npos) {
          result.append(pattern.substr(i, end + 2 - i));
          i = end + 1;
          continue;
        }
      }

      if (ch == '[' && !in_class) {
        in_class = true;
      } else if (ch == ']' && in_class) {
        in_class = false;
      }

      if (ch == '.' && !in_class) {
        result += "[\\s\\S]";
      } else {
        result += ch;
      }
    }
  }

  return result;
}

std::size_t FindLines(const std::vector<std::string_view>& lines, const std::vector<std::string_view>& search,
                      bool trim) {
  if (search.size() > lines.size()) return std::string_view::npos;

  for (std::size_t i = 0; i <= lines.size() - search.size(); ++i) {
    bool matches = true;
    for (std::size_t j = 0; j < search.size(); ++j) {
      if ((trim ? TrimRight(lines[i + j]) : lines[i + j]) != (trim ? TrimRight(search[j]) : search[j])) {
        matches = false;
        break;
      }
    }
    if (matches) return i;
  }

  return std::string_view::npos;
}

enum class Marker { kNone, kStart, kEnd };

bool IsWord(unsigned char ch) {
  // Non-ASCII bytes cannot be part of an ASCII comment prefix. Never mistake
  // a marker embedded in an identifier for an editable-region boundary.
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch >= 0x80;
}

Marker GetMarker(std::string_view line) {
  std::size_t offset = 0;
  while (offset < line.size() && !IsWord(static_cast<unsigned char>(line[offset]))) ++offset;

  const auto suffix = line.substr(offset);
  const auto matches = [suffix](std::string_view token) {
    return suffix.substr(0, token.size()) == token &&
           (suffix.size() == token.size() || !IsWord(static_cast<unsigned char>(suffix[token.size()])));
  };

  if (matches("EVOLVE-BLOCK-START")) return Marker::kStart;
  if (matches("EVOLVE-BLOCK-END")) return Marker::kEnd;

  // Preserve the original Python inline-comment fallback and its precedence.
  if (line.find("# EVOLVE-BLOCK-START") != std::string_view::npos) return Marker::kStart;
  if (line.find("# EVOLVE-BLOCK-END") != std::string_view::npos) return Marker::kEnd;

  return Marker::kNone;
}

using Region = std::pair<std::size_t, std::size_t>;

// Unlike ParseEditableBlocks, this rejects unbalanced or nested markers instead
// of tolerating them: enforcement has to know exactly which spans are editable.
std::optional<std::vector<Region>> StrictRegions(const std::vector<std::string_view>& lines) {
  std::optional<std::size_t> start;
  std::vector<Region> regions;

  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto marker = GetMarker(lines[i]);
    if (marker == Marker::kStart) {
      if (start) return std::nullopt;
      start = i;
    } else if (marker == Marker::kEnd) {
      if (!start) return std::nullopt;
      regions.emplace_back(*start, i);
      start.reset();
    }
  }

  if (start) return std::nullopt;
  return regions;
}
}  // namespace

absl::StatusOr<std::vector<DiffBlock>> CodeParser::ExtractDiffs(std::string_view response, std::string_view pattern) {
  if (pattern == kDefaultDiffPattern) return ExtractStandard(response);

  try {
    const std::regex expression(DotAll(pattern));
    if (expression.mark_count() < 2) {
      return absl::InvalidArgumentError("Diff pattern requires at least two capture groups");
    }

    std::vector<DiffBlock> result;
    using Iterator = std::string_view::const_iterator;
    const std::regex_iterator<Iterator> end;
    for (std::regex_iterator<Iterator> it(response.begin(), response.end(), expression); it != end; ++it) {
      const auto& match = *it;
      if (!match[1].matched || !match[2].matched) {
        return absl::InvalidArgumentError("Diff pattern did not capture both SEARCH and REPLACE");
      }

      result.push_back({std::string(TrimRight(match[1].str())), std::string(TrimRight(match[2].str()))});
    }

    return result;
  } catch (const std::regex_error&) {
    return absl::InvalidArgumentError("Invalid or unmatchable ECMAScript diff pattern");
  }
}

absl::StatusOr<DiffApplication> CodeParser::ApplyDiff(std::string_view original, std::string_view response,
                                                      std::string_view pattern) {
  auto blocks = ExtractDiffs(response, pattern);
  if (!blocks.ok()) return blocks.status();

  return ApplyDiffBlocks(original, *blocks);
}

DiffApplication CodeParser::ApplyDiffBlocks(std::string_view original, const std::vector<DiffBlock>& blocks) {
  auto lines = SplitLines(original);
  std::size_t applied_count = 0;

  for (const auto& block : blocks) {
    const auto search = SplitLines(block.search);
    auto position = FindLines(lines, search, false);
    if (position == std::string_view::npos) position = FindLines(lines, search, true);
    if (position == std::string_view::npos) continue;

    const auto replacement = SplitLines(block.replacement);
    const auto next = lines.erase(lines.begin() + position, lines.begin() + position + search.size());
    lines.insert(next, replacement.begin(), replacement.end());
    ++applied_count;
  }

  return DiffApplication{JoinLines(lines), applied_count};
}

absl::StatusOr<DiffTargets> CodeParser::SplitDiffsByTarget(const std::vector<DiffBlock>& blocks, std::string_view code,
                                                           std::string_view changes_description) {
  const auto code_lines = SplitLines(code);
  const auto description_lines = SplitLines(changes_description);
  DiffTargets result;

  for (const auto& block : blocks) {
    const auto search = SplitLines(block.search);
    const bool in_code = FindLines(code_lines, search, false) != std::string_view::npos;
    const bool in_description = FindLines(description_lines, search, false) != std::string_view::npos;
    if (in_code && in_description) {
      return absl::InvalidArgumentError(
          "Ambiguous diff block: SEARCH matches both code and "
          "changes_description");
    }

    if (in_code) {
      result.code.push_back(block);
    } else if (in_description) {
      result.changes_description.push_back(block);
    } else {
      result.unmatched.push_back(block);
    }
  }

  return result;
}

std::string CodeParser::ParseFullRewrite(std::string_view response, std::string_view language) {
  const std::string opening = "```" + std::string(language) + "\n";
  auto begin = response.find(opening);
  if (begin != std::string_view::npos) {
    begin += opening.size();
    const auto end = response.find("```", begin);
    if (end != std::string_view::npos) {
      return std::string(utils::Trim(response.substr(begin, end - begin)));
    }
  }

  begin = response.find("```");
  if (begin != std::string_view::npos) {
    begin += 3;
    const auto end = response.find("```", begin);
    if (end != std::string_view::npos) {
      return std::string(utils::Trim(response.substr(begin, end - begin)));
    }
  }

  return std::string(response);
}

absl::StatusOr<std::string> CodeParser::FormatDiffSummary(const std::vector<DiffBlock>& blocks, int max_line_len,
                                                          int max_lines) {
  if (max_line_len < 3 || max_lines < 0) {
    return absl::InvalidArgumentError("Summary requires max_line_len >= 3 and max_lines >= 0");
  }

  const auto format_lines = [max_line_len, max_lines](const auto& lines) {
    const auto count = std::min(lines.size(), static_cast<std::size_t>(max_lines));
    std::string result;
    for (std::size_t i = 0; i < count; ++i) {
      if (i != 0) result += '\n';
      auto text = TrimRight(lines[i]);
      result += "  ";
      if (utils::Utf8Prefix(text, max_line_len).size() < text.size()) {
        result.append(utils::Utf8Prefix(text, max_line_len - 3));
        result += "...";
      } else {
        result.append(text);
      }
    }

    if (lines.size() > count) {
      if (count != 0) result += '\n';
      result += "  ... (" + std::to_string(lines.size() - count) + " more lines)";
    }

    return result.empty() ? std::string("  (empty)") : result;
  };

  std::string summary;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    const auto search = SplitLines(utils::Trim(blocks[i].search));
    const auto replacement = SplitLines(utils::Trim(blocks[i].replacement));

    if (i != 0) summary += '\n';
    summary += "Change " + std::to_string(i + 1) + ": ";
    if (search.size() == 1 && replacement.size() == 1) {
      summary += "'";
      summary.append(search.front());
      summary += "' to '";
      summary.append(replacement.front());
      summary += "'";
    } else {
      summary += "Replace:\n" + format_lines(search) + "\nwith:\n" + format_lines(replacement);
    }
  }

  return summary;
}

// Lenient: unbalanced or nested markers are tolerated and simply yield fewer
// blocks, because callers use this to display what is editable.
std::vector<EditableBlock> CodeParser::ParseEditableBlocks(std::string_view code) {
  const auto lines = utils::SplitLines(code);
  std::vector<EditableBlock> result;
  std::optional<std::size_t> start;

  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto marker = GetMarker(lines[i]);
    if (marker == Marker::kStart) {
      start = i;
    } else if (marker == Marker::kEnd && start) {
      result.push_back(
          {*start, i, utils::JoinLines(std::vector<std::string_view>(lines.begin() + *start + 1, lines.begin() + i))});
      start.reset();
    }
  }

  return result;
}

// Reverts edits outside the editable regions by rebuilding the program from the
// original, splicing in only what the candidate changed inside each region.
//
// An original with no well-formed regions imposes no restriction, so the
// proposal passes through untouched. Otherwise the candidate must keep exactly
// the same number of regions. Moving or dropping a marker is an error, not a
// silent partial accept. The result can equal the original, which is how a
// candidate whose every edit fell outside a region ends up rejected upstream.
absl::StatusOr<std::string> CodeParser::EnforceEditableBlocks(std::string_view original, std::string_view proposed) {
  const auto original_lines = utils::SplitLines(original);
  const auto original_regions = StrictRegions(original_lines);
  if (!original_regions || original_regions->empty()) return std::string(proposed);

  const auto proposed_lines = utils::SplitLines(proposed);
  const auto proposed_regions = StrictRegions(proposed_lines);
  if (!proposed_regions || proposed_regions->size() != original_regions->size()) {
    return absl::InvalidArgumentError("EVOLVE-BLOCK markers were changed or removed; expected " +
                                      std::to_string(original_regions->size()) + " evolve block(s)");
  }

  std::vector<std::string_view> result;
  std::size_t cursor = 0;
  for (std::size_t i = 0; i < original_regions->size(); ++i) {
    const auto [original_start, original_end] = (*original_regions)[i];
    const auto [proposed_start, proposed_end] = (*proposed_regions)[i];

    result.insert(result.end(), original_lines.begin() + cursor, original_lines.begin() + original_start + 1);
    result.insert(result.end(), proposed_lines.begin() + proposed_start + 1, proposed_lines.begin() + proposed_end);
    cursor = original_end;
  }

  result.insert(result.end(), original_lines.begin() + cursor, original_lines.end());

  return utils::JoinLines(result);
}
}  // namespace ievolve
