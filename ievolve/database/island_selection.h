#ifndef IEVOLVE_DATABASE_ISLAND_SELECTION_H_
#define IEVOLVE_DATABASE_ISLAND_SELECTION_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "absl/status/statusor.h"

namespace ievolve {

struct IslandState {
  std::size_t population_size = 0;
  double best_score = 0;
  double average_score = 0;
  double diversity = 0;  // Fraction of unique code strings; zero when empty.
  std::int64_t generation = 0;
};

struct IslandSelectionContext {
  std::int64_t iteration = 0;
  std::vector<int> pending_counts;
  std::vector<IslandState> islands;
};

using IslandSelector = std::function<absl::StatusOr<int>(const IslandSelectionContext&)>;

class IslandSelection {
 public:
  // Iterations are zero-based. Default: iteration modulo island count.
  static absl::StatusOr<int> Select(const IslandSelectionContext& context, const IslandSelector& selector = {});
};

}  // namespace ievolve

#endif  // IEVOLVE_DATABASE_ISLAND_SELECTION_H_
