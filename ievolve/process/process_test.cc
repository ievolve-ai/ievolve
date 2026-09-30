#include "ievolve/process/process.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "absl/status/status.h"
#include "gtest/gtest.h"

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#endif

namespace ievolve::process {
namespace {

#ifdef _WIN32
TEST(ProcessTest, ReportsUnsupportedPlatform) {
  ProcessRequest request;
  request.argv = {"unused"};

  EXPECT_EQ(RunProcess(request).status().code(), absl::StatusCode::kUnimplemented);
}
#else

ProcessRequest Request(const std::string& mode) {
  ProcessRequest request;
  request.argv = {IEVOLVE_PROCESS_TEST_CHILD_PATH, mode};
  request.timeout = std::chrono::seconds(5);

  return request;
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern = (std::filesystem::temp_directory_path() / "ievolve-process-XXXXXX").string();
    if (char* result = mkdtemp(pattern.data())) path_ = result;
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

bool IsRunning(pid_t process) {
  if (kill(process, 0) != 0 && errno == ESRCH) return false;

#ifdef __linux__
  // A terminated orphan can await reaping by the host's PID 1 in containers.
  std::ifstream file("/proc/" + std::to_string(process) + "/stat");
  std::string line;
  std::getline(file, line);
  const auto name_end = line.rfind(')');
  if (name_end != std::string::npos && name_end + 2 < line.size() && line[name_end + 2] == 'Z') {
    return false;
  }
#endif

  return true;
}

TEST(ProcessTest, PreservesArgumentBoundariesAndMetacharacters) {
  auto request = Request("args");
  request.argv.insert(request.argv.end(), {"hello world", "$(echo nope);*|&", "", "two\nlines", "'quoted'"});

  const auto result = RunProcess(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->exit_code, 0);
  EXPECT_EQ(result->stdout_text, "11:hello world\n16:$(echo nope);*|&\n0:\n9:two\nlines\n8:'quoted'\n");
  EXPECT_TRUE(result->stderr_text.empty());
}

TEST(ProcessTest, DrainsBothOutputsWhileWritingLargeBinaryInput) {
  auto request = Request("duplex");
  request.stdin_text = std::string(512 * 1024, 'i');
  request.stdin_text[17] = '\0';

  const auto result = RunProcess(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->exit_code, 0);
  EXPECT_EQ(result->stdout_text, std::string(128 * 1024, 'o') + request.stdin_text);
  EXPECT_EQ(result->stderr_text, std::string(128 * 1024, 'e'));
}

TEST(ProcessTest, ClosesStdinForEmptyInput) {
  const auto result = RunProcess(Request("cat"));
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->exit_code, 0);
  EXPECT_TRUE(result->stdout_text.empty());
  EXPECT_TRUE(result->stderr_text.empty());
}

TEST(ProcessTest, ReturnsNonzeroExitWithBothOutputs) {
  const auto result = RunProcess(Request("nonzero"));
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->exit_code, 23);
  EXPECT_EQ(result->stdout_text, "normal output");
  EXPECT_EQ(result->stderr_text, "diagnostic output");
}

TEST(ProcessTest, ReportsMissingExecutableWithoutIncludingArguments) {
  auto request = Request("unused-secret-prompt");
  request.argv.front() = "/ievolve-does-not-exist/process";

  const auto result = RunProcess(request);
  EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);

  EXPECT_EQ(result.status().message().find("unused-secret-prompt"), std::string::npos);
}

TEST(ProcessTest, ToleratesChildClosingStdinWithoutChangingSigpipeHandler) {
  struct sigaction before{};
  ASSERT_EQ(sigaction(SIGPIPE, nullptr, &before), 0);

  auto request = Request("close-stdin");
  request.stdin_text = std::string(1024 * 1024, 'x');

  const auto result = RunProcess(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->exit_code, 0);
  EXPECT_EQ(result->stdout_text, "closed");

  struct sigaction after{};
  ASSERT_EQ(sigaction(SIGPIPE, nullptr, &after), 0);
  EXPECT_EQ(before.sa_handler, after.sa_handler);
}

TEST(ProcessTest, CapsCombinedOutput) {
  auto request = Request("outputs");
  request.max_output_bytes = 10;

  EXPECT_EQ(RunProcess(request).status().code(), absl::StatusCode::kResourceExhausted);
}

