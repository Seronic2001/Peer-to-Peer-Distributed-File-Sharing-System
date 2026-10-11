#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "include/replication_manager.h"
#include "include/tracker_logger.h"
#include "include/tracker_state.h"
#include "protocol.h"

// Global State
TrackerState tracker_state;
std::atomic<bool> should_exit(false);
int lock_fd = -1;  // file descriptor for tracker lock
std::string lock_file_path;

// Lock File Utility
bool acquire_tracker_lock(int tracker_no) {
  lock_file_path = "/tmp/tracker_" + std::to_string(tracker_no) + ".lock";
  lock_fd = open(lock_file_path.c_str(), O_CREAT | O_RDWR, 0666);
  if (lock_fd < 0) {
    perror("open lock file");
    return false;
  }
  if (lockf(lock_fd, F_TLOCK, 0) < 0) {
    // Another instance is already running
    close(lock_fd);
    lock_fd = -1;
    return false;
  }
  // Keep fd open so lock remains held until process exits
  return true;
}

void release_tracker_lock() {
  if (lock_fd >= 0) {
    close(lock_fd);
    unlink(lock_file_path.c_str());
    lock_fd = -1;
  }
}

// Function to parse tracker info
bool parse_tracker_info(const std::string& file_path,
                        std::vector<TrackerInfo>& trackers) {
  int fd = open(file_path.c_str(), O_RDONLY);
  if (fd < 0) {
    log_tracker("Error: Could not open tracker info file: ", file_path);
    return false;
  }

  char buffer[4096];
  ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
  close(fd);

  if (bytes_read <= 0) {
    log_tracker(
        "Error: Could not read from tracker info file or file is empty.");
    return false;
  }
  buffer[bytes_read] = '\0';  // Null-terminate the buffer

  std::stringstream ss(buffer);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty())
      continue;
    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos) {
      log_tracker_err("Invalid format in tracker_info.txt: ", line);
      return false;
    }
    TrackerInfo info;
    info.ip = line.substr(0, colon_pos);
    try {
      int port = std::stoi(line.substr(colon_pos + 1));
      if (port <= 0 || port > 65535) {
        log_tracker("Error: Port out of range in tracker_info.txt: ", line);
        return false;
      }
      info.port = port;
    } catch (const std::exception&) {
      log_tracker("Error: Non-numeric port in tracker_info.txt: ", line);
      return false;
    }
    trackers.push_back(info);
  }
  return true;
}

// Handles console input to listen for the 'quit' command.
void console_handler() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "quit") {
      should_exit = true;
      break;
    }
  }
}

using Tokens = std::vector<std::string>;

// A command available to a logged-in client.
struct CommandSpec {
  size_t num_tokens;  // Command name plus its arguments.
  const char* usage;
  // State-changing commands are forwarded to the backup on success, with the
  // acting user's id appended as the last token.
  bool replicate;
  std::string (*run)(const Tokens& tokens, const std::string& user_id);
};

const std::unordered_map<std::string, CommandSpec>& session_commands() {
  static const std::unordered_map<std::string, CommandSpec> commands = {
      {"create_group",
       {2, "create_group <group_id>", true,
        [](const Tokens& t, const std::string& user) {
          return tracker_state.handle_create_group(t[1], user);
        }}},
      {"join_group",
       {2, "join_group <group_id>", true,
        [](const Tokens& t, const std::string& user) {
          return tracker_state.handle_join_group(t[1], user);
        }}},
      {"leave_group",
       {2, "leave_group <group_id>", true,
        [](const Tokens& t, const std::string& user) {
          return tracker_state.handle_leave_group(t[1], user);
        }}},
      {"accept_request",
       {3, "accept_request <group_id> <user_id>", true,
        [](const Tokens& t, const std::string& user) {
          return tracker_state.handle_accept_request(t[1], t[2], user);
        }}},
      {"upload_file",
       {5, "upload_file <group_id> <file_name> <file_size> <hashes>", true,
        [](const Tokens& t, const std::string& user) {
          long long file_size = -1;
          try {
            file_size = std::stoll(t[3]);
          } catch (const std::exception&) {
          }
          if (file_size < 0) {
            return std::string(
                "ERROR: File size must be a non-negative integer.");
          }
          return tracker_state.handle_upload_file(t[1], t[2], file_size, t[4],
                                                  user);
        }}},
      {"stop_share",
       {3, "stop_share <group_id> <file_name>", true,
        [](const Tokens& t, const std::string& user) {
          return tracker_state.handle_stop_share(t[1], t[2], user);
        }}},
      {"start_downloading",
       {3, "start_downloading <group_id> <file_name>", true,
        [](const Tokens& t, const std::string& user) {
          return tracker_state.handle_start_downloading(t[1], t[2], user);
        }}},
      {"stop_leeching",
       {3, "stop_leeching <group_id> <file_name>", true,
        [](const Tokens& t, const std::string& user) {
          return tracker_state.handle_stop_leeching(t[1], t[2], user);
        }}},
      {"list_groups",
       {1, "list_groups", false,
        [](const Tokens&, const std::string&) {
          return tracker_state.handle_list_groups();
        }}},
      {"list_requests",
       {2, "list_requests <group_id>", false,
        [](const Tokens& t, const std::string& user) {
          return tracker_state.handle_list_requests(t[1], user);
        }}},
      {"list_files",
       {2, "list_files <group_id>", false,
        [](const Tokens& t, const std::string&) {
          return tracker_state.handle_list_files(t[1]);
        }}},
      {"download_file",
       {3, "download_file <group_id> <file_name>", false,
        [](const Tokens& t, const std::string& user) {
          return tracker_state.handle_download_file(t[1], t[2], user);
        }}},
  };
  return commands;
}

