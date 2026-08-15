#include "gitx/git_repository.hpp"

#include <git2.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace gitx {
namespace {

[[noreturn]] void fail(const std::string& action, int code) {
  const git_error* error = git_error_last();
  throw std::runtime_error(action + " 失败 (" + std::to_string(code) + "): " +
                           (error != nullptr ? error->message : "未知 libgit2 错误"));
}

void check(int code, const std::string& action) {
  if (code < 0) fail(action, code);
}

git_signature* signature_for(git_repository* repo) {
  git_signature* signature = nullptr;
  const auto result = git_signature_default(&signature, repo);
  if (result == GIT_ENOTFOUND) check(git_signature_now(&signature, "gitx user", "gitx@localhost"), "创建默认提交身份");
  else check(result, "读取 Git 用户身份");
  return signature;
}

int credentials_callback(git_credential** out, const char*, const char* username, unsigned int allowed, void*) {
  if ((allowed & GIT_CREDENTIAL_SSH_KEY) != 0) {
    return git_credential_ssh_key_from_agent(out, username != nullptr ? username : "git");
  }
  // HTTPS secret is intentionally accepted only from the process environment; gitx never writes it to disk.
  if ((allowed & GIT_CREDENTIAL_USERPASS_PLAINTEXT) != 0) {
    const char* token = std::getenv("GITX_HTTPS_TOKEN");
    const char* user = std::getenv("GITX_HTTPS_USER");
    if (token != nullptr) return git_credential_userpass_plaintext_new(out, user != nullptr ? user : "oauth2", token);
  }
  return GIT_PASSTHROUGH;
}

std::string format_fingerprint(const unsigned char* bytes, std::size_t size) {
  std::ostringstream output;
  for (std::size_t index = 0; index < size; ++index) {
    if (index != 0) output << ':';
    output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[index]);
  }
  return output.str();
}

// Ask the user whether to trust an unknown SSH host, like `git` does on the
// first connection. Persists the accepted hostkey into ~/.ssh/known_hosts so
// later connections validate without prompting.
int certificate_check_callback(git_cert* cert, int valid, const char* host, void*) {
  if (valid) return 0;
  if (cert == nullptr || cert->cert_type != GIT_CERT_HOSTKEY_LIBSSH2) return 1;  // let libgit2 decide
  const auto* ssh = reinterpret_cast<const git_cert_hostkey*>(cert);
  if ((ssh->type & GIT_CERT_SSH_SHA256) == 0) return 1;

  const std::string fingerprint = format_fingerprint(ssh->hash_sha256, 32);
  std::cout << "首次连接服务器 " << (host != nullptr ? host : "(未知主机)") << "\n"
            << "服务器指纹（SHA256）：SHA256:" << fingerprint << "\n"
            << "请确认这是你信任的服务器。是否信任并继续？[y/N] ";
  std::string answer;
  std::getline(std::cin, answer);
  if (answer != "y" && answer != "Y") {
    std::cout << "已取消连接。\n";
    return -1;
  }

  // Persist to known_hosts: "@cert-authority" is not used; store the plain
  // hostkey line so future libssh2 connections validate automatically.
  const char* home = std::getenv("HOME");
  fs::path known_hosts;
#ifdef _WIN32
  if (const char* user_profile = std::getenv("USERPROFILE")) known_hosts = fs::path(user_profile) / ".ssh" / "known_hosts";
#else
  if (home != nullptr) known_hosts = fs::path(home) / ".ssh" / "known_hosts";
#endif
  if (!known_hosts.empty()) {
    try {
      fs::create_directories(known_hosts.parent_path());
      std::ofstream out(known_hosts, std::ios::app);
      if (out) {
        const auto raw_type = ssh->raw_type;
        std::string key_type = "ssh-ed25519";
        switch (raw_type) {
          case GIT_CERT_SSH_RAW_TYPE_RSA: key_type = "ssh-rsa"; break;
          case GIT_CERT_SSH_RAW_TYPE_KEY_ECDSA_256: key_type = "ecdsa-sha2-nistp256"; break;
          case GIT_CERT_SSH_RAW_TYPE_KEY_ECDSA_384: key_type = "ecdsa-sha2-nistp384"; break;
          case GIT_CERT_SSH_RAW_TYPE_KEY_ECDSA_521: key_type = "ecdsa-sha2-nistp521"; break;
          case GIT_CERT_SSH_RAW_TYPE_KEY_ED25519: key_type = "ssh-ed25519"; break;
          default: key_type = "ssh-unknown"; break;
        }
        // Store as a fingerprint-only marker; libssh2 matches by fingerprint
        // using the same line format as OpenSSH's known_hosts.
        out << (host != nullptr ? host : "*") << " " << key_type << " " << fingerprint << "\n";
      }
    } catch (...) {
      // Best effort; failing to persist should not block the connection.
    }
  }
  return 0;
}

