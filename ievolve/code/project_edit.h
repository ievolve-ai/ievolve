#ifndef IEVOLVE_CODE_PROJECT_EDIT_H_
#define IEVOLVE_CODE_PROJECT_EDIT_H_

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

// Path naming the changes description in programs_as_changes_description mode.
inline constexpr std::string_view kChangesDescriptionPath = "CHANGES_DESCRIPTION";

struct FileDiffs {
  std::map<std::string, std::vector<DiffBlock>> files;  // Response order per file.
  std::vector<DiffBlock> changes_description;
};

// Assigns each block, extracted from response, to a file. A block separated
// from the previous one only by whitespace inherits its path. Without a path:
// a single file takes the block, or with a description it is routed by content
// like CodeParser::SplitDiffsByTarget (blocks matching neither are dropped).
// InvalidArgument for an unknown path, an unnamed block among several files,
// or a description path when description is absent.
absl::StatusOr<FileDiffs> GroupDiffsByFile(const std::vector<DiffBlock>& blocks, std::string_view response,
                                           const SourceFiles& files,
                                           std::optional<std::string_view> description = std::nullopt);

struct FilesApplication {
  SourceFiles files;
  std::size_t applied_count = 0;
};

// CodeParser::ApplyDiffBlocks per file; untargeted files are copied unchanged.
// Diffs naming a path absent from files are ignored.
FilesApplication ApplyFileDiffs(const SourceFiles& files, const FileDiffs& diffs);

// A full rewrite: each path line followed by a fenced block replaces that
// file's content with the fence body verbatim; unnamed files stay unchanged.
// One file delegates to CodeParser::ParseFullRewrite with its language.
// InvalidArgument for no blocks, an unknown or missing path, a duplicate path
// or an unterminated fence.
absl::StatusOr<SourceFiles> ParseFileRewrite(std::string_view response, const SourceFiles& files);

// CodeParser::EnforceEditableBlocks per file. The path sets must match.
absl::StatusOr<SourceFiles> EnforceEditableFiles(const SourceFiles& original, const SourceFiles& proposed);

}  // namespace ievolve

#endif  // IEVOLVE_CODE_PROJECT_EDIT_H_
