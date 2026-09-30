#ifndef IEVOLVE_UTILS_THREADS_H_
#define IEVOLVE_UTILS_THREADS_H_

#include <thread>
#include <vector>

namespace ievolve::utils {

// Joins a batch of threads on scope exit, so every early return and every
// exception still waits for the workers before their shared state goes away.
// C++17 has no std::jthread; when this project moves to C++20 the callers can
// hold std::jthread directly and this goes away.
//
// Header-only and dependency-free. Creating the threads remains the caller's
// job, which is where the platform thread library gets linked.
class JoinThreads {
 public:
  explicit JoinThreads(std::vector<std::thread>& threads) : threads_(threads) {}
  ~JoinThreads() {
    for (auto& thread : threads_) {
      if (thread.joinable()) thread.join();
    }
  }

  JoinThreads(const JoinThreads&) = delete;
  JoinThreads& operator=(const JoinThreads&) = delete;

 private:
  std::vector<std::thread>& threads_;
};

}  // namespace ievolve::utils

#endif  // IEVOLVE_UTILS_THREADS_H_
