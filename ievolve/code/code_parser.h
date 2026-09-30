#ifndef IEVOLVE_CODE_CODE_PARSER_H_
#define IEVOLVE_CODE_CODE_PARSER_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace ievolve {

struct DiffBlock {
  std::string search;
  std::string replacement;
};

struct DiffApplication {
  std::string text;
  std::size_t applied_count = 0;
};

struct DiffTargets {
  std::vector<DiffBlock> code;
  std::vector<DiffBlock> changes_description;
  std::vector<DiffBlock> unmatched;
};

struct EditableBlock {
  std::size_t start_line = 0;
  std::size_t end_line = 0;
  std::string content;
};

// Stateless text operations; returned values own their strings. UTF-8 is
// expected. No filesystem access, code execution, or LLM calls occur here.
class CodeParser {
 public:
  inline static constexpr std::string_view kDefaultDiffPattern =
      R"(<<<<<<< SEARCH\n(.*?)=======\n(.*?)>>>>>>> REPLACE)";

  // Standard grammar validates the entire response before returning blocks.
  // Custom patterns use ECMAScript with DOTALL and at least two captures.
  static absl::StatusOr<std::vector<DiffBlock>> ExtractDiffs(std::string_view response,
                                                             std::string_view pattern = kDefaultDiffPattern);
  static absl::StatusOr<DiffApplication> ApplyDiff(std::string_view original, std::string_view response,
                                                   std::string_view pattern = kDefaultDiffPattern);
  // Apply in order; first exact match wins, then try ignoring trailing
  // whitespace on each line. Unmatched edits are skipped and not counted.
  static DiffApplication ApplyDiffBlocks(std::string_view original, const std::vector<DiffBlock>& blocks);
  // Route against the original targets using exact line matches only.
  // A SEARCH matching both targets is an error.
  static absl::StatusOr<DiffTargets> SplitDiffsByTarget(const std::vector<DiffBlock>& blocks, std::string_view code,
                                                        std::string_view changes_description);

  // Prefer a literal language fence; any-fence fallback preserves its content
  // (including an unmatched language label). Plain text is returned unchanged.
  static std::string ParseFullRewrite(std::string_view response, std::string_view language = "python");
  // Zero-based marker-line indices, excluding markers from content. Parsing
  // is permissive like Python; enforcement below validates paired regions.
  static std::vector<EditableBlock> ParseEditableBlocks(std::string_view code);
  // Restore original outside segments and marker lines. No well-formed
  // original regions means no enforcement; malformed proposed regions error.
  static absl::StatusOr<std::string> EnforceEditableBlocks(std::string_view original, std::string_view proposed);
  // Limits apply to multiline blocks. Count UTF-8 code points, not bytes.
  // max_line_len must be >= 3; max_lines may be zero but not negative.
  static absl::StatusOr<std::string> FormatDiffSummary(const std::vector<DiffBlock>& blocks, int max_line_len = 100,
                                                       int max_lines = 30);
};

}  // namespace ievolve

#endif  // IEVOLVE_CODE_CODE_PARSER_H_
