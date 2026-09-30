#include <iostream>
#include <iterator>
#include <string>

#include "nlohmann/json.hpp"

// Deterministic local subprocess used only by integration tests.
int main(int argc, char** argv) {
  const std::string input{std::istreambuf_iterator<char>(std::cin), {}};
  using Json = nlohmann::json;

  if (argc > 1 && std::string(argv[1]) == "exec") {
    std::cout << Json{{"type", "item.completed"}, {"item", {{"type", "agent_message"}, {"text", input}}}}.dump()
              << '\n';
    std::cout << Json{{"type", "turn.completed"}}.dump() << '\n';
  } else {
    std::cout << Json{{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", input}}.dump()
              << '\n';
  }
}
