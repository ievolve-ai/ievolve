#ifndef IEVOLVE_CLI_STOP_H_
#define IEVOLVE_CLI_STOP_H_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "ievolve/utils/log.h"

namespace ievolve::controller {
class Controller;
enum class StopReason;
}  // namespace ievolve::controller

namespace ievolve::cli {

// Returns SIGINT/SIGTERM, or zero for an absent flag or unrelated signal.
int StopSignal(const std::atomic<int>* stop_signal);
const char* StopName(controller::StopReason reason);

// Forwards a caller-owned signal flag to Controller::RequestStop. The flag,
// controller and optional logger must outlive this monitor; destruction joins
// the monitoring thread. Signal handlers are installed by the executable, not
// by this class.
class StopMonitor {
 public:
  StopMonitor(const std::atomic<int>* stop_signal, controller::Controller& controller, utils::Logger* logger = nullptr);
  ~StopMonitor();

  StopMonitor(const StopMonitor&) = delete;
  StopMonitor& operator=(const StopMonitor&) = delete;

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool done_ = false;
  std::thread thread_;
};

}  // namespace ievolve::cli
#endif  // IEVOLVE_CLI_STOP_H_
