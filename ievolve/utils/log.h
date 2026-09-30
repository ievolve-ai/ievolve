#ifndef IEVOLVE_UTILS_LOG_H_
#define IEVOLVE_UTILS_LOG_H_

#include <atomic>
#include <optional>
#include <ostream>
#include <sstream>
#include <string_view>

namespace ievolve::utils {

enum class LogLevel { kDebug, kInfo, kWarning, kError, kCritical };

// Matches the canonical uppercase names accepted by YAML runtime configuration.
// Unknown names return nullopt; case and whitespace are significant.
std::optional<LogLevel> ParseLogLevel(std::string_view name);

// Synchronous logging with an independent minimum severity for each instance.
// Defaults to INFO on std::clog (standard error). A supplied stream must outlive
// the logger and all its calls. Share loggers by reference, not by copying them.
//
// Log calls and level changes are thread-safe. Writes through Logger instances
// are serialized, including instances sharing a stream. Direct writes to that
// stream still require the caller's synchronization. Stream error handling and
// exceptions follow the supplied ostream's policy. CRITICAL does not terminate.
class Logger {
 public:
  Logger();
  explicit Logger(std::ostream& output, LogLevel level = LogLevel::kInfo);

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  void SetLevel(LogLevel level);
  LogLevel GetLevel() const;
  bool IsEnabled(LogLevel level) const;

  // Message parts use ordinary ostream formatting. Suppressed messages skip
  // formatting, but C++ still evaluates arguments before entering the call.
  template <typename... Args>
  void Log(LogLevel level, const Args&... args) {
    if (!IsEnabled(level)) return;

    std::ostringstream message;
    (message << ... << args);
    Write(level, message.str());
  }

  template <typename... Args>
  void Debug(const Args&... args) {
    Log(LogLevel::kDebug, args...);
  }

  template <typename... Args>
  void Info(const Args&... args) {
    Log(LogLevel::kInfo, args...);
  }

  template <typename... Args>
  void Warning(const Args&... args) {
    Log(LogLevel::kWarning, args...);
  }

  template <typename... Args>
  void Error(const Args&... args) {
    Log(LogLevel::kError, args...);
  }

  template <typename... Args>
  void Critical(const Args&... args) {
    Log(LogLevel::kCritical, args...);
  }

 private:
  void Write(LogLevel level, std::string_view message);

  std::ostream& output_;
  std::atomic<LogLevel> level_;
};

}  // namespace ievolve::utils

#endif  // IEVOLVE_UTILS_LOG_H_
