#include "gitx/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace gitx {
namespace {

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string unquote(std::string value) {
  value = trim(std::move(value));
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') return value;
  value = value.substr(1, value.size() - 2);
  // Decode TOML basic-string escapes so patterns written with \"\\(...\"\" load
  // back as the intended single backslashes (e.g. commit.pattern).
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char current = value[index];
    if (current != '\\' || index + 1 >= value.size()) {
      decoded.push_back(current);
      continue;
    }
    const char escaped = value[++index];
    switch (escaped) {
      case 'n': decoded.push_back('\n'); break;
      case 't': decoded.push_back('\t'); break;
      case 'r': decoded.push_back('\r'); break;
      case 'b': decoded.push_back('\b'); break;
      case 'f': decoded.push_back('\f'); break;
      case '"': decoded.push_back('"'); break;
      case '\\': decoded.push_back('\\'); break;
      default: decoded.push_back(escaped); break;
    }
  }
  return decoded;
}

bool parse_bool(const std::string& value, bool fallback) {
  const auto normalized = trim(value);
  if (normalized == "true") return true;
  if (normalized == "false") return false;
  return fallback;
}

std::vector<std::string> parse_array(const std::string& value) {
  std::string contents = trim(value);
  if (contents.size() < 2 || contents.front() != '[' || contents.back() != ']') return {};
  contents = contents.substr(1, contents.size() - 2);
  std::vector<std::string> values;
  std::stringstream stream(contents);
  std::string item;
  while (std::getline(stream, item, ',')) {
    item = unquote(item);
    if (!item.empty()) values.push_back(item);
  }
  return values;
}

void load_toml(const fs::path& path, TeamConfig& config, std::map<std::string, std::string>* aliases) {
  std::ifstream input(path);
  if (!input) return;

  std::string section;
  std::string line;
  while (std::getline(input, line)) {
    const auto comment = line.find('#');
    if (comment != std::string::npos) line.erase(comment);
    line = trim(line);
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    const auto equals = line.find('=');
    if (equals == std::string::npos) continue;
    const auto key = trim(line.substr(0, equals));
    const auto value = trim(line.substr(equals + 1));

    if (section == "commit") {
      if (key == "pattern") config.commit.pattern = unquote(value);
      if (key == "template") config.commit.template_text = unquote(value);
      if (key == "require_issue") config.commit.require_issue = parse_bool(value, config.commit.require_issue);
    } else if (section == "branch") {
      if (key == "default") config.branch.default_branch = unquote(value);
      if (key == "allowed_prefixes") config.branch.allowed_prefixes = parse_array(value);
    } else if (section == "merge") {
      if (key == "protected_targets") config.merge.protected_targets = parse_array(value);
      if (key == "require_clean_worktree") config.merge.require_clean_worktree = parse_bool(value, config.merge.require_clean_worktree);
    } else if (section == "aliases" && aliases != nullptr) {
      (*aliases)[unquote(value)] = key;
    }
  }
}

}  // namespace

TeamConfig ConfigStore::load_team(const fs::path& repository_root) {
  TeamConfig config;
  load_toml(repository_root / ".gitx" / "config.toml", config, &config.aliases);
  return config;
}

std::map<std::string, std::string> ConfigStore::load_user_aliases() {
  TeamConfig ignored;
  std::map<std::string, std::string> aliases;
  load_toml(user_config_path(), ignored, &aliases);
  return aliases;
}

fs::path ConfigStore::user_config_path() {
#ifdef _WIN32
  if (const auto* app_data = std::getenv("APPDATA")) return fs::path(app_data) / "gitx" / "config.toml";
#else
  if (const auto* config_home = std::getenv("XDG_CONFIG_HOME")) return fs::path(config_home) / "gitx" / "config.toml";
  if (const auto* home_dir = std::getenv("HOME")) return fs::path(home_dir) / ".config" / "gitx" / "config.toml";
#endif
  return fs::current_path() / ".gitx-user.toml";
}

bool ConfigStore::write_default_team_config(const fs::path& repository_root) {
  const auto directory = repository_root / ".gitx";
  const auto path = directory / "config.toml";
  if (fs::exists(path)) return false;
  fs::create_directories(directory);
  std::ofstream output(path);
  if (!output) throw std::runtime_error("无法创建团队配置文件: " + path.string());
  output << R"cfg(# gitx 团队规范（该文件应提交到仓库）
[commit]
template = "type(scope): summary"
pattern = "^(feat|fix|docs|refactor|test|chore)(\\([^)]+\\))?: .{1,72}$"
require_issue = false

[branch]
default = "main"
allowed_prefixes = ["feature/", "fix/", "docs/", "chore/"]

[merge]
protected_targets = ["main"]
require_clean_worktree = true

[aliases]
# "保存" = "save"
)cfg";
  return true;
}

std::optional<std::string> ConfigStore::validate_commit(const TeamConfig& config, const std::string& message) {
  try {
    if (!std::regex_match(message, std::regex(config.commit.pattern))) {
      return "提交信息不符合团队规则。模板：" + config.commit.template_text;
    }
  } catch (const std::regex_error&) {
    return "团队配置中的 commit.pattern 不是有效正则表达式。";
  }
  if (config.commit.require_issue && message.find('#') == std::string::npos) {
    return "提交信息必须包含议题编号（例如 #123）。";
  }
  return std::nullopt;
}

std::optional<std::string> ConfigStore::validate_branch(const TeamConfig& config, const std::string& branch) {
  if (branch == config.branch.default_branch) return std::nullopt;
  const auto matches = std::any_of(config.branch.allowed_prefixes.begin(), config.branch.allowed_prefixes.end(),
      [&branch](const std::string& prefix) { return branch.starts_with(prefix); });
  if (!matches) return "分支名称必须以以下前缀之一开始：feature/、fix/、docs/、chore/。";
  return std::nullopt;
}

}  // namespace gitx
