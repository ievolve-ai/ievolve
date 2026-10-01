#include "ievolve/database/program_store.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ievolve {

ProgramStore::ProgramStore(std::vector<std::string> feature_dimensions)
    : feature_dimensions_(std::move(feature_dimensions)) {}

absl::Status ProgramStore::Add(const Program& program) {
  auto status = program.Validate();
  if (!status.ok()) return status;

  if (!std::isfinite(Fitness(program))) return absl::InvalidArgumentError("Program fitness must be finite");
  if (index_.count(program.id) != 0) return absl::AlreadyExistsError("Program ID already exists");

  // The vector and the index must agree. If building the index entry throws,
  // undo the push so the two never drift apart.
  programs_.push_back(program);
  try {
    index_.emplace(program.id, programs_.size() - 1);
  } catch (...) {
    programs_.pop_back();
    throw;
  }

  return absl::OkStatus();
}

// Removing from the middle of the vector shifts later entries, so the index is
// rebuilt from the hole onward.
void ProgramStore::Erase(const std::string& id) {
  const auto found = index_.find(id);
  if (found == index_.end()) return;

  const auto offset = found->second;
  index_.erase(found);
  programs_.erase(programs_.begin() + offset);
  for (std::size_t i = offset; i < programs_.size(); ++i) index_.at(programs_[i].id) = i;
}

void ProgramStore::Clear() {
  programs_.clear();
  index_.clear();
}

bool ProgramStore::Contains(std::string_view id) const { return index_.count(std::string(id)) != 0; }

absl::StatusOr<Program> ProgramStore::Get(std::string_view id) const {
  const auto found = index_.find(std::string(id));
  if (found == index_.end()) return absl::NotFoundError("Program ID not found");

  return programs_[found->second];
}

const Program& ProgramStore::at(const std::string& id) const { return programs_[index_.at(id)]; }

Program& ProgramStore::at(const std::string& id) { return programs_[index_.at(id)]; }

// A named metric that a program lacks, or holds as a non-number, drops that
// program from the ranking instead of scoring it zero. Comparison goes through
// CompareMetricNumbers so large integers keep full precision, and the sort is
// stable so equal scores preserve insertion order.
absl::StatusOr<std::vector<Program>> ProgramStore::Top(int n, const std::optional<std::string>& metric) const {
  if (n < 0) return absl::InvalidArgumentError("Top-N count must be nonnegative");
  if (n == 0) return std::vector<Program>{};

  struct Ranked {
    const Program* program;
    Metrics score;
  };
  std::vector<Ranked> ranked;
  ranked.reserve(programs_.size());
  for (const auto& program : programs_) {
    if (metric && !metric->empty()) {
      const auto score = program.metrics.find(*metric);
      if (score == program.metrics.end() || !(score->is_number() || score->is_boolean())) continue;
      ranked.push_back({&program, *score});
    } else {
      ranked.push_back({&program, Fitness(program)});
    }
  }

  std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& left, const Ranked& right) {
    return CompareMetricNumbers(left.score, right.score) > 0;
  });
  const auto count = std::min(ranked.size(), static_cast<std::size_t>(n));

  std::vector<Program> result;
  result.reserve(count);
  for (std::size_t i = 0; i < count; ++i) result.push_back(*ranked[i].program);

  return result;
}

double ProgramStore::Fitness(const Program& program) const {
  return GetFitnessScore(program.metrics, feature_dimensions_);
}

}  // namespace ievolve
