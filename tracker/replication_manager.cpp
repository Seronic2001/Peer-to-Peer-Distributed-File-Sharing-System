
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>

#include "include/replication_manager.h"
#include "include/tracker_logger.h"
#include "include/tracker_state.h"
#include "protocol.h"

// Protocol on the replication port (client port + 1000): a PRIMARY accepts
// the connection and immediately sends "FULL_STATE_SYNC\n<state>", followed
// by forwarded commands and a HEARTBEAT every 2 seconds. A tracker that is
// not primary accepts nothing, so whoever connects to it hears silence. That
// silence is what tells two trackers starting together that neither of them
// is primary yet.
namespace {

const int HEARTBEAT_INTERVAL_SEC = 2;
// How long a tracker waits for the other side's first message before
// concluding that it is not a primary.
const int ROLE_WAIT_SEC = 2;
// A backup gives up on a primary that misses three heartbeats.
const int PRIMARY_SILENCE_SEC = 3 * HEARTBEAT_INTERVAL_SEC;

void set_io_timeout(int sock, int optname, int seconds) {
  struct timeval timeout;
  timeout.tv_sec = seconds;
  timeout.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, optname, &timeout, sizeof(timeout));
}

}  // namespace

ReplicationManager::ReplicationManager(int self_id,
                                       const std::vector<TrackerInfo>& trackers,
                                       TrackerState& state)
    : self_id(self_id), other_id(self_id == 1 ? 2 : 1), state(state),
      has_other_tracker(trackers.size() > 1), should_stop(false) {
  self_info = trackers[self_id - 1];
  // With a single tracker there is no peer; other_tracker_info is then an
  // unused placeholder.
  other_tracker_info = trackers[has_other_tracker ? other_id - 1 : 0];
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
  std::lock_guard<std::mutex> lock(replication_mutex);
  if (backup_sock < 0) {
    return;  // No backup: it gets the full state when it connects.
  }
  log_tracker_debug("[Replication] Forwarding: ", command_str);
  if (!sendMessage(backup_sock, command_str)) {
    log_tracker("[Replication] Backup disconnected. Will wait for reconnect.");
    close(backup_sock);
    backup_sock = -1;
  }
}

void ReplicationManager::run() {
  // The replication listener exists in every role, but only a primary
  // accepts connections on it.
  int listener_fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(self_info.port + 1000);

  int opt = 1;
  setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  if (bind(listener_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
    log_tracker("[Error] bind for replication port failed: ", strerror(errno));
    close(listener_fd);
    return;
  }
  listen(listener_fd, 16);

  if (has_other_tracker) {
    run_backup_mode();  // Returns once this tracker must become primary.
  } else {
    log_tracker("[Replication] Single tracker setup.");
  }

  if (!should_stop) {
    // A new primary starts with no sessions: clients must log in again,
    // which also publishes their peer addresses here.
    state.clear_sessions();
    is_primary = true;
    run_primary_mode(listener_fd);
  }
  close(listener_fd);
}

void ReplicationManager::run_primary_mode(int listener_fd) {
  log_tracker("[Replication] Running in PRIMARY mode.");

  while (!should_stop) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(listener_fd, &read_fds);
    struct timeval timeout;
    timeout.tv_sec = HEARTBEAT_INTERVAL_SEC;
    timeout.tv_usec = 0;
    int activity = select(listener_fd + 1, &read_fds, NULL, NULL, &timeout);

    if (activity > 0) {
      // A (re)connecting backup always replaces the previous connection.
      int new_sock = accept(listener_fd, NULL, NULL);
      if (new_sock < 0)
        continue;
      set_io_timeout(new_sock, SO_SNDTIMEO, 5);

      // Snapshot and switch-over happen under the replication lock, so no
      // forwarded command can fall between the snapshot and live forwarding.
      std::lock_guard<std::mutex> lock(replication_mutex);
      if (backup_sock >= 0)
        close(backup_sock);
      backup_sock = -1;
      if (sendMessage(new_sock, "FULL_STATE_SYNC\n" + state.serialize_state())) {
        log_tracker("[Replication] Backup tracker connected; full state sent.");
        backup_sock = new_sock;
      } else {
        log_tracker("[Replication] Failed to send state to backup.");
        close(new_sock);
      }
    } else if (activity == 0) {
      std::lock_guard<std::mutex> lock(replication_mutex);
      if (backup_sock >= 0 && !sendMessage(backup_sock, "HEARTBEAT")) {
        log_tracker(
            "[Replication] Backup disconnected. Will wait for reconnect.");
        close(backup_sock);
        backup_sock = -1;
      }
    }
  }

  std::lock_guard<std::mutex> lock(replication_mutex);
  if (backup_sock >= 0)
    close(backup_sock);
  backup_sock = -1;
}

// Connects to the other tracker's replication port. Returns the socket, or
// -1 if the other tracker is not reachable.
int ReplicationManager::connect_to_other_tracker() const {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return -1;
  // The send timeout also bounds connect(), so a dead host cannot hang us.
  set_io_timeout(sock, SO_SNDTIMEO, ROLE_WAIT_SEC);
  set_io_timeout(sock, SO_RCVTIMEO, ROLE_WAIT_SEC);

  struct sockaddr_in other_addr;
  other_addr.sin_family = AF_INET;
  other_addr.sin_port = htons(other_tracker_info.port + 1000);
  if (inet_pton(AF_INET, other_tracker_info.ip.c_str(), &other_addr.sin_addr) <=
          0 ||
      connect(sock, (struct sockaddr*)&other_addr, sizeof(other_addr)) < 0) {
    close(sock);
    return -1;
  }
  return sock;
}

// Follows the other tracker for as long as it acts as primary. Returns when
// this tracker should take over (or on shutdown).
void ReplicationManager::run_backup_mode() {
  bool was_following = false;

  while (!should_stop) {
    int primary_sock = connect_to_other_tracker();
    if (primary_sock < 0) {
      log_tracker("[Replication] Other tracker is not reachable.");
      return;
    }

    std::string message;
    if (!receiveMessage(primary_sock, message)) {
      // Reachable but silent: the other tracker is not serving as primary.
      close(primary_sock);
      if (was_following) {
        log_tracker("[Replication] Primary stopped responding.");
        return;
      }
      if (self_id < other_id) {
        // Both trackers are undecided: the lower id takes the role.
        log_tracker("[Replication] No active primary found.");
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }

    log_tracker("[Replication] Found active primary. Running in BACKUP mode.");
    was_following = true;
    set_io_timeout(primary_sock, SO_RCVTIMEO, PRIMARY_SILENCE_SEC);
    do {
      if (message != "HEARTBEAT") {
        log_tracker_debug("[Replication] Received command: ", message);
        state.process_replicated_command(message);
      }
    } while (!should_stop && receiveMessage(primary_sock, message));
    close(primary_sock);

    // The connection dropped or went quiet. Loop to find out whether the
    // primary is really gone before taking over.
    if (!should_stop)
      log_tracker("[Replication] Lost contact with primary. Re-checking...");
  }
}
