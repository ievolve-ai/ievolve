#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "ievolve/process/process.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace ievolve::process {
namespace {

#ifndef _WIN32

class FileDescriptor {
 public:
  FileDescriptor() = default;
  ~FileDescriptor() { Reset(); }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  int get() const { return descriptor_; }
  void Reset(int descriptor = -1) {
    if (descriptor_ >= 0) close(descriptor_);
    descriptor_ = descriptor;
  }

 private:
  int descriptor_ = -1;
};

absl::Status MakePipe(FileDescriptor& reader, FileDescriptor& writer) {
  int descriptors[2];
#ifdef __linux__
  if (pipe2(descriptors, O_CLOEXEC) < 0) {
#else
  if (pipe(descriptors) < 0) {
#endif
    return absl::ErrnoToStatus(errno, "Creating process pipe");
  }

  reader.Reset(descriptors[0]);
  writer.Reset(descriptors[1]);

  for (FileDescriptor* descriptor : {&reader, &writer}) {
    // Keep pipe handles distinct from dup2's destinations, even when the caller
    // has closed one of its standard descriptors.
    if (descriptor->get() < 3) {
      const int duplicate = fcntl(descriptor->get(), F_DUPFD_CLOEXEC, 3);
      if (duplicate < 0) {
        return absl::ErrnoToStatus(errno, "Duplicating process pipe");
      }
      descriptor->Reset(duplicate);
    }
#ifndef __linux__
    if (fcntl(descriptor->get(), F_SETFD, FD_CLOEXEC) < 0) {
      return absl::ErrnoToStatus(errno, "Protecting process pipe");
    }
#endif
  }

  return absl::OkStatus();
}

class SpawnConfiguration {
 public:
  ~SpawnConfiguration() {
    if (actions_initialized_) posix_spawn_file_actions_destroy(&actions);
    if (attributes_initialized_) posix_spawnattr_destroy(&attributes);
  }

  absl::Status Initialize(const ProcessRequest& request, const std::array<FileDescriptor, 6>& descriptors) {
    int error = posix_spawn_file_actions_init(&actions);
    if (error != 0) return SpawnError(error);
    actions_initialized_ = true;

    error = posix_spawnattr_init(&attributes);
    if (error != 0) return SpawnError(error);
    attributes_initialized_ = true;

    short flags = POSIX_SPAWN_SETPGROUP;
#ifdef __APPLE__
    // macOS lacks pipe2: close all non-action descriptors in the child, also
    // preventing inheritance during another thread's pipe/fcntl interval.
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    error = posix_spawnattr_setflags(&attributes, flags);
    if (error != 0) return SpawnError(error);
    error = posix_spawnattr_setpgroup(&attributes, 0);
    if (error != 0) return SpawnError(error);

    if (request.working_directory) {
#if defined(__APPLE__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 260000
      error = posix_spawn_file_actions_addchdir(&actions, request.working_directory->c_str());
#else
      error = posix_spawn_file_actions_addchdir_np(&actions, request.working_directory->c_str());
#endif
      if (error != 0) return SpawnError(error);
    }

    const std::array<int, 3> sources = {0, 3, 5};
    for (int destination = 0; destination < 3; ++destination) {
      error = posix_spawn_file_actions_adddup2(&actions, descriptors[sources[destination]].get(), destination);
      if (error != 0) return SpawnError(error);
    }
    for (const auto& descriptor : descriptors) {
      error = posix_spawn_file_actions_addclose(&actions, descriptor.get());
      if (error != 0) return SpawnError(error);
    }

    return absl::OkStatus();
  }

  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;

 private:
  static absl::Status SpawnError(int error) { return absl::ErrnoToStatus(error, "Configuring process launch"); }

  bool actions_initialized_ = false;
  bool attributes_initialized_ = false;
};

class ChildProcess {
 public:
  explicit ChildProcess(pid_t process) : process_(process) {}
  ~ChildProcess() {
    if (finished_) return;

    // Descendants can hold the output pipes even after the direct child exits.
    kill(-process_, SIGKILL);
    if (!reaped_) {
      kill(process_, SIGKILL);
      while (waitpid(process_, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
  }

  absl::Status Poll() {
    if (reaped_) return absl::OkStatus();

    const pid_t result = waitpid(process_, &status_, WNOHANG);
    if (result < 0 && errno != EINTR) {
      return absl::ErrnoToStatus(errno, "Waiting for process");
    }

    reaped_ = result == process_;
    return absl::OkStatus();
  }

  bool reaped() const { return reaped_; }
  int Finish() {
    finished_ = true;
    return WIFEXITED(status_) ? WEXITSTATUS(status_) : 128 + WTERMSIG(status_);
  }

 private:
  pid_t process_;
  int status_ = 0;
  bool reaped_ = false;
  bool finished_ = false;
};

absl::Status SetNonblocking(int descriptor) {
  const int flags = fcntl(descriptor, F_GETFL);
  if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
    return absl::ErrnoToStatus(errno, "Setting process pipe nonblocking");
  }

  return absl::OkStatus();
}

ssize_t WriteWithoutSigpipe(int descriptor, const char* bytes, std::size_t size) {
#ifdef __APPLE__
  // F_SETNOSIGPIPE is per-descriptor, with no change to signal disposition.
  return write(descriptor, bytes, size);
#else
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGPIPE);
  sigset_t previous;
  const int error = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
  if (error != 0) {
    errno = error;
    return -1;
  }

  sigset_t pending;
  const bool already_pending = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;

  const ssize_t result = write(descriptor, bytes, size);
  const int saved_errno = errno;
  if (result < 0 && saved_errno == EPIPE && !already_pending) {
    const timespec no_wait = {0, 0};
    while (sigtimedwait(&blocked, nullptr, &no_wait) < 0 && errno == EINTR) {
    }
  }

  pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  errno = saved_errno;
  return result;
#endif
}

absl::Status ReadOutput(FileDescriptor& descriptor, std::string& output, std::size_t& remaining) {
  std::array<char, 16384> buffer;
  const ssize_t count = read(descriptor.get(), buffer.data(), buffer.size());
  if (count == 0) {
    descriptor.Reset();
  } else if (count < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      return absl::ErrnoToStatus(errno, "Reading process output");
    }
  } else {
    const auto size = static_cast<std::size_t>(count);
    if (size > remaining) {
      return absl::ResourceExhaustedError("Process output exceeded byte limit");
    }

    output.append(buffer.data(), size);
    remaining -= size;
  }

  return absl::OkStatus();
}

#endif

}  // namespace

absl::StatusOr<ProcessResult> RunProcess(const ProcessRequest& request) {
#ifdef _WIN32
  return absl::UnimplementedError("Process execution requires POSIX");
#else
  if (request.argv.empty() || request.argv.front().empty()) {
    return absl::InvalidArgumentError("Process executable must be specified");
  }
  for (const auto& argument : request.argv) {
    if (argument.find('\0') != std::string::npos) {
      return absl::InvalidArgumentError("Process arguments must not contain NUL");
    }
  }
  if (request.working_directory && request.working_directory->native().find('\0') != std::string::npos) {
    return absl::InvalidArgumentError("Process directory must not contain NUL");
  }
  if (request.timeout <= std::chrono::milliseconds::zero()) {
    return absl::InvalidArgumentError("Process timeout must be positive");
  }

  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  const auto maximum_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - start);
  const auto deadline = request.timeout >= maximum_timeout ? Clock::time_point::max() : start + request.timeout;