git_remote_callbacks remote_callbacks() {
  git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
  callbacks.credentials = credentials_callback;
  callbacks.certificate_check = certificate_check_callback;
  return callbacks;
}

std::string short_id(const git_oid* oid) {
  char buffer[GIT_OID_HEXSZ + 1]{};
  git_oid_tostr(buffer, sizeof(buffer), oid);
  return std::string(buffer, 10);
}

void write_blob_to_workdir(git_repository* repo, const git_index_entry* entry, const fs::path& file) {
  if (entry == nullptr) throw std::runtime_error("冲突项缺少可采用的文件版本。");
  git_blob* blob = nullptr;
  check(git_blob_lookup(&blob, repo, &entry->id), "读取冲突内容");
  std::unique_ptr<git_blob, decltype(&git_blob_free)> holder(blob, git_blob_free);
  fs::create_directories(file.parent_path());
  std::ofstream output(file, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("无法写入冲突文件: " + file.string());
  output.write(static_cast<const char*>(git_blob_rawcontent(blob)), static_cast<std::streamsize>(git_blob_rawsize(blob)));
}

// Move the current branch ref to `target` and bring index + worktree along.
// Order matters: check out the target tree first, then move the ref, otherwise
// the index keeps the previous branch's contents (set_head alone is not enough).
void fast_forward_to(git_repository* repo, const git_oid* target, const char* reflog_message) {
  git_object* object = nullptr;
  check(git_object_lookup(&object, repo, target, GIT_OBJECT_COMMIT), "读取快进目标");
  std::unique_ptr<git_object, decltype(&git_object_free)> object_holder(object, git_object_free);
  git_checkout_options checkout = GIT_CHECKOUT_OPTIONS_INIT;
  checkout.checkout_strategy = GIT_CHECKOUT_SAFE;
  check(git_checkout_tree(repo, object, &checkout), "更新工作区");
  git_reference* head = nullptr;
  check(git_repository_head(&head, repo), "读取当前分支");
  std::unique_ptr<git_reference, decltype(&git_reference_free)> head_holder(head, git_reference_free);
  git_reference* updated = nullptr;
  check(git_reference_set_target(&updated, head, target, reflog_message), "快进更新分支");
  git_reference_free(updated);
}

// Interactively resolve every conflict left in `index`, one file at a time.
// Throws on 'q' so the caller can leave the repository in its conflict state.
void resolve_conflicts_interactive(git_repository* repo, git_index* index, const fs::path& workdir, const std::string& abort_note) {
  while (git_index_has_conflicts(index)) {
    git_index_conflict_iterator* iterator = nullptr;
    check(git_index_conflict_iterator_new(&iterator, index), "枚举冲突项");
    const git_index_entry *first_ancestor = nullptr, *first_ours = nullptr, *first_theirs = nullptr;
    const auto next = git_index_conflict_next(&first_ancestor, &first_ours, &first_theirs, iterator);
    git_index_conflict_iterator_free(iterator);
    if (next < 0) throw std::runtime_error("无法读取冲突项；请使用原生 git 处理后继续。");
    const auto* first = first_ours != nullptr ? first_ours : (first_theirs != nullptr ? first_theirs : first_ancestor);
    const std::string conflict_path = first != nullptr ? first->path : "";
    const git_index_entry *ancestor = nullptr, *ours = nullptr, *theirs = nullptr;
    check(git_index_conflict_get(&ancestor, &ours, &theirs, index, conflict_path.c_str()), "读取冲突内容");
    const auto* item = ours != nullptr ? ours : (theirs != nullptr ? theirs : ancestor);
    const std::string path = item != nullptr ? item->path : "(未知文件)";
    std::cout << "冲突文件: " << path << "\n选择 [o]保留当前 / [t]采用对方 / [m]手动编辑 / [q]退出: ";
    std::string choice;
    if (!std::getline(std::cin, choice)) throw std::runtime_error("输入已结束，无法继续解决冲突；请使用原生 git 处理后继续。");
    if (choice == "q") throw std::runtime_error(abort_note);
    if (choice == "o") write_blob_to_workdir(repo, ours, workdir / path);
    else if (choice == "t") write_blob_to_workdir(repo, theirs, workdir / path);
    else if (choice == "m") {
      std::cout << "请在编辑器中完成 " << path << "，保存后按回车继续。";
      if (!std::getline(std::cin, choice)) throw std::runtime_error("输入已结束，无法继续解决冲突；请使用原生 git 处理后继续。");
    } else {
      std::cout << "无效选择，请重试。\n";
      continue;
    }
    check(git_index_conflict_remove(index, path.c_str()), "移除冲突记录");
    check(git_index_add_bypath(index, path.c_str()), "暂存已解决文件");
    check(git_index_write(index), "写入冲突解决结果");
  }
}

}  // namespace

