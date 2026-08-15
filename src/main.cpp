#include "gitx/command_names.hpp"
#include "gitx/bundle.hpp"
#include "gitx/config.hpp"
#include "gitx/git_repository.hpp"
#include "gitx/ai_client.hpp"
#include "gitx/version.hpp"

#include <git2.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
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

// Cross-platform environment variable helpers.
void setenv_or_override(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

void unsetenv_var(const char* name) {
#ifdef _WIN32
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

void show_help() {
  std::cout << R"(gitx - 面向团队规范的 Git 向导

命令：
  start [目录]                    初始化仓库并创建 .gitx/config.toml
  start clone <URL> <目录>        克隆仓库
  open                            显示当前仓库信息
  status                          显示工作区改动
  save [提交信息]                 暂存全部改动并按团队规则提交
  save --ai                       用 AI 生成符合规范的提交信息
  history [数量]                  查看提交历史
  explain <提交号>                用 AI 解读一条提交
  review --ai                     用 AI 审查工作区改动
  branch [new|switch] [名称]      查看、创建或切换分支
  integrate merge <分支>          合并分支；冲突时逐块选择解决方式
  integrate rebase <分支>         把当前分支变基到目标分支之上
  sync [fetch|pull|push]          与 origin 同步
  sync publish <URL>              关联 origin 并首次推送当前分支
  stash [save|pop] [说明]         保存或恢复暂存
  tag <名称> [说明]               创建标签
  bundle create <文件> <入口> [目录...] 生成含源码、产物和依赖的自解压包
  config init                     创建默认团队配置
  doctor                          检查环境、远端与同步状态
  x <脚本名> [参数...]            运行扩展脚本（.gitx/scripts/ 或 ~/.config/gitx/scripts/）
  x list                          列出可用扩展脚本

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

// Build an AI client, prompting for and storing the API key on first use.
gitx::AiClient make_ai_client(const gitx::TeamConfig& team) {
  const auto config = gitx::resolve_ai_config(team);
  auto key = gitx::AiClient::load_key();
  if (!key.has_value()) {
    std::cout << "首次使用 AI 功能，请输入 API 密钥（输入后将安全保存到系统钥匙串）：\n";
    const auto input = ask("API Key: ");
    if (input.empty()) throw std::runtime_error("未提供 API 密钥，已取消。");
    gitx::AiClient::store_key(input);
    key = input;
  }
  return gitx::AiClient(config, *key);
}

void print_ai_unavailable() {
  std::cout << "AI 功能未启用：当前构建缺少 libcurl。\n"
            << "  安装 libcurl 开发包后重新构建：\n"
            << "    macOS: 系统自带\n"
            << "    Linux: sudo apt install libcurl4-openssl-dev\n"
            << "    Windows: vcpkg install curl\n";
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
      if (argc > 2 && std::string(argv[2]) == "--ai") {
        if (!gitx::AiClient::available()) { print_ai_unavailable(); return 1; }
        repository.stage_all();
        const auto diff = repository.staged_diff(team_config.ai.max_diff_chars);
        if (diff.empty()) throw std::runtime_error("没有可提交的改动。");
        auto client = make_ai_client(team_config);
        const std::string system_prompt =
            "你是一个严格的 Git 提交信息生成助手。团队规范要求提交信息必须符合以下正则：\n"
            + team_config.commit.pattern + "\n模板：" + team_config.commit.template_text
            + "\n只输出一行提交信息，不要任何解释、引号或多余文字。";
        const std::string user_prompt = "以下是本次改动的 diff：\n\n" + diff;
        std::cout << "AI 正在分析改动并生成提交信息...\n";
        const auto message = client.complete({{"system", system_prompt}, {"user", user_prompt}});
        std::cout << "建议提交信息：\n  " << message << "\n";
        if (const auto issue = ConfigStore::validate_commit(team_config, message)) {
          std::cout << "AI 生成的提交信息不符合团队规则，已拒绝（" << *issue << "）。请重试或手动提交。\n";
          return 1;
        }
        const auto confirm = ask("确认提交？[y/N] ");
        if (confirm != "y" && confirm != "Y") {
          std::cout << "已取消。\n";
          return 0;
        }
        repository.stage_all();
        repository.commit(message, team_config);
        std::cout << "已创建提交。\n";
      } else {
        const auto message = argc > 2 ? join(2, argc, argv) : ask("提交信息（" + team_config.commit.template_text + "）：");
        repository.stage_all();
        repository.commit(message, team_config);
        std::cout << "已创建提交。\n";
      }
    } else if (command == "explain") {
      if (!gitx::AiClient::available()) { print_ai_unavailable(); return 1; }
      if (argc < 3) throw std::runtime_error("用法：gitx explain <提交号>");
      auto client = make_ai_client(team_config);
      const auto diff = repository.commit_diff(argv[2], team_config.ai.max_diff_chars);
      const std::string system_prompt =
          "你是 Git 历史解读助手。用简洁的中文解释一条提交：改了什么、为什么、影响范围、潜在风险。";
      std::cout << "AI 正在解读提交 " << argv[2] << " ...\n";
      const auto explanation = client.complete({{"system", system_prompt}, {"user", "提交的 diff：\n\n" + diff}});
      std::cout << explanation << "\n";
    } else if (command == "review") {
      if (!gitx::AiClient::available()) { print_ai_unavailable(); return 1; }
      auto client = make_ai_client(team_config);
      repository.stage_all();
      const auto diff = repository.staged_diff(team_config.ai.max_diff_chars);
      if (diff.empty()) throw std::runtime_error("没有可审查的改动。");
      const std::string system_prompt =
          "你是代码审查助手。审查以下 diff，用中文列出问题清单：每项标注严重程度（高/中/低）、相关文件、具体问题和修改建议。"
          "没有问题时明确说'未发现问题'。";
      std::cout << "AI 正在审查改动...\n";
      const auto review = client.complete({{"system", system_prompt}, {"user", diff}});
      std::cout << review << "\n";
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
        std::cout << "已发布到服务器。其他成员可用以下命令加入协作：\n"
                  << "  gitx start clone " << argv[3] << " 项目名\n";
      } else throw std::runtime_error("未知同步操作：" + action);
      if (action != "publish") std::cout << "同步完成。\n";
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
    } else if (command == "doctor") {
      int problems = 0;
      auto ok = [&problems](bool good, const std::string& what) {
        std::cout << (good ? "  [通过] " : "  [异常] ") << what << "\n";
        if (!good) ++problems;
      };

      std::cout << "gitx 环境诊断\n";
      ok(true, "gitx " + std::string(gitx::kVersion) + "（libgit2 " LIBGIT2_VERSION "）");

      if (repository.empty_repository()) {
        ok(false, "仓库还没有任何提交，请先 gitx save 创建首次提交。");
      } else {
        ok(true, "仓库存在提交，当前分支：" + repository.current_branch());
      }

      const auto url = repository.remote_url();
      if (!url.has_value()) {
        ok(false, "未配置远端 origin。首次发布请运行：gitx sync publish <服务器URL>");
      } else {
        ok(true, "远端 origin 指向：" + *url);
        const auto [ahead, behind] = repository.divergence_from_remote();
        ok(ahead == 0, "本地有 " + std::to_string(ahead) + " 个未推送的提交" + (ahead == 0 ? "，已全部同步" : "，运行 gitx sync push 推送"));
        ok(behind == 0, "本地落后远端 " + std::to_string(behind) + " 个提交" + (behind == 0 ? "" : "，运行 gitx sync pull 更新"));
      }

      const auto identity = repository.identity_name();
      ok(!identity.empty(), std::string("已配置提交身份：") + (identity.empty() ? "未配置，提交会显示为 gitx user。运行 git config user.name \"你的名字\"" : identity));

#ifdef _WIN32
      const char* profile = std::getenv("USERPROFILE");
#else
      const char* profile = std::getenv("HOME");
#endif
      const auto key_dir = profile != nullptr ? fs::path(profile) / ".ssh" : fs::path();
      bool has_key = false;
      if (!key_dir.empty() && fs::is_directory(key_dir)) {
        for (const auto& entry : fs::directory_iterator(key_dir)) {
          const auto name = entry.path().filename().string();
          if ((name == "id_ed25519" || name == "id_rsa" || name == "id_ecdsa") && entry.is_regular_file()) {
            has_key = true;
            break;
          }
        }
      }
      ok(has_key, std::string("SSH 密钥：") + (has_key ? "已找到" : "未找到（使用 HTTPS 协议可忽略；SSH 推送前需先生成密钥并配置到服务器）"));

      std::cout << (problems == 0 ? "\n诊断完成：一切正常。\n" : "\n诊断完成：发现 " + std::to_string(problems) + " 个问题，请按提示处理。\n");
      if (problems > 0) return 1;
    } else if (command == "x") {
      // User extension scripts: run an executable from the project's
      // .gitx/scripts or the user's ~/.config/gitx/scripts directory.
      const auto find_script = [&](const std::string& name) -> std::optional<fs::path> {
        std::vector<fs::path> roots{root / ".gitx" / "scripts"};
        if (const char* config_home = std::getenv("XDG_CONFIG_HOME")) {
          roots.push_back(fs::path(config_home) / "gitx" / "scripts");
        } else if (const char* home = std::getenv("HOME")) {
          roots.push_back(fs::path(home) / ".config" / "gitx" / "scripts");
        }
        for (const auto& dir : roots) {
          if (!fs::is_directory(dir)) continue;
          // Exact name first, then basename match (hello matches hello.sh).
          const auto exact = dir / name;
          if (fs::is_regular_file(exact) && (fs::status(exact).permissions() & fs::perms::owner_exec) != fs::perms::none) {
            return exact;
          }
          for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const auto perms = fs::status(entry.path()).permissions();
            if ((perms & fs::perms::owner_exec) == fs::perms::none) continue;
            if (entry.path().stem().string() == name || entry.path().filename().string() == name) {
              return entry.path();
            }
          }
        }
        return std::nullopt;
      };

      if (argc > 2 && std::string(argv[2]) == "list") {
        std::vector<fs::path> roots{root / ".gitx" / "scripts"};
        if (const char* config_home = std::getenv("XDG_CONFIG_HOME")) {
          roots.push_back(fs::path(config_home) / "gitx" / "scripts");
        } else if (const char* home = std::getenv("HOME")) {
          roots.push_back(fs::path(home) / ".config" / "gitx" / "scripts");
        }
        std::cout << "可用扩展脚本：\n";
        bool any = false;
        for (const auto& dir : roots) {
          if (!fs::is_directory(dir)) continue;
          for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const auto perms = fs::status(entry.path()).permissions();
            if ((perms & fs::perms::owner_exec) == fs::perms::none) continue;
            std::cout << "  " << entry.path().filename().string()
                      << (dir == root / ".gitx" / "scripts" ? "  (项目)" : "  (用户)") << "\n";
            any = true;
          }
        }
        if (!any) std::cout << "  （无。把可执行脚本放入 .gitx/scripts/ 或 ~/.config/gitx/scripts/）\n";
        return 0;
      }

      if (argc < 3) throw std::runtime_error("用法：gitx x <脚本名> [参数...]，或 gitx x list 查看可用脚本。");
      const auto script = find_script(argv[2]);
      if (!script.has_value()) {
        throw std::runtime_error("找不到可执行脚本 '" + std::string(argv[2]) +
                                 "'。请放入 .gitx/scripts/ 或 ~/.config/gitx/scripts/ 并赋予执行权限。");
      }
      std::ostringstream command;
#ifdef _WIN32
      command << "\"" << script->string() << "\"";
#else
      command << "'" << script->string() << "'";
#endif
      for (int index = 3; index < argc; ++index) {
        command << " \"" << argv[index] << "\"";
      }
      // The repository root is passed as the first environment variable so
      // scripts know where they run.
      const std::string previous = std::getenv("GITX_REPO_ROOT") != nullptr ? std::getenv("GITX_REPO_ROOT") : "";
      setenv_or_override("GITX_REPO_ROOT", root.string());
      const auto exit_code = std::system(command.str().c_str());
      if (!previous.empty()) setenv_or_override("GITX_REPO_ROOT", previous);
      else unsetenv_var("GITX_REPO_ROOT");
      return exit_code == 0 ? 0 : 1;
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
