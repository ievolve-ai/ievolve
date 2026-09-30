#include "ievolve/utils/yaml.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <sstream>

#include "yaml-cpp/yaml.h"

namespace ievolve::utils {
namespace {
using Json = nlohmann::ordered_json;
constexpr std::size_t kMaxYamlBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxYamlNodes = 100000;
constexpr int kMaxYamlDepth = 64;

absl::Status BadYaml(const std::string& path, const std::string& reason) {
  return absl::InvalidArgumentError(path + ": " + reason);
}

bool IsMergeKey(const YAML::Node& node) {
  return node.IsScalar() && node.Scalar() == "<<" && node.Tag() != "!" && node.Tag() != "tag:yaml.org,2002:str";
}

absl::StatusOr<Json> IntegerScalar(std::string number, const std::string& path) {
  number.erase(std::remove(number.begin(), number.end(), '_'), number.end());
  if (number.empty()) return BadYaml(path, "invalid integer");

  const bool negative = number.front() == '-';
  if (number.front() == '-' || number.front() == '+') number.erase(0, 1);
  const std::uint64_t limit = negative ? (std::uint64_t{1} << 63) : std::numeric_limits<std::int64_t>::max();

  auto parse = [&](std::string_view digits, int base) -> absl::StatusOr<std::uint64_t> {
    std::uint64_t value = 0;
    auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (digits.empty() || result.ec != std::errc{} || result.ptr != digits.data() + digits.size() || value > limit) {
      return BadYaml(path, "integer is invalid or out of range");
    }
    return value;
  };

  std::uint64_t value = 0;
  if (number.find(':') != std::string::npos) {
    std::istringstream parts(number);
    std::string part;
    while (std::getline(parts, part, ':')) {
      auto digit = parse(part, 10);
      if (!digit.ok()) return digit.status();
      if (value > (limit - *digit) / 60) return BadYaml(path, "integer is out of range");

      value = value * 60 + *digit;
    }
  } else {
    int base = 10;
    if (number.compare(0, 2, "0b") == 0) {
      base = 2;
      number.erase(0, 2);
    } else if (number.compare(0, 2, "0x") == 0) {
      base = 16;
      number.erase(0, 2);
    } else if (number.size() > 1 && number.front() == '0') {
      base = 8;
    }

    auto result = parse(number, base);
    if (!result.ok()) return result.status();
    value = *result;
  }

  if (negative && value == (std::uint64_t{1} << 63)) return Json(std::numeric_limits<std::int64_t>::min());
  return Json(negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value));
}

absl::StatusOr<Json> FloatScalar(std::string number, const std::string& path) {
  number.erase(std::remove(number.begin(), number.end(), '_'), number.end());
  try {
    if (number.find(':') == std::string::npos) return Json(YAML::Node(number).as<double>());

    const bool negative = !number.empty() && number.front() == '-';
    if (!number.empty() && (number.front() == '-' || number.front() == '+')) number.erase(0, 1);

    double value = 0;
    std::istringstream parts(number);
    std::string part;
    while (std::getline(parts, part, ':')) value = value * 60 + YAML::Node(part).as<double>();

    return Json(negative ? -value : value);
  } catch (const YAML::Exception&) {
    return BadYaml(path, "floating number is invalid or out of range");
  }
}

absl::StatusOr<Json> Decode(const YAML::Node& node, const std::string& path, int depth, std::size_t& remaining) {
  if (depth > kMaxYamlDepth || remaining == 0) return BadYaml(path, "YAML nesting or expanded node limit exceeded");
  --remaining;

  const auto tag = node.Tag();
  if (!node.IsScalar() && !tag.empty() && tag != "?" && tag != "!" &&
      !((node.IsMap() && tag == "tag:yaml.org,2002:map") || (node.IsSequence() && tag == "tag:yaml.org,2002:seq") ||
        (node.IsNull() && tag == "tag:yaml.org,2002:null"))) {
    return BadYaml(path, "tag does not match the YAML node type");
  }
  if (node.IsNull()) return Json(nullptr);

  if (node.IsSequence()) {
    Json values = Json::array();
    for (std::size_t i = 0; i < node.size(); ++i) {
      auto value = Decode(node[i], path + "[" + std::to_string(i) + "]", depth + 1, remaining);
      if (!value.ok()) return value.status();
      values.push_back(std::move(*value));
    }

    return values;
  }

  if (node.IsMap()) {
    Json values = Json::object();
    std::set<std::string> keys;

    // Merge keys have lower priority than explicitly declared fields.
    for (const auto& item : node) {
      if (!item.first.IsScalar()) return BadYaml(path, "mapping keys must be strings");

      const auto name = item.first.Scalar();
      if (!IsMergeKey(item.first)) {
        auto key = Decode(item.first, path + ".<key>", depth + 1, remaining);
        if (!key.ok()) return key.status();
        if (!key->is_string()) return BadYaml(path, "mapping keys must be strings");
      }

      if (!keys.insert(name).second) return BadYaml(path, "duplicate mapping key");
      if (!IsMergeKey(item.first)) continue;

      auto merge = Decode(item.second, path + ".<<", depth + 1, remaining);
      if (!merge.ok()) return merge.status();
      if (!merge->is_object() && !merge->is_array()) return BadYaml(path, "invalid merge mapping");

      const auto sources = merge->is_array() ? *merge : Json::array({*merge});
      // PyYAML inserts later merge sources first, then overwrites their values
      // with earlier sources without moving existing keys. Explicit keys below
      // likewise retain the position of a key that was introduced by a merge.
      for (auto source = sources.rbegin(); source != sources.rend(); ++source) {
        if (!source->is_object()) return BadYaml(path, "invalid merge mapping");
        for (const auto& entry : source->items()) values[entry.key()] = entry.value();
      }
    }

    for (const auto& item : node) {
      const auto name = item.first.Scalar();
      if (IsMergeKey(item.first)) continue;

      auto value = Decode(item.second, path + "." + name, depth + 1, remaining);
      if (!value.ok()) return value.status();
      values[name] = std::move(*value);
    }

    return values;
  }

  if (!node.IsScalar()) return BadYaml(path, "unsupported YAML node");
  const auto text = node.Scalar();
  if (tag == "!" || tag == "tag:yaml.org,2002:str") return Json(text);
  // yaml-cpp represents explicitly tagged nulls as scalars. PyYAML's null
  // constructor ignores the scalar spelling once the tag is explicit.
  if (tag == "tag:yaml.org,2002:null") return Json(nullptr);
  if (tag != "?" && !tag.empty() && tag != "tag:yaml.org,2002:int" && tag != "tag:yaml.org,2002:float" &&
      tag != "tag:yaml.org,2002:bool") {
    return BadYaml(path, "unsupported YAML tag");
  }

  // Match PyYAML SafeLoader's YAML 1.1 implicit resolver before coercion.
  // For example, 08 and 1e3 are strings; 010 and 1:30 are integers.
  static const std::regex boolean(R"(yes|Yes|YES|no|No|NO|true|True|TRUE|false|False|FALSE|on|On|ON|off|Off|OFF)");
  static const std::regex integer(
      R"([-+]?0b[0-1_]+|[-+]?0[0-7_]+|[-+]?(0|[1-9][0-9_]*)|[-+]?0x[0-9a-fA-F_]+|[-+]?[1-9][0-9_]*(:[0-5]?[0-9])+)");
  static const std::regex floating(
      R"([-+]?[0-9][0-9_]*\.[0-9_]*([eE][-+][0-9]+)?|\.[0-9][0-9_]*([eE][-+][0-9]+)?|[-+]?[0-9][0-9_]*(:[0-5]?[0-9])+\.[0-9_]*|[-+]?\.(inf|Inf|INF)|\.(nan|NaN|NAN))");

  const bool implicit = tag.empty() || tag == "?";
  if (tag == "tag:yaml.org,2002:bool" || (implicit && std::regex_match(text, boolean))) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; });

    if (lower == "true" || lower == "yes" || lower == "on") return Json(true);
    if (lower == "false" || lower == "no" || lower == "off") return Json(false);
    return BadYaml(path, "scalar does not match its boolean tag");
  }

  if (tag == "tag:yaml.org,2002:int" || (implicit && std::regex_match(text, integer))) return IntegerScalar(text, path);
  if (tag == "tag:yaml.org,2002:float" || (implicit && std::regex_match(text, floating))) {
    return FloatScalar(text, path);
  }

  return Json(text);
}

