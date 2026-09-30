#ifndef IEVOLVE_UTILS_TEXT_H_
#define IEVOLVE_UTILS_TEXT_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Text helpers shared across components. Every declaration here is plain C++;
// only IsValidUtf8 reaches for a third-party library, and it does so inside the
// implementation, so callers that do not use it pay nothing at link time.
// Whitespace classification follows Python str.isspace(); invalid UTF-8 decodes
// to one replacement unit per byte rather than being an error.
namespace ievolve::utils {

// True when the text is well-formed UTF-8. Implemented by round-tripping
// through the JSON serializer, which is the check the callers already relied
// on, so acceptance is identical to what they each did before.
bool IsValidUtf8(std::string_view text);

// True for any byte that is not a UTF-8 continuation byte.
bool IsCodePointStart(unsigned char byte);

// Counts Unicode code points by counting code-point starts. On well-formed
// input this agrees with decoding; on malformed input it does not, and neither
// approach reproduces Python's replacement-character count exactly. Callers
// validate UTF-8 before measuring, so only the well-formed case is reachable.
std::size_t Utf8Length(std::string_view text);

std::vector<std::string_view> SplitLines(std::string_view text);
std::string JoinLines(const std::vector<std::string_view>& lines);
std::string_view TrimRight(std::string_view text);
std::string_view Trim(std::string_view text);
std::string_view Utf8Prefix(std::string_view text, std::size_t count);

}  // namespace ievolve::utils

#endif  // IEVOLVE_UTILS_TEXT_H_
