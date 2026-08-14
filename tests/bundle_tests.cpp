#include "gitx/bundle.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using gitx::Bundle;

namespace {

fs::path make_temp_dir(const std::string& name) {
  const auto path = fs::temp_directory_path() / name;
  fs::remove_all(path);
  fs::create_directories(path);
  return path;
}

void write_file(const fs::path& file, const std::string& content) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << content;
}

void test_bundle_round_trip() {
  // The real gitx binary as launcher would be ideal, but tests must be
  // hermetic; bundle_launcher mirrors gitx's main() SFX branch.
  const auto launcher = fs::path(BUNDLE_LAUNCHER);
  assert(fs::is_regular_file(launcher));

  const auto root = make_temp_dir("gitx-test-bundle");

  // Project dir with a couple of source files (mimics the gitx repo layout).
  const auto project = root / "project";
  fs::create_directories(project / ".git");
  fs::create_directories(project / "src");
  write_file(project / ".git" / "HEAD", "ref: refs/heads/main\n");
  write_file(project / "src" / "main.c", "int main(void) { return 0; }\n");
  write_file(project / "README.md", "# project\n");

  // Runtime dir: the entry is the launcher binary itself (a native executable
  // on every platform). When the SFX runs it, the env var makes it write a
  // marker so we can assert the round trip completed.
  const auto runtime = root / "dist";
  fs::create_directories(runtime);
  const auto entry = runtime / ("entry" + launcher.extension().string());
  fs::copy_file(launcher, entry, fs::copy_options::overwrite_existing);
  write_file(runtime / "data.txt", "payload\n");

  const auto output = root / ("out-bundle" + launcher.extension().string());
  Bundle::create(launcher, output, project, "dist/" + entry.filename().string(), {runtime});

  assert(fs::is_regular_file(output));
  const auto size = fs::file_size(output);
  assert(size > fs::file_size(launcher));  // appended payload

  // Run the bundle; the extracted entry writes the marker.
  const auto marker = root / "marker.txt";
  const auto command = "\"" + output.string() + "\"";
#ifdef _WIN32
  const auto set_env = "set GITX_BUNDLE_TEST_MARKER=" + marker.string() + "&& ";
#else
  const auto set_env = "GITX_BUNDLE_TEST_MARKER='" + marker.string() + "' ";
#endif
  const auto exit_code = std::system((set_env + command).c_str());
  assert(exit_code == 0);
  assert(fs::is_regular_file(marker));  // the entry actually ran

  fs::remove_all(root);
}

void test_bundle_not_present_returns_false() {
  const auto launcher = fs::path(BUNDLE_LAUNCHER);
  // Plain launcher has no appended bundle; extraction must decline.
  assert(!Bundle::extract_and_launch_if_present(launcher));
}

}  // namespace

int main() {
  test_bundle_round_trip();
  std::cout << "ok: bundle create/extract/launch round trip\n";
  test_bundle_not_present_returns_false();
  std::cout << "ok: plain executable not treated as bundle\n";
  return 0;
}
