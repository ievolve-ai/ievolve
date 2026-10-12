#include "ievolve/controller/staging_directory.h"

#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace ievolve::controller {
namespace fs = std::filesystem;

namespace {
constexpr int kMaxNameAttempts = 100;
}  // namespace

StagingDirectory::StagingDirectory(const fs::path& parent) {
  fs::create_directories(parent);
  std::random_device random;
  for (int attempt = 0; attempt < kMaxNameAttempts; ++attempt) {
    path_ = parent / (".controller-" + std::to_string(random()) + "-" + std::to_string(random()));
    if (fs::create_directory(path_)) return;
  }

  throw std::runtime_error("Cannot allocate controller staging directory");
}

StagingDirectory::~StagingDirectory() {
  if (path_.empty()) return;

  std::error_code error;
  fs::remove_all(path_, error);
}

void StagingDirectory::Publish(const fs::path& destination) {
  fs::rename(path_, destination);
  // The destructor must not delete what has already been handed over.
  path_.clear();
}

void StagingDirectory::PublishContents(const fs::path& destination) {
  fs::create_directories(destination);
  // Listed first: replaced directories are moved into the staging directory.
  std::vector<fs::path> entries;
  for (const auto& entry : fs::directory_iterator(path_)) entries.push_back(entry.path());

  for (const auto& entry : entries) {
    const auto target = destination / entry.filename();
    // rename cannot replace a nonempty directory, so the old one is moved
    // aside into the staging directory, which the destructor removes.
    if (fs::is_directory(fs::symlink_status(target)))
      fs::rename(target, path_ / (".replaced-" + entry.filename().string()));
    fs::rename(entry, target);
  }
}

}  // namespace ievolve::controller
