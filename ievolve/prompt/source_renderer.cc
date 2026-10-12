#include "ievolve/prompt/source_renderer.h"

namespace ievolve {
namespace {

void AppendFile(std::string& out, const std::string& path, const std::string& content) {
  if (!out.empty()) out += "\n\n";
  out += "### File: " + path + "\n```" + LanguageOf(path) + "\n" + content;
  if (!content.empty() && content.back() != '\n') out += '\n';
  out += "```";
}

}  // namespace

std::string RenderSourceFiles(const SourceFiles& files) {
  std::string out;
  for (const auto& [path, content] : files) AppendFile(out, path, content);
  return out;
}

std::string RenderChangedFiles(const SourceFiles& program, const SourceFiles& reference) {
  std::string out;
  for (const auto& [path, content] : program) {
    const auto found = reference.find(path);
    if (found == reference.end() || found->second != content) AppendFile(out, path, content);
  }
  return out.empty() ? "(identical to the current program)" : out;
}

}  // namespace ievolve
