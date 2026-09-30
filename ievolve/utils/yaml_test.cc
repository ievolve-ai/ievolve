#include "ievolve/utils/yaml.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "gtest/gtest.h"

namespace ievolve::utils {
namespace {
using Json = nlohmann::ordered_json;

TEST(YamlTest, ParsesRootsWithoutApplyingConfigurationSchema) {
  struct Case {
    std::string_view yaml;
    Json expected;
  };

  for (const auto& test : std::vector<Case>{{"{unknown: [1, null, true]}", {{"unknown", {1, nullptr, true}}}},
                                            {"[hello, 2]", Json::array({"hello", 2})},
                                            {"{}", Json::object()},
                                            {"[]", Json::array()},
                                            {"null", nullptr},
                                            {"false", false},
                                            {"42", 42},
                                            {"1.5", 1.5},
                                            {"hello", "hello"}}) {
    SCOPED_TRACE(test.yaml);

    const auto parsed = ParseYaml(test.yaml);
    ASSERT_TRUE(parsed.ok()) << parsed.status();

    EXPECT_EQ(*parsed, test.expected);
    EXPECT_EQ(parsed->type(), test.expected.type());
  }
}

TEST(YamlTest, MatchesPythonImplicitScalarTypes) {
  const auto parsed = ParseYaml(
      "[YES, off, ~, 010, 0b1010, 0x10, 1_000, 1:30, 1:30.5, "
      "1.0e-8, 08, 1e3, TrUe, 'true', \"00123\"]");
  ASSERT_TRUE(parsed.ok()) << parsed.status();

  const auto expected =
      Json::array({true, false, nullptr, 8, 10, 16, 1000, 90, 90.5, 1e-8, "08", "1e3", "TrUe", "true", "00123"});
  EXPECT_EQ(*parsed, expected);
  ASSERT_EQ(parsed->size(), expected.size());

  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ((*parsed)[i].type(), expected[i].type()) << "index " << i;
  }
}

TEST(YamlTest, HonorsExplicitScalarTags) {
  const auto parsed = ParseYaml("[!!str null, !!str 123, !!float 1, !!int '12', !!bool OFF, !!null '']");
  ASSERT_TRUE(parsed.ok()) << parsed.status();

  EXPECT_EQ(*parsed, Json::array({"null", "123", 1.0, 12, false, nullptr}));
  EXPECT_TRUE(parsed->at(2).is_number_float());
  EXPECT_EQ(parsed->at(3).type(), Json::value_t::number_integer);
}

TEST(YamlTest, PreservesSignedIntegerBoundaries) {
  const auto parsed = ParseYaml("[-9223372036854775808, 9223372036854775807]");
  ASSERT_TRUE(parsed.ok()) << parsed.status();

  EXPECT_EQ(*parsed, Json::array({std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}));

  for (const auto* yaml :
       {"-9223372036854775809", "9223372036854775808", "0x8000000000000000", "!!int 99999999999999999999"}) {
    SCOPED_TRACE(yaml);
    EXPECT_EQ(ParseYaml(yaml).status().code(), absl::StatusCode::kInvalidArgument);
  }
}

TEST(YamlTest, PreservesMappingDeclarationOrder) {
  const auto parsed = ParseYaml("z: {second: 2, first: 1}\na: last\n");
  ASSERT_TRUE(parsed.ok()) << parsed.status();

  EXPECT_EQ(parsed->dump(), R"({"z":{"second":2,"first":1},"a":"last"})");
}

TEST(YamlTest, PreservesMergeOrderWithExplicitAndFirstSourcePrecedence) {
  const auto parsed = ParseYaml(R"yaml(
first: &first {z: 1, a: first}
second: &second {z: 2, suffix: last}
merged:
  a: explicit
  <<: [*first, *second]
  tail: final
)yaml");
  ASSERT_TRUE(parsed.ok()) << parsed.status();

  EXPECT_EQ(parsed->at("merged").dump(), R"({"z":1,"suffix":"last","a":"explicit","tail":"final"})");
}

TEST(YamlTest, TreatsQuotedAndStringTaggedMergeKeysAsLiteralKeys) {
  for (const auto* yaml : {"{'<<': literal}", "{!!str <<: literal}"}) {
    SCOPED_TRACE(yaml);

    const auto parsed = ParseYaml(yaml);
    ASSERT_TRUE(parsed.ok()) << parsed.status();

    EXPECT_EQ(*parsed, Json({{"<<", "literal"}}));
  }
}

TEST(YamlTest, RequiresExactlyOneDocument) {
  for (const auto* yaml : {"", "# comment only\n", "{}\n---\n{}"}) {
    SCOPED_TRACE(yaml);
    EXPECT_EQ(ParseYaml(yaml).status().code(), absl::StatusCode::kInvalidArgument);
  }

  const auto parsed = ParseYaml("---\nnull\n...\n");
  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_TRUE(parsed->is_null());
}

TEST(YamlTest, RejectsDuplicateOrNonStringMappingKeys) {
  for (const auto* yaml :
       {"{a: 1, a: 2}", "{nested: {a: 1, a: 2}}", "{1: value}", "{true: value}", "{null: value}", "{[a, b]: value}"}) {
    SCOPED_TRACE(yaml);
    EXPECT_EQ(ParseYaml(yaml).status().code(), absl::StatusCode::kInvalidArgument);
  }

  const auto quoted = ParseYaml("{'1': one, 'true': boolean, 'null': empty}");
  ASSERT_TRUE(quoted.ok()) << quoted.status();

  EXPECT_EQ(*quoted, Json({{"1", "one"}, {"true", "boolean"}, {"null", "empty"}}));
}

TEST(YamlTest, RejectsInvalidTagsAndMerges) {
  for (const auto* yaml : {"!!bool maybe", "!!int true", "!!float text", "!custom value", "!!null []", "!!str {}",
                           "{<<: 12}", "{<<: [{a: 1}, 2]}"}) {
    SCOPED_TRACE(yaml);
    EXPECT_EQ(ParseYaml(yaml).status().code(), absl::StatusCode::kInvalidArgument);
  }
}

TEST(YamlTest, ReportsSyntaxLocationWithoutEchoingInputValues) {
  const auto parsed = ParseYaml("items: [secret-value, *private-anchor]\n");
  ASSERT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);

