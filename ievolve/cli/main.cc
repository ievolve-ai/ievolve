#include <csignal>
#include <iostream>

#include "ievolve/cli/cli.h"

namespace {
// The handler records the signal and returns; storing to a lock-free atomic is
// the only work it may safely do. Everything else, including deciding when to
// stop and what exit code to report, happens on the main thread. Installing
// handlers is the executable's job, never the cli library's.
static_assert(std::atomic<int>::is_always_lock_free, "Signal handling requires lock-free atomic integers");
std::atomic<int> stop_signal{0};
void RequestStop(int signal) { stop_signal.store(signal, std::memory_order_relaxed); }
}  // namespace

int main(int argc, char** argv) {
  const auto previous_int = std::signal(SIGINT, RequestStop);
  const auto previous_term = std::signal(SIGTERM, RequestStop);
  if (previous_int == SIG_ERR || previous_term == SIG_ERR) {
    std::cerr << "Cannot install signal handlers\n";
    return 1;
  }

  ievolve::cli::RuntimeOptions runtime;
  runtime.stop_signal = &stop_signal;
  const int result = ievolve::cli::Run({argv + 1, argv + argc}, std::cout, std::cerr, runtime);

  // Leave global signal state as it was found.
  std::signal(SIGINT, previous_int);
  std::signal(SIGTERM, previous_term);

  return result;
}
