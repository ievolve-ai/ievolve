#include "ievolve/cli/stop.h"

#include <chrono>
#include <csignal>

#include "ievolve/controller/controller.h"

namespace ievolve::cli {

int StopSignal(const std::atomic<int>* stop_signal) {
  const int signal = stop_signal ? stop_signal->load() : 0;
  return signal == SIGINT || signal == SIGTERM ? signal : 0;
}

StopMonitor::StopMonitor(const std::atomic<int>* stop_signal, controller::Controller& controller,
                         utils::Logger* logger) {
  if (!stop_signal) return;

  // Logger calls are thread-safe, so the monitoring thread may report too.
  const auto request_stop = [stop_signal, &controller, logger] {
    if (logger) {
      logger->Warning("Received ", StopSignal(stop_signal) == SIGINT ? "SIGINT" : "SIGTERM",
                      "; stopping after the current step and saving a checkpoint");
    }
    controller.RequestStop();
  };

  // Handle a signal that already arrived, so the run stops without waiting for
  // the monitoring thread to start. The stop is then already requested, so no
  // thread is needed.
  if (StopSignal(stop_signal)) {
    request_stop();
    return;
  }

  // A signal handler may only touch a lock-free atomic, so it cannot notify a
  // condition variable. Poll the flag instead; the timed wait exists to react
  // to destruction promptly, not to the signal.
  thread_ = std::thread([this, stop_signal, request_stop] {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!done_) {
      if (StopSignal(stop_signal)) {
        request_stop();
        return;
      }

      condition_.wait_for(lock, std::chrono::milliseconds(10), [&] { return done_; });
    }
  });
}

StopMonitor::~StopMonitor() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = true;
  }

  condition_.notify_one();
  if (thread_.joinable()) thread_.join();
}

// Names printed in the run summary, so they are part of the CLI's observable
// output. The switch carries no default label, which lets -Wswitch flag a newly
// added reason; the trailing return only covers out-of-range values.
const char* StopName(controller::StopReason reason) {
  switch (reason) {
    case controller::StopReason::kIterationLimit:
      return "iteration_limit";
    case controller::StopReason::kTargetScore:
      return "target_score";
    case controller::StopReason::kEarlyStopping:
      return "early_stopping";
    case controller::StopReason::kRequested:
      return "requested";
  }

  return "unknown";
}

}  // namespace ievolve::cli
