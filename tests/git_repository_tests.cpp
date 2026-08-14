#include "gitx/config.hpp"
#include "gitx/git_repository.hpp"

#include <git2.h>

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

// Run `git` in `repo` and return whether it exited 0.
bool git_ok(const fs::path& repo, const std::string& args) {
  const auto command = "git -C \"" + repo.string() + "\" " + args + " >/dev/null 2>&1";
  return std::system(command.c_str()) == 0;
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

void test_stash_tag_and_status() {
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

  repository.create_tag("v0.1.0", "release");
  assert(git_ok(root, "rev-parse v0.1.0^{commit}"));

  fs::remove_all(root);
}

}  // namespace

int main() {
  git_libgit2_init();
  test_init_and_commit();
  std::cout << "ok: init/commit\n";
  test_branch_switch_syncs_index_and_worktree();
  std::cout << "ok: branch switch syncs index and worktree\n";
  test_merge_conflict_interactive("o", "main side\n");
  std::cout << "ok: merge conflict (keep ours)\n";
  test_merge_conflict_interactive("t", "feature side\n");
  std::cout << "ok: merge conflict (take theirs)\n";
  test_rebase_and_empty_patch_skip();
  std::cout << "ok: rebase with empty-patch skip\n";
  test_stash_tag_and_status();
  std::cout << "ok: stash/tag/status\n";
  return 0;
}
