#include "ievolve/llm/cli_client.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>

#include "gtest/gtest.h"
#include "ievolve/config/config.h"
#include "ievolve/llm/claude_code.h"
#include "ievolve/llm/codex.h"
#include "ievolve/llm/ensemble.h"
#include "ievolve/prompt/prompt_sampler.h"
#include "nlohmann/json.hpp"

namespace ievolve {
namespace {
using Json = nlohmann::json;

std::string Fixture(const std::string& name) {
  std::ifstream file(std::string(IEVOLVE_LLM_TEST_DATA_DIR) + "/" + name);
  EXPECT_TRUE(file.good());

  return {std::istreambuf_iterator<char>(file), {}};
}

LLMModelConfig Model(std::string provider) {
  LLMModelConfig config;
  config.provider = std::move(provider);
  config.name = "test-model";
  config.timeout = 2;
  config.retries = 0;
  config.retry_delay = 0;

  return config;
}

std::string Argument(const ProcessRequest& request, const std::string& name) {
  const auto it = std::find(request.argv.begin(), request.argv.end(), name);
  if (it == request.argv.end() || it + 1 == request.argv.end()) return "<absent>";

  return *(it + 1);
}

TEST(CLIClientTest, ClaudePassesLiteralPromptAndReturnsPerCallUsage) {
  CLIOptions options;
  options.claude_executable = "/path with spaces/claude";
  std::filesystem::path temporary;
  options.runner = [&](const ProcessRequest& request) {
    temporary = *request.working_directory;
    EXPECT_TRUE(std::filesystem::is_directory(temporary));
    EXPECT_EQ(request.argv.front(), "/path with spaces/claude");
    EXPECT_EQ(Argument(request, "--output-format"), "json");
    EXPECT_EQ(Argument(request, "--tools"), "");
    EXPECT_EQ(Argument(request, "--system-prompt"), "system \"literal\"");
    EXPECT_EQ(request.stdin_text, "--option $(echo secret) `cmd`\n中文");
    EXPECT_EQ(request.timeout, std::chrono::seconds(2));

    return absl::StatusOr<ProcessResult>(ProcessResult{0, Fixture("claude_success.json"), ""});
  };

  auto config = Model("claude_code");
  config.system_message = "system \"literal\"";
  auto client = CreateLLM(config, options);
  ASSERT_TRUE(client.ok()) << client.status();

  auto result = (*client)->Generate("--option $(echo secret) `cmd`\n中文");
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->text, "int Add(int a, int b) { return a + b; }");
  EXPECT_EQ(result->provider, "claude_code");
  EXPECT_EQ(result->model, "test-model");

  ASSERT_TRUE(result->usage);
  EXPECT_EQ(result->usage->input_tokens, 100);
  EXPECT_EQ(result->usage->cached_input_tokens, 30);
  EXPECT_EQ(result->usage->output_tokens, 20);
  EXPECT_EQ(result->usage->cost_usd, 0.012);

  EXPECT_FALSE(std::filesystem::exists(temporary));
}

TEST(CLIClientTest, CodexUsesStdinAndSeparatesFinalAnswerFromEvents) {
  CLIOptions options;
  options.runner = [](const ProcessRequest& request) {
    EXPECT_EQ(request.argv[1], "exec");
    EXPECT_EQ(request.argv.back(), "-");
    EXPECT_EQ(Argument(request, "--sandbox"), "read-only");
    EXPECT_NE(std::find(request.argv.begin(), request.argv.end(), "approval_policy=\"never\""), request.argv.end());
    EXPECT_NE(std::find(request.argv.begin(), request.argv.end(), "developer_instructions=\"system\\nline\""),
              request.argv.end());
    EXPECT_EQ(request.stdin_text, "hello");

    return absl::StatusOr<ProcessResult>(ProcessResult{0, Fixture("codex_success.jsonl"), "progress"});
  };

  CodexCLILLM client(Model("codex"), options);

  auto result = client.GenerateWithContext("system\nline", {{"user", "hello"}});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->text, "int Add(int a, int b) { return a + b; }");