  // stdin: child reads [0], parent writes [1]; stdout/stderr: parent reads
  // [2]/[4], child writes [3]/[5]. Every handle is closed on every return path.
  std::array<FileDescriptor, 6> descriptors;
  for (std::size_t i = 0; i < descriptors.size(); i += 2) {
    auto status = MakePipe(descriptors[i], descriptors[i + 1]);
    if (!status.ok()) return status;
  }

  SpawnConfiguration configuration;
  auto status = configuration.Initialize(request, descriptors);
  if (!status.ok()) return status;

  std::vector<char*> arguments;
  arguments.reserve(request.argv.size() + 1);
  for (const auto& argument : request.argv) {
    arguments.push_back(const_cast<char*>(argument.c_str()));
  }
  arguments.push_back(nullptr);

  pid_t process;
  const int error = posix_spawnp(&process, arguments.front(), &configuration.actions, &configuration.attributes,
                                 arguments.data(), environ);
  if (error != 0) return absl::ErrnoToStatus(error, "Launching process");
  ChildProcess child(process);

  descriptors[0].Reset();
  descriptors[3].Reset();
  descriptors[5].Reset();

  for (int index : {1, 2, 4}) {
    status = SetNonblocking(descriptors[index].get());
    if (!status.ok()) return status;
  }
#ifdef __APPLE__
  if (fcntl(descriptors[1].get(), F_SETNOSIGPIPE, 1) < 0) {
    return absl::ErrnoToStatus(errno, "Protecting process input from SIGPIPE");
  }
#endif
  if (request.stdin_text.empty()) descriptors[1].Reset();

  ProcessResult result;
  std::size_t input_offset = 0;
  std::size_t remaining = request.max_output_bytes;

  while (true) {
    status = child.Poll();
    if (!status.ok()) return status;
    if (child.reaped()) descriptors[1].Reset();
    if (child.reaped() && descriptors[2].get() < 0 && descriptors[4].get() < 0) {
      result.exit_code = child.Finish();
      return result;
    }

    const auto now = Clock::now();
    if (now >= deadline) {
      return absl::DeadlineExceededError("Process exceeded time limit");
    }
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int wait_ms = static_cast<int>(std::min<decltype(milliseconds)>(milliseconds, 50));

    std::array<pollfd, 3> watched = {
        {{descriptors[1].get(), POLLOUT, 0}, {descriptors[2].get(), POLLIN, 0}, {descriptors[4].get(), POLLIN, 0}}};
    if (poll(watched.data(), watched.size(), wait_ms) < 0) {
      if (errno == EINTR) continue;
      return absl::ErrnoToStatus(errno, "Polling process pipes");
    }
    for (const auto& descriptor : watched) {
      if (descriptor.revents & POLLNVAL) {
        return absl::InternalError("Process pipe became invalid");
      }
    }

    if (watched[0].revents & (POLLERR | POLLHUP)) {
      descriptors[1].Reset();
    } else if (watched[0].revents & POLLOUT) {
      const auto size = std::min<std::size_t>(request.stdin_text.size() - input_offset, 16384);
      const ssize_t count = WriteWithoutSigpipe(descriptors[1].get(), request.stdin_text.data() + input_offset, size);
      if (count > 0) {
        input_offset += static_cast<std::size_t>(count);
        if (input_offset == request.stdin_text.size()) descriptors[1].Reset();
      } else if (count < 0 && errno == EPIPE) {
        descriptors[1].Reset();
      } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        return absl::ErrnoToStatus(errno, "Writing process input");
      }
    }

    if (watched[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      status = ReadOutput(descriptors[2], result.stdout_text, remaining);
      if (!status.ok()) return status;
    }
    if (watched[2].revents & (POLLIN | POLLHUP | POLLERR)) {
      status = ReadOutput(descriptors[4], result.stderr_text, remaining);
      if (!status.ok()) return status;
    }
  }
#endif
}

}  // namespace ievolve::process
