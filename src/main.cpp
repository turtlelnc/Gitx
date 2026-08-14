#include "gitx/command_names.hpp"
#include "gitx/bundle.hpp"
#include "gitx/config.hpp"
#include "gitx/git_repository.hpp"
#include "gitx/version.hpp"

#include <git2.h>

#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
using gitx::CommandNames;
using gitx::ConfigStore;
using gitx::GitRepository;

namespace {

std::string ask(const std::string& prompt) {
  std::cout << prompt;
  std::string result;
  std::getline(std::cin, result);
  return result;
}

std::string join(int begin, int argc, char** argv) {
  std::ostringstream output;
  for (int index = begin; index < argc; ++index) {
    if (index != begin) output << ' ';
    output << argv[index];
  }
  return output.str();
}

void show_help() {
  std::cout << R"(gitx - 面向团队规范的 Git 向导

命令：
  start [目录]                    初始化仓库并创建 .gitx/config.toml
  start clone <URL> <目录>        克隆仓库
  open                            显示当前仓库信息
  status                          显示工作区改动
  save [提交信息]                 暂存全部改动并按团队规则提交
  history [数量]                  查看提交历史
  branch [new|switch] [名称]      查看、创建或切换分支
  integrate merge <分支>          合并分支；冲突时逐块选择解决方式
  integrate rebase <分支>         把当前分支变基到目标分支之上
  sync [fetch|pull|push]          与 origin 同步
  sync publish <URL>              关联 origin 并首次推送当前分支
  stash [save|pop] [说明]         保存或恢复暂存
  tag <名称> [说明]               创建标签
  bundle create <文件> <入口> [目录...] 生成含源码、产物和依赖的自解压包
  config init                     创建默认团队配置

不带参数运行会打开命令向导。HTTPS 认证可通过 GITX_HTTPS_USER / GITX_HTTPS_TOKEN
临时提供；gitx 不会将它们写入磁盘。SSH 使用系统 SSH agent。
)";
}

std::string choose_command() {
  std::cout << "\n请选择操作：\n"
            << "  1) 状态  2) 保存提交  3) 历史  4) 分支  5) 合并  6) 同步  7) 暂存  8) 标签\n";
  const auto choice = ask("> ");
  if (choice == "1") return "status";
  if (choice == "2") return "save";
  if (choice == "3") return "history";
  if (choice == "4") return "branch";
  if (choice == "5") return "integrate";
  if (choice == "6") return "sync";
  if (choice == "7") return "stash";
  if (choice == "8") return "tag";
  return "help";
}

}  // namespace

