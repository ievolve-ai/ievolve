#ifndef IEVOLVE_PROGRAM_ARTIFACT_H_
#define IEVOLVE_PROGRAM_ARTIFACT_H_

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include "ievolve/program/metrics.h"

namespace ievolve {

using ArtifactBytes = std::vector<std::uint8_t>;
using ArtifactValue = std::variant<std::string, ArtifactBytes>;
using ArtifactMap = std::map<std::string, ArtifactValue>;

// Text is a UTF-8 JSON string; bytes use {"__bytes__": "canonical base64"}.
// Keys, aggregate byte limits and filesystem policy belong to the caller.
absl::StatusOr<Metrics> EncodeArtifactValue(const ArtifactValue& value);
absl::StatusOr<ArtifactValue> DecodeArtifactValue(const Metrics& value);

}  // namespace ievolve

#endif  // IEVOLVE_PROGRAM_ARTIFACT_H_
