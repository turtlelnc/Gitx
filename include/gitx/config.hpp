#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gitx {

struct CommitPolicy {
  std::string pattern = "^(feat|fix|docs|refactor|test|chore)(\\([^)]+\\))?: .{1,72}$";
  std::string template_text = "type(scope): summary";
  bool require_issue = false;
};

struct BranchPolicy {
  std::string default_branch = "main";
  std::vector<std::string> allowed_prefixes{"feature/", "fix/", "docs/", "chore/"};
};

struct MergePolicy {
  std::vector<std::string> protected_targets{"main"};
  bool require_clean_worktree = true;
};

struct TeamConfig {
  CommitPolicy commit;
  BranchPolicy branch;
  MergePolicy merge;
  std::map<std::string, std::string> aliases;
};

class ConfigStore {
 public:
  static TeamConfig load_team(const std::filesystem::path& repository_root);
  static std::map<std::string, std::string> load_user_aliases();
  static bool write_default_team_config(const std::filesystem::path& repository_root);
  static std::filesystem::path user_config_path();
  static std::optional<std::string> validate_commit(const TeamConfig& config, const std::string& message);
  static std::optional<std::string> validate_branch(const TeamConfig& config, const std::string& branch);
};

}  // namespace gitx
