# gitx

`gitx` 是一个中文优先、面向小团队规范协作的跨平台 Git 命令行工具。它读写标准 Git 仓库，使用 `.gitx/config.toml` 将提交、分支和合并规范随仓库共享。

## 功能

- 中文交互向导（不带参数运行）与完整的命令行路由
- 仓库操作：`start`（初始化/克隆）、`status`、`save`、`history`、`revert`
- 分支与合并：`branch new|switch`、`integrate merge`、`integrate rebase`（冲突时逐文件选择 o/t/m/q）
- 同步：`sync fetch|pull|push|publish`（SSH 走系统 agent，HTTPS 走环境变量）
- 临时暂存：`stash save|pop`
- 团队规范：`.gitx/config.toml` 定义提交格式、分支前缀、合并前工作区检查，以及中文命令别名

## 构建

需要 CMake 3.24+ 与支持 C++20 的编译器。主要第三方依赖（libgit2、libssh2、curl）均**已 vendor 到 `third_party/`**，克隆仓库即可离线构建，无需联网下载这些依赖。

联网能力（SSH/HTTPS）默认开启：macOS 使用系统 SecureTransport；Linux 需要 OpenSSL 开发包（libssh2 加密后端）；Windows 使用 WinHTTP + 系统 OpenSSH。不需要联网功能时可用 `-DGITX_ENABLE_SSH=OFF -DGITX_ENABLE_HTTPS=OFF` 关闭。

### macOS

```bash
cmake -S . -B mac-build -DBUILD_TESTING=ON
cmake --build mac-build --parallel 8
ctest --test-dir mac-build --output-on-failure
```

### Linux

```bash
sudo apt install libssl-dev   # Debian/Ubuntu 示例（libssh2 加密后端所需）
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Windows（MinGW）

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

## 快速开始

```text
gitx start demo
cd demo
gitx config user set "你的名字" "you@example.com"
gitx save --all "feat(cli): first commit"
gitx revert <要回退的提交号>      # 创建反向提交，不改写历史
gitx branch new feature/login
gitx status
```

运行 `gitx --help` 查看命令；`gitx --version` 查看版本；不带参数则打开中文交互向导。

## 团队规范（.gitx/config.toml）

`gitx start` 会自动生成默认配置，可提交到仓库与团队共享：

```toml
[commit]
template = "type(scope): summary"
pattern = "^(feat|fix|docs|refactor|test|chore)(\\([^)]+\\))?: .{1,72}$"
require_issue = false

[branch]
default = "main"
allowed_prefixes = ["feature/", "fix/", "docs/", "chore/"]

[merge]
protected_targets = ["main"]
require_clean_worktree = true

[aliases]
# "保存" = "save"
```

提交信息与分支名不符合规则时 `gitx` 会拒绝操作；首次提交前还需用 `gitx config user set <姓名> <邮箱>` 设置本仓库署名。急需本地试验时可显式运行 `gitx config user temporary` 创建形如 `设备-mac-arm64-代码` 的临时署名；它只包含系统/架构标签和短码，不包含主机名、内网地址或硬件序列号，也不会在以后自动重写已推送历史。`[aliases]` 支持团队与个人（`~/.config/gitx/config.toml`）的中文命令别名。

`save` 默认只提交已暂存的改动：用 `gitx save --all <提交信息>` 暂存全部，或 `gitx save <文件...> -- <提交信息>` 只暂存指定文件。`save --ai` 读取已暂存内容；`save --ai --all` 会在确认后显式暂存全部工作区改动。

## 安全回退历史

```text
gitx history
gitx revert <提交号>
```

`revert` 不会移动或删除已有提交，而是创建一条反向提交，因此可以安全地回退已经推送、且可能被他人拉取的普通提交。执行前工作区必须干净；合并提交、根提交以及会产生冲突的回退会被拒绝，仓库不会被修改。

## AI 工具链

gitx 内置 AI 助手（默认构建会使用仓库内 vendor 的 libcurl）：

```text
gitx save --ai          # 对已暂存内容生成符合规范的提交信息，确认后提交
gitx save --ai --all    # 明确允许 AI 基于全部工作区改动生成并提交
gitx explain <提交号>    # AI 解读一条提交：改了什么、为什么、影响
gitx review --ai        # AI 审查工作区改动，输出问题清单
gitx changelog --ai     # AI 生成相对默认分支的变更日志
gitx pr create --ai     # AI 生成 Pull Request 标题与描述
```

首次使用会提示输入 API 密钥，安全存入系统钥匙串（macOS Keychain / Windows 凭据管理器 / Linux libsecret）；Linux 没有 libsecret 时才回退至权限为 600 的本地文件。也可用 `GITX_AI_KEY` 环境变量。模型在 `.gitx/config.toml` 的 `[ai]` 段配置：

```toml
[ai]
provider = "deepseek"    # deepseek | openai | ollama | custom
# base_url = ""          # 留空用默认（deepseek-chat / gpt-4o-mini / qwen2.5:7b）
# model = ""
```

支持任意 OpenAI 兼容服务（DeepSeek、OpenAI、通义、Ollama 本地等）。详见 [docs/ai-toolchain.md](docs/ai-toolchain.md)。

完整的功能取舍与后续改进优先级见 [docs/feature-review.md](docs/feature-review.md)。

## 扩展脚本

`gitx x` 可运行任意扩展脚本，脚本放在项目 `.gitx/scripts/`（随仓库共享）或 `~/.config/gitx/scripts/`（个人），支持 shell/python/二进制等任何可执行文件：

```text
gitx x list             # 列出可用脚本
gitx x hello 参数...     # 运行脚本（脚本内可用 $GITX_REPO_ROOT 获取仓库路径）
```

合并或变基冲突选择 `m` 时，gitx 会调用 `GIT_EDITOR`、`VISUAL` 或 `EDITOR` 指定的系统编辑器；未配置时使用平台默认编辑器。

## 认证

SSH 远端使用本机 SSH agent，并要求主机密钥已存在于 `~/.ssh/known_hosts`（可用 `ssh-keyscan` 在可信网络中加入）。HTTPS 认证可在当前进程环境中提供 `GITX_HTTPS_USER` 和 `GITX_HTTPS_TOKEN`；应用不会将凭据写入磁盘。

## 测试

```bash
ctest --test-dir mac-build --output-on-failure
```

覆盖：配置解析与校验、仓库集成（初始化/提交/分支切换/合并冲突/变基/暂存）、AI 工具链端到端（本地 mock LLM）。

## 许可

gitx 自身代码采用 MIT 许可证，详见 [LICENSE](LICENSE)。链接或随包分发的第三方组件（libgit2、libssh2、curl、OpenSSL）各自的许可证与版权声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
