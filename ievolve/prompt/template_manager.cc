#include "ievolve/prompt/template_manager.h"

#include <algorithm>
#include <system_error>
#include <utility>

#include "ievolve/utils/file.h"

namespace ievolve {
namespace {

// A template that is missing, unreadable or not a regular file is a bad
// directory argument; only a read that fails partway is reported as data loss.
absl::StatusOr<std::string> ReadFile(const std::filesystem::path& path) {
  auto text = utils::ReadFile(path);
  if (text.ok() || text.status().code() == absl::StatusCode::kDataLoss) return text;

  return absl::InvalidArgumentError("Not a readable regular file: " + path.string());
}

}  // namespace

TemplateManager::TemplateManager() {
#include "embedded_templates.inc"
  const auto fragments = Metrics::parse(kDefaultFragments, nullptr, false);
  for (const auto& item : fragments.items()) {
    fragments_[item.key()] = item.value().get<std::string>();
  }
}

// Overlays a caller's directory onto the embedded defaults. Loading is
// all-or-nothing: entries are staged and committed only once the whole
// directory has been read, so a malformed file cannot leave half the templates
// replaced.
absl::Status TemplateManager::LoadDirectory(const std::filesystem::path& directory) {
  std::error_code error;
  const bool exists = std::filesystem::exists(directory, error);
  if (error) return absl::InvalidArgumentError(error.message());
  if (!exists) return absl::OkStatus();
  if (!std::filesystem::is_directory(directory, error) || error) {
    return absl::InvalidArgumentError("Not a readable directory: " + directory.string());
  }

  auto templates = templates_;
  auto fragments = fragments_;
  std::filesystem::directory_iterator iterator(directory, error);
  const std::filesystem::directory_iterator end;
  for (; !error && iterator != end; iterator.increment(error)) {
    const auto path = iterator->path();
    if (path.extension() != ".txt" && path.filename() != "fragments.json") {
      continue;
    }

    auto text = ReadFile(path);
    if (!text.ok()) return text.status();
    if (path.extension() == ".txt") {
      templates[path.stem().string()] = std::move(*text);
      continue;
    }

    const auto parsed = Metrics::parse(*text, nullptr, false);
    if (!parsed.is_object()) {
      return absl::InvalidArgumentError("Expected a JSON object in " + path.string());
    }

    for (const auto& item : parsed.items()) {
      if (!item.value().is_string()) {
        return absl::InvalidArgumentError("Fragment must be a string: " + item.key());
      }
      fragments[item.key()] = item.value().get<std::string>();
    }
  }
  if (error) return absl::InvalidArgumentError(error.message());

  templates_.swap(templates);
  fragments_.swap(fragments);

  return absl::OkStatus();
}

bool TemplateManager::HasTemplate(const std::string& name) const { return templates_.find(name) != templates_.end(); }

absl::StatusOr<std::string> TemplateManager::GetTemplate(const std::string& name) const {
  const auto found = templates_.find(name);
  if (found == templates_.end()) {
    return absl::NotFoundError("Template '" + name + "' not found");
  }

  return found->second;
}

std::string TemplateManager::GetFragment(const std::string& name, const Metrics& values) const {
  const auto found = fragments_.find(name);
  if (found == fragments_.end()) return "[Missing fragment: " + name + "]";

  auto result = Format(found->second, values);
  if (!result.ok()) {
    return "[Fragment formatting error: " + std::string(result.status().message()) + "]";
  }

  return *result;
}

void TemplateManager::AddTemplate(const std::string& name, std::string text) { templates_[name] = std::move(text); }

void TemplateManager::AddFragment(const std::string& name, std::string text) { fragments_[name] = std::move(text); }

absl::StatusOr<std::string> TemplateManager::Format(std::string_view text, const Metrics& values) {
  if (!values.is_object()) {
    return absl::InvalidArgumentError("Template values must be an object");
  }

  std::string result;
  for (std::size_t i = 0; i < text.size();) {
    const char ch = text[i++];
    if (ch != '{' && ch != '}') {
      result += ch;
      continue;
    }

    if (i < text.size() && text[i] == ch) {
      result += ch;
      ++i;
      continue;
    }

    if (ch == '}') return absl::InvalidArgumentError("Unmatched '}'");
    const auto end = text.find('}', i);
    if (end == std::string_view::npos) {
      return absl::InvalidArgumentError("Unmatched '{'");
    }

    const auto field = text.substr(i, end - i);
    const auto colon = field.find(':');
    const std::string name(field.substr(0, colon));
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        })) {
      return absl::InvalidArgumentError("Unsupported template field: " + name);
    }

    const auto value = values.find(name);
    if (value == values.end()) return absl::NotFoundError("'" + name + "'");

    int precision = -1;
    if (colon != std::string_view::npos && colon + 1 < field.size()) {
      const auto spec = field.substr(colon + 1);
      if (spec.size() < 3 || spec.front() != '.' || spec.back() != 'f') {
        return absl::InvalidArgumentError("Unsupported format: " + std::string(spec));
      }

      precision = 0;
      for (std::size_t j = 1; j + 1 < spec.size(); ++j) {
        if (spec[j] < '0' || spec[j] > '9' || precision > 32) {
          return absl::InvalidArgumentError("Invalid precision");
        }
        precision = precision * 10 + spec[j] - '0';
      }

      if (precision > 32 || (!value->is_number() && !value->is_boolean())) {
        return absl::InvalidArgumentError("Invalid numeric format for '" + name + "'");
      }
    }

    result += FormatValue(*value, precision);
    i = end + 1;
  }

  return result;
}

}  // namespace ievolve
