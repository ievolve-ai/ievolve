#include "ievolve/database/selection.h"

#include <algorithm>
#include <limits>

namespace ievolve {
// Chooses the island for the next iteration. The default is a zero-based
// round robin over iteration, not Python's modulo wraparound of an arbitrary
// index: every island index here is validated rather than folded into range.
// A custom selector is equally distrusted, so a bad or throwing one becomes a
// status instead of an out-of-range island.
absl::StatusOr<int> IslandSelection::Select(const IslandSelectionContext& context, const IslandSelector& selector) {
  if (context.iteration < 0 || context.islands.empty() ||
      context.islands.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      context.pending_counts.size() != context.islands.size() ||
      std::any_of(context.pending_counts.begin(), context.pending_counts.end(), [](int count) { return count < 0; })) {
    return absl::InvalidArgumentError("Invalid island selection context");
  }

  try {
    auto selected = selector ? selector(context)
                             : absl::StatusOr<int>(static_cast<int>(context.iteration % context.islands.size()));
    if (!selected.ok()) return selected.status();
    if (*selected < 0 || static_cast<std::size_t>(*selected) >= context.islands.size()) {
      return absl::InvalidArgumentError("Selector returned an invalid island");
    }

    return *selected;
  } catch (...) {
    return absl::InternalError("Island selector threw an exception");
  }
}
}  // namespace ievolve
