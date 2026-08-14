#pragma once

// Version of gitx itself, injected by CMake at configure time.
#ifndef GITX_VERSION
#define GITX_VERSION "0.0.0-dev"
#endif

namespace gitx {
inline constexpr const char* kVersion = GITX_VERSION;
}
