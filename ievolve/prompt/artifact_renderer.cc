#include "ievolve/prompt/artifact_renderer.h"

#include <regex>

#include "ievolve/utils/text.h"

namespace ievolve {
namespace {

std::string ReplaceInvalidUtf8(std::string_view bytes) {
  std::string result;
  for (std::size_t i = 0; i < bytes.size();) {
    const auto first = static_cast<unsigned char>(bytes[i]);
    if (first < 0x80) {
      result += bytes[i++];
      continue;
    }

    int length = 0;
    if (first >= 0xc2 && first <= 0xdf) length = 2;
    if (first >= 0xe0 && first <= 0xef) length = 3;
    if (first >= 0xf0 && first <= 0xf4) length = 4;

    int consumed = 1;
    for (; consumed < length && i + consumed < bytes.size(); ++consumed) {
      const auto byte = static_cast<unsigned char>(bytes[i + consumed]);
      if (byte < 0x80 || byte > 0xbf) break;
      if (consumed == 1 && ((first == 0xe0 && byte < 0xa0) || (first == 0xed && byte > 0x9f) ||
                            (first == 0xf0 && byte < 0x90) || (first == 0xf4 && byte > 0x8f))) {
        break;
      }
    }

    if (length != 0 && consumed == length) {
      result.append(bytes.substr(i, length));
    } else {
      result += "\xef\xbf\xbd";
    }
    i += consumed;
  }

  return result;
}

}  // namespace

std::string DecodeArtifact(std::string_view bytes, bool security_filter) {
  std::string result = ReplaceInvalidUtf8(bytes);
  if (!security_filter) return result;

  static const std::regex kAnsi("\x1b(?:\\[[0-?]*[ -/]*[@-~]|[@-Z\\\\-_])");
  static const std::regex kApiKey("sk-[A-Za-z0-9]{48}", std::regex::icase);
  static const std::regex kLongToken("[A-Za-z0-9]{32,}");
  static const std::regex kPassword("password[=:]\\s*[^\\s]+", std::regex::icase);
  static const std::regex kToken("token[=:]\\s*[^\\s]+", std::regex::icase);

  result = std::regex_replace(result, kAnsi, "");
  // Match full API keys before the generic token pattern can consume their
  // body.
  result = std::regex_replace(result, kApiKey, "<REDACTED_API_KEY>");
  result = std::regex_replace(result, kLongToken, "<REDACTED_TOKEN>");
  result = std::regex_replace(result, kPassword, "password=<REDACTED>");
  return std::regex_replace(result, kToken, "token=<REDACTED>");
}

std::string RenderArtifacts(const Artifacts& artifacts, std::size_t max_characters, bool security_filter,
                            std::string_view title) {
  if (artifacts.empty()) return "";

  std::string result = "## " + std::string(title) + "\n\n";
  for (const auto& artifact : artifacts) {
    std::string content = DecodeArtifact(artifact.second, security_filter);
    std::size_t characters = 0;
    for (std::size_t i = 0; i < content.size(); ++i) {
      if (utils::IsCodePointStart(static_cast<unsigned char>(content[i])) && characters++ == max_characters) {
        content.resize(i);
        content += "\n... (truncated)";
        break;
      }
    }

    if (&artifact != &artifacts.front()) result += "\n\n";
    result += "### " + artifact.first + "\n```\n" + content + "\n```";
  }

  return result;
}

}  // namespace ievolve
