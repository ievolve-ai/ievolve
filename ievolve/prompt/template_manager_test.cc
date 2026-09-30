#include "ievolve/prompt/template_manager.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

#include "gtest/gtest.h"

namespace ievolve {
namespace {

TEST(TemplateManagerTest, FormatsNamedFieldsAndPreservesInsertedBraces) {
  auto result = TemplateManager::Format("{{\"score\": {score:.4f}}} {code}", {{"score", 0.5}, {"code", "{score}"}});
  ASSERT_TRUE(result.ok()) << result.status();

  EXPECT_EQ(*result, "{\"score\": 0.5000} {score}");
}

TEST(TemplateManagerTest, RejectsMissingFieldsAndMalformedFormats) {
  EXPECT_EQ(TemplateManager::Format("{missing}", Metrics::object()).status().code(), absl::StatusCode::kNotFound);

  for (const auto* text : {"{", "}", "{v:.100f}", "{v!r}", "{v.x}"}) {
    EXPECT_FALSE(TemplateManager::Format(text, {{"v", 1}}).ok()) << text;
  }

  EXPECT_FALSE(TemplateManager::Format("{v:.4f}", {{"v", "text"}}).ok());
}

TEST(TemplateManagerTest, FormatsNonfiniteValuesAsPythonText) {
  auto result =
      TemplateManager::Format("{nan} {positive} {negative}", {{"nan", std::numeric_limits<double>::quiet_NaN()},
                                                              {"positive", std::numeric_limits<double>::infinity()},
                                                              {"negative", -std::numeric_limits<double>::infinity()}});
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(*result, "nan inf -inf");
}

TEST(TemplateManagerTest, DefaultsAndFragmentDiagnosticsAreAvailable) {
  TemplateManager manager;

  ASSERT_TRUE(manager.GetTemplate("diff_user").ok());
  EXPECT_NE(manager.GetTemplate("diff_user")->find("<<<<<<< SEARCH"), std::string::npos);

  EXPECT_EQ(manager.GetFragment("fitness_improved", {{"prev", 0.2}, {"current", 0.5}}),
            "Fitness improved: 0.2000 → 0.5000");

  EXPECT_EQ(manager.GetFragment("missing"), "[Missing fragment: missing]");
  EXPECT_EQ(manager.GetFragment("fitness_stable"), "[Fragment formatting error: 'current']");
  EXPECT_EQ(manager.GetTemplate("missing").status().code(), absl::StatusCode::kNotFound);
}

class DirectoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("ievolve_templates_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory_);
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }
  std::filesystem::path directory_;
};

TEST_F(DirectoryTest, CustomOverridesCascadeAndMissingDirectoryFallsBack) {
  std::ofstream(directory_ / "diff_user.txt") << "custom {current_program}";
  std::ofstream(directory_ / "fragments.json") << R"({"artifact_title":"Output"})";

  TemplateManager manager;
  ASSERT_TRUE(manager.LoadDirectory(directory_).ok());

  EXPECT_EQ(*manager.GetTemplate("diff_user"), "custom {current_program}");
  EXPECT_TRUE(manager.HasTemplate("full_rewrite_user"));
  EXPECT_EQ(manager.GetFragment("artifact_title"), "Output");
  EXPECT_EQ(manager.GetFragment("inspiration_type_diverse"), "Diverse");

  EXPECT_TRUE(manager.LoadDirectory(directory_ / "missing").ok());
}

TEST_F(DirectoryTest, InvalidFragmentsDoNotPartiallyOverwriteTemplates) {
  TemplateManager manager;
  const auto original = manager.GetTemplate("diff_user");
  ASSERT_TRUE(original.ok()) << original.status();

  std::ofstream(directory_ / "diff_user.txt") << "changed";
  for (const auto* json : {"{broken", "[]", R"({"bad":42})"}) {
    std::ofstream(directory_ / "fragments.json") << json;

    EXPECT_FALSE(manager.LoadDirectory(directory_).ok());
    EXPECT_EQ(*manager.GetTemplate("diff_user"), *original);
  }

  EXPECT_FALSE(manager.LoadDirectory(directory_ / "diff_user.txt").ok());
}

TEST_F(DirectoryTest, RejectsDirectoryNamedAsTemplateWithoutOverwritingDefaults) {
  TemplateManager manager;
  const auto original = manager.GetTemplate("diff_user");
  ASSERT_TRUE(original.ok());

  std::filesystem::create_directory(directory_ / "diff_user.txt");

  EXPECT_FALSE(manager.LoadDirectory(directory_).ok());
  EXPECT_EQ(*manager.GetTemplate("diff_user"), *original);
}

}  // namespace
}  // namespace ievolve
