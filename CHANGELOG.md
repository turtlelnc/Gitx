# 更新日志

本文件记录 gitx 各版本的变更。格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### 新增

- `config user show|set|temporary`：可在 Gitx 内设置当前仓库署名。临时设备署名包含可读的系统/架构标签与短码，必须显式启用，之后不会自动重写历史。
- `save --all` 与 `save <文件...> -- <提交信息>`：分别支持全量和选择性暂存；普通 `save` 只提交已有暂存区。
- `revert <提交号>`：以反向提交安全回退普通提交，不改写已分享历史；回退冲突会在修改仓库前拒绝。
- `sync set-origin <URL>`：明确修改远端地址；重复 `sync publish` 在 URL 相同情况下可安全复用。

### 修复

- 未配置完整身份时不再创建 `gitx user` 提交，而是给出 Gitx 内置的配置指引。
- 克隆服务器 HEAD 错指空分支时，自动恢复唯一分支或 `main`，避免创建无关根历史。
- `doctor` 区分“存在 SSH 私钥文件”和“SSH agent 实际加载密钥”。
- `status` 改为可读的已暂存/未暂存状态；同步反馈明确显示当前分支与远端分支。

### 移除

- 移除与团队 Git 协作主线无关的自解压 `bundle` 功能及其专用 zlib 依赖。
- 移除维护成本高、能力有限的内置 TUI 与文本编辑器；冲突手动编辑改用用户熟悉的系统编辑器。
- 移除语义模糊的 `open`；仓库路径和当前分支改由 `status` 统一展示。
- 移除只有“创建”能力、无法形成完整生命周期的 `tag` 包装，标签管理暂用原生 Git。

## [1.0.0] - 2026-08-15

首个正式版本。

### 新增

- 联网能力：启用 libgit2 的 SSH（libssh2）与 HTTPS 传输。macOS 使用系统 SecureTransport，Linux 使用 OpenSSL，Windows 使用 WinHTTP。`sync fetch/pull/push` 与 `start clone` 现已可用。
- `integrate rebase <分支>`：把当前分支变基到目标分支之上；冲突时沿用 merge 的逐块问答（o/t/m/q）；内容重复的提交自动跳过（等价于 `git rebase --empty=drop`）。
- `--version` / `-V`：输出版本号与 libgit2 版本。
- bundle 完整性校验：包尾写入 CRC-32，解压前校验，防篡改与损坏。
- bundle 临时目录自动清理：无论启动成功、失败或包损坏，解压目录都会在退出后删除。
- 自动化测试扩充至三个目标：配置解析、仓库集成（init/commit/branch switch/merge 冲突/rebase/stash/tag）、bundle 往返（create/extract/launch/完整性）。
- CMake 选项 `GITX_ENABLE_SSH` / `GITX_ENABLE_HTTPS`，默认开启，可关闭以跳过网络依赖。

### 修复

- 切换分支后索引残留旧分支内容：`branch switch` 与 fast-forward 合并/拉取现在先检出目标树再移动引用，`git status` 不再出现虚假的暂存改动。
- `gitx start <新目录>` 无法创建目录：初始化时设置 `GIT_REPOSITORY_INIT_MKPATH`。
- 新建仓库（尚无提交）时 `open` 报错：现在正确返回初始分支名。
- TOML 读取器未解码字符串转义，导致默认提交正则 `\\(...\\)` 加载后无法匹配：现已解码 `\\`、`\"`、`\n` 等转义。
- bundle 不保存文件权限，解压后入口失去可执行位：每文件头部记录权限模式并在提取时恢复（包格式升级为 `GITXBND3`，旧包不再识别）。

## [0.1.0] - 2026-08-14

开发迭代版本（未发布）。

- 创建 CMake 工程、MIT 许可证、README、配置单元测试。
- 实现 `start`、`open`、`status`、`save`、`history`、`branch`、`integrate merge`、`sync`、`stash`、`tag`、`config` 等 CLI 入口。
- 实现 `.gitx/config.toml` 团队规范：提交格式、分支前缀、合并前工作区检查、命令别名。
- 实现 libgit2 仓库操作层：初始化、克隆、状态、暂存、提交、分支、标签、stash、合并冲突逐块问答。
- 新增 `bundle create` 自解压交付包（初版，无完整性校验与清理）。
- 本地 libgit2 v1.8.4 编译（SSH/HTTPS 关闭）。
