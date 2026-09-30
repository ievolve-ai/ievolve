#include "ievolve/code/code_distance.h"

#include <algorithm>
#include <numeric>
#include <vector>

#include "absl/status/status.h"

namespace ievolve {
namespace {

absl::StatusOr<char32_t> NextCodePoint(std::string_view* text) {
  const auto first = static_cast<unsigned char>((*text)[0]);
  std::size_t width = 0;
  char32_t value = 0;
  char32_t minimum = 0;

  if (first < 0x80) {
    width = 1;
    value = first;
  } else if (first >= 0xc2 && first <= 0xdf) {
    width = 2;
    value = first & 0x1f;
    minimum = 0x80;
  } else if (first >= 0xe0 && first <= 0xef) {
    width = 3;
    value = first & 0x0f;
    minimum = 0x800;
  } else if (first >= 0xf0 && first <= 0xf4) {
    width = 4;
    value = first & 0x07;
    minimum = 0x10000;
  } else {
    return absl::InvalidArgumentError("Code must be valid UTF-8");
  }

  if (text->size() < width) {
    return absl::InvalidArgumentError("Truncated UTF-8 code point");
  }

  for (std::size_t index = 1; index < width; ++index) {
    const auto next = static_cast<unsigned char>((*text)[index]);
    if ((next & 0xc0) != 0x80) {
      return absl::InvalidArgumentError("Invalid UTF-8 continuation byte");
    }
    value = (value << 6) | (next & 0x3f);
  }

  if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
    return absl::InvalidArgumentError("Invalid UTF-8 code point");
  }

  text->remove_prefix(width);
  return value;
}

}  // namespace

absl::StatusOr<std::size_t> CodeDistance::Levenshtein(std::string_view left, std::string_view right) {
  if (left == right) {
    // Identity still validates UTF-8, without quadratic work on long strings.
    while (!left.empty()) {
      const auto point = NextCodePoint(&left);
      if (!point.ok()) return point.status();
    }
    return 0;
  }

  if (left.size() < right.size()) std::swap(left, right);

  // Decode only the shorter byte string and stream the other. Since each code
  // point occupies at most four bytes, storage is O(min(code-point counts)).
  std::vector<char32_t> columns;
  while (!right.empty()) {
    const auto point = NextCodePoint(&right);
    if (!point.ok()) return point.status();
    columns.push_back(*point);
  }

  std::vector<std::size_t> row(columns.size() + 1);
  std::iota(row.begin(), row.end(), std::size_t{0});
  while (!left.empty()) {
    const auto point = NextCodePoint(&left);
    if (!point.ok()) return point.status();

    std::size_t diagonal = row[0];
    ++row[0];
    for (std::size_t index = 0; index < columns.size(); ++index) {
      const std::size_t previous = row[index + 1];
      row[index + 1] = std::min({previous + 1, row[index] + 1, diagonal + (*point == columns[index] ? 0 : 1)});
      diagonal = previous;
    }
  }

  return row.back();
}

}  // namespace ievolve
