#include "gitx/command_names.hpp"

#include <array>

namespace gitx {

CommandNames::CommandNames(std::map<std::string, std::string> team_aliases,
                           std::map<std::string, std::string> user_aliases) {
  static constexpr std::array commands{
      "start", "open", "status", "save", "history", "branch", "integrate", "sync", "stash", "tag", "config"};
  for (const auto command : commands) lookup_.emplace(command, command);
  for (const auto& [alias, command] : team_aliases) lookup_[alias] = command;
  for (const auto& [alias, command] : user_aliases) lookup_[alias] = command;
}

std::string CommandNames::canonicalize(const std::string& input) const {
  if (const auto item = lookup_.find(input); item != lookup_.end()) return item->second;
  return input;
}

std::vector<std::string> CommandNames::canonical_commands() const {
  return {"start", "open", "status", "save", "history", "branch", "integrate", "sync", "stash", "tag", "config"};
}

}  // namespace gitx
