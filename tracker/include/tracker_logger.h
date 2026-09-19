#pragma once

// ----------------------------------------------------------------------------
// Thread-safe, leveled, colored logging for the tracker.
//
//   log_tracker(...)       -> important events (connections, lifecycle). Always on.
//   log_tracker_warn(...)  -> recoverable problems. Always on, yellow.
//   log_tracker_err(...)   -> failures. Always on, red. Never crashes.
//   log_tracker_debug(...) -> per-command chatter. Compiled out unless
//                             P2P_LOG_LEVEL >= LOG_DEBUG (make debug).
//
// Release builds (make release) set P2P_LOG_LEVEL=LOG_INFO by default so the
// console only shows the necessary outputs.
// ----------------------------------------------------------------------------

#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#define LOG_ERROR 0
#define LOG_INFO 1
#define LOG_DEBUG 2

#ifndef P2P_LOG_LEVEL
#define P2P_LOG_LEVEL LOG_INFO
#endif

namespace tracker_ui {

inline constexpr const char* RESET = "\x1b[0m";
inline constexpr const char* BOLD = "\x1b[1m";
inline constexpr const char* DIM = "\x1b[2m";
inline constexpr const char* RED = "\x1b[31m";
inline constexpr const char* GREEN = "\x1b[32m";
inline constexpr const char* YELLOW = "\x1b[33m";
inline constexpr const char* CYAN = "\x1b[36m";

inline std::mutex& cout_mutex() {
  static std::mutex m;
  return m;
}

inline std::string styled(const char* color, const std::string& text) {
  return std::string(color) + text + RESET;
}

inline std::string tagged(const char* color, const char* tag,
                          const std::string& text) {
  return styled(color, std::string(tag) + " " + text);
}

}  // namespace tracker_ui

template <typename... Args>
void log_tracker(Args const&... args) {
  std::lock_guard<std::mutex> lock(tracker_ui::cout_mutex());
  std::stringstream ss;
  (ss << ... << args);
  std::cout << ss.str() << std::endl;
}

template <typename... Args>
void log_tracker_warn(Args const&... args) {
  std::lock_guard<std::mutex> lock(tracker_ui::cout_mutex());
  std::stringstream ss;
  (ss << ... << args);
  std::cout << tracker_ui::tagged(tracker_ui::YELLOW, "[warn]", ss.str())
            << std::endl;
}

template <typename... Args>
void log_tracker_err(Args const&... args) {
  std::lock_guard<std::mutex> lock(tracker_ui::cout_mutex());
  std::stringstream ss;
  (ss << ... << args);
  std::cout << tracker_ui::tagged(tracker_ui::RED, "[error]", ss.str())
            << std::endl;
}

#if P2P_LOG_LEVEL >= LOG_DEBUG
template <typename... Args>
void log_tracker_debug(Args const&... args) {
  std::lock_guard<std::mutex> lock(tracker_ui::cout_mutex());
  std::stringstream ss;
  (ss << ... << args);
  std::cout << tracker_ui::styled(tracker_ui::DIM, ss.str()) << std::endl;
}
#else
#define log_tracker_debug(...) ((void)0)
#endif