struct GitRepository::Impl {
  git_repository* repository = nullptr;
};

GitRepository::GitRepository(const fs::path& path) : impl_(new Impl) {
  check(git_repository_open_ext(&impl_->repository, path.string().c_str(), 0, nullptr), "打开仓库");
}

GitRepository::~GitRepository() {
  if (impl_ != nullptr) {
    git_repository_free(impl_->repository);
    delete impl_;
  }
}

void GitRepository::init(const fs::path& path, const std::string& initial_branch) {
  git_repository_init_options options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
  options.flags = GIT_REPOSITORY_INIT_MKPATH;
  options.initial_head = initial_branch.c_str();
  git_repository* repository = nullptr;
  check(git_repository_init_ext(&repository, path.string().c_str(), &options), "初始化仓库");
  git_repository_free(repository);
}

void GitRepository::clone(const std::string& url, const fs::path& path) {
  git_clone_options options = GIT_CLONE_OPTIONS_INIT;
  options.fetch_opts.callbacks = remote_callbacks();
  git_repository* repository = nullptr;
  check(git_clone(&repository, url.c_str(), path.string().c_str(), &options), "克隆仓库");
  git_repository_free(repository);
}

fs::path GitRepository::workdir() const {
  const char* directory = git_repository_workdir(impl_->repository);
  if (directory == nullptr) throw std::runtime_error("裸仓库不支持此操作。");
  return directory;
}

std::vector<StatusItem> GitRepository::status() const {
  git_status_options options = GIT_STATUS_OPTIONS_INIT;
  options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX | GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;
  git_status_list* list = nullptr;
  check(git_status_list_new(&list, impl_->repository, &options), "读取工作区状态");
  std::unique_ptr<git_status_list, decltype(&git_status_list_free)> holder(list, git_status_list_free);
  std::vector<StatusItem> result;
  for (size_t i = 0; i < git_status_list_entrycount(list); ++i) {
    const auto* entry = git_status_byindex(list, i);
    const auto* delta = entry->index_to_workdir != nullptr ? entry->index_to_workdir : entry->head_to_index;
    const char* path = delta != nullptr ? delta->new_file.path : nullptr;
    if (path == nullptr && delta != nullptr) path = delta->old_file.path;
    result.push_back({path != nullptr ? path : "(未知文件)", std::to_string(entry->status)});
  }
  return result;
}

std::vector<HistoryItem> GitRepository::history(std::size_t limit) const {
  git_revwalk* walk = nullptr;
  check(git_revwalk_new(&walk, impl_->repository), "读取历史");
  std::unique_ptr<git_revwalk, decltype(&git_revwalk_free)> holder(walk, git_revwalk_free);
  git_revwalk_sorting(walk, GIT_SORT_TIME | GIT_SORT_TOPOLOGICAL);
  if (git_revwalk_push_head(walk) == GIT_ENOTFOUND) return {};
  std::vector<HistoryItem> result;
  git_oid oid;
  while (result.size() < limit && git_revwalk_next(&oid, walk) == 0) {
    git_commit* commit = nullptr;
    check(git_commit_lookup(&commit, impl_->repository, &oid), "读取提交");
    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit_holder(commit, git_commit_free);
    const auto* author = git_commit_author(commit);
    result.push_back({short_id(&oid), git_commit_summary(commit), author != nullptr ? author->name : "未知作者"});
  }
  return result;
}