  ASSERT_TRUE(result->usage);
  EXPECT_EQ(result->usage->input_tokens, 150);
  EXPECT_EQ(result->usage->cached_input_tokens, 50);
  EXPECT_EQ(result->usage->output_tokens, 25);
}

TEST(CLIClientTest, CodexEscapesDelInTomlInstructionsWithoutChangingText) {
  const std::string system = "before\n" + std::string(1, '\x7f') + "after \"quoted\" \\u007f 中文";
  CLIOptions options;
  options.runner = [&](const ProcessRequest& request) {
    const std::string prefix = "developer_instructions=";
    const auto argument = std::find_if(request.argv.begin(), request.argv.end(), [&](const std::string& value) {
      return value.compare(0, prefix.size(), prefix) == 0;
    });
    EXPECT_NE(argument, request.argv.end());
    if (argument != request.argv.end()) {
      EXPECT_EQ(*argument,
                "developer_instructions=\"before\\n\\u007fafter \\\"quoted\\\" "
                "\\\\u007f 中文\"");
      EXPECT_EQ(Json::parse(argument->substr(prefix.size())).get<std::string>(), system);
    }

    return absl::StatusOr<ProcessResult>(ProcessResult{0, Fixture("codex_success.jsonl"), ""});
  };

  const auto result = CodexCLILLM(Model("codex"), options).GenerateWithContext(system, {{"user", "hello"}});
  ASSERT_TRUE(result.ok()) << result.status();
}

TEST(CLIClientTest, PreservesAssistantHistoryAndExplicitEmptySystemMessage) {
  CLIOptions options;
  options.runner = [](const ProcessRequest& request) {
    const auto payload = Json::parse(request.stdin_text);
    EXPECT_EQ(payload.at("messages"), (Json::array({{{"role", "user"}, {"content", "first"}},
                                                    {{"role", "assistant"}, {"content", "answer"}},
                                                    {{"role", "user"}, {"content", "second"}}})));
    EXPECT_EQ(Argument(request, "--system-prompt"), "<absent>");

    return absl::StatusOr<ProcessResult>(ProcessResult{0, Fixture("claude_success.json"), ""});
  };

  auto config = Model("claude_code");
  config.system_message = "default";
  ClaudeCodeLLM client(config, options);

  EXPECT_TRUE(client.GenerateWithContext("", {{"user", "first"}, {"assistant", "answer"}, {"user", "second"}}).ok());
}

TEST(CLIClientTest, OverridesEffortAndTimeoutPerCall) {
  CLIOptions options;
  options.runner = [](const ProcessRequest& request) {
    EXPECT_EQ(Argument(request, "--effort"), "high");
    EXPECT_EQ(request.timeout, std::chrono::seconds(7));

    return absl::StatusOr<ProcessResult>(ProcessResult{0, Fixture("claude_success.json"), ""});
  };

  ClaudeCodeLLM client(Model("claude_code"), options);
  GenerationOptions overrides;
  overrides.timeout = 7;
  overrides.reasoning_effort = "high";

  EXPECT_TRUE(client.Generate("hello", overrides).ok());
}

TEST(CLIClientTest, LegacyBudgetDoesNotLimitEitherProvider) {
  for (const std::string provider : {"codex", "claude_code"}) {
    SCOPED_TRACE(provider);
    const auto config =
        Config::ParseYaml("llm: {models: [{provider: " + provider + ", max_budget_usd: 1, retries: 0}]}\n");
    ASSERT_TRUE(config.ok()) << config.status();

    CLIOptions options;
    options.runner = [&](const ProcessRequest& request) {
      EXPECT_EQ(Argument(request, "--max-budget-usd"), "<absent>");
      return absl::StatusOr<ProcessResult>(
          ProcessResult{0, Fixture(provider == "codex" ? "codex_success.jsonl" : "claude_success.json"), ""});
    };

    const auto client = CreateLLM(config->llm.models.front(), options);
    ASSERT_TRUE(client.ok()) << client.status();

    const auto result = (*client)->Generate("hello");
    ASSERT_TRUE(result.ok()) << result.status();

    EXPECT_FALSE(result->text.empty());
  }
}

