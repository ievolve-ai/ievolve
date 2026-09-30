#include "ievolve/utils/threads.h"

#include <atomic>
#include <stdexcept>

#include "gtest/gtest.h"

namespace ievolve::utils {
namespace {

TEST(JoinThreadsTest, JoinsEveryThreadOnScopeExit) {
  std::atomic<int> finished{0};
  std::vector<std::thread> threads;

  {
    JoinThreads join(threads);
    for (int i = 0; i < 4; ++i) {
      threads.emplace_back([&finished] { ++finished; });
    }
  }

  // All four ran to completion before the scope ended, without any explicit
  // join in this test.
  EXPECT_EQ(finished.load(), 4);
  for (const auto& thread : threads) EXPECT_FALSE(thread.joinable());
}

TEST(JoinThreadsTest, JoinsThreadsWhenTheScopeExitsByException) {
  std::atomic<int> finished{0};
  std::vector<std::thread> threads;

  try {
    JoinThreads join(threads);
    threads.emplace_back([&finished] { ++finished; });
    throw std::runtime_error("unwind");
  } catch (const std::runtime_error&) {
  }

  EXPECT_EQ(finished.load(), 1);
  EXPECT_FALSE(threads.front().joinable());
}

TEST(JoinThreadsTest, AcceptsAnEmptyBatch) {
  std::vector<std::thread> threads;
  {
    JoinThreads join(threads);
  }
  EXPECT_TRUE(threads.empty());
}

TEST(JoinThreadsTest, SkipsThreadsThatWereAlreadyJoined) {
  std::atomic<int> finished{0};
  std::vector<std::thread> threads;

  {
    JoinThreads join(threads);
    threads.emplace_back([&finished] { ++finished; });
    threads.front().join();
  }

  EXPECT_EQ(finished.load(), 1);
}

}  // namespace
}  // namespace ievolve::utils