int main(int argc, char** argv) {
  git_libgit2_init();
  try {
    if (argc == 1 && gitx::Bundle::extract_and_launch_if_present(fs::absolute(argv[0]))) {
      git_libgit2_shutdown();
      return 0;
    }
    std::string requested = argc > 1 ? argv[1] : "";
    if (requested == "--version" || requested == "-V") {
      std::cout << "gitx " << gitx::kVersion
                << " (libgit2 " << LIBGIT2_VERSION << ")\n";
      git_libgit2_shutdown();
      return 0;
    }
    if (requested == "--help" || requested == "-h" || requested == "help") {
      show_help();
      git_libgit2_shutdown();
      return 0;
    }

    if (requested == "start") {
      if (argc > 2 && std::string(argv[2]) == "clone") {
        if (argc < 5) throw std::runtime_error("用法：gitx start clone <URL> <目录>");
        GitRepository::clone(argv[3], argv[4]);
        ConfigStore::write_default_team_config(argv[4]);
        std::cout << "已克隆仓库，并创建团队规范文件。\n";
      } else {
        const fs::path path = argc > 2 ? fs::path(argv[2]) : fs::current_path();
        GitRepository::init(path, "main");
        ConfigStore::write_default_team_config(path);
        std::cout << "已初始化仓库（main），并创建 .gitx/config.toml。\n";
      }
      git_libgit2_shutdown();
      return 0;
    }

    GitRepository repository(fs::current_path());
    const auto root = repository.workdir();
    const auto team_config = ConfigStore::load_team(root);
    const CommandNames names(team_config.aliases, ConfigStore::load_user_aliases());
    std::string command = requested.empty() ? choose_command() : names.canonicalize(requested);

    if (command == "open") {
      std::cout << "仓库：" << root.string() << "\n当前分支：" << repository.current_branch() << "\n";
    } else if (command == "status") {
      const auto items = repository.status();
      if (items.empty()) std::cout << "工作区干净。\n";
      for (const auto& item : items) std::cout << "[" << item.state << "] " << item.path << "\n";
    } else if (command == "save") {
      const auto message = argc > 2 ? join(2, argc, argv) : ask("提交信息（" + team_config.commit.template_text + "）：");
      repository.stage_all();
      repository.commit(message, team_config);
      std::cout << "已创建提交。\n";
    } else if (command == "history") {
      const auto limit = argc > 2 ? static_cast<std::size_t>(std::stoul(argv[2])) : 20U;
      for (const auto& item : repository.history(limit)) std::cout << item.id << " " << item.summary << " — " << item.author << "\n";
    } else if (command == "branch") {
      const std::string action = argc > 2 ? argv[2] : "";
      if (action == "new") {
        if (argc < 4) throw std::runtime_error("用法：gitx branch new <名称>");
        repository.create_and_switch_branch(argv[3], team_config);
      } else if (action == "switch") {
        if (argc < 4) throw std::runtime_error("用法：gitx branch switch <名称>");
        repository.checkout_branch(argv[3]);
      } else {
        const auto current = repository.current_branch();
        for (const auto& branch : repository.branches()) std::cout << (branch == current ? "* " : "  ") << branch << "\n";
      }
    } else if (command == "integrate") {
      const auto action = argc > 2 ? std::string(argv[2]) : ask("选择 merge 或 rebase：");
      if (action != "merge" && action != "rebase") throw std::runtime_error("未知 integrate 操作：" + action);
      const auto branch = argc > 3 ? std::string(argv[3]) : ask(action == "merge" ? "要合并的分支：" : "要变基到的分支：");
      if (action == "merge") {
        repository.merge_branch_interactive(branch, team_config);
        std::cout << "合并完成。\n";
      } else {
        repository.rebase_onto(branch, team_config);
        std::cout << "变基完成。\n";
      }
    } else if (command == "sync") {
      const auto action = argc > 2 ? std::string(argv[2]) : ask("选择 fetch、pull、push 或 publish：");
      if (action == "fetch") repository.fetch();
      else if (action == "pull") repository.pull();
      else if (action == "push") repository.push();
      else if (action == "publish") {
        if (argc < 4) throw std::runtime_error("用法：gitx sync publish <URL>");
        repository.add_remote("origin", argv[3]);
        repository.push();
      } else throw std::runtime_error("未知同步操作：" + action);
      std::cout << "同步完成。\n";
    } else if (command == "stash") {
      const auto action = argc > 2 ? std::string(argv[2]) : ask("选择 save 或 pop：");
      if (action == "save") repository.stash_save(argc > 3 ? join(3, argc, argv) : ask("暂存说明："));
      else if (action == "pop") repository.stash_pop();
      else throw std::runtime_error("用法：gitx stash [save|pop] [说明]");
      std::cout << "暂存操作完成。\n";
    } else if (command == "tag") {
      const auto name = argc > 2 ? std::string(argv[2]) : ask("标签名称：");
      const auto message = argc > 3 ? join(3, argc, argv) : ask("标签说明（可留空）：");
      repository.create_tag(name, message);
      std::cout << "已创建标签。\n";
    } else if (command == "bundle") {
      if (argc < 5 || std::string(argv[2]) != "create") {
        throw std::runtime_error("用法：gitx bundle create <输出文件> <运行时入口相对路径> [产物或依赖目录...]");
      }
      std::vector<fs::path> runtime;
      for (int index = 5; index < argc; ++index) runtime.emplace_back(argv[index]);
      if (runtime.empty()) throw std::runtime_error("请至少提供一个产物或依赖目录。");
      gitx::Bundle::create(fs::absolute(argv[0]), argv[3], root, argv[4], runtime);
      std::cout << "已生成自解压包：" << fs::absolute(argv[3]).string() << "\n";
    } else if (command == "config") {
      if (argc > 2 && std::string(argv[2]) == "init") {
        std::cout << (ConfigStore::write_default_team_config(root) ? "已创建团队配置。\n" : "团队配置已存在，未覆盖。\n");
      } else {
        std::cout << "团队配置：" << (root / ".gitx" / "config.toml").string() << "\n"
                  << "个人配置：" << ConfigStore::user_config_path().string() << "\n";
      }
    } else {
      show_help();
      git_libgit2_shutdown();
      return command == "help" ? 0 : 2;
    }
  } catch (const std::exception& error) {
    std::cerr << "错误：" << error.what() << "\n";
    git_libgit2_shutdown();
    return 1;
  }
  git_libgit2_shutdown();
  return 0;
}
