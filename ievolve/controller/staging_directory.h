#ifndef IEVOLVE_CONTROLLER_STAGING_DIRECTORY_H_
#define IEVOLVE_CONTROLLER_STAGING_DIRECTORY_H_

#include <filesystem>

namespace ievolve::controller {

// A private, uniquely named directory under parent where output is prepared
// before being published by rename. It resides on the destination filesystem,
// so renames out of it never copy. Removed on destruction unless published.
// Filesystem failures throw std::filesystem::filesystem_error.
class StagingDirectory {
 public:
  explicit StagingDirectory(const std::filesystem::path& parent);
  ~StagingDirectory();
  StagingDirectory(const StagingDirectory&) = delete;
  StagingDirectory& operator=(const StagingDirectory&) = delete;

  const std::filesystem::path& path() const { return path_; }

  // Renames the whole directory to destination in one step, so a reader never
  // observes it half-written. Ownership passes to destination.
  void Publish(const std::filesystem::path& destination);
  // Moves each entry into destination, creating it if needed and replacing
  // same-named entries. Atomic per entry only, not as a whole.
  void PublishContents(const std::filesystem::path& destination);

 private:
  std::filesystem::path path_;
};

}  // namespace ievolve::controller
#endif  // IEVOLVE_CONTROLLER_STAGING_DIRECTORY_H_
