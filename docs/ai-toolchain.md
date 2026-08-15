# gitx AI 工具链设计（v1）

> 目标：让"不懂 git 的新手"和"想要效率的老手"都能用 AI 完成规范化的 Git 协作。
> 本文档是设计与实现约定，随实现更新。

## 架构

```
  gitx 命令（save --ai / explain / review --ai）
        │
        ▼
   AI 客户端模块（完全内置，链接 libcurl）
        │  OpenAI 兼容协议（/chat/completions，支持自定义 base_url）
        ▼
   LLM 提供商（DeepSeek / Ollama / 通义 / OpenAI / 任何兼容服务）
```

- **完全内置**：gitx 内部完成 HTTP/TLS 请求，不依赖用户额外安装命令行工具（系统 libcurl 即可）。
- **协议中立**：OpenAI 兼容协议 + 可配置 `base_url`，一家代码适配所有提供商。
- **可选依赖**：`find_package(CURL)`；未找到时 AI 命令给出中文安装提示，其余功能不受影响。

## 配置（.gitx/config.toml 新增 [ai] 段）

```toml
[ai]
# 提供商：deepseek | openai | ollama | custom
provider = "deepseek"
# 覆盖默认 base_url（custom 时必填）
base_url = ""
# 模型名
model = ""
# 调用超时（秒）
timeout = 60
# 发送给 LLM 的 diff 最大字符数，超出截断
max_diff_chars = 8000
```

默认值（provider 未配置时按此推断）：

| provider | base_url | 默认 model |
|---|---|---|
| deepseek | `https://api.deepseek.com/v1` | `deepseek-chat` |
| openai | `https://api.openai.com/v1` | `gpt-4o-mini` |
| ollama | `http://localhost:11434/v1` | `qwen2.5:7b`（本地） |
| custom | 必填 | 必填 |

## API 密钥存储（系统钥匙串）

- 首次使用 AI 命令时交互输入密钥，存入系统钥匙串：
  - macOS：Keychain（`security add-generic-password`）
  - Windows：Credential Manager（`cmdkey`）
  - Linux：libsecret（`secret-tool`），缺失时退回 `~/.config/gitx/secret.toml`（600 权限）并提示
- gitx 从钥匙串读取密钥发送请求，**不落盘、不写入配置**。
- 环境变量 `GITX_AI_KEY` 优先于钥匙串（CI/临时场景）。

## v1 核心命令

### 1. `gitx save --ai`

流程：收集工作区 diff → 附上团队规范（commit.pattern / template）→ 请求 LLM 生成提交信息 → 展示给用户 → 确认后校验并提交。

Prompt 要点：告知规范模板与 pattern，要求输出 `type(scope): summary`，若不符合格式由 gitx 本地校验拒绝重试。

### 2. `gitx explain <提交号>`

流程：读取指定提交的 diff 摘要 → 请求 LLM → 输出中文解释：改了什么、为什么、影响范围、潜在风险。

### 3. `gitx review --ai`

流程：收集未推送提交（或工作区改动）的 diff → 请求 LLM 审查 → 输出问题清单（严重程度 + 行号 + 建议）。不修改任何文件，只报告。

### 4. `gitx changelog --ai`

流程：收集当前分支相对默认分支（`[branch].default`）的提交列表 → 请求 LLM → 输出按类型分组的中文 CHANGELOG 片段。

### 5. `gitx pr create --ai [目标分支]`

流程：收集当前分支相对目标分支（默认 `[branch].default`）的提交列表 → 请求 LLM → 输出 PR 标题（`type(scope): summary` 格式）与中文描述。当前只生成内容文本，实际创建 PR 待平台集成（v2）。

## 上下文收集与安全

- 只发送**已暂存/已提交的 diff**，绝不发送密钥、远端 URL 中的凭据。
- `max_diff_chars` 截断超大 diff；可配置为 0 禁用 AI（仅本地校验）。
- 默认提示用户"将把改动发送到远程 AI 服务"，可用 `gitx config` 关闭（provider=ollama 时为本地，无需提示）。
- LLM 输出未经本地规则校验不采纳（save --ai 强制走 validate_commit）。

## 扩展机制（自选扩展）

### 自定义子命令：`gitx x <脚本名> [参数...]`

- 脚本位置（按优先级）：
  1. 项目内 `.gitx/scripts/<脚本名>`（随仓库共享，团队规范的一部分）
  2. 用户级 `~/.config/gitx/scripts/<脚本名>`（个人扩展）
- 任意可执行文件（sh/py/二进制）均可；gitx 把参数原样传入，stdin 传入仓库根路径。
- `gitx x list` 列出可用脚本。

### 事件钩子（后续迭代）

`pre-save` / `post-save` / `pre-merge` / `post-checkout` 等，脚本位于 `.gitx/hooks/`，gitx 在对应事件前后调用，失败可中断操作。

## 实现计划

1. **AI 客户端模块**（`src/ai_client.cpp` / `include/gitx/ai_client.hpp`）：
   - libcurl POST + JSON 解析（手写极简 JSON，避免新依赖）
   - 配置读取、钥匙串读写、prompt 组装
2. **save --ai**：diff 收集（复用 libgit2 diff API）+ 生成 + 校验 + 提交
3. **explain**：提交 diff → 解释
4. **review --ai**：范围 diff → 审查报告
5. **扩展机制**：`gitx x` 路由 + 脚本发现
6. **测试**：AI 模块用 mock 服务器（本地 HTTP）验证请求/解析；不依赖真实 LLM
7. **文档**：README 增补 AI 章节 + 扩展指南

## 依赖矩阵

| 平台 | libcurl | 说明 |
|---|---|---|
| macOS | 系统自带 | 无需安装 |
| Linux | `libcurl4-openssl-dev` | apt/dnf 安装 |
| Windows | vcpkg `curl` | CI 与文档注明 |

未找到 libcurl 时：AI 命令输出中文安装指引，`gitx --version` 标注 `(no AI)`。
