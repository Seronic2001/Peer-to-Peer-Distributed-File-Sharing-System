#pragma once

#include <iostream>
#include <mutex>
#include <sstream>

extern std::mutex cout_mutex;
extern void redraw_line();

// This function can accept any number of arguments of any type that can be
// streamed to std::cout (e.g., strings, numbers).
template <typename... Args>
void log_message(Args... args) {
  std::lock_guard<std::mutex> lock(cout_mutex);
  std::stringstream ss;

  // This is a C++17 "fold expression" that streams all arguments into ss
  (ss << ... << args);

  std::cout << "\r\x1b[K";
  std::cout << ss.str() << std::endl;
  redraw_line();
  std::cout << std::flush;
}
