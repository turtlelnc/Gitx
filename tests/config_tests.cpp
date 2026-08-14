#include "gitx/command_names.hpp"
#include "gitx/config.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
  const auto root = std::filesystem::temp_directory_path() / "gitx-config-test";
  std::filesystem::remove_all(root);
  assert(gitx::ConfigStore::write_default_team_config(root));
  const auto config = gitx::ConfigStore::load_team(root);
  assert(!gitx::ConfigStore::validate_commit(config, "feat(cli): add status").has_value());
  assert(gitx::ConfigStore::validate_commit(config, "bad message").has_value());
  assert(!gitx::ConfigStore::validate_branch(config, "feature/login").has_value());
  assert(gitx::ConfigStore::validate_branch(config, "random").has_value());

  gitx::CommandNames names({{"保存", "save"}}, {{"提交", "save"}});
  assert(names.canonicalize("保存") == "save");
  assert(names.canonicalize("提交") == "save");
  assert(names.canonicalize("status") == "status");
  std::filesystem::remove_all(root);
}
