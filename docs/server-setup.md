# gitx 服务器配置指南

> gitx 是纯客户端，服务器端是**标准 Git 服务器**，不需要安装任何 gitx 专用程序。
> 本指南覆盖 macOS / Windows / Linux 三种服务器系统，先以最简的 bare 仓库（方案 A）跑通，再升级到 Gitea 自托管（方案 B）。

## 原理

```
  gitx 客户端（任意系统）
        │  gitx start clone / sync publish / sync push|pull
        ▼
  标准 Git 服务器（bare 仓库 或 Gitea）
        │  SSH 协议（走系统 OpenSSH）或 HTTPS 协议
        ▼
   服务器上的 Git 仓库
```

- **bare 仓库**：没有工作区的纯中央仓库，只存历史，是团队协作的标准形态。
- 客户端与服务器之间走标准 Git 协议，任何 git 客户端都能互连。

---

## 方案 A：bare 仓库（最简，先跑通）

### 服务器配置

#### Linux（Debian/Ubuntu 示例，CentOS 把 apt 换成 yum/dnf）

```bash
# 1. 安装 git
sudo apt update && sudo apt install -y git

# 2. 建一个仅供 git 使用的账号（可选但推荐，安全）
sudo useradd -m -s /usr/bin/git-shell git

# 3. 建中央仓库目录
sudo mkdir -p /srv/git
sudo chown git:git /srv/git

# 4. 创建团队仓库（bare = 无工作区，只存历史）
sudo -u git git init --bare /srv/git/team-project.git

# 5. 确认 sshd 已开启（默认开启）
sudo systemctl enable --now ssh
```

#### macOS

```bash
# 1. 确认已装命令行工具（自带 git）
xcode-select --install

# 2. 开启 SSH 远程登录
#    系统设置 → 通用 → 共享 → 远程登录 → 开启
#    （或命令行：sudo systemsetup -setremotelogin on）

# 3. 建仓库目录（放在当前用户的某处，如 ~/git）
mkdir -p ~/git
git init --bare ~/git/team-project.git
```

> macOS 上如果不想让服务器用户有完整 shell，可用系统自带用户配合 `git-shell`，
> 但对小型团队，直接用该 macOS 账号的 SSH 即可，下面"安全加固"一节再讲限制。

#### Windows

```bash
# 1. 安装 Git for Windows（https://git-scm.com），一路默认
# 2. 安装 OpenSSH Server（管理员 PowerShell）
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Start-Service sshd
Set-Service -Name sshd -StartupType Automatic

# 3. 建仓库目录
mkdir C:\git
cd C:\git
git init --bare team-project.git
```

> Windows 上 SSH 登录默认用的是 Windows 用户账号（如 Administrator），
> 客户端连接时用 `git@服务器IP:C:/git/team-project.git` 之类的格式，
> 用户名、路径按实际调整。

### 客户端接入（所有系统一致）

```bash
# 克隆
gitx start clone git@服务器IP:/srv/git/team-project.git 项目名
cd 项目名

# 首次提交并发布到服务器
git config user.name "你的名字"
git config user.email "you@example.com"
gitx save "feat(cli): 首次提交"
gitx sync publish git@服务器IP:/srv/git/team-project.git

# 日常协作
gitx sync push        # 推送自己的改动
gitx sync pull        # 拉取别人的改动
gitx sync fetch       # 只拉取不合并
```

其他成员加入：

```bash
gitx start clone git@服务器IP:/srv/git/team-project.git 项目名
```

### 安全加固（强烈建议）

1. **SSH 密钥登录，禁用密码**（服务器上）：

```bash
# Linux：编辑 /etc/ssh/sshd_config
#   PasswordAuthentication no
sudo systemctl restart ssh
```

2. **只允许 git 操作，不给 shell**（Linux 已用 git-shell，客户端仍照常使用）：

```bash
# 客户端把公钥加到服务器
ssh-copy-id git@服务器IP
```

3. **限制 git 账号只能访问仓库目录**（Linux，可选）：
   `git-shell` 已经限制只能 git 命令；如需进一步隔离可加 `authorized_keys` 的 `command=` 前缀。

---

## 方案 B：升级到 Gitea 自托管（带网页管理）

跑通 A 之后，想有网页界面（在线看代码、用户管理、issue），用 Gitea。**跨平台统一用 Docker**：

```bash
# 任意装好 Docker 的系统上
docker run -d --name gitea \
  -p 3000:3000 \
  -p 2222:22 \
  -v /var/lib/gitea:/data \
  -e GITEA__server__SSH_PORT=2222 \
  gitea/gitea:latest
```

- 网页管理：`http://服务器IP:3000`
- 在网页上创建仓库（如 `team-project`）

客户端接入（两种协议任选）：

```bash
# HTTP 方式
gitx start clone http://服务器IP:3000/用户名/team-project.git 项目名

# SSH 方式（注意端口 2222）
gitx start clone ssh://git@服务器IP:2222/用户名/team-project.git 项目名
```

> Gitea 的 SSH 端口映射到宿主 2222，因为 22 常被系统 sshd 占用。
> HTTPS 方式可再配反向代理（Caddy/Nginx）加证书。

---

## 常见问题

**Q: 客户端提示 host key 确认？**
A: 首次连接新服务器属正常现象，确认指纹后输入 yes 即可。

**Q: 服务器在路由器/NAT 后面？**
A: 需要在路由器上把 22（或 Gitea 的 2222）端口转发到服务器内网 IP，或配置 DDNS。

**Q: 客户端走 HTTPS 时凭据怎么给？**
A: 环境变量 `GITX_HTTPS_USER` / `GITX_HTTPS_TOKEN`（gitx 刻意不把凭据写盘）。

**Q: 我该用 SSH 还是 HTTPS？**
A: 团队内部建议 SSH（密钥认证，无需每次输密码）；需要公网/浏览器推送时用 HTTPS。

---

## 检查清单

- [ ] 服务器能 `git init --bare` 建仓库（或 Gitea 网页能建仓库）
- [ ] 客户端 `gitx start clone <URL> 项目名` 成功
- [ ] `gitx sync publish <URL>` 首次推送成功
- [ ] 另一台机器克隆后 `gitx sync pull` 能看到推送的提交
