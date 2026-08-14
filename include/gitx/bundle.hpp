#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace gitx {
class Bundle {
 public:
  static void create(const std::filesystem::path& launcher, const std::filesystem::path& output,
                     const std::filesystem::path& project, const std::string& entry,
                     const std::vector<std::filesystem::path>& runtime_paths);
  static bool extract_and_launch_if_present(const std::filesystem::path& executable);
};
}