bool is_success(const std::string& response) {
  return response.rfind("SUCCESS", 0) == 0;
}

// A peer port as sent with login: decimal digits in the range 1-65535.
bool is_valid_port(const std::string& port) {
  if (port.empty() || port.size() > 5 ||
      port.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  int value = std::stoi(port);
  return value > 0 && value <= 65535;
}

// Runs one client command and returns the response. `current_user_id` is the
// connection's session: empty until a successful login.
std::string run_client_command(int client_sock,
                               const std::string& message,
                               const Tokens& tokens,
                               std::string& current_user_id,
                               ReplicationManager& replication_manager) {
  const std::string& command = tokens[0];

  if (command == "create_user") {
    if (tokens.size() != 3) {
      return "ERROR: Usage: create_user <user_id> <password>";
    }
    std::string response =
        tracker_state.handle_create_user(tokens[1], tokens[2]);
    if (is_success(response)) {
      replication_manager.forward_command(message);
    }
    return response;
  }

  if (command == "login") {
    if (!current_user_id.empty()) {
      return "ERROR: A user is already logged in on this connection.";
    }
    // The client appends its peer port as the fourth token.
    if (tokens.size() != 4 || !is_valid_port(tokens[3])) {
      return "ERROR: Usage: login <user_id> <password>";
    }
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    char client_ip[INET_ADDRSTRLEN];
    if (getpeername(client_sock, (struct sockaddr*)&addr, &addr_len) != 0 ||
        !inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip))) {
      return "ERROR: Could not determine your address.";
    }
    std::string response = tracker_state.handle_login(tokens[1], tokens[2]);
    if (is_success(response)) {
      current_user_id = tokens[1];
      tracker_state.add_peer_address(
          current_user_id, std::string(client_ip) + ":" + tokens[3]);
    }
    return response;
  }

  if (current_user_id.empty()) {
    return "ERROR: You must be logged in to perform this action.";
  }

  if (command == "logout") {
    if (tokens.size() != 1) {
      return "ERROR: logout takes no arguments.";
    }
    std::string response = tracker_state.handle_logout(current_user_id);
    current_user_id.clear();
    return response;
  }

  auto it = session_commands().find(command);
  if (it == session_commands().end()) {
    return "ERROR: Unknown command '" + command + "'";
  }
  const CommandSpec& spec = it->second;
  if (tokens.size() != spec.num_tokens) {
    return std::string("ERROR: Usage: ") + spec.usage;
  }
  std::string response = spec.run(tokens, current_user_id);
  if (spec.replicate && is_success(response)) {
    replication_manager.forward_command(message + " " + current_user_id);
  }
  return response;
}

