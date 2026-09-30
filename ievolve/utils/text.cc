#include "ievolve/utils/text.h"

#include <cstdint>

#include "nlohmann/json.hpp"

namespace ievolve::utils {
namespace {
struct Rune {
  std::uint32_t value;
  std::size_t bytes;
};

Rune ReadRune(std::string_view text, std::size_t offset) {
  const auto first = static_cast<unsigned char>(text[offset]);
  if (first < 0x80) return {first, 1};

  const std::size_t bytes = first >= 0xc2 && first <= 0xdf   ? 2
                            : first >= 0xe0 && first <= 0xef ? 3
                            : first >= 0xf0 && first <= 0xf4 ? 4
                                                             : 1;
  if (bytes == 1 || text.size() - offset < bytes) return {0xfffd, 1};

  std::uint32_t value = first & (0x7f >> bytes);
  for (std::size_t i = 1; i < bytes; ++i) {
    const auto next = static_cast<unsigned char>(text[offset + i]);
    if ((next & 0xc0) != 0x80) return {0xfffd, 1};
    value = (value << 6) | (next & 0x3f);
  }

  if ((bytes == 2 && value < 0x80) || (bytes == 3 && value < 0x800) || (bytes == 4 && value < 0x10000) ||
      value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
    return {0xfffd, 1};
  }

  return {value, bytes};
}

// Python str.isspace(), including the four ASCII information separators.
bool IsWhitespace(std::uint32_t value) {
  return (value >= 0x09 && value <= 0x0d) || (value >= 0x1c && value <= 0x20) || value == 0x85 || value == 0xa0 ||
         value == 0x1680 || (value >= 0x2000 && value <= 0x200a) || value == 0x2028 || value == 0x2029 ||
         value == 0x202f || value == 0x205f || value == 0x3000;
}
}  // namespace

bool IsValidUtf8(std::string_view text) {
  try {
    (void)nlohmann::json(std::string(text)).dump();
    return true;
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

bool IsCodePointStart(unsigned char byte) { return (byte & 0xc0) != 0x80; }

std::size_t Utf8Length(std::string_view text) {
  std::size_t length = 0;
  for (unsigned char byte : text) {
    if (IsCodePointStart(byte)) ++length;
  }

  return length;
}

std::vector<std::string_view> SplitLines(std::string_view text) {
  std::vector<std::string_view> result;
  std::size_t start = 0;

  while (true) {
    const auto end = text.find('\n', start);
    if (end == std::string_view::npos) {
      result.push_back(text.substr(start));
      return result;
    }

    result.push_back(text.substr(start, end - start));
    start = end + 1;
  }
}

std::string JoinLines(const std::vector<std::string_view>& lines) {
  std::string result;
  bool first = true;

  for (const auto line : lines) {
    if (!first) result += '\n';
    first = false;
    result.append(line);
  }

  return result;
}

std::string_view TrimRight(std::string_view text) {
  std::size_t last = 0;
  for (std::size_t i = 0; i < text.size();) {
    const auto rune = ReadRune(text, i);
    i += rune.bytes;
    if (!IsWhitespace(rune.value)) last = i;
  }

  return text.substr(0, last);
}

std::string_view Trim(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size()) {
    const auto rune = ReadRune(text, start);
    if (!IsWhitespace(rune.value)) break;
    start += rune.bytes;
  }

  return TrimRight(text.substr(start));
}

std::string_view Utf8Prefix(std::string_view text, std::size_t count) {
  std::size_t end = 0;
  while (count > 0 && end < text.size()) {
    end += ReadRune(text, end).bytes;
    --count;
  }

  return text.substr(0, end);
}
}  // namespace ievolve::utils
