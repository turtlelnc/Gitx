# gitx 新手教程

> 本教程面向完全没用过 git 类工具的初学者。全程不需要任何 git 基础。

## 先把 gitx 想成"存档管理工具"

和游戏存档一模一样：

| gitx 概念 | 游戏类比 | 实际作用 |
|---|---|---|
| 仓库 | 存档文件夹 | 项目文件夹 + 隐藏的存档记录 |
| `save`（提交） | 存档点 | 把当前所有文件的状态拍一张快照 |
| `history` | 读档列表 | 查看所有存档点 |
| `status` | 检查改动 | 看"上次存档后改了什么" |
| `branch`（分支） | 平行世界 | 开副本放心尝试，不影响主线 |

**核心心法：改一点，存一次。** 存得越勤越不怕改坏。

## 第 1 步：启动 gitx

```bash
gitx --version
```

看到 `gitx 1.0.0 (libgit2 1.8.4)` 即成功。嫌路径长可以设别名：

```bash
alias gitx="/path/to/gitx"
```

## 第 2 步：创建第一个项目

```bash
cd ~/Desktop
gitx start 我的第一个项目
```

gitx 会建好文件夹、初始化存档记录、生成团队规范文件（`.gitx/config.toml`，新手先不用管）。

## 第 3 步：设置你的身份（只需一次）

提交会署名，先设置一次：

```bash
cd 我的第一个项目
git config user.name "你的名字"
git config user.email "you@example.com"
```

## 第 4 步：写文件并存档

用编辑器在项目里新建一个文件（如 `笔记.txt`）写点内容，然后：

```bash
gitx status          # 看到新文件
gitx save "feat(笔记): 我的第一条记录"
```

提交信息必须符合 `类型(范围): 一句话` 格式：

- `feat(笔记): 添加第一条记录` — 新增功能
- `fix(登录): 修复按钮不响应` — 修 bug
- `docs(说明): 更新教程` — 改文档

写错会被拒绝，这是保护你——以后翻历史一眼看懂每条存档。

## 第 5 步：查看存档

```bash
gitx history         # 提交号 + 说明 + 作者
gitx status          # "工作区干净" = 全部已存档
```

## 第 6 步：改坏不怕

改动后 `gitx history` 看记录，改回去再 `gitx save` 存新档。旧版本永远在历史里。

## 第 7 步：开平行世界尝试

```bash
gitx branch new feature/实验   # 进入"实验"世界
gitx branch switch main        # 回到主线
gitx branch                    # 查看所有世界
```

## 新手先不碰的进阶功能

- `integrate merge` / `integrate rebase` — 合并与变基
- `sync` — 与远程仓库同步
- `bundle` — 自解压交付包
- `stash` — 临时暂存

等基础操作熟练后再学。

## 还想了解更多？

- 不带参数运行 `gitx`，会弹出中文交互菜单，跟着提示走即可。
- `gitx --help` 查看全部命令。
- 进阶用法见 [README](../README.md)。
