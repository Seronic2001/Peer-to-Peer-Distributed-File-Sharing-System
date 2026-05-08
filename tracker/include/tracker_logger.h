#pragma once

#include <iostream>
#include <mutex>
#include <sstream>

inline std::mutex tracker_cout_mutex;

// A thread-safe logger for the tracker.
// Prints any number of arguments to the console atomically.
template <typename... Args>
void log_tracker(Args const&... args) {
  std::lock_guard<std::mutex> lock(tracker_cout_mutex);
  std::stringstream ss;
  // Use a fold expression (C++17) to stream all arguments.
  (ss << ... << args);
  std::cout << ss.str() << std::endl;
}
