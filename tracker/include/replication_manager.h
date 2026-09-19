#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tracker_state.h"

struct TrackerInfo {
  std::string ip;
  int port;
};

class ReplicationManager {
 public:
  ReplicationManager(int self_id,
                     const std::vector<TrackerInfo>& trackers,
                     TrackerState& state);
  ~ReplicationManager();

  void start();
  void stop();

  bool is_primary_() const;
  void forward_command(const std::string& command_str);

 private:
  void run();
  void run_primary_mode(int listener_fd);
  void run_backup_mode();

  int self_id;
  TrackerState& state;
  // Copy of the tracker list: run() needs the size to decide whether an
  // election peer even exists (single-tracker deployments must not probe).
  std::vector<TrackerInfo> trackers_ref;
  TrackerInfo self_info;
  TrackerInfo other_tracker_info;

  std::atomic<bool> is_primary;
  std::atomic<bool> should_stop;

  std::mutex replication_mutex;
  int backup_sock = -1;
  std::thread replication_thread;

  std::deque<std::string> command_queue;
};