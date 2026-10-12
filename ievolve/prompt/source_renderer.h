#ifndef IEVOLVE_PROMPT_SOURCE_RENDERER_H_
#define IEVOLVE_PROMPT_SOURCE_RENDERER_H_

#include <string>

#include "ievolve/program/source_files.h"

namespace ievolve {

// Every file as a "### File: <path>" heading over a fenced block tagged with
// the path's language, separated by blank lines. Models answer with the same
// path lines, which code/project_edit.h parses.
std::string RenderSourceFiles(const SourceFiles& files);

// Only the files whose content differs from (or is absent in) reference, or a
// placeholder when there are none. Keeps other programs short in a prompt that
// already shows the reference in full.
std::string RenderChangedFiles(const SourceFiles& program, const SourceFiles& reference);

// What {current_program} shows for a program, with the {language} a template
// may fence it in. A single file is its bare content tagged with its
// LanguageOf, which keeps single-file prompts identical to Python's; several
// files are rendered by RenderSourceFiles under "text".
struct SourceView {
  std::string text;
  std::string language;
};
SourceView ViewSourceFiles(const SourceFiles& files);

// Every content concatenated in path order, for heuristics that inspect the
// program text as a whole.
std::string JoinSourceFiles(const SourceFiles& files);

}  // namespace ievolve

#endif  // IEVOLVE_PROMPT_SOURCE_RENDERER_H_