TEST(CLIClientTest, RetriesTransientFailuresAndDiscardsPartialOutput) {
  int attempts = 0;
  CLIOptions options;
  options.runner = [&](const ProcessRequest&) -> absl::StatusOr<ProcessResult> {
    ++attempts;
    if (attempts == 1) return absl::DeadlineExceededError("timeout");
    if (attempts == 2) return ProcessResult{1, "partial", "private error"};

    return ProcessResult{0, Fixture("claude_success.json"), ""};
  };

  auto config = Model("claude_code");
  config.retries = 2;
  ClaudeCodeLLM client(config, options);

  auto result = client.Generate("hello");
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->text, "int Add(int a, int b) { return a + b; }");
  EXPECT_EQ(attempts, 3);
}

TEST(CLIClientTest, StopsAtRetryLimitAndDoesNotExposeOutput) {
  int attempts = 0;
  CLIOptions options;
  options.runner = [&](const ProcessRequest&) {
    ++attempts;
    return absl::StatusOr<ProcessResult>(ProcessResult{9, "private stdout", "private stderr"});
  };

  auto config = Model("codex");
  config.retries = 1;

  auto result = CodexCLILLM(config, options).Generate("private prompt");
  EXPECT_EQ(result.status().code(), absl::StatusCode::kUnavailable);

  EXPECT_EQ(attempts, 2);
  EXPECT_EQ(std::string(result.status().message()).find("private"), std::string::npos);
}

TEST(CLIClientTest, DoesNotRetryMissingExecutableOrMalformedProtocol) {
  for (bool missing : {true, false}) {
    int attempts = 0;
    CLIOptions options;
    options.runner = [&](const ProcessRequest&) -> absl::StatusOr<ProcessResult> {
      ++attempts;
      if (missing) return absl::NotFoundError("missing");

      return ProcessResult{0, "malformed", ""};
    };

    auto config = Model("claude_code");
    config.retries = 3;

    auto result = ClaudeCodeLLM(config, options).Generate("hello");
    EXPECT_EQ(result.status().code(), missing ? absl::StatusCode::kNotFound : absl::StatusCode::kDataLoss);

    EXPECT_EQ(attempts, 1);
  }
}

TEST(CLIClientTest, RejectsErrorOrMalformedClaudeResultEvenWithZeroExit) {
  for (const auto& output : {"{}", "[]", "null",
                             "{\"type\":\"result\",\"subtype\":\"error_max_budget_usd\",\"is_"
                             "error\":true,\"result\":\"private\"}",
                             "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
                             "\"result\":\" \"}"}) {
    CLIOptions options;
    options.runner = [&](const ProcessRequest&) { return absl::StatusOr<ProcessResult>(ProcessResult{0, output, ""}); };

    const auto result = ClaudeCodeLLM(Model("claude_code"), options).Generate("hello");
    EXPECT_FALSE(result.ok()) << output;

    EXPECT_EQ(std::string(result.status().message()).find("private"), std::string::npos);
  }
}

TEST(CLIClientTest, RejectsIncompleteOrFailedCodexTurns) {
  for (const auto& output : {"", "null\n", "{bad}\n", "{\"type\":\"turn.completed\"}\n",
                             "{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_message\","
                             "\"text\":\"partial\"}}\n",
                             "{\"type\":\"turn.failed\",\"error\":{\"message\":\"private\"}}\n"}) {
    CLIOptions options;
    options.runner = [&](const ProcessRequest&) { return absl::StatusOr<ProcessResult>(ProcessResult{0, output, ""}); };

    const auto result = CodexCLILLM(Model("codex"), options).Generate("hello");
    EXPECT_FALSE(result.ok()) << output;

    EXPECT_EQ(std::string(result.status().message()).find("private"), std::string::npos);
  }
}

