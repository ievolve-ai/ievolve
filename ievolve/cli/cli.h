#ifndef IEVOLVE_CLI_CLI_H_
#define IEVOLVE_CLI_CLI_H_

#include <atomic>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "ievolve/controller/controller.h"
#include "ievolve/llm/cli_client.h"

namespace ievolve::cli {

struct Arguments {
  std::optional<std::filesystem::path> config_file;
  bool help = false;
  bool version = false;
};

struct RuntimeOptions {
  // Defaults to real Claude Code/Codex clients. Tests can inject an offline
  // LLM.
  LLMFactory llm_factory;
  // Optional lock-free signal flag, owned by caller for the duration of Run.
  // This library never installs process-wide signal handlers.
  const std::atomic<int>* stop_signal = nullptr;
};

// args excludes argv[0]; only --config/-c, --help/-h and --version are accepted.
absl::StatusOr<Arguments> ParseArguments(const std::vector<std::string>& args);
std::string Help();
// Loads all runtime settings from YAML. Relative run paths resolve against the
// configuration directory; bare executable names retain PATH lookup.
// 0 completed; 1 runtime failure; 2 invalid usage/configuration; 128+signal
// stopped.
int Run(const std::vector<std::string>& args, std::ostream& output, std::ostream& errors,
        const RuntimeOptions& runtime = {});

}  // namespace ievolve::cli
#endif  // IEVOLVE_CLI_CLI_H_