TEST(ProcessTest, AllowsExactlyTheCombinedOutputLimit) {
  auto request = Request("outputs");
  request.max_output_bytes = 14;

  const auto result = RunProcess(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->stdout_text, "1234567");
  EXPECT_EQ(result->stderr_text, "abcdefg");
}

TEST(ProcessTest, SetsWorkingDirectoryWithoutChangingParentDirectory) {
  TemporaryDirectory directory;
  ASSERT_FALSE(directory.path().empty());

  const auto parent_directory = std::filesystem::current_path();
  auto request = Request("cwd");
  request.working_directory = directory.path();

  const auto result = RunProcess(request);
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(std::filesystem::path(result->stdout_text), std::filesystem::canonical(directory.path()));
  EXPECT_EQ(std::filesystem::current_path(), parent_directory);
}

class ProcessDescendantTest : public ::testing::TestWithParam<const char*> {};

TEST_P(ProcessDescendantTest, TerminatesProcessGroupAndReapsChildOnError) {
  TemporaryDirectory directory;
  ASSERT_FALSE(directory.path().empty());

  const auto pid_file = directory.path() / "pids";
  auto request = Request(GetParam());
  request.argv.push_back(pid_file.string());
  request.timeout = std::chrono::milliseconds(300);
  const bool output_error = std::string(GetParam()) == "flood-descendants";
  if (output_error) request.max_output_bytes = 10;

  const auto start = std::chrono::steady_clock::now();
  const auto result = RunProcess(request);
  EXPECT_EQ(result.status().code(),
            output_error ? absl::StatusCode::kResourceExhausted : absl::StatusCode::kDeadlineExceeded);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));

  pid_t child = 0;
  pid_t descendant = 0;
  std::ifstream(pid_file) >> child >> descendant;
  ASSERT_GT(child, 0);
  ASSERT_GT(descendant, 0);

  const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (IsRunning(descendant) && std::chrono::steady_clock::now() < cleanup_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_FALSE(IsRunning(child));
  EXPECT_FALSE(IsRunning(descendant));

  // Bound the effects of a broken implementation before reporting its failure.
  if (IsRunning(descendant)) kill(descendant, SIGKILL);
  if (IsRunning(child)) kill(child, SIGKILL);

  int status = 0;
  errno = 0;
  EXPECT_EQ(waitpid(child, &status, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
}

INSTANTIATE_TEST_SUITE_P(ProcessTest, ProcessDescendantTest,
                         ::testing::Values("descendants", "orphan-pipes", "flood-descendants"));

TEST(ProcessTest, ExecutesConcurrentCallsWithoutPipeInheritance) {
  std::vector<std::future<absl::StatusOr<ProcessResult>>> futures;
  for (int i = 0; i < 12; ++i) {
    futures.push_back(std::async(std::launch::async, [i] {
      auto request = Request("duplex");
      request.stdin_text = std::string(64 * 1024, static_cast<char>('a' + i));
      return RunProcess(request);
    }));
  }

  for (std::size_t i = 0; i < futures.size(); ++i) {
    const auto result = futures[i].get();
    ASSERT_TRUE(result.ok()) << result.status();

    EXPECT_EQ(result->exit_code, 0);
    EXPECT_EQ(result->stdout_text, std::string(128 * 1024, 'o') + std::string(64 * 1024, static_cast<char>('a' + i)));
    EXPECT_EQ(result->stderr_text, std::string(128 * 1024, 'e'));
  }
}

TEST(ProcessTest, RejectsInvalidArgumentsAndTimeouts) {
  ProcessRequest request;
  EXPECT_EQ(RunProcess(request).status().code(), absl::StatusCode::kInvalidArgument);

  request = Request("args");
  request.argv.front().clear();

  EXPECT_EQ(RunProcess(request).status().code(), absl::StatusCode::kInvalidArgument);

  request = Request("args");
  request.argv.push_back(std::string("a\0b", 3));

  EXPECT_EQ(RunProcess(request).status().code(), absl::StatusCode::kInvalidArgument);

  request = Request("cat");
  request.timeout = std::chrono::milliseconds::zero();

  EXPECT_EQ(RunProcess(request).status().code(), absl::StatusCode::kInvalidArgument);
}

#endif

}  // namespace
}  // namespace ievolve::process