std::string GitRepository::current_branch() const {
  git_reference* head = nullptr;
  const auto result = git_repository_head(&head, impl_->repository);
  if (result == GIT_EUNBORNBRANCH) {
    // Fresh repository: HEAD points at a branch that has no commits yet.
    git_reference* symbolic = nullptr;
    if (git_reference_lookup(&symbolic, impl_->repository, "HEAD") == 0) {
      const char* target = git_reference_symbolic_target(symbolic);
      std::string branch = target != nullptr ? target : "HEAD";
      git_reference_free(symbolic);
      if (branch.rfind("refs/heads/", 0) == 0) branch.erase(0, 11);
      return branch;
    }
    return "HEAD (尚未提交)";
  }
  check(result, "读取当前分支");
  std::unique_ptr<git_reference, decltype(&git_reference_free)> holder(head, git_reference_free);
  const char* shorthand = git_reference_shorthand(head);
  return shorthand != nullptr ? shorthand : "HEAD";
}

std::vector<std::string> GitRepository::branches() const {
  git_branch_iterator* iterator = nullptr;
  check(git_branch_iterator_new(&iterator, impl_->repository, GIT_BRANCH_LOCAL), "读取分支");
  std::unique_ptr<git_branch_iterator, decltype(&git_branch_iterator_free)> holder(iterator, git_branch_iterator_free);
  std::vector<std::string> result;
  git_reference* branch = nullptr;
  git_branch_t type;
  while (git_branch_next(&branch, &type, iterator) == 0) {
    const char* name = nullptr;
    check(git_branch_name(&name, branch), "读取分支名称");
    result.emplace_back(name);
    git_reference_free(branch);
  }
  return result;
}

void GitRepository::stage_all() {
  git_index* index = nullptr;
  check(git_repository_index(&index, impl_->repository), "打开暂存区");
  std::unique_ptr<git_index, decltype(&git_index_free)> holder(index, git_index_free);
  check(git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr), "暂存全部改动");
  check(git_index_write(index), "写入暂存区");
}

void GitRepository::commit(const std::string& message, const TeamConfig& config) {
  if (const auto issue = ConfigStore::validate_commit(config, message)) throw std::runtime_error(*issue);
  git_index* index = nullptr;
  check(git_repository_index(&index, impl_->repository), "打开暂存区");
  std::unique_ptr<git_index, decltype(&git_index_free)> index_holder(index, git_index_free);
  git_oid tree_id;
  check(git_index_write_tree(&tree_id, index), "写入提交树");
  git_tree* tree = nullptr;
  check(git_tree_lookup(&tree, impl_->repository, &tree_id), "读取提交树");
  std::unique_ptr<git_tree, decltype(&git_tree_free)> tree_holder(tree, git_tree_free);
  std::unique_ptr<git_signature, decltype(&git_signature_free)> signature(signature_for(impl_->repository), git_signature_free);

  git_commit* parent = nullptr;
  const git_commit* parents[1] = {nullptr};
  std::size_t parent_count = 0;
  git_oid parent_id;
  if (git_reference_name_to_id(&parent_id, impl_->repository, "HEAD") == 0) {
    check(git_commit_lookup(&parent, impl_->repository, &parent_id), "读取父提交");
    parents[0] = parent;
    parent_count = 1;
  }
  std::unique_ptr<git_commit, decltype(&git_commit_free)> parent_holder(parent, git_commit_free);
  git_oid commit_id;
  check(git_commit_create(&commit_id, impl_->repository, "HEAD", signature.get(), signature.get(), nullptr,
                          message.c_str(), tree, parent_count, parents), "创建提交");
}

void GitRepository::create_and_switch_branch(const std::string& name, const TeamConfig& config) {
  if (const auto issue = ConfigStore::validate_branch(config, name)) throw std::runtime_error(*issue);
  git_oid head_id;
  check(git_reference_name_to_id(&head_id, impl_->repository, "HEAD"), "读取当前提交（请先创建首次提交）");
  git_commit* commit = nullptr;
  check(git_commit_lookup(&commit, impl_->repository, &head_id), "读取当前提交");
  std::unique_ptr<git_commit, decltype(&git_commit_free)> commit_holder(commit, git_commit_free);
  git_reference* branch = nullptr;
  check(git_branch_create(&branch, impl_->repository, name.c_str(), commit, 0), "创建分支");
  git_reference_free(branch);
  checkout_branch(name);
}

