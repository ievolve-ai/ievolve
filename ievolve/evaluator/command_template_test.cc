#include "ievolve/evaluator/command_template.h"

#include "gtest/gtest.h"

namespace ievolve::evaluator {
namespace {
TEST(CommandTemplateTest, PreservesLiteralArgumentsAndDoesNotReexpandPaths) {
  CommandContext ctx{"/tmp/base {source}", "/tmp/work space", "/tmp/input.cpp"};
  auto argv = ExpandCommand({"tool", "-I{base_dir}/include", "{source}", "{{source}}", "$(x);*", ""}, ctx);
  ASSERT_TRUE(argv.ok()) << argv.status();
  EXPECT_EQ(*argv, (std::vector<std::string>{"tool", "-I/tmp/base {source}/include", "/tmp/input.cpp", "{source}",
                                             "$(x);*", ""}));
}
TEST(CommandTemplateTest, ResolvesOnlyRelativeExecutablesWithDirectories) {
  CommandContext ctx{"/tmp/base", "/tmp/work space", std::nullopt};
  auto argv = ExpandCommand({"./bin/tool", "relative.txt", "{workspace}"}, ctx);
  ASSERT_TRUE(argv.ok());
  EXPECT_EQ(*argv, (std::vector<std::string>{"/tmp/base/bin/tool", "relative.txt", "/tmp/work space"}));
  EXPECT_EQ(ExpandCommand({"tool"}, ctx)->front(), "tool");
  EXPECT_EQ(ExpandCommand({"/bin/tool"}, ctx)->front(), "/bin/tool");
}
TEST(CommandTemplateTest, RejectsMalformedCommandsBeforeLaunch) {
  for (const auto& argv : std::vector<std::vector<std::string>>{{},
                                                                {""},
                                                                {"tool", "{unknown}"},
                                                                {"tool", "{"},
                                                                {"tool", "}"},
                                                                {"tool", "{source}"},
                                                                {"tool", std::string("a\0b", 3)}}) {
    EXPECT_FALSE(ValidateCommandTemplate(argv, false).ok());
  }
  EXPECT_TRUE(ValidateCommandTemplate({"tool", "{source}", "{{}}", ""}, true).ok());
}
}  // namespace
}  // namespace ievolve::evaluator
