#include "gitx/command_names.hpp"

#include <array>

namespace gitx {
namespace {
constexpr std::array kBuiltins{
    "start", "open", "status", "save", "history", "explain", "review",
    "branch", "integrate", "sync", "stash", "tag", "config", "doctor", "bundle", "x"};
}

CommandNames::CommandNames(std::map<std::string, std::string> team_aliases,
                           std::map<std::string, std::string> user_aliases) {
  for (const auto command : kBuiltins) lookup_.emplace(command, command);
  for (const auto& [alias, command] : team_aliases) lookup_[alias] = command;
  for (const auto& [alias, command] : user_aliases) lookup_[alias] = command;
}

std::string CommandNames::canonicalize(const std::string& input) const {
  if (const auto item = lookup_.find(input); item != lookup_.end()) return item->second;
  return input;
}

std::vector<std::string> CommandNames::canonical_commands() const {
  return {kBuiltins.begin(), kBuiltins.end()};
}

}  // namespace gitx