TEST(CLIClientTest, RejectsInvalidUsageInsteadOfThrowing) {
  for (const auto& value : {Json(-1), Json("100"), Json(true), Json(std::numeric_limits<std::uint64_t>::max())}) {
    auto output = Json::parse(Fixture("claude_success.json"));
    output["usage"]["input_tokens"] = value;

    CLIOptions options;
    options.runner = [&](const ProcessRequest&) {
      return absl::StatusOr<ProcessResult>(ProcessResult{0, output.dump(), ""});
    };

    EXPECT_EQ(ClaudeCodeLLM(Model("claude_code"), options).Generate("hello").status().code(),
              absl::StatusCode::kDataLoss);
  }
}

TEST(CLIClientTest, RejectsUnsupportedProviderAndInvalidOptionsBeforeLaunch) {
  for (const auto& provider : {"", "openai", "copilot_cli", "codex_cli", "typo"}) {
    EXPECT_FALSE(CreateLLM(Model(provider)).ok());
  }

  int calls = 0;
  CLIOptions options;
  options.runner = [&](const ProcessRequest&) -> absl::StatusOr<ProcessResult> {
    ++calls;
    return absl::InternalError("must not launch");
  };

  for (int kind = 0; kind < 6; ++kind) {
    auto config = Model("claude_code");
    if (kind == 0) config.timeout = 0;
    if (kind == 1) config.retries = -1;
    if (kind == 2) config.retry_delay = -1;
    if (kind == 3) config.reasoning_effort = "invalid";
    if (kind == 4) config.manual_mode = true;
    if (kind == 5) config.allow_all_tools = true;

    EXPECT_FALSE(ClaudeCodeLLM(config, options).Generate("hello").ok());
  }

  ClaudeCodeLLM client(Model("claude_code"), options);
  EXPECT_FALSE(client.Generate(LLMRequest{}).ok());
  EXPECT_FALSE(client.GenerateWithContext("", {{"tool", "payload"}}).ok());
  EXPECT_FALSE(client.Generate(std::string("bad\xff", 4)).ok());

  EXPECT_EQ(calls, 0);
}

TEST(CLIClientTest, UsesCliDefaultsAndAllowsMissingUsage) {
  CLIOptions options;
  options.runner = [](const ProcessRequest& request) {
    EXPECT_EQ(Argument(request, "--model"), "<absent>");
    EXPECT_EQ(Argument(request, "--max-budget-usd"), "<absent>");

    return absl::StatusOr<ProcessResult>(
        ProcessResult{0, R"({"type":"result","subtype":"success","is_error":false,"result":"ok"})", ""});
  };

  auto config = Model("claude_code");
  config.name.reset();

  auto result = ClaudeCodeLLM(config, options).Generate("hello");
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->text, "ok");
  EXPECT_FALSE(result->usage);
}

TEST(CLIClientTest, RejectsCodexMessagesOutsideSuccessfulTurn) {
  const std::string message = R"({"type":"item.completed","item":{"type":"agent_message","text":"partial"}})";
  const std::string completion = R"({"type":"turn.completed"})";

  for (const auto& output : {Fixture("codex_success.jsonl") + "{\"type\":\"turn.started\"}\n" + message + "\n",
                             completion + "\n" + message + "\n", Fixture("codex_success.jsonl") + completion + "\n",
                             Fixture("codex_success.jsonl") + message + "\n"}) {
    CLIOptions options;
    options.runner = [&](const ProcessRequest&) { return absl::StatusOr<ProcessResult>(ProcessResult{0, output, ""}); };

    EXPECT_EQ(CodexCLILLM(Model("codex"), options).Generate("hello").status().code(), absl::StatusCode::kDataLoss);
  }
}