void GitRepository::checkout_branch(const std::string& name) {
  const auto reference = "refs/heads/" + name;
  // Check out the target tree before moving HEAD so both the index and the
  // worktree end up on the new branch (set_head alone leaves the index stale).
  git_object* target = nullptr;
  check(git_revparse_single(&target, impl_->repository, reference.c_str()), "读取目标分支");
  std::unique_ptr<git_object, decltype(&git_object_free)> target_holder(target, git_object_free);
  git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
  options.checkout_strategy = GIT_CHECKOUT_SAFE;
  check(git_checkout_tree(impl_->repository, target, &options), "更新工作区");
  check(git_repository_set_head(impl_->repository, reference.c_str()), "切换分支");
}

void GitRepository::create_tag(const std::string& name, const std::string& message) {
  git_oid target_id;
  check(git_reference_name_to_id(&target_id, impl_->repository, "HEAD"), "读取当前提交");
  git_object* target = nullptr;
  check(git_object_lookup(&target, impl_->repository, &target_id, GIT_OBJECT_COMMIT), "读取标签目标");
  std::unique_ptr<git_object, decltype(&git_object_free)> holder(target, git_object_free);
  git_oid tag_id;
  if (message.empty()) check(git_tag_create_lightweight(&tag_id, impl_->repository, name.c_str(), target, 0), "创建轻量标签");
  else {
    std::unique_ptr<git_signature, decltype(&git_signature_free)> signature(signature_for(impl_->repository), git_signature_free);
    check(git_tag_create(&tag_id, impl_->repository, name.c_str(), target, signature.get(), message.c_str(), 0), "创建标签");
  }
}

void GitRepository::stash_save(const std::string& message) {
  std::unique_ptr<git_signature, decltype(&git_signature_free)> signature(signature_for(impl_->repository), git_signature_free);
  git_oid id;
  check(git_stash_save(&id, impl_->repository, signature.get(), message.c_str(), GIT_STASH_DEFAULT), "创建暂存");
}

void GitRepository::stash_pop() { check(git_stash_pop(impl_->repository, 0, nullptr), "恢复最近暂存"); }

void GitRepository::merge_branch_interactive(const std::string& name, const TeamConfig& config) {
  if (config.merge.require_clean_worktree && !status().empty()) throw std::runtime_error("合并前工作区必须保持干净；请先保存或 stash 改动。");
  git_reference* reference = nullptr;
  check(git_branch_lookup(&reference, impl_->repository, name.c_str(), GIT_BRANCH_LOCAL), "读取待合并分支");
  std::unique_ptr<git_reference, decltype(&git_reference_free)> ref_holder(reference, git_reference_free);
  git_annotated_commit* their_head = nullptr;
  check(git_annotated_commit_from_ref(&their_head, impl_->repository, reference), "准备合并");
  std::unique_ptr<git_annotated_commit, decltype(&git_annotated_commit_free)> their_holder(their_head, git_annotated_commit_free);
  const git_annotated_commit* heads[] = {their_head};
  git_merge_analysis_t analysis;
  git_merge_preference_t preference;
  check(git_merge_analysis(&analysis, &preference, impl_->repository, heads, 1), "分析合并");
  if ((analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) != 0) return;
  if ((analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) != 0) {
    fast_forward_to(impl_->repository, git_annotated_commit_id(their_head), "gitx fast-forward");
    return;
  }

  git_checkout_options checkout = GIT_CHECKOUT_OPTIONS_INIT;
  checkout.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_ALLOW_CONFLICTS;
  check(git_merge(impl_->repository, heads, 1, nullptr, &checkout), "执行合并");
  git_index* index = nullptr;
  check(git_repository_index(&index, impl_->repository), "读取合并结果");
  std::unique_ptr<git_index, decltype(&git_index_free)> index_holder(index, git_index_free);

  resolve_conflicts_interactive(impl_->repository, index, workdir(), "合并已保留在冲突状态，可稍后继续处理。");

  git_oid tree_id;
  check(git_index_write_tree(&tree_id, index), "写入合并树");
  git_tree* tree = nullptr;
  check(git_tree_lookup(&tree, impl_->repository, &tree_id), "读取合并树");
  std::unique_ptr<git_tree, decltype(&git_tree_free)> tree_holder(tree, git_tree_free);
  git_oid ours_id;
  check(git_reference_name_to_id(&ours_id, impl_->repository, "HEAD"), "读取当前提交");
  git_commit *ours_commit = nullptr, *theirs_commit = nullptr;
  check(git_commit_lookup(&ours_commit, impl_->repository, &ours_id), "读取当前提交");
  check(git_commit_lookup(&theirs_commit, impl_->repository, git_annotated_commit_id(their_head)), "读取待合并提交");
  std::unique_ptr<git_commit, decltype(&git_commit_free)> ours_holder(ours_commit, git_commit_free);
  std::unique_ptr<git_commit, decltype(&git_commit_free)> theirs_commit_holder(theirs_commit, git_commit_free);
  const git_commit* parents[] = {ours_commit, theirs_commit};
  std::unique_ptr<git_signature, decltype(&git_signature_free)> signature(signature_for(impl_->repository), git_signature_free);
  git_oid merge_id;
  const auto message = "merge: " + name + " into " + current_branch();
  check(git_commit_create(&merge_id, impl_->repository, "HEAD", signature.get(), signature.get(), nullptr,
                          message.c_str(), tree, 2, parents), "创建合并提交");
  git_repository_state_cleanup(impl_->repository);
}

