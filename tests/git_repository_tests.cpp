#include "gitx/config.hpp"
#include "gitx/git_repository.hpp"

#include <git2.h>

#ifdef _WIN32
#include <crtdbg.h>
#include <cstdlib>
#endif

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using gitx::ConfigStore;
using gitx::GitRepository;

namespace {

fs::path make_temp_dir(const std::string& name) {
  const auto path = fs::temp_directory_path() / name;
  fs::remove_all(path);
  fs::create_directories(path);
  return path;
}

void set_identity(const fs::path& repo) {
  const auto git = "git -C \"" + repo.string() + "\" config ";
  std::system((git + "user.name Tester").c_str());
  std::system((git + "user.email tester@example.com").c_str());
}

void write_file(const fs::path& file, const std::string& content) {
  std::ofstream out(file, std::ios::trunc);
  out << content;
}

std::string read_file(const fs::path& file) {
  std::ifstream in(file);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void test_init_and_commit() {
  const auto root = make_temp_dir("gitx-test-init");
  GitRepository::init(root, "main");
  set_identity(root);
  const auto config = ConfigStore::load_team(root);

  GitRepository repository(root);
  assert(repository.current_branch() == "main");

  write_file(root / "a.txt", "hello\n");
  repository.stage_all();
  repository.commit("feat(cli): first commit", config);
  assert(!repository.history(5).empty());
  assert(repository.history(1).front().summary == "feat(cli): first commit");

  // AI preview must be able to inspect changes without silently staging them.
  write_file(root / "preview.txt", "pending\n");
  assert(repository.staged_diff(1024).empty());
  assert(!repository.working_diff(1024).empty());
  assert(repository.staged_diff(1024).empty());

  fs::remove_all(root);
}

void test_identity_and_selective_stage() {
  const auto root = make_temp_dir("gitx-test-identity-stage");
  GitRepository::init(root, "main");
  const auto config = ConfigStore::load_team(root);
  GitRepository repository(root);

  const auto temporary = repository.set_temporary_identity();
  assert(!temporary.empty());
  assert(repository.has_identity());
  assert(repository.uses_temporary_identity());
  repository.set_identity("Tester", "tester@example.com");
  assert(repository.identity_name() == "Tester");
  assert(repository.identity_email() == "tester@example.com");
  assert(!repository.uses_temporary_identity());

  write_file(root / "one.txt", "one\n");
  write_file(root / "two.txt", "two\n");
  repository.stage_paths({"one.txt"});
  assert(repository.has_staged_changes());
  repository.commit("feat(test): stage one file", config);
  const auto items = repository.status();
  assert(items.size() == 1);
  assert(items.front().path == "two.txt");
  assert(items.front().state.find("未暂存：未跟踪") != std::string::npos);

  fs::remove_all(root);
}

void test_clone_recovers_invalid_remote_head() {
  const auto root = make_temp_dir("gitx-test-clone-invalid-head");
  const auto source = root / "source";
  const auto server = root / "server.git";
  GitRepository::init(source, "main");
  set_identity(source);
  const auto config = ConfigStore::load_team(source);
  GitRepository author(source);
  write_file(source / "hello.txt", "hello\n");
  author.stage_all();
  author.commit("feat(test): initial history", config);

  assert(std::system(("git init --bare \"" + server.string() + "\"").c_str()) == 0);
  author.add_remote("origin", server.string());
  author.push();
  assert(std::system(("git --git-dir=\"" + server.string() + "\" symbolic-ref HEAD refs/heads/master").c_str()) == 0);

  const auto copy = root / "copy";
  GitRepository::clone(server.string(), copy);
  GitRepository cloned(copy);
  assert(cloned.current_branch() == "main");
  assert(cloned.history(1).front().summary == "feat(test): initial history");

  fs::remove_all(root);
}

void test_revert_local_commit() {
  const auto root = make_temp_dir("gitx-test-revert-local");
  GitRepository::init(root, "main");
  set_identity(root);
  const auto config = ConfigStore::load_team(root);
  GitRepository repository(root);
  write_file(root / "note.txt", "correct\n");
  repository.stage_all();
  repository.commit("feat(test): add correct note", config);
  write_file(root / "note.txt", "incorrect\n");
  repository.stage_all();
  repository.commit("fix(test): introduce incorrect note", config);
  const auto target = repository.history(1).front().id;

  repository.revert_commit(target, config);
  assert(read_file(root / "note.txt") == "correct\n");
  assert(repository.status().empty());
  assert(repository.history(1).front().summary == "fix(revert): 回退 " + target);
  fs::remove_all(root);
}

void test_revert_pushed_commit() {
  const auto root = make_temp_dir("gitx-test-revert-pushed");
  const auto source = root / "source";
  const auto server = root / "server.git";
  GitRepository::init(source, "main");
  set_identity(source);
  const auto config = ConfigStore::load_team(source);
  GitRepository repository(source);
  write_file(source / "note.txt", "correct\n");
  repository.stage_all();
  repository.commit("feat(test): add server note", config);
  write_file(source / "note.txt", "incorrect\n");
  repository.stage_all();
  repository.commit("fix(test): publish incorrect note", config);
  const auto target = repository.history(1).front().id;
  assert(std::system(("git init --bare \"" + server.string() + "\"").c_str()) == 0);
  repository.add_remote("origin", server.string());
  repository.push();

  repository.revert_commit(target, config);
  repository.push();
  const auto copy = root / "copy";
  GitRepository::clone(server.string(), copy);
  assert(read_file(copy / "note.txt") == "correct\n");
  fs::remove_all(root);
}

void test_revert_conflict_leaves_repository_unchanged() {
  const auto root = make_temp_dir("gitx-test-revert-conflict");
  GitRepository::init(root, "main");
  set_identity(root);
  const auto config = ConfigStore::load_team(root);
  GitRepository repository(root);
  write_file(root / "note.txt", "value=one\n");
  repository.stage_all();
  repository.commit("feat(test): add note", config);
  write_file(root / "note.txt", "value=two\n");
  repository.stage_all();
  repository.commit("fix(test): change note", config);
  const auto target = repository.history(1).front().id;
  write_file(root / "note.txt", "value=three\n");
  repository.stage_all();
  repository.commit("fix(test): change note again", config);
  const auto count = repository.history(10).size();

  bool rejected = false;
  try {
    repository.revert_commit(target, config);
  } catch (const std::runtime_error& error) {
    rejected = std::string(error.what()).find("冲突") != std::string::npos;
  }
  assert(rejected);
  assert(read_file(root / "note.txt") == "value=three\n");
  assert(repository.status().empty());
  assert(repository.history(10).size() == count);
  fs::remove_all(root);
}

void test_branch_switch_syncs_index_and_worktree() {
  const auto root = make_temp_dir("gitx-test-branch");
  GitRepository::init(root, "main");
  set_identity(root);
  const auto config = ConfigStore::load_team(root);
  GitRepository repository(root);

  write_file(root / "f.txt", "base\n");
  repository.stage_all();
  repository.commit("feat(cli): base", config);

  repository.create_and_switch_branch("feature/login", config);
  assert(repository.current_branch() == "feature/login");
  write_file(root / "f.txt", "feature change\n");
  repository.stage_all();
  repository.commit("feat(cli): feature work", config);

  repository.checkout_branch("main");
  // Worktree must hold main's version and the index must not be left dirty.
  assert(read_file(root / "f.txt") == "base\n");
  assert(repository.status().empty());

  repository.checkout_branch("feature/login");
  assert(read_file(root / "f.txt") == "feature change\n");
  assert(repository.status().empty());

  fs::remove_all(root);
}

void test_merge_conflict_interactive(const std::string& answer, const std::string& expected) {
  const auto root = make_temp_dir("gitx-test-merge-" + answer);
  GitRepository::init(root, "main");
  set_identity(root);
  const auto config = ConfigStore::load_team(root);
  GitRepository repository(root);

  write_file(root / "f.txt", "base\n");
  repository.stage_all();
  repository.commit("feat(cli): base", config);

  repository.create_and_switch_branch("feature/x", config);
  write_file(root / "f.txt", "feature side\n");
  repository.stage_all();
  repository.commit("feat(cli): feature side", config);

  repository.checkout_branch("main");
  write_file(root / "f.txt", "main side\n");
  repository.stage_all();
  repository.commit("feat(main): main side", config);

  // Merge feature/x into main produces a conflict; feed the chosen answer.
  std::cin.clear();
  const auto saved = std::cin.rdbuf();
  std::istringstream input(answer + "\n");
  std::cin.rdbuf(input.rdbuf());
  repository.merge_branch_interactive("feature/x", config);
  std::cin.rdbuf(saved);

  assert(read_file(root / "f.txt") == expected);
  assert(repository.status().empty());
  // A merge commit with two parents was created.
  assert(repository.history(2).size() == 2);

  fs::remove_all(root);
}

void test_rebase_and_empty_patch_skip() {
  const auto root = make_temp_dir("gitx-test-rebase");
  GitRepository::init(root, "main");
  set_identity(root);
  const auto config = ConfigStore::load_team(root);
  GitRepository repository(root);

  write_file(root / "f.txt", "base\n");
  repository.stage_all();
  repository.commit("feat(cli): base", config);

  repository.create_and_switch_branch("feature/x", config);
  write_file(root / "c.txt", "same\n");
  repository.stage_all();
  repository.commit("feat(cli): empty change", config);
  write_file(root / "f.txt", "feature\n");
  repository.stage_all();
  repository.commit("feat(cli): real change", config);

  repository.checkout_branch("main");
  write_file(root / "c.txt", "same\n");
  repository.stage_all();
  repository.commit("feat(main): same change", config);

  repository.checkout_branch("feature/x");
  std::cin.clear();
  const auto saved = std::cin.rdbuf();
  std::istringstream input("o\n");
  std::cin.rdbuf(input.rdbuf());
  repository.rebase_onto("main", config);
  std::cin.rdbuf(saved);

  assert(repository.status().empty());
  const auto items = repository.history(10);
  // The identical change was dropped; the real change was replayed on top.
  assert(items.size() >= 3);
  assert(items.front().summary == "feat(cli): real change");
  bool found_empty = false;
  for (const auto& item : items)
    if (item.summary == "feat(cli): empty change") found_empty = true;
  assert(!found_empty);

  fs::remove_all(root);
}

void test_stash_and_status() {
  const auto root = make_temp_dir("gitx-test-misc");
  GitRepository::init(root, "main");
  set_identity(root);
  const auto config = ConfigStore::load_team(root);
  GitRepository repository(root);

  write_file(root / "f.txt", "hello\n");
  repository.stage_all();
  repository.commit("feat(cli): first", config);

  write_file(root / "f.txt", "dirty\n");
  repository.stash_save("wip");
  assert(repository.status().empty());
  repository.stash_pop();
  assert(read_file(root / "f.txt") == "dirty\n");

  fs::remove_all(root);
}

}  // namespace

int main() {
#ifdef _WIN32
  // On Windows a failed assert() pops a modal "Debug Assertion Failed"
  // dialog that hangs CI. Route assertions to stderr and exit instead.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
  git_libgit2_init();
  test_init_and_commit();
  std::cout << "ok: init/commit\n";
  test_identity_and_selective_stage();
  std::cout << "ok: identity/selective stage\n";
  test_clone_recovers_invalid_remote_head();
  std::cout << "ok: clone recovers invalid remote HEAD\n";
  test_revert_local_commit();
  std::cout << "ok: local revert\n";
  test_revert_pushed_commit();
  std::cout << "ok: pushed revert\n";
  test_revert_conflict_leaves_repository_unchanged();
  std::cout << "ok: revert conflict leaves repository unchanged\n";
  test_branch_switch_syncs_index_and_worktree();
  std::cout << "ok: branch switch syncs index and worktree\n";
  test_merge_conflict_interactive("o", "main side\n");
  std::cout << "ok: merge conflict (keep ours)\n";
  test_merge_conflict_interactive("t", "feature side\n");
  std::cout << "ok: merge conflict (take theirs)\n";
  test_rebase_and_empty_patch_skip();
  std::cout << "ok: rebase with empty-patch skip\n";
  test_stash_and_status();
  std::cout << "ok: stash/status\n";
  return 0;
}
