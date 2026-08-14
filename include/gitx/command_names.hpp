#pragma once

#include <map>
#include <string>
#include <vector>

namespace gitx {

class CommandNames {
 public:
  CommandNames(std::map<std::string, std::string> team_aliases,
               std::map<std::string, std::string> user_aliases);

  std::string canonicalize(const std::string& input) const;
  std::vector<std::string> canonical_commands() const;

 private:
  std::map<std::string, std::string> lookup_;
};

}  // namespace gitx