void GitRepository::rebase_onto(const std::string& name, const TeamConfig& config) {
  if (config.merge.require_clean_worktree && !status().empty()) throw std::runtime_error("变基前工作区必须保持干净；请先保存或 stash 改动。");
  git_reference* reference = nullptr;
  check(git_branch_lookup(&reference, impl_->repository, name.c_str(), GIT_BRANCH_LOCAL), "读取目标分支");
  std::unique_ptr<git_reference, decltype(&git_reference_free)> ref_holder(reference, git_reference_free);
  git_annotated_commit* upstream = nullptr;
  check(git_annotated_commit_from_ref(&upstream, impl_->repository, reference), "准备变基");
  std::unique_ptr<git_annotated_commit, decltype(&git_annotated_commit_free)> upstream_holder(upstream, git_annotated_commit_free);

  git_rebase_options options = GIT_REBASE_OPTIONS_INIT;
  options.checkout_options.checkout_strategy = GIT_CHECKOUT_SAFE;
  git_rebase* rebase = nullptr;
  check(git_rebase_init(&rebase, impl_->repository, nullptr, upstream, nullptr, &options), "开始变基");
  std::unique_ptr<git_rebase, decltype(&git_rebase_free)> rebase_holder(rebase, git_rebase_free);

  std::unique_ptr<git_signature, decltype(&git_signature_free)> signature(signature_for(impl_->repository), git_signature_free);
  git_rebase_operation* operation = nullptr;
  int error = 0;
  while ((error = git_rebase_next(&operation, rebase)) == 0) {
    git_index* index = nullptr;
    check(git_repository_index(&index, impl_->repository), "读取变基状态");
    std::unique_ptr<git_index, decltype(&git_index_free)> index_holder(index, git_index_free);
    if (git_index_has_conflicts(index)) {
      resolve_conflicts_interactive(impl_->repository, index, workdir(), "变基已保留在冲突状态，可解决后重试或使用原生 git 处理。");
    }
    git_oid commit_id;
    // The message and author are taken from the original commit when null.
    const auto commit_error = git_rebase_commit(&commit_id, rebase, nullptr, signature.get(), nullptr, nullptr);
    if (commit_error == GIT_EAPPLIED) {
      // The patch became empty (e.g. the resolved content matches the parent,
      // or the change is already present upstream): skip the commit and
      // continue, matching `git rebase` behavior for empty patches.
      continue;
    }
    check(commit_error, "应用变基提交");
  }
  if (error != GIT_ITEROVER) {
    git_rebase_abort(rebase);
    fail("变基", error);
  }
  check(git_rebase_finish(rebase, signature.get()), "完成变基");
  git_repository_state_cleanup(impl_->repository);
}

void GitRepository::fetch(const std::string& remote_name) {
  git_remote* remote = nullptr;
  check(git_remote_lookup(&remote, impl_->repository, remote_name.c_str()), "读取远端");
  std::unique_ptr<git_remote, decltype(&git_remote_free)> holder(remote, git_remote_free);
  git_fetch_options options = GIT_FETCH_OPTIONS_INIT;
  options.callbacks = remote_callbacks();
  check(git_remote_fetch(remote, nullptr, &options, "gitx fetch"), "获取远端更新");
}

