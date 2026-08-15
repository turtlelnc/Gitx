#pragma once

#include <string>
#include <vector>

namespace gitx {
namespace term {

// A single key press, normalized across platforms.
struct Key {
  enum Type {
    Char,       // printable character (value in `ch`)
    Enter, Tab, Esc,
    Up, Down, Left, Right,
    Home, End, PageUp, PageDown,
    Backspace, Delete,
    CtrlC, CtrlD, CtrlS, CtrlX, CtrlG, CtrlW, CtrlA, CtrlE,
    Unknown
  } type = Unknown;
  char ch = 0;
};

// RAII switch to raw (non-canonical, no echo) terminal mode.
class RawMode {
 public:
  RawMode();
  ~RawMode();
  RawMode(const RawMode&) = delete;
  RawMode& operator=(const RawMode&) = delete;

 private:
  void* state_ = nullptr;
};

// Query the terminal size; returns {rows, cols}, {0,0} on failure.
std::pair<int, int> size();
int rows();
int cols();

// ANSI screen control.
void clear_screen();
void hide_cursor();
void show_cursor();
void move_to(int row, int col);  // 1-based
void save_cursor();
void restore_cursor();

// Output helpers (ANSI SGR).
void set_color(int fg);  // 30-37, or 0 to reset
void set_bold(bool on);
void write(const std::string& text);
void write_line(const std::string& text);
void flush();

// Read one normalized key (blocks).
Key read_key();

// True when stdin is an interactive terminal.
bool is_interactive();

}  // namespace term
}  // namespace gitx
