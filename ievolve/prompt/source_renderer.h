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

}  // namespace ievolve

#endif  // IEVOLVE_PROMPT_SOURCE_RENDERER_H_
