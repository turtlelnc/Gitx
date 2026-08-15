#pragma once

#include "gitx/git_repository.hpp"
#include "gitx/config.hpp"

namespace gitx {
namespace tui {

// Full-screen interactive status/stage/commit interface.
// Returns 0 on clean exit, non-zero on error.
int run(GitRepository& repository, const TeamConfig& config);

}  // namespace tui
}  // namespace gitx
