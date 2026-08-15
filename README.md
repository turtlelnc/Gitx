# gitx

`gitx` 是一个中文优先、面向小团队规范协作的跨平台 Git 命令行工具。它读写标准 Git 仓库，使用 `.gitx/config.toml` 将提交、分支和合并规范随仓库共享。

## 功能

- 中文交互向导（不带参数运行）与完整的命令行路由
- 仓库操作：`start`（初始化/克隆）、`open`、`status`、`save`、`history`
- 分支与合并：`branch new|switch`、`integrate merge`、`integrate rebase`（冲突时逐块选择 o/t/m/q）
- 同步：`sync fetch|pull|push|publish`（SSH 走系统 agent，HTTPS 走环境变量）
- 暂存与标签：`stash save|pop`、`tag`
- 团队规范：`.gitx/config.toml` 定义提交格式、分支前缀、合并前工作区检查，以及中文命令别名
- 自解压交付包：`bundle create` 将源码、产物与依赖压入单个可执行文件

## 构建

需要 CMake 3.24+ 与支持 C++20 的编译器。所有第三方依赖（libgit2、libssh2、curl、zlib）均**已 vendor 到 `third_party/`**，克隆仓库即可离线构建，无需联网下载或安装第三方库。

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
gitx save "feat(cli): first commit"
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

提交信息与分支名不符合规则时 `gitx` 会拒绝操作；`[aliases]` 支持团队与个人（`~/.config/gitx/config.toml`）的中文命令别名。

## 自解压交付包

`bundle` 会把当前仓库的源代码（不含 `.git`）、运行产物和依赖目录压缩进一个可执行的自解压包。接收者直接运行该文件，`gitx` 会校验包完整性、解压至临时目录、启动入口程序，随后自动清理临时目录。

```text
gitx bundle create demo-bundle.exe bin/demo.exe dist
```

入口路径相对于所提供的运行时目录。例如上例会把整个仓库作为 `source/`，把 `dist/` 放入 `runtime/dist/`；入口应相应写成 `dist/demo.exe`。自解压包只应在与构建时相同的操作系统与 CPU 架构上运行。

## AI 工具链

gitx 内置 AI 助手（需要 libcurl，macOS/Linux 系统自带；Windows 需 vcpkg 安装）：

```text
gitx save --ai          # AI 生成符合团队规范的提交信息，确认后提交
gitx explain <提交号>    # AI 解读一条提交：改了什么、为什么、影响
gitx review --ai        # AI 审查工作区改动，输出问题清单
gitx changelog --ai     # AI 生成相对默认分支的变更日志
gitx pr create --ai     # AI 生成 Pull Request 标题与描述
```

首次使用会提示输入 API 密钥，安全存入系统钥匙串（macOS Keychain / Windows 凭据管理器 / Linux libsecret），也可用 `GITX_AI_KEY` 环境变量。模型在 `.gitx/config.toml` 的 `[ai]` 段配置：

```toml
[ai]
provider = "deepseek"    # deepseek | openai | ollama | custom
# base_url = ""          # 留空用默认（deepseek-chat / gpt-4o-mini / qwen2.5:7b）
# model = ""
```

支持任意 OpenAI 兼容服务（DeepSeek、OpenAI、通义、Ollama 本地等）。详见 [docs/ai-toolchain.md](docs/ai-toolchain.md)。

## 扩展脚本

`gitx x` 可运行任意扩展脚本，脚本放在项目 `.gitx/scripts/`（随仓库共享）或 `~/.config/gitx/scripts/`（个人），支持 shell/python/二进制等任何可执行文件：

```text
gitx x list             # 列出可用脚本
gitx x hello 参数...     # 运行脚本（脚本内可用 $GITX_REPO_ROOT 获取仓库路径）
```

## 认证

SSH 远端使用本机 SSH agent；首次连接未知服务器会显示指纹并确认。HTTPS 认证可在当前进程环境中提供 `GITX_HTTPS_USER` 和 `GITX_HTTPS_TOKEN`；应用不会将凭据写入磁盘。

## 测试

```bash
ctest --test-dir mac-build --output-on-failure
```

覆盖：配置解析与校验、仓库集成（初始化/提交/分支切换/合并冲突/变基/暂存/标签）、bundle 往返（创建/提取/启动/完整性）、AI 工具链端到端（本地 mock LLM）。

## 许可

gitx 自身代码采用 MIT 许可证，详见 [LICENSE](LICENSE)。链接或随包分发的第三方组件（libgit2、zlib、libssh2、OpenSSL）各自的许可证与版权声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
