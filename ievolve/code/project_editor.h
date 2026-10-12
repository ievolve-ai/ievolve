#ifndef IEVOLVE_CODE_PROJECT_EDITOR_H_
#define IEVOLVE_CODE_PROJECT_EDITOR_H_

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "ievolve/code/code_parser.h"
#include "ievolve/program/source_files.h"

// Model edits addressed to the files of a project. A path line names the file:
// the last nonempty line before a SEARCH block or rewrite fence, ignoring
// fence lines, surrounding whitespace, '#', '*', backticks, a "File:" prefix
// and a trailing ':'. A project of exactly one file keeps the single-file
// behavior, so the path line is optional there.
namespace ievolve {

struct FileDiffs {
  std::map<std::string, std::vector<DiffBlock>> files;  // Response order per file.
  std::vector<DiffBlock> changes_description;
};

struct FilesApplication {
  SourceFiles files;
  std::size_t applied_count = 0;
};

// Stateless like CodeParser; returned values own their data.
class ProjectEditor {
 public:
  // Path naming the changes description in programs_as_changes_description mode.
  inline static constexpr std::string_view kChangesDescriptionPath = "CHANGES_DESCRIPTION";

  // Assigns each block, extracted from response, to a file. A block separated
  // from the previous one only by whitespace inherits its path. Without a path:
  // a single file takes the block, or with a description it is routed by content
  // like CodeParser::SplitDiffsByTarget (blocks matching neither are dropped).
  // InvalidArgument for an unknown path, an unnamed block among several files,
  // or a description path when description is absent.
  static absl::StatusOr<FileDiffs> GroupDiffs(const std::vector<DiffBlock>& blocks, std::string_view response,
                                              const SourceFiles& files,
                                              std::optional<std::string_view> description = std::nullopt);

  // CodeParser::ApplyDiffBlocks per file; untargeted files are copied unchanged.
  // Diffs naming a path absent from files are ignored.
  static FilesApplication ApplyDiffs(const SourceFiles& files, const FileDiffs& diffs);

  // A full rewrite: each path line followed by a fenced block replaces that
  // file's content with the fence body verbatim; unnamed files stay unchanged.
  // One file delegates to CodeParser::ParseFullRewrite with its language.
  // InvalidArgument for no blocks, an unknown or missing path, a duplicate path
  // or an unterminated fence.
  static absl::StatusOr<SourceFiles> ParseRewrite(std::string_view response, const SourceFiles& files);

  // CodeParser::EnforceEditableBlocks per file. The path sets must match.
  static absl::StatusOr<SourceFiles> EnforceEditableBlocks(const SourceFiles& original, const SourceFiles& proposed);
};

}  // namespace ievolve

#endif  // IEVOLVE_CODE_PROJECT_EDITOR_H_