// Handles all communication with a single connected client.
void handle_client(int client_sock, ReplicationManager& replication_manager) {
  if (replication_manager.is_primary_()) {
    sendMessage(client_sock, "ROLE PRIMARY");
  } else {
    sendMessage(client_sock, "ROLE BACKUP");
    close(client_sock);
    log_tracker_debug("[Client Thread] Refused connection from socket ",
                      client_sock, " (I am a backup)");
    return;
  }
  log_tracker_debug("[Client Thread] New client connected on socket ",
                    client_sock);

  std::string current_user_id;
  std::string message;
  while (receiveMessage(client_sock, message)) {
    log_tracker_debug("[Client Thread] Received from ", client_sock, ": ",
                      message);

    std::stringstream ss(message);
    Tokens tokens;
    std::string token;
    while (ss >> token) {
      tokens.push_back(token);
    }
    // Every message gets exactly one response; clients rely on that.
    std::string response =
        tokens.empty() ? "ERROR: Empty command."
                       : run_client_command(client_sock, message, tokens,
                                            current_user_id,
                                            replication_manager);

    log_tracker_debug("[Client Thread] Sending response to ", client_sock,
                      ": ", response);
    sendMessage(client_sock, response);
  }

  if (!current_user_id.empty()) {
    log_tracker("[Client Thread] Client disconnected: ", current_user_id);
    tracker_state.handle_logout(current_user_id);
  } else {
    log_tracker("[Client Thread] Client disconnected: socket ", client_sock);
  }
  close(client_sock);
}

int main(int argc, char const* argv[]) {
  if (argc != 3) {
    log_tracker("Usage: ", argv[0], " <tracker_info.txt> <tracker_no>");
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);  // Ignore SIGPIPE globally

  std::string tracker_info_path = argv[1];
  int tracker_no = 0;
  try {
    tracker_no = std::stoi(argv[2]);
  } catch (const std::exception&) {
    log_tracker("Error: tracker_no must be an integer, got '", argv[2], "'.");
    return 1;
  }

  // Enforce single instance per tracker ID
  if (!acquire_tracker_lock(tracker_no)) {
    log_tracker_err("Another instance of tracker ", tracker_no,
                    " is already running.");
    return 1;
  }
  atexit(release_tracker_lock);

  std::vector<TrackerInfo> all_trackers;
  if (!parse_tracker_info(tracker_info_path, all_trackers)) {
    return 1;
  }

  if (tracker_no <= 0 || (size_t)tracker_no > all_trackers.size()) {
    log_tracker("[Error] Invalid tracker number specified.");
    return 1;
  }

  // Start Console Handler Thread
  std::thread console_thread(console_handler);
  console_thread.detach();

  // Start Replication Manager
  ReplicationManager replication_manager(tracker_no, all_trackers,
                                         tracker_state);
  replication_manager.start();

  // Setup Client-Facing Server Socket
  TrackerInfo my_info = all_trackers[tracker_no - 1];
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);

  // socket setup
  struct sockaddr_in address;
  int opt = 1;
  socklen_t addrlen = sizeof(address);
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(my_info.port);

  if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
    if (errno == EADDRINUSE) {
      log_tracker("[Error] Client port ", my_info.port,
                  " is already in use. Is another tracker instance running?");
    } else {
      log_tracker_err("Bind failed for client port: ", strerror(errno));
    }
    exit(EXIT_FAILURE);
  }

  listen(server_fd, 10);

  log_tracker("Tracker ", tracker_no, " setup on port ", my_info.port,
              ". Waiting for role...");
  log_tracker_warn("Type 'quit' and press Enter to shut down.");

  while (!should_exit) {
    if (replication_manager.is_primary_()) {
      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(server_fd, &read_fds);

      struct timeval timeout;
      timeout.tv_sec = 1;  // Check for exit signal every 1 second
      timeout.tv_usec = 0;

      int activity;
      // Wrap select in a loop to handle EINTR
      do {
        activity = select(server_fd + 1, &read_fds, NULL, NULL, &timeout);
      } while (activity < 0 && errno == EINTR);

      if (activity < 0) {
        // A real error occurred (not an interruption)
        perror("select error");
        break;
      }

      if (activity > 0 && FD_ISSET(server_fd, &read_fds)) {
        int new_socket =
            accept(server_fd, (struct sockaddr*)&address, &addrlen);
        if (new_socket < 0)
          continue;
        std::thread client_handler(handle_client, new_socket,
                                   std::ref(replication_manager));
        client_handler.detach();
      }
    } else {
      // If in backup mode, we don't accept client connections.
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  // Graceful Shutdown
  log_tracker("Shutdown signal received. Closing server...");
  replication_manager.stop();
  close(server_fd);
  // Give detached threads a moment to finish cleanly
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return 0;
}
