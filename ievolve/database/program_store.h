#ifndef IEVOLVE_DATABASE_PROGRAM_STORE_H_
#define IEVOLVE_DATABASE_PROGRAM_STORE_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "ievolve/program/program.h"

namespace ievolve {

// Insertion-ordered programs with an ID index. It knows nothing about islands,
// cells or archives; callers that track program IDs elsewhere clean those up
// themselves. A value type: copying it copies every program, which the
// database transaction relies on to roll back.
class ProgramStore {
 public:
  explicit ProgramStore(std::vector<std::string> feature_dimensions);

  // Validates the program, requires a finite fitness and a unique ID.
  absl::Status Add(const Program& program);
  // Removes the program if present; later entries keep their relative order.
  void Erase(const std::string& id);
  void Clear();

  bool Contains(std::string_view id) const;
  absl::StatusOr<Program> Get(std::string_view id) const;
  // Precondition: Contains(id).
  const Program& at(const std::string& id) const;
  Program& at(const std::string& id);
  // Ranks by a named metric, or by fitness when none is given.
  absl::StatusOr<std::vector<Program>> Top(int n, const std::optional<std::string>& metric) const;

  double Fitness(const Program& program) const;
  const std::vector<Program>& programs() const { return programs_; }
  const std::vector<std::string>& feature_dimensions() const { return feature_dimensions_; }
  std::size_t size() const { return programs_.size(); }
  bool empty() const { return programs_.empty(); }

 private:
  std::vector<std::string> feature_dimensions_;
  std::vector<Program> programs_;
  std::unordered_map<std::string, std::size_t> index_;
};

}  // namespace ievolve

#endif  // IEVOLVE_DATABASE_PROGRAM_STORE_H_
