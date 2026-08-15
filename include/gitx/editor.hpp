#pragma once

#include <filesystem>
#include <string>

namespace gitx {
namespace editor {

// Interactive line-based text editor for the given file. Returns true on
// successful save, false when the user quit without saving.
// Throws std::runtime_error with a Chinese message on I/O failure.
bool edit_file(const std::filesystem::path& path);

}  // namespace editor
}  // namespace gitx
