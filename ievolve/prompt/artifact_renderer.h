#ifndef IEVOLVE_PROMPT_ARTIFACT_RENDERER_H_
#define IEVOLVE_PROMPT_ARTIFACT_RENDERER_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ievolve {

using Artifacts = std::vector<std::pair<std::string, std::string>>;

// Replaces malformed UTF-8 with U+FFFD and optionally strips ANSI sequences and
// common credentials. This is a best-effort text filter, not a secret detector.
std::string DecodeArtifact(std::string_view bytes, bool security_filter = true);

std::string RenderArtifacts(const Artifacts& artifacts, std::size_t max_characters, bool security_filter,
                            std::string_view title);

}  // namespace ievolve

#endif  // IEVOLVE_PROMPT_ARTIFACT_RENDERER_H_
