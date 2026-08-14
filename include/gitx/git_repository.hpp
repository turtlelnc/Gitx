#pragma once

#include "gitx/config.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace gitx {

struct StatusItem {
  std::string path;
  std::string state;
};

struct HistoryItem {
  std::string id;
  std::string summary;
  std::string author;
};

class GitRepository {
 public:
  explicit GitRepository(const std::filesystem::path& path);
  ~GitRepository();
  GitRepository(const GitRepository&) = delete;
  GitRepository& operator=(const GitRepository&) = delete;

  static void init(const std::filesystem::path& path, const std::string& initial_branch);
  static void clone(const std::string& url, const std::filesystem::path& path);

  std::filesystem::path workdir() const;
  std::vector<StatusItem> status() const;
  std::vector<HistoryItem> history(std::size_t limit = 20) const;
  std::string current_branch() const;
  std::vector<std::string> branches() const;
  void stage_all();
  void commit(const std::string& message, const TeamConfig& config);
  void create_and_switch_branch(const std::string& name, const TeamConfig& config);
  void checkout_branch(const std::string& name);
  void create_tag(const std::string& name, const std::string& message);
  void stash_save(const std::string& message);
  void stash_pop();
  void merge_branch_interactive(const std::string& name, const TeamConfig& config);
  void rebase_onto(const std::string& name, const TeamConfig& config);
  void fetch(const std::string& remote_name = "origin");
  void pull(const std::string& remote_name = "origin");
  void push(const std::string& remote_name = "origin");
  void add_remote(const std::string& name, const std::string& url);

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace gitx
