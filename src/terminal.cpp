#include "gitx/terminal.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

namespace gitx {
namespace term {

namespace {

const char* kReset = "\x1b[0m";

#ifdef _WIN32
struct WinState {
  HANDLE in = INVALID_HANDLE_VALUE;
  HANDLE out = INVALID_HANDLE_VALUE;
  DWORD old_in_mode = 0;
  DWORD old_out_mode = 0;
};
#else
struct PosixState {
  termios old_term;
};
#endif

}  // namespace

bool is_interactive() {
#ifdef _WIN32
  return _isatty(_fileno(stdin)) != 0;
#else
  return isatty(STDIN_FILENO) != 0;
#endif
}

RawMode::RawMode() {
  if (!is_interactive()) return;
#ifdef _WIN32
  auto* state = new WinState;
  state->in = GetStdHandle(STD_INPUT_HANDLE);
  state->out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (state->in != INVALID_HANDLE_VALUE) {
    GetConsoleMode(state->in, &state->old_in_mode);
    SetConsoleMode(state->in, ENABLE_VIRTUAL_TERMINAL_INPUT);
  }
  if (state->out != INVALID_HANDLE_VALUE) {
    GetConsoleMode(state->out, &state->old_out_mode);
    SetConsoleMode(state->out, state->old_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
  }
  state_ = state;
#else
  auto* state = new PosixState;
  tcgetattr(STDIN_FILENO, &state->old_term);
  termios raw = state->old_term;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= CS8;
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  state_ = state;
#endif
}

RawMode::~RawMode() {
#ifdef _WIN32
  if (state_ != nullptr) {
    auto* state = static_cast<WinState*>(state_);
    if (state->in != INVALID_HANDLE_VALUE) SetConsoleMode(state->in, state->old_in_mode);
    if (state->out != INVALID_HANDLE_VALUE) SetConsoleMode(state->out, state->old_out_mode);
    delete state;
  }
#else
  if (state_ != nullptr) {
    auto* state = static_cast<PosixState*>(state_);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &state->old_term);
    delete state;
  }
#endif
}

std::pair<int, int> size() {
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO info;
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out, &info)) {
    return {static_cast<int>(info.srWindow.Bottom - info.srWindow.Top + 1),
            static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1)};
  }
  return {0, 0};
#else
  winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
    return {static_cast<int>(ws.ws_row), static_cast<int>(ws.ws_col)};
  }
  return {0, 0};
#endif
}

int rows() { return size().first; }
int cols() { return size().second; }

void clear_screen() { write("\x1b[2J\x1b[H"); }
void hide_cursor() { write("\x1b[?25l"); }
void show_cursor() { write("\x1b[?25h"); }
void move_to(int row, int col) { write("\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H"); }
void save_cursor() { write("\x1b[s"); }
void restore_cursor() { write("\x1b[u"); }

void set_color(int fg) {
  if (fg == 0) write(kReset);
  else write("\x1b[" + std::to_string(fg) + "m");
}

void set_bold(bool on) { write(on ? "\x1b[1m" : "\x1b[22m"); }

void write(const std::string& text) {
  fputs(text.c_str(), stdout);
}

void write_line(const std::string& text) {
  fputs(text.c_str(), stdout);
  fputc('\n', stdout);
}

void flush() {
  fflush(stdout);
}

Key read_key() {
  Key key;
  const auto read_byte = []() -> int {
#ifdef _WIN32
    return _getch_nolock();
#else
    unsigned char byte = 0;
    if (read(STDIN_FILENO, &byte, 1) != 1) return -1;
    return byte;
#endif
  };

  const int first = read_byte();
  if (first < 0) { key.type = Key::Unknown; return key; }

  if (first == '\r' || first == '\n') { key.type = Key::Enter; return key; }
  if (first == '\t') { key.type = Key::Tab; return key; }
  if (first == 27) {  // ESC: could be a lone escape or an escape sequence
    const int second = read_byte();
    if (second < 0) { key.type = Key::Esc; return key; }
    if (second == '[') {
      const int third = read_byte();
      if (third == 'A') { key.type = Key::Up; return key; }
      if (third == 'B') { key.type = Key::Down; return key; }
      if (third == 'C') { key.type = Key::Right; return key; }
      if (third == 'D') { key.type = Key::Left; return key; }
      if (third == 'H') { key.type = Key::Home; return key; }
      if (third == 'F') { key.type = Key::End; return key; }
      if (third == '3') { read_byte(); key.type = Key::Delete; return key; }
      if (third == '5') { read_byte(); key.type = Key::PageUp; return key; }
      if (third == '6') { read_byte(); key.type = Key::PageDown; return key; }
      key.type = Key::Unknown;
      return key;
    }
    if (second == 'O') {
      const int third = read_byte();
      if (third == 'H') { key.type = Key::Home; return key; }
      if (third == 'F') { key.type = Key::End; return key; }
      key.type = Key::Unknown;
      return key;
    }
    key.type = Key::Unknown;
    return key;
  }
  if (first == 127 || first == 8) { key.type = Key::Backspace; return key; }
  if (first == 3) { key.type = Key::CtrlC; return key; }
  if (first == 4) { key.type = Key::CtrlD; return key; }
  if (first == 19) { key.type = Key::CtrlS; return key; }
  if (first == 24) { key.type = Key::CtrlX; return key; }
  if (first == 7) { key.type = Key::CtrlG; return key; }
  if (first == 23) { key.type = Key::CtrlW; return key; }
  if (first == 1) { key.type = Key::CtrlA; return key; }
  if (first == 5) { key.type = Key::CtrlE; return key; }
  if (first >= 32) {
    key.type = Key::Char;
    key.ch = static_cast<char>(first);
    return key;
  }
  key.type = Key::Unknown;
  return key;
}

}  // namespace term
}  // namespace gitx
