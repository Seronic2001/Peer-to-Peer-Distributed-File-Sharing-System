#pragma once

#include <atomic>
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
  // Sends a state-changing command to the backup, if one is connected. A
  // backup that connects later receives the full state instead.
  void forward_command(const std::string& command_str);

 private:
  void run();
  void run_primary_mode(int listener_fd);
  void run_backup_mode();
  int connect_to_other_tracker() const;

  int self_id;
  int other_id;
  TrackerState& state;
  // False in a single-tracker deployment: there is nobody to elect with.
  bool has_other_tracker;
  TrackerInfo self_info;
  TrackerInfo other_tracker_info;

  std::atomic<bool> is_primary;
  std::atomic<bool> should_stop;

  std::mutex replication_mutex;  // Guards backup_sock and writes to it.
  int backup_sock = -1;
  std::thread replication_thread;
};
