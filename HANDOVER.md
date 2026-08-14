# gitx 开发交接（2026-08-15 · v1.0.0 已发布）

## 当前状态

`gitx` 1.0.0 已定稿：C++20 跨平台 Git CLI，使用 libgit2 v1.8.4，提供中文向导、团队 TOML 规则、分支/提交/合并/变基/远端操作，以及带完整性校验与自动清理的自解压交付包。本工作区已初始化为 git 仓库（初始提交 `7a1bd0a`）。

## v1.0.0 完成的工作（相对 0.1.0 新增）

- **联网能力**：`CMakeLists.txt` 新增 `GITX_ENABLE_SSH` / `GITX_ENABLE_HTTPS`（默认 ON）。macOS 用系统 SecureTransport + Homebrew libssh2；Linux 需 libssl-dev/libssh2-1-dev；Windows 用 WinHTTP + libssh2。`sync fetch/pull/push`、`start clone`、`sync publish` 均已实测通过（本地裸仓库往返）。
- **`integrate rebase <分支>`**：libgit2 `git_rebase_*` 实现；冲突沿用 merge 的 o/t/m/q 逐块问答；`GIT_EAPPLIED`（空补丁，如内容与上游重复）自动跳过，等价 `git rebase --empty=drop`。
- **`--version` / `-V`**：输出 `gitx 1.0.0 (libgit2 1.8.4)`（`include/gitx/version.hpp` + CMake 注入 `GITX_VERSION`）。
- **bundle 健壮性**（格式升级 `GITXBND3`）：
  - 包尾 CRC-32，解压前校验，篡改/损坏即拒绝（已实测翻转 payload 字节被拦截）。
  - 临时目录无论成功/失败/异常一律清理（try/catch + RAII）。
  - 递归遍历显式跳过 `.git` 目录（修复在 git 仓库内打包失败的问题）。
- **自动化测试扩充至 3 个目标**：`gitx.config`、`gitx.repository`（init/commit/branch switch 索引同步/merge 冲突 o 与 t/rebase 空补丁跳过/stash/tag）、`gitx.bundle`（create/extract/launch 往返 + 非 bundle 文件返回 false；入口用原生可执行文件 + 环境变量标记，跨平台，Windows 亦可跑）。
- **文档**：README 全面更新（三平台构建、团队规范、bundle 用法）、新增 CHANGELOG.md。
- **CI**：`.github/workflows/ci.yml`，GitHub Actions 三平台矩阵（ubuntu/macos/windows），含各自网络依赖安装。**尚未在任何托管平台启用**——需要把仓库推到 GitHub/Gitee 后才会跑。

## 本机（macOS）构建命令

```bash
brew install libssh2          # SSH 传输；HTTPS 用系统 SecureTransport
cmake -S . -B mac-build -DBUILD_TESTING=ON
cmake --build mac-build --parallel 8
ctest --test-dir mac-build --output-on-failure
```

`local-build`、`cmake-build`、`verified-build`、`build` 是 Windows/MinGW 时代目录（缓存 `D:/mingw64` 路径），不可复用；`mac-build` 是本机原生构建。

## v1.0.0 发布产物

- `release/gitx-v1.0.0-macos-arm64`：自解压发布包（约 129MB，含源码 + 自包含运行时）。已实测：复制到其他目录后 `--version` / `start` / `save` / `status` 全部正常，临时目录自动清理。
- 运行时自包含做法：`release/dist/` 内 gitx + libgitx_core + libgit2.1.8 + libssh2.1 全部用 `install_name_tool` 改写为 `@loader_path` 相对引用，并用 `codesign --force --sign -` ad-hoc 签名（macOS 修改二进制后必须重签，否则 SIGKILL）。
- 生成命令：`./mac-build/gitx bundle create release/gitx-v1.0.0-macos-arm64 gitx release/dist`（须在仓库根目录运行）。
- 129MB 主要来自打包全部源码（含 third_party/libgit2 源码树约 100MB）。如后续要瘦身，可在 bundle 时排除 `third_party` 或用更小的 source 目录。

## 已知限制 / 后续优先项

- **CI 未激活**：仓库尚未推送至任何托管平台；推送后 `.github/workflows/ci.yml` 才会在 push/PR 时跑三平台构建+测试。Windows/Linux 的编译正确性目前只靠 CI 配置保证，本机未实测。
- HTTPS 仅支持临时环境变量 `GITX_HTTPS_USER` / `GITX_HTTPS_TOKEN`，尚未实现系统凭据库接入（macOS Keychain / Windows Credential Manager）。
- bundle 仍由用户显式列出运行时目录；尚未自动扫描依赖的 DLL/.so/.dylib（不过 v1.0.0 发布流程已手工完成等价工作）。
- bundle 无数字签名；macOS 上分发仍需用户绕过 Gatekeeper（ad-hoc 签名不足以通过公证）。
- `integrate rebase` 为"整段变基到目标分支"语义，未实现交互式 reword/squash。
- 无日志/调试开关；错误信息为中文硬编码，未做 i18n 框架。
- 测试覆盖良好但非穷尽：fetch/pull/push 依赖本地裸仓库手工验证，未纳入自动化测试（需要 `git` 命令，CI 上有）。

## 关键文件

- `CMakeLists.txt`：依赖解析、SSH/HTTPS 开关、构建与测试目标。
- `src/main.cpp`：CLI 路由与交互（含 `--version`）。
- `src/git_repository.cpp`：libgit2 适配层（含 rebase、fast-forward 辅助）。
- `src/bundle.cpp`：自解压包格式 `GITXBND3`、压缩、CRC 校验、提取、启动与清理。
- `src/config.cpp`：最小 TOML 读取（含转义解码）与团队规范。
- `tests/`：config / git_repository / bundle 三个测试目标。
- `README.md`、`CHANGELOG.md`：用户说明与版本历史。
- `.github/workflows/ci.yml`：三平台 CI 矩阵（待托管激活）。
