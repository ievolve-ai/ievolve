#include "ievolve/utils/log.h"

#include <array>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "ievolve/utils/threads.h"

namespace ievolve::utils {
namespace {

TEST(LogTest, ParsesCanonicalConfigurationLevels) {
  const std::array<std::pair<std::string_view, LogLevel>, 5> levels = {{
      {"DEBUG", LogLevel::kDebug},
      {"INFO", LogLevel::kInfo},
      {"WARNING", LogLevel::kWarning},
      {"ERROR", LogLevel::kError},
      {"CRITICAL", LogLevel::kCritical},
  }};
  for (const auto& [name, level] : levels) {
    SCOPED_TRACE(name);
    EXPECT_EQ(ParseLogLevel(name), level);
  }

  for (const std::string_view name : {"", "debug", "WARN", " INFO", "INFO ", "TRACE", "verbose"}) {
    SCOPED_TRACE(name);
    EXPECT_FALSE(ParseLogLevel(name));
  }
  EXPECT_FALSE(ParseLogLevel(std::string_view("INFO\0", 5)));
}

TEST(LogTest, DefaultsToInfoOnStandardError) {
  testing::internal::CaptureStderr();
  Logger logger;
  logger.Debug("hidden");
  logger.Info("ready");
  const auto output = testing::internal::GetCapturedStderr();

  EXPECT_EQ(output, "[INFO] ready\n");
}

TEST(LogTest, WritesEachSeverityAndCombinesMessageParts) {
  std::ostringstream output;
  Logger logger(output, LogLevel::kDebug);
  logger.Debug("iteration=", 3);
  logger.Info("score=", 1.5);
  logger.Warning("retry ", 2, '/', 3);
  logger.Error("evaluation failed");
  logger.Critical("cannot continue");
  logger.Log(LogLevel::kInfo, "done");

  EXPECT_EQ(output.str(),
            "[DEBUG] iteration=3\n[INFO] score=1.5\n[WARNING] retry 2/3\n"
            "[ERROR] evaluation failed\n[CRITICAL] cannot continue\n[INFO] done\n");
}

TEST(LogTest, ThresholdIncludesItsOwnSeverityAndAllHigherSeverities) {
  const std::array<std::pair<LogLevel, std::string_view>, 5> cases = {
      {{LogLevel::kDebug, "[DEBUG] d\n[INFO] i\n[WARNING] w\n[ERROR] e\n[CRITICAL] c\n"},
       {LogLevel::kInfo, "[INFO] i\n[WARNING] w\n[ERROR] e\n[CRITICAL] c\n"},
       {LogLevel::kWarning, "[WARNING] w\n[ERROR] e\n[CRITICAL] c\n"},
       {LogLevel::kError, "[ERROR] e\n[CRITICAL] c\n"},
       {LogLevel::kCritical, "[CRITICAL] c\n"}}};
  std::ostringstream output;
  Logger logger(output);

  for (const auto& [level, expected] : cases) {
    SCOPED_TRACE(static_cast<int>(level));
    output.str("");
    logger.SetLevel(level);
    logger.Debug("d");
    logger.Info("i");
    logger.Warning("w");
    logger.Error("e");
    logger.Critical("c");

    EXPECT_EQ(logger.GetLevel(), level);
    EXPECT_EQ(output.str(), expected);
  }

  logger.SetLevel(LogLevel::kDebug);
  EXPECT_TRUE(logger.IsEnabled(LogLevel::kDebug));
  logger.Debug("enabled again");
  EXPECT_NE(output.str().find("[DEBUG] enabled again\n"), std::string::npos);
}

struct CountFormatting {
  int& calls;
};

std::ostream& operator<<(std::ostream& output, const CountFormatting& value) {
  ++value.calls;
  return output << "formatted";
}

TEST(LogTest, SuppressedMessagesAreNotFormatted) {
  std::ostringstream output;
  Logger logger(output, LogLevel::kWarning);
  int calls = 0;

  EXPECT_FALSE(logger.IsEnabled(LogLevel::kInfo));
  logger.Info(CountFormatting{calls});
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(output.str().empty());

  logger.Warning(CountFormatting{calls});
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(output.str(), "[WARNING] formatted\n");
}

TEST(LogTest, LoggersKeepIndependentLevelsAndOutputStreams) {
  std::ostringstream first, second;
  Logger quiet(first, LogLevel::kError);
  Logger verbose(second, LogLevel::kDebug);

  quiet.Info("hidden");
  verbose.Info("visible");
  quiet.SetLevel(LogLevel::kCritical);
  verbose.Debug("still visible");

  EXPECT_TRUE(first.str().empty());
  EXPECT_EQ(second.str(), "[INFO] visible\n[DEBUG] still visible\n");
}

TEST(LogTest, ConcurrentLoggersSharingAStreamWriteCompleteRecords) {
  std::ostringstream output;
  Logger first(output), second(output);
  std::set<std::string> expected;
  for (int worker = 0; worker < 8; ++worker) {
    for (int record = 0; record < 100; ++record) {
      expected.insert("[INFO] worker=" + std::to_string(worker) + " record=" + std::to_string(record));
    }
  }

  {
    std::vector<std::thread> workers;
    JoinThreads join(workers);
    for (int worker = 0; worker < 8; ++worker) {
      workers.emplace_back([&, worker] {
        auto& logger = worker % 2 == 0 ? first : second;
        for (int record = 0; record < 100; ++record) {
          logger.Info("worker=", worker, " record=", record);
        }
      });
    }
  }

  std::istringstream records(output.str());
  std::string line;
  std::size_t count = 0;
  while (std::getline(records, line)) {
    EXPECT_EQ(expected.erase(line), 1u) << line;
    ++count;
  }
  EXPECT_EQ(count, 800u);
  EXPECT_TRUE(expected.empty());
}

}  // namespace
}  // namespace ievolve::utils
