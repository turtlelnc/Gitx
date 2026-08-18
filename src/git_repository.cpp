#include "gitx/git_repository.hpp"

#include <git2.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

bool edit_with_system_editor(const fs::path& file) {
  const char* editor = std::getenv("GIT_EDITOR");
  if (editor == nullptr || *editor == '\0') editor = std::getenv("VISUAL");
  if (editor == nullptr || *editor == '\0') editor = std::getenv("EDITOR");
#ifdef _WIN32
  if (editor == nullptr || *editor == '\0') editor = "notepad.exe";
  const auto path = file.string();
  return _spawnlp(_P_WAIT, editor, editor, path.c_str(), nullptr) == 0;
#else
  if (editor == nullptr || *editor == '\0') editor = "vi";
  const pid_t child = fork();
  if (child < 0) return false;
  if (child == 0) {
    execlp(editor, editor, file.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

std::string config_value(git_repository* repo, const char* key) {
  git_config* config = nullptr;
  if (git_repository_config_snapshot(&config, repo) != 0) return {};
  std::unique_ptr<git_config, decltype(&git_config_free)> holder(config, git_config_free);
  const char* value = nullptr;
  if (git_config_get_string(&value, config, key) != 0 || value == nullptr) return {};
  return value;
}

git_signature* signature_for(git_repository* repo) {
  const auto name = config_value(repo, "user.name");
  const auto email = config_value(repo, "user.email");
  if (name.empty() || email.empty()) {
    throw std::runtime_error(
        "尚未配置提交身份。运行：gitx config user set \"姓名\" \"邮箱\"；"
        "如仅需临时本地提交，可明确运行：gitx config user temporary。");
  }
  git_signature* signature = nullptr;
  const auto result = git_signature_default(&signature, repo);
  check(result, "读取 Git 用户身份");
  return signature;
}

std::string safe_device_label() {
#ifdef __APPLE__
  std::string system = "mac";
#elif defined(_WIN32)
  std::string system = "windows";
#elif defined(__linux__)
  std::string system = "linux";
#else
  std::string system = "device";
#endif
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
  return system + "-arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return system + "-x64";
#else
  return system;
#endif
}

std::string device_suffix() {
  static constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  std::random_device random;
  std::string suffix;
  suffix.reserve(6);
  for (int index = 0; index < 6; ++index) suffix.push_back(alphabet[random() % (sizeof(alphabet) - 1)]);
  return suffix;
}

void set_config_value(git_repository* repo, const char* key, const std::string& value) {
  git_config* config = nullptr;
  check(git_repository_config(&config, repo), "打开仓库配置");
  std::unique_ptr<git_config, decltype(&git_config_free)> holder(config, git_config_free);
  check(git_config_set_string(config, key, value.c_str()), "写入仓库配置");
}

void delete_config_value(git_repository* repo, const char* key) {
  git_config* config = nullptr;
  check(git_repository_config(&config, repo), "打开仓库配置");
  std::unique_ptr<git_config, decltype(&git_config_free)> holder(config, git_config_free);
  const auto result = git_config_delete_entry(config, key);
  if (result != 0 && result != GIT_ENOTFOUND) check(result, "清理仓库配置");
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

// Accept only host keys already validated by libgit2.  A SHA-256 fingerprint
// is not an OpenSSH known_hosts entry (that format requires the raw public
// key), so writing it out and accepting it here would turn host-key changes
// into a click-through prompt.  Users must add a new host key with OpenSSH
// tooling, then retry.
int certificate_check_callback(git_cert* cert, int valid, const char* host, void*) {
  if (valid) return 0;
  if (cert == nullptr || cert->cert_type != GIT_CERT_HOSTKEY_LIBSSH2) return -1;
  const auto* ssh = reinterpret_cast<const git_cert_hostkey*>(cert);
  if ((ssh->type & GIT_CERT_SSH_SHA256) == 0) return -1;

  const std::string fingerprint = format_fingerprint(ssh->hash_sha256, 32);
  std::cerr << "SSH 主机密钥未通过 known_hosts 校验："
            << (host != nullptr ? host : "(未知主机)") << "\n"
            << "SHA256 指纹：" << fingerprint << "\n"
            << "请先通过 ssh-keyscan 或可信管理员将该主机密钥写入 ~/.ssh/known_hosts，再重试。\n";
  return -1;
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
      std::cout << "正在用系统编辑器编辑 " << path << "（保存退出后自动继续）...\n";
      const auto saved = edit_with_system_editor(workdir / path);
      if (!saved) {
        std::cout << "编辑器未能正常完成，保留原冲突内容。可设置 GIT_EDITOR、VISUAL 或 EDITOR 后重试。\n";
        continue;
      }
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
  std::unique_ptr<git_repository, decltype(&git_repository_free)> holder(repository, git_repository_free);

  // A bare server can have an invalid HEAD (for example it still points at
  // master while its only branch is main). libgit2 then leaves the clone on an
  // unborn branch. Never let a later save create an unrelated root history.
  git_reference* head = nullptr;
  const auto head_result = git_repository_head(&head, repository);
  if (head_result == 0) {
    git_reference_free(head);
    return;
  }
  if (head_result != GIT_EUNBORNBRANCH) check(head_result, "读取克隆后的默认分支");

  git_reference_iterator* iterator = nullptr;
  check(git_reference_iterator_glob_new(&iterator, repository, "refs/remotes/origin/*"), "读取远端分支");
  std::unique_ptr<git_reference_iterator, decltype(&git_reference_iterator_free)> iterator_holder(iterator, git_reference_iterator_free);
  std::vector<std::string> branches;
  git_reference* reference = nullptr;
  while (git_reference_next(&reference, iterator) == 0) {
    const char* name = git_reference_name(reference);
    const std::string prefix = "refs/remotes/origin/";
    if (name != nullptr && std::string(name).starts_with(prefix) && std::string(name) != "refs/remotes/origin/HEAD") {
      branches.push_back(std::string(name).substr(prefix.size()));
    }
    git_reference_free(reference);
  }
  if (branches.empty()) {
    throw std::runtime_error("远端仓库没有可检出的默认分支；请先在服务器创建并推送初始提交。");
  }

  std::string branch;
  if (std::find(branches.begin(), branches.end(), "main") != branches.end()) branch = "main";
  else if (branches.size() == 1) branch = branches.front();
  else {
    std::ostringstream message;
    message << "远端默认分支无效，且存在多个分支：";
    for (const auto& item : branches) message << " " << item;
    message << "。请修复服务器 HEAD 后再克隆。";
    throw std::runtime_error(message.str());
  }

  const auto remote_ref = "refs/remotes/origin/" + branch;
  check(git_reference_lookup(&reference, repository, remote_ref.c_str()), "读取远端默认分支");
  std::unique_ptr<git_reference, decltype(&git_reference_free)> remote_holder(reference, git_reference_free);
  const auto local_ref = "refs/heads/" + branch;
  git_reference* local = nullptr;
  check(git_reference_create(&local, repository, local_ref.c_str(), git_reference_target(reference), 0,
                             "gitx recover invalid remote HEAD"), "创建本地默认分支");
  git_reference_free(local);
  check(git_repository_set_head(repository, local_ref.c_str()), "设置本地默认分支");
  git_checkout_options checkout = GIT_CHECKOUT_OPTIONS_INIT;
  checkout.checkout_strategy = GIT_CHECKOUT_SAFE;
  check(git_checkout_head(repository, &checkout), "检出远端默认分支");
  set_config_value(repository, ("branch." + branch + ".remote").c_str(), "origin");
  set_config_value(repository, ("branch." + branch + ".merge").c_str(), "refs/heads/" + branch);
}

bool GitRepository::ssh_agent_has_identity() {
  git_credential* credential = nullptr;
  const auto result = git_credential_ssh_key_from_agent(&credential, "git");
  if (result != 0 || credential == nullptr) return false;
  git_credential_free(credential);
  return true;
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
  const auto describe = [](unsigned int status, bool index) {
    if (index) {
      if (status & GIT_STATUS_INDEX_NEW) return std::string("新增");
      if (status & GIT_STATUS_INDEX_MODIFIED) return std::string("修改");
      if (status & GIT_STATUS_INDEX_DELETED) return std::string("删除");
      if (status & GIT_STATUS_INDEX_RENAMED) return std::string("重命名");
      if (status & GIT_STATUS_INDEX_TYPECHANGE) return std::string("类型变更");
    } else {
      if (status & GIT_STATUS_WT_NEW) return std::string("未跟踪");
      if (status & GIT_STATUS_WT_MODIFIED) return std::string("修改");
      if (status & GIT_STATUS_WT_DELETED) return std::string("删除");
      if (status & GIT_STATUS_WT_RENAMED) return std::string("重命名");
      if (status & GIT_STATUS_WT_TYPECHANGE) return std::string("类型变更");
    }
    return std::string{};
  };
  for (size_t i = 0; i < git_status_list_entrycount(list); ++i) {
    const auto* entry = git_status_byindex(list, i);
    const auto* delta = entry->index_to_workdir != nullptr ? entry->index_to_workdir : entry->head_to_index;
    const char* path = delta != nullptr ? delta->new_file.path : nullptr;
    if (path == nullptr && delta != nullptr) path = delta->old_file.path;
    std::string state;
    const auto staged = describe(entry->status, true);
    const auto unstaged = describe(entry->status, false);
    if (!staged.empty()) state += "已暂存：" + staged;
    if (!unstaged.empty()) {
      if (!state.empty()) state += "；";
      state += "未暂存：" + unstaged;
    }
    result.push_back({path != nullptr ? path : "(未知文件)", state.empty() ? "状态未知" : state});
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

std::vector<HistoryItem> GitRepository::commits_since(const std::string& base_ref) const {
  git_revwalk* walk = nullptr;
  check(git_revwalk_new(&walk, impl_->repository), "读取历史");
  std::unique_ptr<git_revwalk, decltype(&git_revwalk_free)> holder(walk, git_revwalk_free);
  git_revwalk_sorting(walk, GIT_SORT_TIME | GIT_SORT_TOPOLOGICAL);
  if (git_revwalk_push_head(walk) == GIT_ENOTFOUND) return {};
  git_oid base_id;
  git_object* base_object = nullptr;
  if (git_revparse_single(&base_object, impl_->repository, base_ref.c_str()) != 0) {
    // Base ref not found: treat everything as new.
    std::vector<HistoryItem> result;
    git_oid oid;
    while (git_revwalk_next(&oid, walk) == 0) {
      git_commit* commit = nullptr;
      check(git_commit_lookup(&commit, impl_->repository, &oid), "读取提交");
      std::unique_ptr<git_commit, decltype(&git_commit_free)> commit_holder(commit, git_commit_free);
      const auto* author = git_commit_author(commit);
      result.push_back({short_id(&oid), git_commit_summary(commit), author != nullptr ? author->name : "未知作者"});
    }
    return result;
  }
  git_oid_cpy(&base_id, git_object_id(base_object));
  git_object_free(base_object);
  check(git_revwalk_hide(walk, &base_id), "排除基准提交");
  std::vector<HistoryItem> result;
  git_oid oid;
  while (git_revwalk_next(&oid, walk) == 0) {
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
  check(git_index_update_all(index, nullptr, nullptr, nullptr), "更新已跟踪文件的暂存状态");
  check(git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr), "暂存全部改动");
  check(git_index_write(index), "写入暂存区");
}

void GitRepository::stage_paths(const std::vector<std::string>& paths) {
  if (paths.empty()) throw std::runtime_error("请至少提供一个要暂存的文件路径。");
  git_index* index = nullptr;
  check(git_repository_index(&index, impl_->repository), "打开暂存区");
  std::unique_ptr<git_index, decltype(&git_index_free)> holder(index, git_index_free);
  std::vector<char*> values;
  values.reserve(paths.size());
  for (const auto& path : paths) values.push_back(const_cast<char*>(path.c_str()));
  git_strarray specs{values.data(), values.size()};
  check(git_index_update_all(index, &specs, nullptr, nullptr), "更新指定文件的暂存状态");
  check(git_index_add_all(index, &specs, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr), "暂存指定文件");
  check(git_index_write(index), "写入暂存区");
}

bool GitRepository::has_staged_changes() const {
  return !staged_diff(64 * 1024).empty();
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

void GitRepository::revert_commit(const std::string& commit_id, const TeamConfig& config) {
  if (!status().empty()) {
    throw std::runtime_error("工作区或暂存区存在改动。请先提交或 stash，再执行历史回退。");
  }

  git_object* object = nullptr;
  check(git_revparse_single(&object, impl_->repository, commit_id.c_str()), "读取要回退的提交");
  std::unique_ptr<git_object, decltype(&git_object_free)> object_holder(object, git_object_free);
  git_commit* target = nullptr;
  check(git_commit_lookup(&target, impl_->repository, git_object_id(object)), "读取要回退的提交");
  std::unique_ptr<git_commit, decltype(&git_commit_free)> target_holder(target, git_commit_free);
  if (git_commit_parentcount(target) != 1) {
    throw std::runtime_error("暂不支持回退合并提交或根提交。请先选择一条普通提交，避免错误主线回退。");
  }

  git_reference* head = nullptr;
  check(git_repository_head(&head, impl_->repository), "读取当前提交");
  std::unique_ptr<git_reference, decltype(&git_reference_free)> head_holder(head, git_reference_free);
  const auto* head_id = git_reference_target(head);
  if (head_id == nullptr) throw std::runtime_error("当前 HEAD 不是可回退的本地分支。");
  git_commit* current = nullptr;
  check(git_commit_lookup(&current, impl_->repository, head_id), "读取当前提交");
  std::unique_ptr<git_commit, decltype(&git_commit_free)> current_holder(current, git_commit_free);

  // git_revert_commit calculates into an in-memory index. A conflict therefore
  // leaves both the working directory and branch untouched.
  git_index* inverse_index = nullptr;
  git_merge_options merge_options = GIT_MERGE_OPTIONS_INIT;
  check(git_revert_commit(&inverse_index, impl_->repository, target, current, 0, &merge_options), "计算回退结果");
  std::unique_ptr<git_index, decltype(&git_index_free)> index_holder(inverse_index, git_index_free);
  if (git_index_has_conflicts(inverse_index)) {
    throw std::runtime_error("回退会产生冲突，已保持仓库不变。请先在新分支上处理，或使用原生 Git 指定合并策略。");
  }

  git_oid tree_id;
  check(git_index_write_tree_to(&tree_id, inverse_index, impl_->repository), "写入回退提交树");
  git_tree* tree = nullptr;
  check(git_tree_lookup(&tree, impl_->repository, &tree_id), "读取回退提交树");
  std::unique_ptr<git_tree, decltype(&git_tree_free)> tree_holder(tree, git_tree_free);
  const auto message = "fix(revert): 回退 " + short_id(git_commit_id(target));
  if (const auto issue = ConfigStore::validate_commit(config, message)) throw std::runtime_error(*issue);
  std::unique_ptr<git_signature, decltype(&git_signature_free)> signature(signature_for(impl_->repository), git_signature_free);
  const git_commit* parents[] = {current};
  git_oid new_commit;
  check(git_commit_create(&new_commit, impl_->repository, "HEAD", signature.get(), signature.get(), nullptr,
                          message.c_str(), tree, 1, parents), "创建反向提交");

  git_checkout_options checkout = GIT_CHECKOUT_OPTIONS_INIT;
  // The worktree was verified clean before calculating the inverse tree. The
  // index still describes the pre-revert commit, so SAFE may treat this
  // intended update as a local modification; force the already-computed tree.
  checkout.checkout_strategy = GIT_CHECKOUT_FORCE;
  check(git_checkout_tree(impl_->repository, reinterpret_cast<git_object*>(tree), &checkout), "更新回退后的工作区");
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
  set_upstream(remote_name);
}

void GitRepository::add_remote(const std::string& name, const std::string& url) {
  git_remote* remote = nullptr;
  check(git_remote_create(&remote, impl_->repository, name.c_str(), url.c_str()), "添加远端");
  git_remote_free(remote);
}

void GitRepository::set_remote_url(const std::string& name, const std::string& url) {
  git_remote* remote = nullptr;
  check(git_remote_lookup(&remote, impl_->repository, name.c_str()), "读取远端");
  std::unique_ptr<git_remote, decltype(&git_remote_free)> holder(remote, git_remote_free);
  check(git_remote_set_url(impl_->repository, name.c_str(), url.c_str()), "修改远端地址");
}

void GitRepository::set_upstream(const std::string& remote_name) {
  const auto branch = current_branch();
  set_config_value(impl_->repository, ("branch." + branch + ".remote").c_str(), remote_name);
  set_config_value(impl_->repository, ("branch." + branch + ".merge").c_str(), "refs/heads/" + branch);
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
  return config_value(impl_->repository, "user.name");
}

std::string GitRepository::identity_email() const {
  return config_value(impl_->repository, "user.email");
}

bool GitRepository::has_identity() const {
  return !identity_name().empty() && !identity_email().empty();
}

bool GitRepository::uses_temporary_identity() const {
  return config_value(impl_->repository, "gitx.identity.kind") == "temporary";
}

void GitRepository::set_identity(const std::string& name, const std::string& email) {
  if (name.empty() || email.empty()) throw std::runtime_error("姓名和邮箱均不能为空。");
  if (email.find('@') == std::string::npos) throw std::runtime_error("邮箱格式无效。请提供包含 @ 的邮箱地址。");
  set_config_value(impl_->repository, "user.name", name);
  set_config_value(impl_->repository, "user.email", email);
  delete_config_value(impl_->repository, "gitx.identity.kind");
}

std::string GitRepository::set_temporary_identity() {
  const auto label = safe_device_label();
  const auto suffix = device_suffix();
  const auto name = "设备-" + label + "-" + suffix;
  const auto email = "gitx+" + label + "-" + suffix + "@local.invalid";
  set_config_value(impl_->repository, "user.name", name);
  set_config_value(impl_->repository, "user.email", email);
  set_config_value(impl_->repository, "gitx.identity.kind", "temporary");
  return name;
}

namespace {

std::string diff_to_text(git_diff* diff, std::size_t max_chars) {
  std::string result;
  git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, [](const git_diff_delta*, const git_diff_hunk*, const git_diff_line* line, void* payload) -> int {
    auto* output = static_cast<std::string*>(payload);
    output->append(line->content, line->content_len);
    return 0;
  }, &result);
  if (result.size() > max_chars) {
    result.resize(max_chars);
    result += "\n...（diff 过长已截断）";
  }
  return result;
}

}  // namespace

std::string GitRepository::staged_diff(std::size_t max_chars) const {
  // Diff of HEAD tree vs index: reflects everything currently staged,
  // including newly added files after a stage_all. When HEAD is unborn
  // (no commits yet) the diff is the whole index against an empty tree.
  git_tree* head_tree = nullptr;
  std::unique_ptr<git_tree, decltype(&git_tree_free)> head_tree_holder(nullptr, git_tree_free);
  git_oid head_id;
  if (git_reference_name_to_id(&head_id, impl_->repository, "HEAD") == 0) {
    git_commit* head_commit = nullptr;
    check(git_commit_lookup(&head_commit, impl_->repository, &head_id), "读取 HEAD 提交");
    std::unique_ptr<git_commit, decltype(&git_commit_free)> head_holder(head_commit, git_commit_free);
    check(git_commit_tree(&head_tree, head_commit), "读取 HEAD 树");
    head_tree_holder.reset(head_tree);
  }

  git_index* index = nullptr;
  check(git_repository_index(&index, impl_->repository), "打开暂存区");
  std::unique_ptr<git_index, decltype(&git_index_free)> index_holder(index, git_index_free);

  git_diff* diff = nullptr;
  check(git_diff_tree_to_index(&diff, impl_->repository, head_tree, index, nullptr), "生成暂存差异");
  std::unique_ptr<git_diff, decltype(&git_diff_free)> diff_holder(diff, git_diff_free);
  return diff_to_text(diff, max_chars);
}

std::string GitRepository::working_diff(std::size_t max_chars) const {
  git_tree* head_tree = nullptr;
  std::unique_ptr<git_tree, decltype(&git_tree_free)> head_tree_holder(nullptr, git_tree_free);
  git_oid head_id;
  if (git_reference_name_to_id(&head_id, impl_->repository, "HEAD") == 0) {
    git_commit* head_commit = nullptr;
    check(git_commit_lookup(&head_commit, impl_->repository, &head_id), "读取 HEAD 提交");
    std::unique_ptr<git_commit, decltype(&git_commit_free)> head_holder(head_commit, git_commit_free);
    check(git_commit_tree(&head_tree, head_commit), "读取 HEAD 树");
    head_tree_holder.reset(head_tree);
  }

  git_diff_options options = GIT_DIFF_OPTIONS_INIT;
  options.flags = GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_SHOW_UNTRACKED_CONTENT | GIT_DIFF_RECURSE_UNTRACKED_DIRS;
  // Compare HEAD directly with the worktree. This yields the final contents
  // regardless of whether an individual file is staged, unstaged, or new.
  git_diff* diff = nullptr;
  check(git_diff_tree_to_workdir(&diff, impl_->repository, head_tree, &options), "生成工作区差异");
  std::unique_ptr<git_diff, decltype(&git_diff_free)> diff_holder(diff, git_diff_free);
  return diff_to_text(diff, max_chars);
}

std::string GitRepository::commit_diff(const std::string& commit_id, std::size_t max_chars) const {
  git_oid oid;
  if (git_oid_fromstr(&oid, commit_id.c_str()) != 0) {
    // Allow short ids via revparse (handles prefixes and "HEAD~1" specs).
    git_object* resolved = nullptr;
    check(git_revparse_single(&resolved, impl_->repository, commit_id.c_str()), "解析提交号");
    std::unique_ptr<git_object, decltype(&git_object_free)> resolved_holder(resolved, git_object_free);
    git_oid_cpy(&oid, git_object_id(resolved));
  }
  git_commit* commit = nullptr;
  check(git_commit_lookup(&commit, impl_->repository, &oid), "读取提交");
  std::unique_ptr<git_commit, decltype(&git_commit_free)> commit_holder(commit, git_commit_free);

  git_commit* parent = nullptr;
  const git_commit* parents[1] = {nullptr};
  std::size_t parent_count = 0;
  if (git_commit_parentcount(commit) > 0) {
    check(git_commit_parent(&parent, commit, 0), "读取父提交");
    parents[0] = parent;
    parent_count = 1;
  }
  std::unique_ptr<git_commit, decltype(&git_commit_free)> parent_holder(parent, git_commit_free);

  git_tree* commit_tree = nullptr;
  check(git_commit_tree(&commit_tree, commit), "读取提交树");
  std::unique_ptr<git_tree, decltype(&git_tree_free)> commit_tree_holder(commit_tree, git_tree_free);

  git_diff* diff = nullptr;
  if (parent_count == 1) {
    git_tree* parent_tree = nullptr;
    check(git_commit_tree(&parent_tree, parent), "读取父提交树");
    std::unique_ptr<git_tree, decltype(&git_tree_free)> parent_tree_holder(parent_tree, git_tree_free);
    check(git_diff_tree_to_tree(&diff, impl_->repository, parent_tree, commit_tree, nullptr), "生成提交差异");
  } else {
    check(git_diff_tree_to_tree(&diff, impl_->repository, nullptr, commit_tree, nullptr), "生成提交差异");
  }
  std::unique_ptr<git_diff, decltype(&git_diff_free)> diff_holder(diff, git_diff_free);
  return diff_to_text(diff, max_chars);
}

}  // namespace gitx
