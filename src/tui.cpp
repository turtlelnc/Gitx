#include "gitx/tui.hpp"
#include "gitx/terminal.hpp"
#include "gitx/editor.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace gitx {
namespace tui {
namespace {

std::string status_symbol(const std::string& raw) {
  // libgit2 status is a bitmask serialized as a decimal string; map common
  // bits to a friendly symbol.
  const auto value = std::stoi(raw);
  if ((value & 0x0100) != 0) return "M";   // WT_MODIFIED
  if ((value & 0x0200) != 0) return "+";   // WT_NEW (untracked)
  if ((value & 0x0040) != 0) return "M";   // INDEX_MODIFIED
  if ((value & 0x0080) != 0) return "+";   // INDEX_NEW
  if ((value & 0x0010) != 0) return "D";   // WT_DELETED
  return "?";
}

void render(const std::vector<StatusItem>& items, int selected, int top, int rows, int cols) {
  term::clear_screen();
  term::set_bold(true);
  term::write("gitx — 工作区状态");
  term::set_bold(false);
  term::write("\r\n");
  const int list_rows = rows - 2;
  for (int offset = 0; offset < list_rows; ++offset) {
    const int index = top + offset;
    if (index >= static_cast<int>(items.size())) {
      term::write("~\x1b[K\r\n");
      continue;
    }
    const auto& item = items[static_cast<std::size_t>(index)];
    if (index == selected) {
      term::set_color(7);
      term::write("> ");
    } else {
      term::write("  ");
    }
    const std::string symbol = status_symbol(item.state);
    if (symbol == "+") term::set_color(32);
    else if (symbol == "M") term::set_color(33);
    else if (symbol == "D") term::set_color(31);
    else term::set_color(0);
    term::write(symbol + " ");
    term::set_color(0);
    term::write(item.path);
    term::write("\x1b[K\r\n");
  }
  term::set_color(7);
  std::string hint = "↑↓ 选择  [空格]切换暂存  [d]查看diff  [c]提交  [q]退出  [e]编辑文件";
  if (cols > 0) hint = hint.substr(0, static_cast<std::size_t>(cols));
  term::write(hint);
  term::set_color(0);
  term::flush();
}

std::string show_diff(GitRepository& repository, const std::string& path) {
  // Best-effort: show the staged diff; individual-file diff is not yet wired
  // through libgit2 in GitRepository, so we show the global staged diff.
  (void)path;
  return repository.staged_diff(4000);
}

}  // namespace

int run(GitRepository& repository, const TeamConfig& config) {
  if (!term::is_interactive()) {
    std::cout << "TUI 需要交互式终端。\n";
    return 1;
  }
  (void)config;

  int selected = 0;
  int top = 0;
  bool done = false;

  {
    term::RawMode raw;
    term::hide_cursor();
    while (!done) {
      auto items = repository.status();
      if (selected >= static_cast<int>(items.size())) selected = std::max(0, static_cast<int>(items.size()) - 1);
      const auto [rows, cols] = term::size();
      if (rows <= 0 || cols <= 0) {
        std::cout << "无法获取终端尺寸。\n";
        break;
      }
      if (top > selected) top = selected;
      if (selected >= top + rows - 2) top = selected - rows + 3;
      render(items, selected, top, rows, cols);

      const auto key = term::read_key();
      switch (key.type) {
        case term::Key::Up:    if (selected > 0) --selected; break;
        case term::Key::Down:  ++selected; break;
        case term::Key::PageUp:   selected -= rows - 2; if (selected < 0) selected = 0; break;
        case term::Key::PageDown: selected += rows - 2; break;
        case term::Key::Char:
          if (key.ch == 'q' || key.ch == 'Q') { done = true; break; }
          if (key.ch == 'd' || key.ch == 'D') {
            term::clear_screen();
            term::move_to(1, 1);
            const std::string diff = show_diff(repository, items.empty() ? "" : items[static_cast<std::size_t>(selected)].path);
            std::cout << diff << "\n\n[按任意键返回]\n";
            term::flush();
            term::read_key();
          }
          if (key.ch == 'c' || key.ch == 'C') {
            // Commit flow: stage all, ask for message.
            repository.stage_all();
            term::clear_screen();
            term::move_to(1, 1);
            term::show_cursor();
            std::cout << "提交信息（" << config.commit.template_text << "）：\n> ";
            term::flush();
            std::string message;
            std::getline(std::cin, message);
            term::hide_cursor();
            if (message.empty()) {
              std::cout << "已取消提交。\n";
              term::flush();
              term::read_key();
            } else {
              try {
                repository.commit(message, config);
                std::cout << "已创建提交。\n";
              } catch (const std::exception& error) {
                std::cout << "提交失败: " << error.what() << "\n";
              }
              term::flush();
              term::read_key();
            }
          }
          if (key.ch == 'e' || key.ch == 'E') {
            if (!items.empty()) {
              term::clear_screen();
              term::move_to(1, 1);
              term::show_cursor();
              const auto path = repository.workdir() / items[static_cast<std::size_t>(selected)].path;
              editor::edit_file(path);
              term::hide_cursor();
            }
          }
          break;
        case term::Key::CtrlC:
        case term::Key::Esc:
        case term::Key::CtrlX:
          done = true;
          break;
        default:
          break;
      }
    }
    term::show_cursor();
    term::clear_screen();
    term::move_to(1, 1);
  }
  return 0;
}

}  // namespace tui
}  // namespace gitx
