#include "gitx/editor.hpp"
#include "gitx/terminal.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace gitx {
namespace editor {
namespace {

// A small, dependency-free line editor. The whole file lives in memory as a
// list of lines; the screen shows a viewport with a status bar at the bottom.
struct Buffer {
  std::vector<std::string> lines;
  int cursor_row = 0;   // 0-based index into lines
  int cursor_col = 0;   // 0-based column within the current line
  int top_line = 0;     // first visible line
  bool modified = false;
  fs::path path;
};

std::vector<std::string> load_lines(const fs::path& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    // New file: start with a single empty line.
    return {""};
  }
  std::string line;
  while (std::getline(in, line)) {
    // Strip a trailing \r (CRLF files edited in place stay CRLF on write? we
    // normalize to LF for simplicity).
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
  }
  if (lines.empty()) lines.push_back("");
  return lines;
}

void save(Buffer& buffer) {
  std::ofstream out(buffer.path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("无法写入文件: " + buffer.path.string());
  for (std::size_t index = 0; index < buffer.lines.size(); ++index) {
    out << buffer.lines[index];
    if (index + 1 < buffer.lines.size()) out << '\n';
  }
  buffer.modified = false;
}

void clamp_cursor(Buffer& buffer) {
  if (buffer.lines.empty()) buffer.lines.push_back("");
  buffer.cursor_row = std::clamp(buffer.cursor_row, 0, static_cast<int>(buffer.lines.size()) - 1);
  const auto& line = buffer.lines[static_cast<std::size_t>(buffer.cursor_row)];
  buffer.cursor_col = std::clamp(buffer.cursor_col, 0, static_cast<int>(line.size()));
}

void render(const Buffer& buffer, int rows, int cols, bool interactive) {
  term::clear_screen();
  const int viewport = std::max(rows - 1, 1);
  for (int offset = 0; offset < viewport; ++offset) {
    const int index = buffer.top_line + offset;
    if (index < static_cast<int>(buffer.lines.size())) {
      const auto& line = buffer.lines[static_cast<std::size_t>(index)];
      const auto shown = line.substr(0, static_cast<std::size_t>(std::max(cols, 0)));
      term::write(shown);
    }
    term::write("\x1b[K\r\n");
  }
  // Status bar.
  term::set_color(7);
  const std::string name = buffer.path.filename().string();
  const std::string status = name + "  " +
      (buffer.modified ? "[已修改]" : "[未修改]") + "  行 " +
      std::to_string(buffer.cursor_row + 1) + "/" + std::to_string(buffer.lines.size()) +
      "  Ctrl+S 保存  Ctrl+X 退出  Ctrl+G 帮助";
  term::write(status.substr(0, static_cast<std::size_t>(cols)));
  term::set_color(0);
  term::move_to(buffer.cursor_row - buffer.top_line + 1, buffer.cursor_col + 1);
  term::flush();
  (void)interactive;
}

}  // namespace

bool edit_file(const fs::path& path) {
  if (!term::is_interactive()) {
    std::cout << "编辑器需要交互式终端。\n";
    return false;
  }

  Buffer buffer;
  buffer.path = path;
  buffer.lines = load_lines(path);
  bool saved = false;

  {
    term::RawMode raw;
    term::hide_cursor();
    bool done = false;
    while (!done) {
      const auto [rows, cols] = term::size();
      if (rows <= 0 || cols <= 0) {
        std::cout << "无法获取终端尺寸。\n";
        break;
      }
      render(buffer, rows, cols, true);
      const auto key = term::read_key();
      switch (key.type) {
        case term::Key::CtrlS:
          save(buffer);
          break;
        case term::Key::CtrlX:
          if (buffer.modified) {
            // Ask for confirmation inline.
            term::move_to(rows, 1);
            term::write("文件未保存，按 Ctrl+S 保存后退出，或按其他键继续编辑");
            term::flush();
            const auto k2 = term::read_key();
            if (k2.type == term::Key::CtrlS) {
              save(buffer);
              done = true;
              saved = true;
            }
          } else {
            done = true;
            saved = true;
          }
          break;
        case term::Key::CtrlG:
          term::move_to(rows, 1);
          term::write("↑↓ 移动  ←→ 移动光标  退格删除  Enter 换行  Ctrl+S 保存  Ctrl+X 退出");
          term::flush();
          term::read_key();
          break;
        case term::Key::Up:    if (buffer.cursor_row > 0) --buffer.cursor_row; clamp_cursor(buffer); break;
        case term::Key::Down:  ++buffer.cursor_row; clamp_cursor(buffer); break;
        case term::Key::Left:  if (buffer.cursor_col > 0) --buffer.cursor_col; break;
        case term::Key::Right: ++buffer.cursor_col; clamp_cursor(buffer); break;
        case term::Key::Home:  buffer.cursor_col = 0; break;
        case term::Key::End:   buffer.cursor_col = static_cast<int>(buffer.lines[static_cast<std::size_t>(buffer.cursor_row)].size()); break;
        case term::Key::Backspace: {
          auto& line = buffer.lines[static_cast<std::size_t>(buffer.cursor_row)];
          if (buffer.cursor_col > 0) {
            line.erase(static_cast<std::size_t>(buffer.cursor_col - 1), 1);
            --buffer.cursor_col;
            buffer.modified = true;
          } else if (buffer.cursor_row > 0) {
            const int prev = buffer.cursor_row - 1;
            auto& prev_line = buffer.lines[static_cast<std::size_t>(prev)];
            buffer.cursor_col = static_cast<int>(prev_line.size());
            prev_line += line;
            buffer.lines.erase(buffer.lines.begin() + buffer.cursor_row);
            buffer.cursor_row = prev;
            buffer.modified = true;
          }
          break;
        }
        case term::Key::Delete: {
          auto& line = buffer.lines[static_cast<std::size_t>(buffer.cursor_row)];
          if (buffer.cursor_col < static_cast<int>(line.size())) {
            line.erase(static_cast<std::size_t>(buffer.cursor_col), 1);
            buffer.modified = true;
          } else if (buffer.cursor_row + 1 < static_cast<int>(buffer.lines.size())) {
            line += buffer.lines[static_cast<std::size_t>(buffer.cursor_row + 1)];
            buffer.lines.erase(buffer.lines.begin() + buffer.cursor_row + 1);
            buffer.modified = true;
          }
          break;
        }
        case term::Key::Enter: {
          auto& line = buffer.lines[static_cast<std::size_t>(buffer.cursor_row)];
          const std::string rest = line.substr(static_cast<std::size_t>(buffer.cursor_col));
          line.erase(static_cast<std::size_t>(buffer.cursor_col));
          buffer.lines.insert(buffer.lines.begin() + buffer.cursor_row + 1, rest);
          ++buffer.cursor_row;
          buffer.cursor_col = 0;
          buffer.modified = true;
          break;
        }
        case term::Key::Char: {
          auto& line = buffer.lines[static_cast<std::size_t>(buffer.cursor_row)];
          line.insert(static_cast<std::size_t>(buffer.cursor_col), 1, key.ch);
          ++buffer.cursor_col;
          buffer.modified = true;
          break;
        }
        case term::Key::PageUp:   buffer.top_line = std::max(buffer.top_line - rows + 2, 0); break;
        case term::Key::PageDown: buffer.top_line += rows - 2; break;
        case term::Key::CtrlC:
        case term::Key::Esc:
          done = true;
          break;
        default:
          break;
      }
      // Keep cursor row visible.
      if (buffer.cursor_row < buffer.top_line) buffer.top_line = buffer.cursor_row;
      if (buffer.cursor_row >= buffer.top_line + rows - 1) buffer.top_line = buffer.cursor_row - rows + 2;
    }
    term::show_cursor();
    term::clear_screen();
    term::move_to(1, 1);
  }

  std::cout << (saved ? "已保存 " : "") << buffer.path.string() << "\n";
  return saved;
}

}  // namespace editor
}  // namespace gitx
