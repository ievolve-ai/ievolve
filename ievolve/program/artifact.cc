#include "ievolve/program/artifact.h"

#include "absl/strings/escaping.h"

namespace ievolve {
namespace {

absl::Status ValidateText(const Metrics& value) {
  try {
    (void)value.dump();
    return absl::OkStatus();
  } catch (const Metrics::exception&) {
    return absl::InvalidArgumentError("Artifact text must be UTF-8");
  }
}

}  // namespace

absl::StatusOr<Metrics> EncodeArtifactValue(const ArtifactValue& value) {
  if (const auto* text = std::get_if<std::string>(&value)) {
    Metrics encoded = *text;
    const auto status = ValidateText(encoded);
    if (!status.ok()) return status;

    return encoded;
  }

  const auto& bytes = std::get<ArtifactBytes>(value);
  return Metrics{{"__bytes__", absl::Base64Escape(std::string(bytes.begin(), bytes.end()))}};
}

absl::StatusOr<ArtifactValue> DecodeArtifactValue(const Metrics& value) {
  if (value.is_string()) {
    const auto status = ValidateText(value);
    if (!status.ok()) return status;

    return ArtifactValue(value.get<std::string>());
  }

  if (!value.is_object() || value.size() != 1 || !value.contains("__bytes__") || !value["__bytes__"].is_string()) {
    return absl::InvalidArgumentError("Artifact must be text or a binary marker");
  }

  const auto& encoded = value["__bytes__"].get_ref<const std::string&>();
  std::string decoded;
  if (!absl::Base64Unescape(encoded, &decoded) || absl::Base64Escape(decoded) != encoded) {
    return absl::InvalidArgumentError("Artifact base64 must be canonical");
  }

  return ArtifactValue(ArtifactBytes(decoded.begin(), decoded.end()));
}

}  // namespace ievolve
