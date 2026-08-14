# gitx 开发交接（2026-08-14）

## 当前目标

`gitx` 是 C++20 跨平台 Git CLI：使用 libgit2 操作标准 Git 仓库，提供中文向导、团队 TOML 规则、分支/提交/合并/远端操作，以及内置自解压交付包。

## 已完成的工作

- 创建 CMake 工程、MIT 许可证、README、配置单元测试。
- 实现 `start`、`open`、`status`、`save`、`history`、`branch`、`integrate merge`、`sync`、`stash`、`tag`、`config` 等 CLI 入口。
- 实现 `.gitx/config.toml`：提交格式、分支前缀、合并前工作区检查，以及团队/个人一对一命令别名读取。
- 实现 libgit2 仓库操作层：初始化、克隆、状态、暂存、提交、分支、标签、stash、合并冲突逐块问答、fetch/pull/push。
- 新增 `gitx bundle create <输出文件> <运行时入口相对路径> <产物或依赖目录...>`：把当前仓库源码（不含 `.git`）和指定运行时目录压缩到 gitx 可执行文件尾部。该 SFX 启动时会解压到临时目录并启动入口。
- 用户已下载并解压 libgit2 v1.8.4 至 `third_party/libgit2-1.8.4`；CMake 现优先使用此本地目录，不再需要联网下载它。

## 本机依赖与构建命令

- 编译器：MinGW g++ 15.2.0（C++20）。
- 构建系统：CMake 4.2、MinGW Makefiles。
- 本地依赖：`third_party/libgit2-1.8.4`，其内置 zlib 用于 bundle 压缩。

```powershell
cmake -S . -B local-build -G "MinGW Makefiles" -DBUILD_TESTING=ON
cmake --build local-build --parallel 4
ctest --test-dir local-build --output-on-failure
```

若迁移到新设备，复制整个工作区（至少保留 `third_party/libgit2-1.8.4`），安装 CMake 3.24+ 和带 C++20 的编译器后执行以上命令。

## 最后一次构建状态

- 先前的链接失败根因已修复：libgit2 的 `libgit2` 是 OBJECT target，真正可链接 target 是 `libgit2package`。
- 已在 macOS（Apple clang 21、CMake 4.1）上从零验证：`cmake -S . -B mac-build -DBUILD_TESTING=ON` 配置成功，`cmake --build mac-build --parallel 8` 全部构建成功（`gitx`、`gitx_core`、`gitx_tests`），`ctest` 全部通过。
- 修复了三个在此次验证中发现的问题：
  1. TOML 读取器 `unquote` 未解码基本字符串转义，导致默认 `commit.pattern`（写入为 `\\([^)]+\\)`）加载后变成双反斜杠、永远无法匹配 `feat(cli): ...`。已在 `src/config.cpp` 中补充 TOML 转义解码（`\\`、`\"`、`\n`、`\t`、`\r`、`\b`、`\f`）。
  2. `GitRepository::init` 未设置 `GIT_REPOSITORY_INIT_MKPATH`，`gitx start <新目录>` 无法创建目录。已在 `src/git_repository.cpp` 中设置该标志。
  3. `checkout_branch` 先 `set_head` 再 `checkout_head`，切换分支后索引残留旧分支内容（`git status` 显示暂存改动）。已改为先 `git_checkout_tree`（目标分支）再 `set_head`，索引与工作区均正确更新。
  4. bundle 格式未保存文件权限，解压后入口脚本失去可执行位。已把每文件头部改为 `长度+权限模式+原始大小+压缩大小`，提取时恢复权限；魔数从 `GITXBND1` 升为 `GITXBND2`（旧包不再识别）。
- 冒烟测试全部通过：start/save/status/history/branch（new+switch）/integrate merge（冲突选择 o 与 t 两条路径）/stash save+pop/tag/config/open/bundle create+运行解压。

## 本机（macOS）构建命令

```bash
cmake -S . -B mac-build -DBUILD_TESTING=ON
cmake --build mac-build --parallel 8
ctest --test-dir mac-build --output-on-failure
```

`local-build`、`cmake-build`、`verified-build`、`build` 是 Windows/MinGW 时代的目录，含 `D:/mingw64` 缓存路径，迁移到 macOS 后不可直接复用；如需在 Windows 上构建请重新配置。

## 已知限制 / 后续优先项

- 当前本地 libgit2 配置禁用了 SSH 与 HTTPS，因为设备尚未安装 libssh2 / OpenSSL。因此本地仓库功能和 bundle 可编译，联网 Git 操作需要补齐依赖后启用。
- `integrate rebase` 仍未实现；CLI 目前明确只支持 `integrate merge`。
- bundle 目前由用户显式列出运行时产物/依赖目录；尚未自动扫描 DLL、`.so` 或 `.dylib`。
- bundle 解压至临时目录后不清理（已知限制，尚未实现退出后清理策略）；包完整性校验和签名亦未实现。
- HTTPS 当前代码仅支持临时环境变量 `GITX_HTTPS_USER` / `GITX_HTTPS_TOKEN`，还未实现系统凭据库接入。
- 单元测试仅有 `config_tests` 一个目标；git_repository / bundle 尚无自动化测试，仅靠手动冒烟。

## 关键文件

- `CMakeLists.txt`：依赖解析与构建目标。
- `src/main.cpp`：CLI 路由与交互。
- `src/git_repository.cpp`：libgit2 适配层。
- `src/bundle.cpp`：自解压包格式、压缩、提取和启动。
- `src/config.cpp`：最小 TOML 读取与团队规范。
- `README.md`：用户说明。
