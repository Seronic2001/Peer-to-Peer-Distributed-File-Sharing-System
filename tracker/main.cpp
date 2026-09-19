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
      log_tracker("Error: Invalid format in tracker_info.txt: ", line);
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

// Handles all communication with a single connected client.
void handle_client(int client_sock, ReplicationManager& replication_manager) {
  if (replication_manager.is_primary_()) {
    sendMessage(client_sock, "ROLE PRIMARY");
  } else {
    sendMessage(client_sock, "ROLE BACKUP");
    close(client_sock);
    log_tracker("[Client Thread] Refused connection from socket ", client_sock,
                " (I am a backup)");
    return;
  }
  log_tracker("[Client Thread] New client connected on socket ", client_sock);

  std::string current_user_id;
  bool client_is_logged_in = false;

  std::string message;
  while (receiveMessage(client_sock, message)) {
    log_tracker("[Client Thread] Received from ", client_sock, ": ", message);

    std::stringstream ss(message);
    std::vector<std::string> tokens;
    std::string token;
    while (ss >> token) {
      tokens.push_back(token);
    }

    if (tokens.empty())
      continue;

    std::string command = tokens[0];
    std::string response;

    if (command == "login" && client_is_logged_in) {
      response = "ERROR: A user is already logged in on this connection.";
    } else if (command != "create_user" && command != "login" &&
               current_user_id.empty()) {
      response = "ERROR: You must be logged in to perform this action.";
    } else {
      // Command Handling
      if (command == "create_user") {
        if (tokens.size() != 3) {
          response = "ERROR: Usage: create_user <user_id> <password>";
        } else {
          response = tracker_state.handle_create_user(tokens[1], tokens[2]);
          if (response.rfind("SUCCESS", 0) == 0) {
            replication_manager.forward_command(message);
          }
        }
      } else if (command == "login") {
        if (tokens.size() != 4) {  // Here i am also sending the port
          response = "ERROR: Usage: login <user_id> <password>";          } else {
            response = tracker_state.handle_login(tokens[1], tokens[2]);
            if (response.rfind("SUCCESS", 0) == 0) {
              current_user_id = tokens[1];
              client_is_logged_in = true;
              struct sockaddr_in addr;
              socklen_t addr_len = sizeof(addr);
              if (getpeername(client_sock, (struct sockaddr*)&addr, &addr_len) ==
                  0) {
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));
                std::string ip_port = std::string(client_ip) + ":" + tokens[3];

                tracker_state.add_peer_address(current_user_id, ip_port);
              } else {
                perror("getpeername failed");
              }
              // Forward a placeholder port to the backup: the backup has no
              // client socket, so it cannot learn the peer IP. Storing the
              // port here would create the bogus address "<port>:<port>".
              // The backup is a metadata mirror only; a newly promoted
              // primary invalidates sessions, forcing re-login (which then
              // publishes real addresses to the new primary).
              replication_manager.forward_command("login " + tokens[1] + " " +
                                                  tokens[2] + " 0");
            }
          }
      } else if (command == "logout") {
        if (tokens.size() != 1) {
          response = "ERROR: logout takes no arguments.";
        } else {
          response = tracker_state.handle_logout(current_user_id);
          if (response.rfind("SUCCESS", 0) == 0) {
            replication_manager.forward_command(command + " " +
                                                current_user_id);
            current_user_id.clear();
            client_is_logged_in = false;
          }
        }
      } else if (command == "create_group") {
        if (tokens.size() != 2) {
          response = "ERROR: Usage: create_group <group_id>";
        } else {
          response =
              tracker_state.handle_create_group(tokens[1], current_user_id);
          if (response.rfind("SUCCESS", 0) == 0) {
            // Forward with the owner id appended; the backup's replicated
            // form of create_group takes <group_id> <owner_id>.
            replication_manager.forward_command(message + " " +
                                                current_user_id);
          }
        }
      } else if (command == "join_group") {
        if (tokens.size() != 2) {
          response = "ERROR: Usage: join_group <group_id>";
        } else {
          response =
              tracker_state.handle_join_group(tokens[1], current_user_id);
          if (response.rfind("SUCCESS", 0) == 0) {
            // Forward with the requester id appended: without it the backup
            // never records the pending join request.
            replication_manager.forward_command(message + " " +
                                                current_user_id);
          }
        }
      } else if (command == "leave_group") {
        if (tokens.size() != 2) {
          response = "ERROR: Usage: leave_group <group_id>";
        } else {
          response =
              tracker_state.handle_leave_group(tokens[1], current_user_id);
          if (response.rfind("SUCCESS", 0) == 0) {
            replication_manager.forward_command(message + " " +
                                                current_user_id);
          }
        }
      } else if (command == "accept_request") {
        if (tokens.size() != 3) {
          response = "ERROR: Usage: accept_request <group_id> <user_id>";
        } else {
          response = tracker_state.handle_accept_request(tokens[1], tokens[2],
                                                         current_user_id);
          if (response.rfind("SUCCESS", 0) == 0) {
            replication_manager.forward_command(message + " " +
                                                current_user_id);
          }
        }
      } else if (command == "upload_file") {
        if (tokens.size() != 5) {
          response = "ERROR: Malformed upload request.";
        } else {
          long long file_size = 0;
          try {
            file_size = std::stoll(tokens[3]);
          } catch (const std::exception&) {
            response = "ERROR: File size must be an integer.";
            log_tracker("[Client Thread] Sending response to ", client_sock,
                        ": ", response);
            sendMessage(client_sock, response);
            continue;
          }
          if (file_size < 0) {
            response = "ERROR: File size must be non-negative.";
            log_tracker("[Client Thread] Sending response to ", client_sock,
                        ": ", response);
            sendMessage(client_sock, response);
            continue;
          }
          response = tracker_state.handle_upload_file(
              tokens[1], tokens[2], file_size, tokens[4], current_user_id);
          if (response.rfind("SUCCESS", 0) == 0) {
            // Replicated form of upload_file takes the user id as the 6th
            // token; keep forwarding the identity explicitly.
            replication_manager.forward_command(message + " " +
                                                current_user_id);
          }
        }
      } else if (command == "stop_share") {
        if (tokens.size() != 3) {
          response = "ERROR: Usage: stop_share <group_id> <file_name>";
        } else {
          response = tracker_state.handle_stop_share(tokens[1], tokens[2],
                                                     current_user_id);
          if (response.rfind("SUCCESS", 0) == 0) {
            replication_manager.forward_command(message + " " +
                                                current_user_id);
          }
        }
      } else if (command == "start_downloading") {
        if (tokens.size() != 3) {
          response = "ERROR: Usage: start_downloading <group_id> <file_name>";
        } else {
          response = tracker_state.handle_start_downloading(
              tokens[1], tokens[2], current_user_id);
          if (response.rfind("SUCCESS", 0) == 0) {
            replication_manager.forward_command(message + " " +
                                                current_user_id);
          }
        }
      } else if (command == "stop_leeching") {
        if (tokens.size() != 3) {
          response = "ERROR: Usage: stop_leeching <group_id> <file_name>";
        } else {
          response = tracker_state.handle_stop_leeching(tokens[1], tokens[2],
                                                        current_user_id);
          if (response.rfind("SUCCESS", 0) == 0) {
            replication_manager.forward_command(message + " " +
                                                current_user_id);
          }
        }
      }
      // Read-only commands
      else if (command == "list_groups") {
        if (tokens.size() != 1) {
          response = "ERROR: list_groups takes no arguments.";
        } else {
          response = tracker_state.handle_list_groups();
        }
      } else if (command == "list_requests") {
        if (tokens.size() != 2) {
          response = "ERROR: Usage: list_requests <group_id>";
        } else {
          response =
              tracker_state.handle_list_requests(tokens[1], current_user_id);
        }
      } else if (command == "list_files") {
        if (tokens.size() != 2) {
          response = "ERROR: Usage: list_files <group_id>";
        } else {
          response = tracker_state.handle_list_files(tokens[1]);
        }
      } else if (command == "download_file") {
        if (tokens.size() < 3) {
          response =
              "ERROR: Incorrect number of arguments for download_file "
              "command.";
        } else {
          response = tracker_state.handle_download_file(tokens[1], tokens[2],
                                                        current_user_id);
        }
      } else {
        response = "ERROR: Unknown command '" + command + "'";
      }
    }

    log_tracker("[Client Thread] Sending response to ", client_sock, ": ",
                response);
    sendMessage(client_sock, response);
  }

  if (client_is_logged_in && !current_user_id.empty()) {
    log_tracker("[Client Thread] Client disconnected: ", current_user_id);
    tracker_state.handle_logout(current_user_id);
    std::string logout_cmd = "logout " + current_user_id;
    replication_manager.forward_command(logout_cmd);
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
    log_tracker("[Error] Another instance of tracker ", tracker_no,
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
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
             sizeof(opt));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(my_info.port);

  if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
    if (errno == EADDRINUSE) {
      log_tracker("[Error] Client port ", my_info.port,
                  " is already in use. Is another tracker instance running?");
    } else {
      log_tracker("[Error] Bind failed for client port: ", strerror(errno));
    }
    exit(EXIT_FAILURE);
  }

  listen(server_fd, 10);

  log_tracker("Tracker ", tracker_no, " setup on port ", my_info.port,
              ". Waiting for role...");
  log_tracker("Type 'quit' and press Enter to shut down.");

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