void Encode(const Json& value, YAML::Emitter& emitter) {
  if (value.is_object()) {
    emitter << YAML::BeginMap;
    for (const auto& item : value.items()) {
      emitter << YAML::Key << YAML::DoubleQuoted << item.key() << YAML::Value;
      Encode(item.value(), emitter);
    }
    emitter << YAML::EndMap;
  } else if (value.is_array()) {
    emitter << YAML::BeginSeq;
    for (const auto& item : value) Encode(item, emitter);
    emitter << YAML::EndSeq;
  } else if (value.is_null()) {
    emitter << YAML::Null;
  } else if (value.is_string()) {
    emitter << YAML::DoubleQuoted << value.get<std::string>();
  } else if (value.is_boolean()) {
    emitter << value.get<bool>();
  } else if (value.is_number_unsigned()) {
    emitter << value.get<std::uint64_t>();
  } else if (value.is_number_integer()) {
    emitter << value.get<std::int64_t>();
  } else {
    // An explicit float tag preserves type for PyYAML when the emitter uses
    // exponent notation without a decimal point (for example, 1e-08).
    emitter << YAML::VerbatimTag("tag:yaml.org,2002:float") << value.get<double>();
  }
}
}  // namespace

absl::StatusOr<Json> ParseYaml(std::string_view yaml) {
  if (yaml.size() > kMaxYamlBytes) return BadYaml("config", "YAML exceeds 8 MiB");

  try {
    auto documents = YAML::LoadAll(std::string(yaml));
    if (documents.size() != 1) return BadYaml("config", "expected one YAML document");

    std::size_t remaining = kMaxYamlNodes;
    return Decode(documents.front(), "config", 0, remaining);
  } catch (const YAML::Exception& error) {
    // Parser messages can include input values. Report location only.
    return BadYaml("config", "invalid YAML at line " + std::to_string(error.mark.line + 1) + ", column " +
                                 std::to_string(error.mark.column + 1));
  }
}

