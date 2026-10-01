#include "ievolve/database/program_store.h"

#include <limits>

#include "gtest/gtest.h"

namespace ievolve {
namespace {

Program Candidate(std::string id, double score) {
  Program program;
  program.id = std::move(id);
  program.code = program.id;
  program.metrics = {{"combined_score", score}};
  program.timestamp = 1;

  return program;
}

std::vector<std::string> Ids(const ProgramStore& store) {
  std::vector<std::string> ids;
  for (const auto& program : store.programs()) ids.push_back(program.id);

  return ids;
}

TEST(ProgramStoreTest, AddKeepsInsertionOrderAndLooksUpById) {
  ProgramStore store({});

  ASSERT_TRUE(store.Add(Candidate("a", 1)).ok());
  ASSERT_TRUE(store.Add(Candidate("b", 2)).ok());

  EXPECT_EQ(Ids(store), (std::vector<std::string>{"a", "b"}));
  EXPECT_TRUE(store.Contains("b"));
  EXPECT_FALSE(store.Contains("c"));
  EXPECT_EQ(store.at("b").metrics["combined_score"], 2);
  EXPECT_EQ(store.size(), 2u);
}

TEST(ProgramStoreTest, GetReturnsDetachedCopyOrNotFound) {
  ProgramStore store({});
  ASSERT_TRUE(store.Add(Candidate("a", 1)).ok());

  auto copy = store.Get("a");
  ASSERT_TRUE(copy.ok());
  copy->code = "changed";

  EXPECT_EQ(store.at("a").code, "a");
  EXPECT_EQ(store.Get("missing").status().code(), absl::StatusCode::kNotFound);
}

TEST(ProgramStoreTest, RejectsDuplicateIdAndNonFiniteFitness) {
  ProgramStore store({});
  ASSERT_TRUE(store.Add(Candidate("a", 1)).ok());

  EXPECT_EQ(store.Add(Candidate("a", 2)).code(), absl::StatusCode::kAlreadyExists);
  EXPECT_EQ(store.Add(Candidate("inf", std::numeric_limits<double>::infinity())).code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(store.size(), 1u);
}

TEST(ProgramStoreTest, EraseFromMiddleKeepsIndexConsistent) {
  ProgramStore store({});
  for (const auto* id : {"a", "b", "c", "d"}) ASSERT_TRUE(store.Add(Candidate(id, 1)).ok());

  store.Erase("b");
  store.Erase("missing");

  EXPECT_EQ(Ids(store), (std::vector<std::string>{"a", "c", "d"}));
  EXPECT_FALSE(store.Contains("b"));
  EXPECT_EQ(store.at("c").id, "c");
  EXPECT_EQ(store.at("d").id, "d");
  ASSERT_TRUE(store.Add(Candidate("b", 1)).ok());
  EXPECT_EQ(store.at("b").id, "b");
}

TEST(ProgramStoreTest, ClearRemovesEverything) {
  ProgramStore store({});
  ASSERT_TRUE(store.Add(Candidate("a", 1)).ok());

  store.Clear();

  EXPECT_TRUE(store.empty());
  EXPECT_FALSE(store.Contains("a"));
}

TEST(ProgramStoreTest, TopRanksStablyAndSkipsMissingNamedMetric) {
  ProgramStore store({});
  ASSERT_TRUE(store.Add(Candidate("low", 1)).ok());
  ASSERT_TRUE(store.Add(Candidate("tie-first", 3)).ok());
  ASSERT_TRUE(store.Add(Candidate("tie-second", 3)).ok());
  auto named = Candidate("named", 0);
  named.metrics["extra"] = 9;
  ASSERT_TRUE(store.Add(named).ok());

  auto top = store.Top(3, std::nullopt);
  ASSERT_TRUE(top.ok());
  ASSERT_EQ(top->size(), 3u);
  EXPECT_EQ((*top)[0].id, "tie-first");
  EXPECT_EQ((*top)[1].id, "tie-second");
  EXPECT_EQ((*top)[2].id, "low");

  auto by_metric = store.Top(10, "extra");
  ASSERT_TRUE(by_metric.ok());
  ASSERT_EQ(by_metric->size(), 1u);
  EXPECT_EQ(by_metric->front().id, "named");

  EXPECT_EQ(store.Top(-1, std::nullopt).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ProgramStoreTest, CopiesAreIndependent) {
  ProgramStore store({});
  ASSERT_TRUE(store.Add(Candidate("a", 1)).ok());

  ProgramStore copy = store;
  copy.Erase("a");
  ASSERT_TRUE(copy.Add(Candidate("b", 1)).ok());

  EXPECT_EQ(Ids(store), (std::vector<std::string>{"a"}));
  EXPECT_EQ(Ids(copy), (std::vector<std::string>{"b"}));
}

}  // namespace
}  // namespace ievolve
