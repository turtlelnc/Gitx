# 更新日志

本文件记录 gitx 各版本的变更。格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

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