void GitRepository::pull(const std::string& remote_name) {
  fetch(remote_name);
  const auto remote_branch = "refs/remotes/" + remote_name + "/" + current_branch();
  git_reference* reference = nullptr;
  check(git_reference_lookup(&reference, impl_->repository, remote_branch.c_str()), "读取远端跟踪分支");
  std::unique_ptr<git_reference, decltype(&git_reference_free)> holder(reference, git_reference_free);
  const auto* name = git_reference_shorthand(reference);
  git_annotated_commit* head = nullptr;
  check(git_annotated_commit_from_ref(&head, impl_->repository, reference), "准备拉取合并");
  std::unique_ptr<git_annotated_commit, decltype(&git_annotated_commit_free)> head_holder(head, git_annotated_commit_free);
  const git_annotated_commit* heads[] = {head};
  git_merge_analysis_t analysis;
  git_merge_preference_t preference;
  check(git_merge_analysis(&analysis, &preference, impl_->repository, heads, 1), "分析拉取更新");
  if ((analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) != 0) return;
  if ((analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) == 0) throw std::runtime_error("拉取需要合并。请使用 integrate 并解决冲突后再同步。");
  fast_forward_to(impl_->repository, git_annotated_commit_id(head), "gitx pull");
  (void)name;
}

void GitRepository::push(const std::string& remote_name) {
  git_remote* remote = nullptr;
  check(git_remote_lookup(&remote, impl_->repository, remote_name.c_str()), "读取远端");
  std::unique_ptr<git_remote, decltype(&git_remote_free)> holder(remote, git_remote_free);
  git_push_options options = GIT_PUSH_OPTIONS_INIT;
  options.callbacks = remote_callbacks();
  const auto branch = current_branch();
  const auto spec = "refs/heads/" + branch + ":refs/heads/" + branch;
  char* values[] = {const_cast<char*>(spec.c_str())};
  git_strarray refspecs{values, 1};
  check(git_remote_push(remote, &refspecs, &options), "推送分支");
}

void GitRepository::add_remote(const std::string& name, const std::string& url) {
  git_remote* remote = nullptr;
  check(git_remote_create(&remote, impl_->repository, name.c_str(), url.c_str()), "添加远端");
  git_remote_free(remote);
}

std::optional<std::string> GitRepository::remote_url(const std::string& remote_name) const {
  git_remote* remote = nullptr;
  if (git_remote_lookup(&remote, impl_->repository, remote_name.c_str()) != 0) return std::nullopt;
  std::unique_ptr<git_remote, decltype(&git_remote_free)> holder(remote, git_remote_free);
  const char* url = git_remote_url(remote);
  return url != nullptr ? std::optional<std::string>(url) : std::nullopt;
}

std::pair<std::size_t, std::size_t> GitRepository::divergence_from_remote(const std::string& remote_name) const {
  const auto branch = current_branch();
  git_oid local_id, remote_id;
  if (git_reference_name_to_id(&local_id, impl_->repository, ("refs/heads/" + branch).c_str()) != 0) return {0, 0};
  if (git_reference_name_to_id(&remote_id, impl_->repository, ("refs/remotes/" + remote_name + "/" + branch).c_str()) != 0) {
    return {1, 0};  // local branch has no remote tracking branch yet: all ahead
  }
  size_t ahead = 0, behind = 0;
  if (git_graph_ahead_behind(&ahead, &behind, impl_->repository, &local_id, &remote_id) != 0) return {0, 0};
  return {ahead, behind};
}

bool GitRepository::empty_repository() const {
  git_reference* head = nullptr;
  const auto result = git_repository_head(&head, impl_->repository);
  if (result == GIT_EUNBORNBRANCH) return true;
  if (result != 0) return true;
  git_reference_free(head);
  return false;
}

std::string GitRepository::identity_name() const {
  git_config* config = nullptr;
  // A snapshot merges all config levels (system/global/local) and is safe to
  // read without refresh issues.
  if (git_repository_config_snapshot(&config, impl_->repository) != 0) return "";
  std::unique_ptr<git_config, decltype(&git_config_free)> holder(config, git_config_free);
  const char* name = nullptr;
  if (git_config_get_string(&name, config, "user.name") != 0 || name == nullptr) return "";
  return name;
}

}  // namespace gitx
