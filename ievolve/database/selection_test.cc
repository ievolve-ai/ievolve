#include "ievolve/database/selection.h"

#include <stdexcept>

#include "gtest/gtest.h"

namespace ievolve {
namespace {
TEST(IslandSelectionTest, DefaultsToZeroBasedRoundRobin) {
  IslandSelectionContext context{0, {0, 0, 0}, {{}, {}, {}}};
  for (int iteration = 0; iteration < 10; ++iteration) {
    context.iteration = iteration;

    auto selected = IslandSelection::Select(context);
    ASSERT_TRUE(selected.ok()) << selected.status();

    EXPECT_EQ(*selected, iteration % 3);
  }
}

TEST(IslandSelectionTest, CustomSelectorReceivesLoadAndRejectsInvalidResults) {
  IslandSelectionContext context{5, {2, 0}, {{}, {}}};

  auto selected = IslandSelection::Select(context, [](const auto& snapshot) {
    EXPECT_EQ(snapshot.iteration, 5);
    EXPECT_EQ(snapshot.pending_counts, (std::vector<int>{2, 0}));

    return absl::StatusOr<int>(1);
  });
  ASSERT_TRUE(selected.ok());

  EXPECT_EQ(*selected, 1);

  for (int index : {-1, 2}) {
    EXPECT_EQ(
        IslandSelection::Select(context, [index](const auto&) { return absl::StatusOr<int>(index); }).status().code(),
        absl::StatusCode::kInvalidArgument);
  }

  EXPECT_EQ(
      IslandSelection::Select(
          context, [](const auto&) -> absl::StatusOr<int> { throw std::runtime_error("private callback details"); })
          .status()
          .code(),
      absl::StatusCode::kInternal);
}

TEST(IslandSelectionTest, ValidatesContextBeforeInvokingCallback) {
  int calls = 0;
  IslandSelector selector = [&](const auto&) {
    ++calls;
    return 0;
  };

  for (const auto& context : std::vector<IslandSelectionContext>{{}, {-1, {0}, {{}}}, {0, {}, {{}}}, {0, {-1}, {{}}}}) {
    EXPECT_EQ(IslandSelection::Select(context, selector).status().code(), absl::StatusCode::kInvalidArgument);
  }

  EXPECT_EQ(calls, 0);
}
}  // namespace
}  // namespace ievolve
