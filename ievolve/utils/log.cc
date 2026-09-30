#include "ievolve/utils/log.h"

#include <array>
#include <iostream>
#include <mutex>
#include <utility>

namespace ievolve::utils {
namespace {

constexpr std::array<std::pair<std::string_view, LogLevel>, 5> kLevels = {{
    {"DEBUG", LogLevel::kDebug},
    {"INFO", LogLevel::kInfo},
    {"WARNING", LogLevel::kWarning},
    {"ERROR", LogLevel::kError},
    {"CRITICAL", LogLevel::kCritical},
}};

std::string_view LevelName(LogLevel level) {
  for (const auto& entry : kLevels) {
    if (entry.second == level) return entry.first;
  }

  return "UNKNOWN";
}

// A shared lock also protects different loggers that borrow the same stream.
std::mutex& OutputMutex() {
  static std::mutex mutex;
  return mutex;
}

}  // namespace

std::optional<LogLevel> ParseLogLevel(std::string_view name) {
  for (const auto& entry : kLevels) {
    if (entry.first == name) return entry.second;
  }

  return std::nullopt;
}

Logger::Logger() : Logger(std::clog) {}

Logger::Logger(std::ostream& output, LogLevel level) : output_(output), level_(level) {}

void Logger::SetLevel(LogLevel level) { level_.store(level, std::memory_order_relaxed); }

LogLevel Logger::GetLevel() const { return level_.load(std::memory_order_relaxed); }

bool Logger::IsEnabled(LogLevel level) const { return level >= GetLevel(); }

void Logger::Write(LogLevel level, std::string_view message) {
  std::lock_guard<std::mutex> lock(OutputMutex());
  if (!IsEnabled(level)) return;

  output_ << '[' << LevelName(level) << "] " << message << '\n';
  output_.flush();
}

}  // namespace ievolve::utils
