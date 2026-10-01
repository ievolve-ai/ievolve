#ifndef IEVOLVE_DATABASE_DATABASE_CODEC_H_
#define IEVOLVE_DATABASE_DATABASE_CODEC_H_

#include <optional>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "ievolve/database/checkpoint.h"
#include "ievolve/database/population.h"
#include "ievolve/database/program_store.h"

// Converts database state to and from CheckpointData, the in-memory form that
// Checkpoint reads and writes on disk. File layout and atomic publication stay
// in Checkpoint; this unit owns only the metadata schema and its validation.
namespace ievolve::database_codec {

// Captures everything a resumed run needs: programs and, in population mode,
// island and cell ownership, archive, best pointers, counters, feature
// statistics and the RNG stream position. A null population encodes core mode.
absl::StatusOr<CheckpointData> Encode(const ProgramStore& programs, const Population* population);

// Overwrites programs and, in population mode, the population's state, which
// keeps its configuration and strategy. Callers decode into copies: on error
// both may be partially written. Returns FailedPrecondition when the mode,
// feature dimensions or population configuration differ from the target's,
// and DataLoss for any malformed or inconsistent state.
absl::Status Decode(const CheckpointData& data, ProgramStore& programs, std::optional<Population>& target);

}  // namespace ievolve::database_codec

#endif  // IEVOLVE_DATABASE_DATABASE_CODEC_H_