  const std::string message(parsed.status().message());
  EXPECT_NE(message.find("line 1"), std::string::npos);
  EXPECT_NE(message.find("column"), std::string::npos);
  EXPECT_EQ(message.find("secret-value"), std::string::npos);
  EXPECT_EQ(message.find("private-anchor"), std::string::npos);
}

TEST(YamlTest, RejectsDocumentsBeyondTheByteLimit) {
  // Pad a comment so the document is valid at exactly 8 MiB.
  std::string yaml = "#" + std::string(8 * 1024 * 1024 - 4, 'a') + "\n{}";

  const auto parsed = ParseYaml(yaml);
  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(*parsed, Json::object());

  yaml += '\n';
  EXPECT_EQ(ParseYaml(yaml).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(YamlTest, RejectsExcessiveNesting) {
  const auto at_limit = ParseYaml(std::string(64, '[') + "0" + std::string(64, ']'));
  ASSERT_TRUE(at_limit.ok()) << at_limit.status();

  EXPECT_EQ(ParseYaml(std::string(65, '[') + "0" + std::string(65, ']')).status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(YamlTest, RejectsRecursiveAliases) {
  for (const auto* yaml : {"&loop [*loop]", "&loop {self: *loop}", "&loop {<<: *loop}"}) {
    SCOPED_TRACE(yaml);
    EXPECT_EQ(ParseYaml(yaml).status().code(), absl::StatusCode::kInvalidArgument);
  }
}

TEST(YamlTest, LimitsExpandedAliasNodes) {
  // This small, shallow document expands beyond 100,000 nodes.
  std::string yaml = "a0: &a0 [0, 0]\n";
  for (int i = 1; i <= 16; ++i) {
    const auto name = "a" + std::to_string(i);
    const auto previous = "*a" + std::to_string(i - 1);
    yaml += name + ": &" + name + " [" + previous + ", " + previous + "]\n";
  }

  EXPECT_EQ(ParseYaml(yaml).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(YamlTest, EmitsAllJsonRootTypesWithoutLosingScalarTypes) {
  for (const auto& input : std::vector<Json>{nullptr, true, -42, std::uint64_t{42}, 1.0, 1e-8, 1.2345678901234567,
                                             std::numeric_limits<double>::min(), std::numeric_limits<double>::max(),
                                             "null", "true", "00123", "first\nsecond\n世界", Json::array(),
                                             Json::object(), Json::array({1, "two", nullptr})}) {
    SCOPED_TRACE(input.dump());

    const auto yaml = EmitYaml(input);
    ASSERT_TRUE(yaml.ok()) << yaml.status();
    ASSERT_FALSE(yaml->empty());
    EXPECT_EQ(yaml->back(), '\n');

    const auto parsed = ParseYaml(*yaml);
    ASSERT_TRUE(parsed.ok()) << parsed.status();

    EXPECT_EQ(*parsed, input);
    // YAML integers are decoded as signed int64, including positive ones.
    EXPECT_EQ(parsed->type(), input.is_number_unsigned() ? Json::value_t::number_integer : input.type());
  }
}

TEST(YamlTest, EmitsMappingsWithOrderAndStringKeysPreserved) {
  const Json input = {{"z", {{"second", 2}, {"first", 1}}}, {"true", "off"}, {"<<", "literal"}, {"12", "08"}};

  const auto yaml = EmitYaml(input);
  ASSERT_TRUE(yaml.ok()) << yaml.status();

  const auto parsed = ParseYaml(*yaml);
  ASSERT_TRUE(parsed.ok()) << parsed.status();

  EXPECT_EQ(parsed->dump(), R"({"z":{"second":2,"first":1},"true":"off","<<":"literal","12":"08"})");
}

class YamlFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("ievolve_yaml_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    owns_dir_ = std::filesystem::create_directory(dir_);
    ASSERT_TRUE(owns_dir_);
  }

  void TearDown() override {
    if (!owns_dir_) return;

    std::error_code error;
    std::filesystem::remove_all(dir_, error);
    EXPECT_FALSE(error) << error.message();
  }

  std::filesystem::path dir_;
  bool owns_dir_ = false;
};

TEST_F(YamlFileTest, WritesOnlyTheSuppliedViewAndTruncatesExistingContents) {
  const auto path = dir_ / "document with spaces.yaml";
  const std::string text = "items: [1, 2]\nignored suffix";
  const std::string_view yaml(text.data(), text.find("ignored"));

  ASSERT_TRUE(WriteYaml(yaml, path).ok());

  {
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), yaml);
  }

  const auto parsed = ReadYaml(path);
  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(*parsed, Json({{"items", {1, 2}}}));

  ASSERT_TRUE(WriteYaml("null\n", path).ok());
  EXPECT_EQ(std::filesystem::file_size(path), 5u);

  const auto replaced = ReadYaml(path);
  ASSERT_TRUE(replaced.ok()) << replaced.status();
  EXPECT_TRUE(replaced->is_null());
}

TEST_F(YamlFileTest, ReadsEmittedYamlWithoutLosingOrderOrTypes) {
  const Json input = {{"z", "00123"}, {"a", 1e-8}, {"values", {1, true, nullptr}}};

  const auto yaml = EmitYaml(input);
  ASSERT_TRUE(yaml.ok()) << yaml.status();

  const auto path = dir_ / "emitted.yaml";
  ASSERT_TRUE(WriteYaml(*yaml, path).ok());

  const auto parsed = ReadYaml(path);
  ASSERT_TRUE(parsed.ok()) << parsed.status();

  EXPECT_EQ(*parsed, input);
  EXPECT_TRUE(parsed->at("a").is_number_float());
}

TEST_F(YamlFileTest, ReportsMissingFilesAndNonRegularInputs) {
  EXPECT_EQ(ReadYaml(dir_ / "absent.yaml").status().code(), absl::StatusCode::kNotFound);
  EXPECT_EQ(ReadYaml(dir_).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(YamlFileTest, ReportsOutputOpenFailures) {
  EXPECT_EQ(WriteYaml("{}", dir_).code(), absl::StatusCode::kPermissionDenied);
  EXPECT_EQ(WriteYaml("{}", dir_ / "absent" / "file.yaml").code(), absl::StatusCode::kPermissionDenied);
}

TEST_F(YamlFileTest, PropagatesYamlErrorsFromFiles) {
  const auto path = dir_ / "invalid.yaml";
  ASSERT_TRUE(WriteYaml("items: [", path).ok());

  EXPECT_EQ(ReadYaml(path).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(YamlFileTest, RejectsOversizedFiles) {
  const auto path = dir_ / "large.yaml";

  {
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(file);

    // Keep the oversized document valid so a syntax error cannot satisfy the
    // rejection assertion if the input-size checks are accidentally removed.
    file << '#' << std::string(8 * 1024 * 1024, 'a') << "\n{}";
    file.close();
    ASSERT_TRUE(file);
  }

  EXPECT_EQ(ReadYaml(path).status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace ievolve::utils
