#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>

namespace {

bool WriteAll(int descriptor, const std::string& text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t size = write(descriptor, text.data() + offset, text.size() - offset);
    if (size < 0 && errno == EINTR) continue;
    if (size <= 0) return false;

    offset += static_cast<std::size_t>(size);
  }

  return true;
}

int EchoInput() {
  char buffer[4096];
  while (true) {
    const ssize_t size = read(STDIN_FILENO, buffer, sizeof(buffer));
    if (size < 0 && errno == EINTR) continue;
    if (size < 0) return 2;
    if (size == 0) return 0;

    if (!WriteAll(STDOUT_FILENO, std::string(buffer, static_cast<std::size_t>(size)))) {
      return 3;
    }
  }
}

}  // namespace
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  return 0;
#else
  if (argc < 2) return 1;

  const std::string mode = argv[1];
  if (mode == "args") {
    for (int i = 2; i < argc; ++i) {
      const std::string argument = argv[i];
      if (!WriteAll(STDOUT_FILENO, std::to_string(argument.size()) + ":" + argument + "\n")) {
        return 2;
      }
    }

    return 0;
  }

  if (mode == "cat") return EchoInput();

  if (mode == "duplex") {
    if (!WriteAll(STDOUT_FILENO, std::string(128 * 1024, 'o')) ||
        !WriteAll(STDERR_FILENO, std::string(128 * 1024, 'e'))) {
      return 2;
    }

    return EchoInput();
  }

  if (mode == "nonzero") {
    WriteAll(STDOUT_FILENO, "normal output");
    WriteAll(STDERR_FILENO, "diagnostic output");
    return 23;
  }

  if (mode == "close-stdin") {
    close(STDIN_FILENO);
    WriteAll(STDOUT_FILENO, "closed");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return 0;
  }

  if (mode == "outputs") {
    WriteAll(STDOUT_FILENO, "1234567");
    WriteAll(STDERR_FILENO, "abcdefg");
    return 0;
  }

  if (mode == "cwd") {
    return WriteAll(STDOUT_FILENO, std::filesystem::current_path().string()) ? 0 : 2;
  }

  if ((mode == "descendants" || mode == "orphan-pipes" || mode == "flood-descendants") && argc == 3) {
    const pid_t descendant = fork();
    if (descendant < 0) return 2;
    if (descendant == 0) {
      std::this_thread::sleep_for(std::chrono::seconds(30));
      return 0;
    }

    {
      std::ofstream file(argv[2]);
      file << getpid() << " " << descendant << "\n";
    }

    if (mode == "orphan-pipes") return 0;
    if (mode == "flood-descendants") {
      while (WriteAll(STDOUT_FILENO, std::string(64 * 1024, 'x'))) {
      }
      return 0;
    }

    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 0;
  }

  return 1;
#endif
}