TEST(CLIClientTest, DoesNotRetryClaudeResourceLimitsWithAnyExitStatus) {
  for (const auto& subtype : {"error_max_turns", "error_max_budget_usd", "error_max_structured_output_retries"}) {
    for (int exit_code : {0, 1}) {
      int calls = 0;
      CLIOptions options;
      const auto output =
          Json{{"type", "result"}, {"subtype", subtype}, {"is_error", true}, {"result", "private"}}.dump();
      options.runner = [&](const ProcessRequest&) {
        ++calls;
        return absl::StatusOr<ProcessResult>(ProcessResult{exit_code, output, ""});
      };

      auto config = Model("claude_code");
      config.retries = 3;

      EXPECT_EQ(ClaudeCodeLLM(config, options).Generate("hello").status().code(), absl::StatusCode::kResourceExhausted);

      EXPECT_EQ(calls, 1) << subtype << " / " << exit_code;
    }
  }
}

TEST(CLIClientTest, DoesNotRetryCliUsageOrLaunchErrors) {
  for (int exit_code : {2, 126, 127}) {
    int calls = 0;
    CLIOptions options;
    options.runner = [&](const ProcessRequest&) {
      ++calls;
      return absl::StatusOr<ProcessResult>(ProcessResult{exit_code, "", "private"});
    };

    auto config = Model("codex");
    config.retries = 3;

    EXPECT_FALSE(CodexCLILLM(config, options).Generate("hello").ok());

    EXPECT_EQ(calls, 1) << exit_code;
  }
}

TEST(CLIClientTest, AcceptsUnspecifiedCodexPhaseButRejectsNonzeroSuccessEnvelope) {
  CLIOptions options;
  options.runner = [](const ProcessRequest&) {
    return absl::StatusOr<ProcessResult>(
        ProcessResult{0,
                      "{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_message\","
                      "\"phase\":null,\"text\":\"answer\"}}\n{\"type\":\"turn.completed\"}\n",
                      ""});
  };

  auto result = CodexCLILLM(Model("codex"), options).Generate("hello");
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(result->text, "answer");

  options.runner = [](const ProcessRequest&) {
    return absl::StatusOr<ProcessResult>(ProcessResult{1, Fixture("claude_success.json"), ""});
  };

  EXPECT_FALSE(ClaudeCodeLLM(Model("claude_code"), options).Generate("hello").ok());
}

TEST(CLIClientTest, ConfigPromptAndBothProvidersWorkWithRealFakeCliProcess) {
#ifdef _WIN32
  GTEST_SKIP() << "POSIX process integration";
#else
  auto config = Config::FromJson({{"llm",
                                   {{"retries", 0},
                                    {"models",
                                     {{{"provider", "claude_code"}, {"name", "claude-test"}},
                                      {{"provider", "codex"}, {"name", "codex-test"}}}}}}});
  ASSERT_TRUE(config.ok()) << config.status();

  auto prompt = PromptSampler(config->prompt, 42).BuildPrompt({});
  ASSERT_TRUE(prompt.ok()) << prompt.status();

  CLIOptions options;
  options.claude_executable = IEVOLVE_FAKE_CLI_PATH;
  options.codex_executable = IEVOLVE_FAKE_CLI_PATH;
  auto ensemble =
      LLMEnsemble::Create(config->llm.models, [&](const LLMModelConfig& model) { return CreateLLM(model, options); });
  ASSERT_TRUE(ensemble.ok()) << ensemble.status();

  LLMRequest request;
  request.system_message = prompt->system;
  request.messages = {{"user", prompt->user}};

  auto results = (*ensemble)->GenerateAll(request);
  ASSERT_TRUE(results.ok()) << results.status();

  ASSERT_EQ(results->size(), 2);
  EXPECT_EQ((*results)[0].text, prompt->user);
  EXPECT_EQ((*results)[1].text, prompt->user);
  EXPECT_EQ((*results)[0].provider, "claude_code");
  EXPECT_EQ((*results)[1].provider, "codex");
#endif
}
}  // namespace
}  // namespace ievolve
