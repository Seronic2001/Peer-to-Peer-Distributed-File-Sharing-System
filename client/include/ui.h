#pragma once

// ----------------------------------------------------------------------------
// Thread-safe, leveled, colored logging + terminal styling for the client.
//
// Design:
//   * log_message(...)  -> user-facing output (INFO level). Always shown.
//   * log_warn(...)     -> recoverable problems. Always shown (yellow).
//   * log_error(...)    -> failures. Always shown (red). Never crashes.
//   * log_debug(...)    -> protocol/peer chatter. Only compiled in when
//                          P2P_LOG_LEVEL >= LOG_DEBUG (make debug).
//   * ui_info()/ui_error()/ui_hint() -> pre-colored single-line helpers for
//                          the interactive prompt path.
//
// Release builds (make release) set P2P_LOG_LEVEL=LOG_INFO by default:
// debug chatter is compiled out entirely (zero cost) and only the necessary
// outputs remain.
//
// All writes take cout_mutex and redraw the input line afterwards, so
// background threads (downloads, peer handlers) can log at any time without
// corrupting the line editor's state.
// ----------------------------------------------------------------------------

#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

extern std::mutex cout_mutex;
extern void redraw_line();

// ----- Log levels (compile-time filter) -----
#define LOG_ERROR 0
#define LOG_INFO 1
#define LOG_DEBUG 2

#ifndef P2P_LOG_LEVEL
#define P2P_LOG_LEVEL LOG_INFO
#endif

// ----- ANSI styling -----
namespace ui {

inline std::atomic<bool> raw_mode_active{false};

inline constexpr const char* RESET = "\x1b[0m";
inline constexpr const char* BOLD = "\x1b[1m";
inline constexpr const char* DIM = "\x1b[2m";
inline constexpr const char* RED = "\x1b[31m";
inline constexpr const char* GREEN = "\x1b[32m";
inline constexpr const char* YELLOW = "\x1b[33m";
inline constexpr const char* BLUE = "\x1b[34m";
inline constexpr const char* MAGENTA = "\x1b[35m";
inline constexpr const char* CYAN = "\x1b[36m";

// Clears to end of line, used before overwriting the prompt line.
inline constexpr const char* CLEAR_EOL = "\x1b[K";

inline std::string styled(const char* color, const std::string& text) {
  return std::string(color) + text + RESET;
}

enum class Level { Error = 0, Warn = 1, Info = 2, Debug = 3 };

// Core emitter: serializes, prints above the prompt line, redraws input.
// Never throws and never exits the process.
template <typename... Args>
void emit(Level level, const char* color, const char* tag, Args... args) {
  std::stringstream ss;
  (ss << ... << args);

  std::string out;
  switch (level) {
    case Level::Error:
      out = styled(RED, std::string(tag) + " " + ss.str());
      break;
    case Level::Warn:
      out = styled(YELLOW, std::string(tag) + " " + ss.str());
      break;
    case Level::Debug:
      out = styled(DIM, ss.str());
      break;
    case Level::Info:
    default:
      out = ss.str();
      break;
  }
  (void)color;

  std::lock_guard<std::mutex> lock(cout_mutex);
  if (!raw_mode_active.load()) {
    std::cout << out << "\n" << std::flush;
    return;
  }

  std::string safe_out;
  safe_out.reserve(out.size() + 16);
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == '\n' && (i == 0 || out[i - 1] != '\r')) {
      safe_out += "\r\n";
    } else {
      safe_out += out[i];
    }
  }

  std::cout << "\r" << CLEAR_EOL << safe_out << "\r\n";
  redraw_line();
  std::cout << std::flush;
}

// Convenience helpers used across the client.
template <typename... Args>
void ui_info(const std::string& msg) {
  emit(Level::Info, CYAN, "", msg);
}

}  // namespace ui

// ----- user-facing output (was: everything through log_message) -----
template <typename... Args>
void log_message(Args... args) {
  ui::emit(ui::Level::Info, ui::CYAN, "", args...);
}

// ----- successes: green so they pop (login OK, upload OK, download done) -----
template <typename... Args>
void log_success(Args... args) {
  ui::emit(ui::Level::Info, ui::GREEN, "", args...);
}

// ----- warnings: recoverable issues the user should notice -----
template <typename... Args>
void log_warn(Args... args) {
  ui::emit(ui::Level::Warn, ui::YELLOW, "[warn]", args...);
}

// ----- errors: failures; never crash the CLI -----
template <typename... Args>
void log_error(Args... args) {
  ui::emit(ui::Level::Error, ui::RED, "[error]", args...);
}

// ----- debug chatter: compiled out in release builds -----
#if P2P_LOG_LEVEL >= LOG_DEBUG
template <typename... Args>
void log_debug(Args... args) {
  ui::emit(ui::Level::Debug, ui::DIM, "[debug]", args...);
}
#else
#define log_debug(...) ((void)0)
#endif

namespace ui {

// Visible character count of a string, skipping ANSI escape sequences.
// Used to place the cursor after a colored prompt.
inline size_t visible_len(const std::string& s) {
  size_t width = 0;
  bool in_escape = false;
  for (char c : s) {
    if (in_escape) {
      if (c == 'm' || c == 'K' || c == 'J' || c == 'H') in_escape = false;
      continue;
    }
    if (c == '\x1b') {
      in_escape = true;
      continue;
    }
    ++width;
  }
  return width;
}

inline std::string bold_color(const char* color, const std::string& text) {
  return std::string(BOLD) + color + text + RESET;
}

}  // namespace ui