absl::StatusOr<Json> ReadYaml(const std::filesystem::path& path) {
  std::error_code error;
  const auto state = std::filesystem::status(path, error);
  if (error == std::errc::no_such_file_or_directory || (!error && !std::filesystem::exists(state))) {
    return absl::NotFoundError("config file does not exist");
  }
  if (error) return absl::PermissionDeniedError("cannot inspect config file");
  if (!std::filesystem::is_regular_file(state)) return BadYaml("config", "expected a regular file");

  const auto size = std::filesystem::file_size(path, error);
  if (error) return absl::UnknownError("cannot read config file size");
  if (size > kMaxYamlBytes) return BadYaml("config", "YAML exceeds 8 MiB");

  std::ifstream input(path, std::ios::binary);
  if (!input) return absl::PermissionDeniedError("cannot open config file");

  std::string text;
  char buffer[4096];
  while (input.read(buffer, sizeof(buffer)) || input.gcount() != 0) {
    text.append(buffer, static_cast<std::size_t>(input.gcount()));
    if (text.size() > kMaxYamlBytes) return BadYaml("config", "YAML exceeds 8 MiB");
  }
  if (input.bad()) return absl::UnknownError("cannot read config file");

  return ParseYaml(text);
}

absl::StatusOr<std::string> EmitYaml(const Json& input) {
  YAML::Emitter output;
  output.SetDoublePrecision(std::numeric_limits<double>::max_digits10);

  Encode(input, output);
  if (!output.good()) return absl::InternalError("cannot serialize configuration");

  return std::string(output.c_str()) + "\n";
}

absl::Status WriteYaml(std::string_view yaml, const std::filesystem::path& path) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return absl::PermissionDeniedError("cannot open config output file");

  file.write(yaml.data(), static_cast<std::streamsize>(yaml.size()));
  file.close();
  if (!file) return absl::UnknownError("cannot write config output file");

  return absl::OkStatus();
}
}  // namespace ievolve::utils
