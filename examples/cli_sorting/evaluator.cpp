#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ievolve/process/process.h"
#include "nlohmann/json.hpp"

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
struct Rejected {
  std::string reason;
};

void Require(bool valid) {
  if (!valid) throw std::invalid_argument("Invalid protocol v1 request");
}
void Fields(const Json& object, std::initializer_list<const char*> keys) {
  Require(object.is_object() && object.size() == keys.size());
  for (const char* key : keys) Require(object.contains(key));
}
std::int64_t Limit(const Json& value, bool zero_allowed) {
  Require(value.is_number_integer());
  if (value.is_number_unsigned()) Require(value.get<std::uint64_t>() <= std::numeric_limits<std::int64_t>::max());
  const auto number = value.get<std::int64_t>();
  Require(number >= (zero_allowed ? 0 : 1));
  return number;
}

Json ReadRequest() {
  std::string text;
  char buffer[4096];
  while (std::cin) {
    std::cin.read(buffer, sizeof(buffer));
    text.append(buffer, static_cast<std::size_t>(std::cin.gcount()));
    Require(text.size() <= 1024 * 1024);
  }
  std::vector<std::set<std::string>> keys;
  auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
    using Event = Json::parse_event_t;
    if (event == Event::object_start || event == Event::array_start) Require(depth < 64);
    if (event == Event::object_start) keys.emplace_back();
    if (event == Event::object_end) keys.pop_back();
    if (event == Event::key) Require(keys.back().insert(value.get<std::string>()).second);
    return true;
  };
  return Json::parse(text, callback);
}

std::vector<std::vector<long long>> Cases() {
  std::vector<std::vector<long long>> result = {{}, {7}, {3, 1, 2}, {2, -1, 2, 0, -1}};
  std::vector<long long> ordered, reverse, random;
  for (int i = 0; i < 128; ++i) ordered.push_back(i);
  for (int i = 383; i >= 0; --i) reverse.push_back(i);
  std::uint32_t state = 42;
  for (int i = 0; i < 384; ++i) {
    state = state * 1664525u + 1013904223u;
    random.push_back(static_cast<long long>(state % 2001) - 1000);
  }
  result.push_back(std::move(ordered));
  result.push_back(std::move(reverse));
  result.push_back(std::move(random));
  return result;
}

Json Evaluate(const Json& request) {
  Fields(request, {"protocol_version", "candidate", "limits", "enable_artifacts"});
  Require(Limit(request.at("protocol_version"), false) == 1 && request.at("enable_artifacts").is_boolean());
  const auto& candidate = request.at("candidate");
  Fields(candidate, {"argv", "cwd"});
  Require(candidate.at("argv").is_array() && !candidate.at("argv").empty());
  for (const auto& arg : candidate.at("argv"))
    Require(arg.is_string() && arg.get<std::string>().find('\0') == std::string::npos);
  const auto argv = candidate.at("argv").get<std::vector<std::string>>();
  Require(!argv.front().empty() && candidate.at("cwd").is_string());
  const auto cwd = std::filesystem::path(candidate.at("cwd").get<std::string>());
  Require(cwd.is_absolute() && cwd.native().find('\0') == std::string::npos && std::filesystem::is_directory(cwd));
  Fields(request.at("limits"), {"evaluation_timeout_ms", "max_artifact_bytes"});
  const auto budget = Limit(request.at("limits").at("evaluation_timeout_ms"), false);
  const auto artifact_limit = Limit(request.at("limits").at("max_artifact_bytes"), true);
  const auto start = Clock::now();

  const auto run_case = [&](const std::vector<long long>& values) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    if (elapsed >= budget) throw Rejected{"evaluation_timeout"};
    ievolve::process::ProcessRequest process;
    process.argv = argv;
    process.working_directory = cwd;
    process.inherit_process_group = true;
    process.timeout = std::chrono::milliseconds(std::min<std::int64_t>(2000, budget - elapsed));
    process.max_output_bytes = 1024 * 1024;
    std::ostringstream input;
    input << values.size() << '\n';
    for (auto value : values) input << value << ' ';
    input << '\n';
    process.stdin_text = input.str();
    const auto before = Clock::now();
    auto run = ievolve::process::RunProcess(process);
    const double duration = std::chrono::duration<double, std::milli>(Clock::now() - before).count();
    if (!run.ok()) {
      if (absl::IsDeadlineExceeded(run.status())) throw Rejected{"candidate_timeout"};
      if (absl::IsResourceExhausted(run.status())) throw Rejected{"candidate_output_limit"};
      throw std::runtime_error(std::string(run.status().message()));
    }
    if (run->exit_code) throw Rejected{"candidate_exit"};
    std::istringstream output(run->stdout_text);
    std::vector<long long> actual;
    long long value;
    // Parse tokens separately so integer overflow cannot look like ordinary EOF.
    std::string token;
    while (output >> token) {
      std::size_t consumed = 0;
      try {
        value = std::stoll(token, &consumed);
      } catch (const std::exception&) {
        throw Rejected{"invalid_output"};
      }
      if (consumed != token.size()) throw Rejected{"invalid_output"};
      actual.push_back(value);
    }
    auto expected = values;
    std::sort(expected.begin(), expected.end());
    if (actual != expected) throw Rejected{"wrong_answer"};
    return duration;
  };

  Json response;
  try {
    const auto inputs = Cases();
    for (const auto& values : inputs) run_case(values);
    run_case(inputs.back());
    double runtime_ms = 0;
    for (int i = 0; i < 5; ++i) runtime_ms += run_case(inputs.back()) / 5;
    const double score = 1 / std::max(runtime_ms, 0.001);
    Require(std::isfinite(score));
    response = {{"protocol_version", 1},
                {"passed", true},
                {"metrics", {{"combined_score", score}, {"runtime_ms", runtime_ms}}}};
  } catch (const Rejected& error) {
    response = {{"protocol_version", 1}, {"passed", false}, {"reason", error.reason}, {"metrics", Json::object()}};
  }
  if (request.at("enable_artifacts").get<bool>()) {
    std::string summary = "Seven correctness cases, one warmup, five measured CLI invocations.";
    if (artifact_limit < static_cast<std::int64_t>(summary.size()))
      summary.resize(static_cast<std::size_t>(artifact_limit));
    response["artifacts"] = {{"summary", summary}};
  }
  return response;
}
}  // namespace

int main() {
  try {
    std::cout << Evaluate(ReadRequest()).dump() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Evaluator infrastructure error: " << error.what() << '\n';
    return 2;
  }
}
