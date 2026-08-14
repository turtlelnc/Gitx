// A minimal launcher that mirrors gitx's main(): if this executable has a
// bundle appended, extract it to a temp dir and run the entry, then exit.
// Used by bundle_tests to exercise the real SFX code path.
#include "gitx/bundle.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

int main(int argc, char** argv) {
  // When the test invokes this binary as the bundle *entry*, it sets
  // GITX_BUNDLE_TEST_MARKER: write the marker and exit so the round trip can
  // be asserted without shell scripts (works on Windows too).
  if (const char* marker = std::getenv("GITX_BUNDLE_TEST_MARKER")) {
    std::ofstream out(marker);
    out << "bundle-ran\n";
    return 0;
  }
  (void)argc;
  gitx::Bundle::extract_and_launch_if_present(std::filesystem::absolute(argv[0]));
  return 0;
}
