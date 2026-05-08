
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>

#include "include/replication_manager.h"
#include "include/tracker_logger.h"
#include "include/tracker_state.h"
#include "protocol.h"

ReplicationManager::ReplicationManager(int self_id,
                                       const std::vector<TrackerInfo>& trackers,
                                       TrackerState& state)
    : self_id(self_id), state(state), should_stop(false) {
  self_info = trackers[self_id - 1];
  other_tracker_info = trackers[self_id == 1 ? 1 : 0];
  is_primary = false;  // Start undecided, election will decide
}

ReplicationManager::~ReplicationManager() {
  stop();
}

void ReplicationManager::start() {
  replication_thread = std::thread(&ReplicationManager::run, this);
}

void ReplicationManager::stop() {
  should_stop = true;
  if (replication_thread.joinable()) {
    replication_thread.join();
  }
}

bool ReplicationManager::is_primary_() const {
  return is_primary.load();
}

void ReplicationManager::forward_command(const std::string& command_str) {
  log_tracker("[Replication] Attempting to forward: ", command_str);

  std::lock_guard<std::mutex> lock(replication_mutex);

  // Always enqueue the command
  command_queue.push_back(command_str);

  // Cap queue size
  if (command_queue.size() > 1000) {
    log_tracker("[Replication] Command queue too large. Forcing full resync.");
    command_queue.clear();
    // Instead of commands, rely on next FULL_STATE_SYNC
    return;
  }

  if (is_primary && backup_sock >= 0) {
    if (!sendMessage(backup_sock, command_str)) {
      log_tracker("[Replication] Failed to forward command, keeping in queue: ",
                  command_str);
      close(backup_sock);
      backup_sock = -1;
    }
  } else if (is_primary) {
    log_tracker("[Replication] Queued command (backup not connected): ",
                command_str);
  }
}

void ReplicationManager::run() {
  // Start a listener for replication (always)
  int listener_fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(self_info.port + 1000);

  int opt = 1;
  setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
             sizeof(opt));

  if (bind(listener_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
    log_tracker("[Error] bind for replication port failed: ", strerror(errno));
    close(listener_fd);
    return;
  }

  listen(listener_fd, 1);

  while (!should_stop) {
    // Try connecting to the other tracker first (for election)
    int probe_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in other_addr;
    other_addr.sin_family = AF_INET;
    other_addr.sin_port = htons(other_tracker_info.port + 1000);
    inet_pton(AF_INET, other_tracker_info.ip.c_str(), &other_addr.sin_addr);
    if (connect(probe_sock, (struct sockaddr*)&other_addr, sizeof(other_addr)) <
        0) {
      if (!is_primary) {
        log_tracker("[Replication] No active primary found. Becoming PRIMARY.");
        is_primary = true;
      }
      close(probe_sock);
    } else {
      log_tracker("[Replication] Found active primary. Staying BACKUP.");
      is_primary = false;
      close(probe_sock);
    }

    if (is_primary) {
      run_primary_mode(listener_fd);
    } else {
      run_backup_mode();
    }
  }
  close(listener_fd);
}

void ReplicationManager::run_primary_mode(int listener_fd) {
  log_tracker("[Replication] Running in PRIMARY mode.");

  // Ensure backup socket is reset at the start of primary mode
  {
    std::lock_guard<std::mutex> lock(replication_mutex);
    if (backup_sock >= 0) {
      close(backup_sock);
    }
    backup_sock = -1;
  }

  while (!should_stop && is_primary) {
    int current_backup_sock = -1;
    {
      std::lock_guard<std::mutex> lock(replication_mutex);
      current_backup_sock = backup_sock;
    }

    if (current_backup_sock < 0) {
      log_tracker("[Replication] Waiting for a backup tracker to connect...");

      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(listener_fd, &read_fds);

      struct timeval timeout;
      timeout.tv_sec = 2;  // Check for a new connection every 2 seconds
      timeout.tv_usec = 0;

      int activity = select(listener_fd + 1, &read_fds, NULL, NULL, &timeout);
      if (activity > 0) {
        int new_sock = accept(listener_fd, NULL, NULL);
        if (new_sock >= 0) {
          log_tracker("[Replication] Backup tracker connected.");
          log_tracker("[Replication] Sending full state to new backup...");
          std::string full_state = state.serialize_state();
          std::string sync_message = "FULL_STATE_SYNC\n" + full_state;

          if (!sendMessage(new_sock, sync_message)) {
            log_tracker("[Replication] Failed to send state to backup.");
            close(new_sock);
          } else {
            log_tracker("[Replication] Full state sync complete.");
            std::lock_guard<std::mutex> lock(replication_mutex);
            backup_sock = new_sock;

            // Flush queued commands
            for (const auto& cmd : command_queue) {
              if (!sendMessage(backup_sock, cmd)) {
                log_tracker(
                    "[Replication] Failed while flushing queue. "
                    "Will retry on next reconnect.");
                close(backup_sock);
                backup_sock = -1;
                break;
              }
            }
            if (backup_sock >= 0) {
              log_tracker("[Replication] Command queue flushed to backup.");
              command_queue.clear();
            }
          }
        }
      }
    } else {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      std::lock_guard<std::mutex> lock(replication_mutex);
      if (backup_sock >= 0) {
        if (!sendMessage(backup_sock, "HEARTBEAT")) {
          log_tracker(
              "[Replication] Backup disconnected. Will wait for reconnect.");
          close(backup_sock);
          backup_sock = -1;
        }
      }
    }
  }

  // Cleanup on exit
  std::lock_guard<std::mutex> lock(replication_mutex);
  if (backup_sock >= 0)
    close(backup_sock);
}

void ReplicationManager::run_backup_mode() {
  log_tracker("[Replication] Running in BACKUP mode. Waiting for primary...");

  int primary_sock = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in primary_addr;
  primary_addr.sin_family = AF_INET;
  primary_addr.sin_port = htons(other_tracker_info.port + 1000);
  inet_pton(AF_INET, other_tracker_info.ip.c_str(), &primary_addr.sin_addr);

  // Keep trying to connect to the primary
  while (connect(primary_sock, (struct sockaddr*)&primary_addr,
                 sizeof(primary_addr)) < 0) {
    if (should_stop) {
      close(primary_sock);
      return;
    }
    log_tracker("[Replication] Could not connect to primary. Retrying...");
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }

  log_tracker("[Replication] Primary tracker connected.");

  while (!should_stop && !is_primary) {
    std::string message;
    if (receiveMessage(primary_sock, message)) {
      if (message != "HEARTBEAT") {
        log_tracker("[Replication] Received command: ", message);
        state.process_replicated_command(message);
      }
    } else {
      log_tracker("[Replication] Primary disconnected. Promoting to PRIMARY.");
      is_primary = true;
      state.clear_sessions();
      log_tracker(
          "[Replication] All client sessions invalidated. Clients must "
          "re-login.");
      break;  // Exit loop to re-evaluate role
    }
  }
  close(primary_sock);
}