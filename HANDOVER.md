# gitx 开发交接（2026-08-15 · v1.0.0 + AI 工具链 + 全依赖 vendor）

## 当前状态

`gitx` 是 C++20 跨平台 Git CLI：中文向导、团队 TOML 规则、分支/提交/合并/变基/远端操作、AI 工具链、自解压交付包。**所有第三方依赖已 vendor 到 `third_party/`，克隆仓库即可离线构建**。CI 三平台（Windows/macOS/Ubuntu）全绿。仓库托管于 `git@github.com:turtlelnc/Gitx.git`（main 分支）。

## 功能清单

### 核心 Git 操作
- `start`（init/clone）、`open`、`status`、`save`、`history`、`branch`（new/switch）、`tag`、`stash`、`config`
- `integrate merge`：冲突逐块问答（o/t/m/q）
- `integrate rebase <分支>`：变基；空补丁自动跳过（等价 `git rebase --empty=drop`）
- `sync fetch/pull/push/publish`：SSH（libssh2 或 Windows 系统 OpenSSH exec）+ HTTPS（SecureTransport/WinHTTP）
- `bundle create`：自解压交付包（格式 `GITXBND3`，CRC-32 完整性校验 + 临时目录自动清理）
- `doctor`：环境/远端/身份/SSH 密钥诊断
- `x <脚本>`：扩展脚本机制（`.gitx/scripts/` 项目级 + `~/.config/gitx/scripts/` 用户级，basename 匹配，`$GITX_REPO_ROOT` 环境变量）

### AI 工具链（需 libcurl，已 vendored）
- `save --ai`：AI 生成符合团队规范的提交信息（本地校验后确认提交）
- `explain <提交号>`：AI 解读提交
- `review --ai`：AI 审查工作区改动
- 配置 `.gitx/config.toml` `[ai]` 段：provider（deepseek/openai/ollama/custom）+ base_url + model
- 密钥存系统钥匙串（macOS Keychain/Windows 凭据管理器/Linux libsecret），`GITX_AI_KEY` 环境变量覆盖
- 协议：OpenAI 兼容（/chat/completions），任意兼容服务可用

## 依赖 vendor（third_party/）

| 依赖 | 版本 | 体积 | 说明 |
|---|---|---|---|
| libgit2 | 1.8.4 | 9.5MB | 精简 tests/fuzzers/CI |
| libssh2 | 1.11.1 | 2.2MB | patch：`BUILD_TESTING`→`LIBSSH2_BUILD_TESTING` |
| curl | 8.7.1 | 12MB | 静态 libcurl；macOS SecureTransport / Windows Schannel；`HTTP_ONLY`、禁无关协议；patch：`BUILD_TESTING`→`CURL_BUILD_TESTING` |
| zlib | 1.3.1 | 1MB | 仅压缩 API 源码（Windows bundled-zlib 回退） |

- 各依赖保留 `.tar.gz` 备份在 third_party/。
- **仍依赖系统**：OpenSSL（libssh2 加密后端，macOS/Linux 平台加密基础设施）、C++20 编译器、CMake 3.24+。
- CMake 关键点：`LIBSSH2_INCLUDE_DIR`/`LIBSSH2_LIBRARY` 缓存变量指向 vendored 版；curl `CURL_BUILD_TESTING=OFF`；顶层 `BUILD_TESTING` 不受依赖干扰。

## 构建命令（三平台，离线）

```bash
# macOS（无需 brew 装任何库）
cmake -S . -B mac-build -DBUILD_TESTING=ON
cmake --build mac-build --parallel 8
ctest --test-dir mac-build --output-on-failure

# Linux（仅需 libssl-dev）
sudo apt install libssl-dev
cmake -S . -B build -DBUILD_TESTING=ON && cmake --build build && ctest --test-dir build

# Windows（MinGW）
cmake -S . -B build -G "MinGW Makefiles" -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build -C Debug --output-on-failure
```

## 测试（4 个目标，三平台全绿）

- `gitx.config`：TOML 解析/校验/别名
- `gitx.repository`：init/commit/branch switch 索引同步/merge 冲突（o/t）/rebase 空补丁跳过/stash/tag
- `gitx.bundle`：create/extract/launch 往返 + 完整性
- `gitx.ai`：本地 mock LLM 端到端（save --ai/explain/review --ai；无 AI 支持或 Python 缺失时跳过）

## 已知限制 / 后续优先项

- 冲突解决 AI 辅助：尚未实现（AI 工具链 v2 规划）。`pr create --ai` 与 `changelog --ai` 已可生成文案，但不会直接调用托管平台 API 创建 PR 或写入 CHANGELOG。
- `save --ai` 的 AI 输出可能不稳定，本地规则校验兜底；无网络时需 provider=ollama。
- bundle 不自动扫描依赖 DLL/.so/.dylib（发布流程手工等价完成）。
- macOS 分发需绕过 Gatekeeper（ad-hoc 签名未公证）。
- HTTPS 凭据仅环境变量（`GITX_HTTPS_USER`/`GITX_HTTPS_TOKEN`），未接入系统凭据库。
- 无 i18n 框架、无日志开关。

## 关键文件

- `CMakeLists.txt`：依赖 vendor 解析、SSH/HTTPS/AI 开关、构建与测试目标。
- `src/main.cpp`：CLI 路由（含 AI 命令、`x` 扩展、doctor）。
- `src/git_repository.cpp`：libgit2 适配层（diff/commit/rebase/fast-forward/远端）。
- `src/ai_client.cpp`：AI 客户端（libcurl + OpenAI 兼容 + 钥匙串）。
- `src/bundle.cpp`：自解压包 `GITXBND3`。
- `src/config.cpp`：TOML 解析（含 `[ai]` 段）。
- `tests/`：config / git_repository / bundle / ai_smoke（+ mock_llm.py）。
- `docs/`：getting-started（新手）、server-setup（服务器）、ai-toolchain（AI 设计）。
- `.github/workflows/ci.yml`：三平台 CI。
