#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hashing.h"
#include "include/client_peer.h"
#include "include/download_manager.h"
#include "include/ui.h"
#include "protocol.h"

// Global State
std::unordered_map<std::string, SeededFileInfo> seeded_files;
std::mutex seeded_files_mutex;
std::unordered_map<std::string, std::shared_ptr<DownloadState>> g_downloads;
std::mutex g_downloads_mutex;
std::atomic<bool> exit_program(false);
std::atomic_bool is_logged_in(false);  // Client-side authentication state
// The authenticated user's id. Logically session-global: the prompt builder
// (which runs on the main thread) and login/logout handlers all use it.
std::string current_user_id = "";

// State for Raw Mode Line Editor
std::string current_line;
size_t cursor_pos = 0;
std::mutex cout_mutex;
struct termios orig_termios;

// Command history for Up/Down arrow navigation.
static std::vector<std::string> input_history;
static size_t history_nav = 0;        // Current position while navigating.
static std::string nav_snapshot;      // Line being typed when nav started.

std::string get_application_home() {
  // This function points to a specific, hardcoded directory.
  const char* home_dir_cstr = getenv("HOME");
  if (home_dir_cstr == nullptr) {
    log_message(
        "Error: Could not determine home directory to build application path.");
    // Fallback to the current directory if HOME isn't set.
    return ".";
  }
  std::string home_dir(home_dir_cstr);

  // Prioritize the specific development directory
  std::string dev_path =
      home_dir + "/Programming/Assignments/AOS/2025201056_A3";
  struct stat st;
  if (stat(dev_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    return dev_path;
  }

  // Fallback to the standard user-specific hidden directory
  return home_dir;
}

void disable_raw_mode() {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
  ui::raw_mode_active.store(false);
}

void enable_raw_mode() {
  tcgetattr(STDIN_FILENO, &orig_termios);
  atexit(disable_raw_mode);
  struct termios raw = orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  ui::raw_mode_active.store(true);
}

// Builds the dynamic colored prompt: green user when logged in, dim red
// "anonymous" otherwise. The caller must hold cout_mutex.
std::string build_prompt() {
  if (is_logged_in.load() && !current_user_id.empty()) {
    return ui::bold_color(ui::GREEN, current_user_id) +
           ui::styled(ui::DIM, "@p2p") +
           ui::bold_color(ui::CYAN, " >> ");
  }
  return ui::styled(ui::DIM, "anon") + ui::styled(ui::DIM, "@p2p") +
         ui::bold_color(ui::CYAN, " >> ");
}

void redraw_line() {
  // Prompt is recomputed every redraw so a login/logout instantly recolors it.
  const std::string prompt = build_prompt();
  std::cout << "\r\x1b[K" << prompt << current_line;
  if (cursor_pos < current_line.size()) {
    size_t back = current_line.size() - cursor_pos;
    std::cout << "\x1b[" << back << "D";
  }
  std::cout << std::flush;
}

// Prints a tracker response with semantics-aware coloring: SUCCESS green,
// ERROR red, anything else cyan.
void log_tracker_response(const std::string& response) {
  if (response.rfind("SUCCESS", 0) == 0) {
    log_message(ui::bold_color(ui::GREEN, "[Tracker]"), " ",
                ui::styled(ui::GREEN, response));
  } else if (response.rfind("ERROR", 0) == 0) {
    log_error(ui::bold_color(ui::RED, "[Tracker]"), " ",
              ui::styled(ui::RED, response));
  } else {
    log_message(ui::bold_color(ui::CYAN, "[Tracker]"), " ", response);
  }
}

// Local 'help' output. Never touches the network.
void print_help() {
  const auto cmd = [](const std::string& name, const std::string& args,
                      const std::string& desc) {
    std::string pad1(16 > name.size() ? 16 - name.size() : 1, ' ');
    std::string pad2(29 > args.size() ? 29 - args.size() : 1, ' ');
    log_message("  ", ui::bold_color(ui::GREEN, name), pad1,
                ui::styled(ui::DIM, args), pad2, desc);
  };
  log_message(ui::bold_color(ui::MAGENTA, "Commands:"));
  cmd("create_user", "<user_id> <password>", "register a new user");
  cmd("login", "<user_id> <password>", "authenticate user session");
  cmd("logout", "", "end current session");
  cmd("create_group", "<group_id>", "create group (owner = you)");
  cmd("join_group", "<group_id>", "request to join a group");
  cmd("leave_group", "<group_id>", "leave a group");
  cmd("accept_request", "<group_id> <user_id>", "approve a pending join request");
  cmd("list_groups", "", "list all groups in network");
  cmd("list_requests", "<group_id>", "list pending join requests");
  cmd("list_files", "<group_id>", "list files shared in group");
  cmd("upload_file", "<group_id> <file_path>", "share a file with group");
  cmd("download_file", "<group> <file> <dest> [alg]", "download [alg: rarest|sequential|random]");
  cmd("stop_share", "<group_id> <file_name>", "stop sharing a file");
  cmd("show_downloads", "", "show active and finished downloads");
  cmd("clear", "", "clear the terminal screen");
  cmd("quit", "", "exit the client");
  log_message(ui::styled(ui::DIM,
                         "Keys: Tab complete · Up/Down history · Left/Right move · "
                         "Home/End · Ctrl+U clear · Ctrl+W delete word"));
}

// ----------------------------------------------------------------------------
// Tab completion + persistent history
// ----------------------------------------------------------------------------

namespace {

// Commands offered for first-word completion (kept in sync with print_help).
const std::vector<std::string>& known_commands() {
  static const std::vector<std::string> cmds = {
      "create_user",     "login",          "logout",           "create_group",
      "join_group",      "leave_group",    "accept_request",   "list_groups",
      "list_requests",   "list_files",     "upload_file",      "download_file",
      "stop_share",      "show_downloads", "clear",            "help",
      "quit",            "exit",
  };
  return cmds;
}

const size_t kMaxHistoryEntries = 200;  // In memory and on disk.

std::string history_file_path() {
  return get_application_home() + "/.p2p_client_history";
}

// Loads the history file, keeping only the newest kMaxHistoryEntries.
// A missing or malformed file simply yields an empty history. When the file
// held more entries than we keep, it is rewritten trimmed so it cannot grow
// without bound.
void load_history() {
  std::ifstream in(history_file_path());
  if (!in) return;
  std::vector<std::string> all;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) all.push_back(line);
  }
  in.close();
  if (all.empty()) return;

  const size_t overflow =
      all.size() > kMaxHistoryEntries ? all.size() - kMaxHistoryEntries : 0;
  input_history.assign(all.begin() + overflow, all.end());
  history_nav = input_history.size();

  if (overflow > 0) {  // Rewrite the file trimmed to the retained tail.
    std::ofstream out(history_file_path(), std::ios::trunc);
    for (const auto& cmd : input_history) out << cmd << "\n";
  }
  log_debug("[History] Loaded ", input_history.size(), " commands from ",
            history_file_path());
}

// Adds a command to the in-memory history (dedup consecutive duplicates,
// like bash) and appends it to the history file so it survives restarts.
// Called from the input thread only.
void record_command(const std::string& cmd) {
  if (cmd.empty()) return;
  if (input_history.empty() || input_history.back() != cmd) {
    input_history.push_back(cmd);
    if (input_history.size() > kMaxHistoryEntries)
      input_history.erase(input_history.begin());
    std::ofstream out(history_file_path(), std::ios::app);
    if (out) out << cmd << "\n";
  }
  history_nav = input_history.size();
}

// Completes the word before the cursor: command names for the first word,
// file paths for later words. Inserts the longest common prefix when several
// candidates match, otherwise lists them above the prompt. Never throws and
// never crashes; caller must hold cout_mutex.
void tab_complete() {
  size_t word_start = cursor_pos;
  while (word_start > 0 &&
         !isspace((unsigned char)current_line[word_start - 1]))
    --word_start;
  const std::string prefix =
      current_line.substr(word_start, cursor_pos - word_start);
  if (prefix.empty()) return;

  std::vector<std::string> matches;
  if (word_start == 0) {
    for (const auto& cmd : known_commands())
      if (cmd.rfind(prefix, 0) == 0) matches.push_back(cmd);
  } else {
    // Split the prefix into directory + filename and list the directory.
    std::string dir = ".";
    std::string file_part = prefix;
    const size_t slash = prefix.rfind('/');
    if (slash != std::string::npos) {
      dir = prefix.substr(0, slash + 1);
      file_part = prefix.substr(slash + 1);
    }
    if (!dir.empty() && dir[0] == '~') {  // Basic ~ expansion.
      if (const char* home = getenv("HOME")) {
        if (dir == "~") {
          dir = home;
        } else if (dir.rfind("~/", 0) == 0) {
          dir = std::string(home) + dir.substr(1);
        }
      }
    }
    if (DIR* d = opendir(dir.c_str())) {
      struct dirent* ent;
      while ((ent = readdir(d)) != nullptr) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (name.rfind(file_part, 0) != 0) continue;
        if (name[0] == '.' && (file_part.empty() || file_part[0] != '.'))
          continue;  // Hide dotfiles unless explicitly asked for.
        const std::string full = (dir == ".") ? name : dir + name;
        std::string match = full;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
          match += "/";  // Walkable paths for directory matches.
        matches.push_back(match);
      }
      closedir(d);
    }
  }

  if (matches.empty()) return;
  if (matches.size() == 1) {
    std::string completion = matches[0];
    if (word_start == 0 && completion.back() != '/') completion += " ";
    current_line.replace(word_start, cursor_pos - word_start, completion);
    cursor_pos = word_start + completion.size();
    redraw_line();
    return;
  }

  // Multiple matches: complete the longest common prefix first.
  std::string lcp = matches[0];
  for (const auto& m : matches) {
    size_t k = 0;
    while (k < lcp.size() && k < m.size() && lcp[k] == m[k]) ++k;
    lcp.resize(k);
  }
  if (lcp.size() > prefix.size()) {
    current_line.replace(word_start, cursor_pos - word_start, lcp);
    cursor_pos = word_start + lcp.size();
    redraw_line();
    return;
  }

  // Nothing more to type: list the matches above the prompt. Raw mode has
  // OPOST off, so every line break must be an explicit "\r\n".
  const size_t kMaxListed = 50;
  std::cout << "\r\n";
  size_t col = 0;
  size_t shown = 0;
  for (const auto& m : matches) {
    if (shown++ == kMaxListed) {
      std::cout << "  ... and " << (matches.size() - kMaxListed) << " more\r\n";
      break;
    }
    std::string cell = m;
    if (cell.size() < 18) cell.append(18 - cell.size(), ' ');
    std::cout << ui::styled(ui::CYAN, cell);
    if (++col == 4) {
      std::cout << "\r\n";
      col = 0;
    }
  }
  if (col != 0) std::cout << "\r\n";
  redraw_line();
}

}  // namespace

// Helper to check connection health
bool safe_send(int sockfd, const std::string& msg) {
  if (!sendMessage(sockfd, msg)) {
    exit_program = true;
    if (sockfd >= 0)
      close(sockfd);
    return false;
  }
  return true;
}

bool safe_recv(int sockfd, std::string& response) {
  if (!receiveMessage(sockfd, response)) {
    exit_program = true;
    if (sockfd >= 0)
      close(sockfd);
    return false;
  }
  return true;
}

void parse_meta_content(const std::string& content,
                        std::string& group_id,
                        std::string& logical_name,
                        std::string& local_path,
                        long long& file_size,
                        std::string& hashes) {
  std::stringstream ss(content);
  std::string line;
  while (std::getline(ss, line)) {
    size_t colon_pos = line.find(':');
    if (colon_pos != std::string::npos) {
      std::string key = line.substr(0, colon_pos);
      std::string value = line.substr(colon_pos + 1);
      if (key == "group_id")
        group_id = value;
      else if (key == "logical_name")
        logical_name = value;
      else if (key == "local_path")
        local_path = value;
      else if (key == "file_size")
        file_size = std::stoll(value);
      else if (key == "hashes")
        hashes = value;
    }
  }
}

void save_metadata_file(const std::string& user_id,
                        const std::string& group_id,
                        const std::string& file_name,
                        const std::string& local_path,
                        long long file_size,
                        const std::string& hashes) {
  // Create user-specific directories
  std::string app_home = get_application_home();
  std::string p2pclient = app_home + "/.p2p_client";
  std::string p2pclientUser = p2pclient + "/users";
  std::string user_dir = app_home + "/.p2p_client/users/" + user_id;
  mkdir(p2pclient.c_str(), 0755);
  mkdir(p2pclientUser.c_str(), 0755);
  mkdir(user_dir.c_str(), 0755);

  std::string meta_path = user_dir + "/" + group_id + "_" + file_name + ".meta";

  // O_TRUNC will clear the file if it exists, ensuring we write fresh data.
  int fd = open(meta_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) {
    log_error("Could not open .meta file for writing: ", meta_path);
    return;
  }

  std::stringstream ss;
  ss << "group_id:" << group_id << "\n";
  ss << "logical_name:" << file_name << "\n";
  ss << "local_path:" << local_path << "\n";
  ss << "file_size:" << file_size << "\n";
  ss << "hashes:" << hashes << "\n";
  std::string content = ss.str();

  if (write(fd, content.c_str(), content.length()) < 0) {
    log_error("Could not save metadata for '", file_name, "': ",
              strerror(errno));
  }
  close(fd);
}

void load_state_from_disk(const std::string& user_id) {
  std::string app_home = get_application_home();
  if (app_home.empty())
    return;
  std::string user_dir = app_home + "/.p2p_client/users/" + user_id;
  DIR* dir = opendir(user_dir.c_str());
  if (!dir)
    return;  // No state to load, or directory doesn't exist yet.

  std::lock_guard<std::mutex> lock(seeded_files_mutex);
  seeded_files.clear();  // Clear any old state from a previous user

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string filename = entry->d_name;
    if (filename.size() > 5 &&
        filename.substr(filename.size() - 5) == ".meta") {
      std::string meta_path = user_dir + "/" + filename;

      int fd = open(meta_path.c_str(), O_RDONLY);
      if (fd < 0)
        continue;

      char buffer[4096];  // Assume .meta files are smaller than 4KB
      ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
      close(fd);

      if (bytes_read > 0) {
        buffer[bytes_read] = '\0';  // Null-terminate the string
        std::string content(buffer);

        std::string group_id, logical_name, local_path, hashes;
        long long file_size;

        parse_meta_content(content, group_id, logical_name, local_path,
                           file_size, hashes);

        if (!logical_name.empty() && !local_path.empty()) {
          // Check if the actual data file still exists on disk
          struct stat file_stat_buffer;
          if (stat(local_path.c_str(), &file_stat_buffer) == 0) {
            seeded_files[logical_name] = {local_path, file_size};
          } else {
            log_message("[State] Metadata found for '", logical_name,
                        "' but local file is missing.");
          }
        }
      }
    }
  }
  closedir(dir);
  log_debug("[State] Loaded ", seeded_files.size(),
              " seeded files from disk for user ", user_id);
}

int main(int argc, char const* argv[]) {
  if (argc != 3) {
    log_message("Usage: ", argv[0],
                " <YOUR_IP>:<YOUR_PORT> <tracker_info.txt>");
    return 1;
  }

  // Parse self listening address from argv[1]
  std::string self_addr_str = argv[1];
  std::string tracker_info_path = argv[2];
  std::string self_ip;
  std::string self_port_str;
  int self_port;

  size_t colon_pos_self = self_addr_str.find(':');
  if (colon_pos_self == std::string::npos) {
    log_error("Invalid listening address format. Use IP:PORT");
    return 1;
  }
  self_ip = self_addr_str.substr(0, colon_pos_self);
  self_port_str = self_addr_str.substr(colon_pos_self + 1);
  try {
    self_port = std::stoi(self_port_str);
  } catch (const std::exception&) {
    log_error("Invalid port number: '", self_port_str, "'");
    return 1;
  }
  if (self_port <= 0 || self_port > 65535) {
    log_error("Port out of range (1-65535): ", self_port);
    return 1;
  }

  // Read tracker addresses from tracker_info.txt
  std::vector<std::string> tracker_addresses;
  int fd = open(tracker_info_path.c_str(), O_RDONLY);
  if (fd < 0) {
    log_error("Could not open tracker info file: ", tracker_info_path);
    return 1;
  }
  char buffer[1024];  // Assume tracker info file is reasonably small
  ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
  close(fd);

  if (bytes_read > 0) {
    buffer[bytes_read] = '\0';  // Null-terminate the string
    std::stringstream ss(buffer);
    std::string line;
    while (std::getline(ss, line)) {
      if (!line.empty()) {
        tracker_addresses.push_back(line);
      }
    }
  }

  if (tracker_addresses.empty()) {
    log_error("No tracker addresses found in ", tracker_info_path);
    return 1;
  }

  // Loop through trackers and attempt to connect (failover)
  int sockfd = -1;
  bool connected_to_tracker = false;
  struct timeval timeout;
  for (const auto& addr : tracker_addresses) {
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
      perror("socket creation failed");
      continue;
    }

    timeout.tv_sec = 2;  // 2-second timeout
    timeout.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout,
                   sizeof timeout) < 0) {
      perror("setsockopt(SO_RCVTIMEO) failed");
      close(sockfd);
      continue;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout,
                   sizeof timeout) < 0) {
      perror("setsockopt(SO_SNDTIMEO) failed");
      close(sockfd);
      continue;
    }

    size_t colon_pos = addr.find(':');
    if (colon_pos == std::string::npos)
      continue;
    std::string tracker_ip = addr.substr(0, colon_pos);
    int tracker_port = 0;
    try {
      tracker_port = std::stoi(addr.substr(colon_pos + 1));
    } catch (const std::exception&) {
      log_warn("Skipping malformed tracker address: ", addr);
      close(sockfd);
      continue;
    }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(tracker_port);
    if (inet_pton(AF_INET, tracker_ip.c_str(), &serv_addr.sin_addr) <= 0) {
      close(sockfd);
      continue;
    }

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == 0) {
      std::string role_response;
      if (receiveMessage(sockfd, role_response) &&
          role_response == "ROLE PRIMARY") {
        log_message("Successfully connected to PRIMARY tracker at ", addr);
        connected_to_tracker = true;
        break;
      } else {
        log_message("Could not get primary role from ", addr,
                    ". Trying next...");
        close(sockfd);
      }
    } else {
      log_message("Could not connect to tracker at ", addr, ". Trying next...");
      close(sockfd);
    }
  }

  if (!connected_to_tracker) {
    log_error("Failed to connect to any available tracker.");
    return 1;
  }

  struct timeval operation_timeout;
  operation_timeout.tv_sec = 5;  // 5-second timeout for commands
  operation_timeout.tv_usec = 0;
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&operation_timeout,
             sizeof operation_timeout);
  setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&operation_timeout,
             sizeof operation_timeout);

  // Prepare Peer Server Socket with the static address
  int peer_server_fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in peer_addr;
  peer_addr.sin_family = AF_INET;
  peer_addr.sin_port = htons(self_port);
  // Allow rebinding after a restart while old connections from this port
  // sit in TIME_WAIT (mirrors the tracker's socket setup).
  int peer_opt = 1;
  setsockopt(peer_server_fd, SOL_SOCKET, SO_REUSEADDR, &peer_opt,
             sizeof(peer_opt));
  if (inet_pton(AF_INET, self_ip.c_str(), &peer_addr.sin_addr) <= 0) {
    log_error("Invalid listening IP address provided.");
    close(peer_server_fd);
    return 1;
  }

  if (bind(peer_server_fd, (struct sockaddr*)&peer_addr, sizeof(peer_addr)) <
      0) {
    perror("bind (peer server) failed");
    log_error("Could not bind to ", self_addr_str,
              ". Is the port already in use?");
    return 1;
  }

  bool is_peer_server_running = false;

  // Load persisted history from previous runs before the input loop starts.
  load_history();

  // Print welcome banner before entering raw mode so it formats cleanly
  // across all terminal environments.
  {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << "\n"
              << ui::bold_color(ui::MAGENTA,
                                "  ╔══════════════════════════════════════╗")
              << "\n"
              << ui::bold_color(ui::MAGENTA,
                                "  ║  P2P  Distributed  File  Sharing     ║")
              << "\n"
              << ui::bold_color(ui::MAGENTA,
                                "  ╚══════════════════════════════════════╝")
              << "\n\n"
              << ui::styled(ui::DIM,
                            "  Type 'help' for commands, 'quit' to stop.")
              << "\n\n"
              << std::flush;
  }

  // Main Input Loop
  enable_raw_mode();
  {
    std::lock_guard<std::mutex> lock(cout_mutex);
    redraw_line();
    std::cout << std::flush;
  }

  bool safe_exit = false;

  while (!exit_program) {
    // Handle background task completions
    {
      std::lock_guard<std::mutex> lock(g_downloads_mutex);
      for (auto it = g_downloads.begin(); it != g_downloads.end();) {
        auto state = it->second;

        if (state->download_complete.load()) {
          // This download has finished. Let's process it.
          if (state->download_successful.load()) {
            log_message("Download of '", state->file_name,
                        "' complete. Notifying tracker...");

            char resolved_path[PATH_MAX];
            if (realpath(state->destination_path.c_str(), resolved_path) ==
                NULL) {
              log_message("Error: Could not resolve downloaded file path: ",
                          state->destination_path);
              // Even on error, we must remove it from the download list.
              it = g_downloads.erase(it);
              continue;
            }
            std::string absolute_path = resolved_path;

            // Construct the upload_file command to become a seeder.
            std::string upload_cmd = "upload_file " + state->group_id + " " +
                                     state->file_name + " " +
                                     std::to_string(state->file_size) + " " +
                                     state->concatenated_piece_hashes;

            // Send the command using the main thread's safe socket.
            std::string response;
            if (safe_send(sockfd, upload_cmd) && safe_recv(sockfd, response)) {
              log_tracker_response(response);
              if (response.rfind("SUCCESS", 0) == 0) {
                // Update local state now that the tracker has confirmed.
                std::lock_guard<std::mutex> seed_lock(seeded_files_mutex);
                seeded_files[state->file_name] = {absolute_path,
                                                  state->file_size};
                save_metadata_file(current_user_id, state->group_id,
                                   state->file_name, absolute_path,
                                   state->file_size,
                                   state->concatenated_piece_hashes);
              }
            }
          } else {
            log_message("Download of '", state->file_name,
                        "' failed verification.");
            // Inform the tracker that we are no longer leeching this file
            std::string stop_leeching_cmd =
                "stop_leeching " + state->group_id + " " + state->file_name;
            safe_send(sockfd, stop_leeching_cmd);
          }

          // The download is processed, remove it from the active downloads
          // map.
          it = g_downloads.erase(it);

        } else {
          // This download is still in progress, move to the next one.
          ++it;
        }
      }
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);

    struct timeval select_timeout;
    select_timeout.tv_sec = 0;
    select_timeout.tv_usec = 100000;  // Poll every 100ms

    int activity =
        select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &select_timeout);

    if (activity < 0) {
      perror("select error");
      break;
    }

    if (activity > 0 && FD_ISSET(STDIN_FILENO, &read_fds)) {
      char c;
      if (read(STDIN_FILENO, &c, 1) != 1) {
        // Error reading from stdin, break loop
        break;
      }

      if (c == '\n' || c == '\r') {
        std::string command_to_process = current_line;
        current_line.clear();
        cursor_pos = 0;

        std::stringstream ss(command_to_process);
        std::vector<std::string> tokens;
        std::string token;
        while (ss >> token) {
          tokens.push_back(token);
        }
        if (tokens.empty()) {
          std::lock_guard<std::mutex> lock(cout_mutex);
          redraw_line();
          continue;  // Whitespace-only input.
        }

        // Remember the command (dedup consecutive duplicates like bash) in
        // memory and in the history file so it survives restarts.
        record_command(command_to_process);

        std::string command = tokens[0];

        if (command == "clear" || command == "cls") {
          std::lock_guard<std::mutex> lock(cout_mutex);
          std::cout << "\x1b[2J\x1b[3J\x1b[H" << std::flush;
          redraw_line();
          continue;
        }

        log_message(ui::styled(ui::DIM, "ran: "), command_to_process);

        if (command == "quit" || command == "exit") {
          exit_program = true;
          safe_exit = true;
          break;
        }

        if (command == "help" || command == "?") {
          print_help();
          continue;
        }

        bool should_send = true;
        std::string response;

          // Guard every per-command token access below: a malformed command
          // must print usage, never crash the CLI or skip state updates.
          auto has_args = [&](size_t n) {
            if (tokens.size() < n) {
              log_warn("Usage: ", command, " is missing arguments (expected ",
                       n - 1, "). Type 'help' for command list.");
              should_send = false;
              return false;
            }
            return true;
          };

          // Argument and Authentication Pre-Checks
          if (command != "create_user" && command != "login") {
            if (!is_logged_in.load()) {
              log_error("You must be logged in to use this command.");
              should_send = false;
            }
          }

          if (command == "login" && should_send) {
            command_to_process += " " + self_port_str;
          } else if (command == "upload_file") {
            should_send = false;  // Handled specially
            if (!is_logged_in.load()) {
              log_error("You must be logged in to upload a file.");
            } else if (!has_args(3)) {
              // Usage hint already printed by has_args.
            } else {
              std::string group_id = tokens[1];
              std::string user_provided_path = tokens[2];

              // The logical name is the part of the path after the last '/'
              std::string logical_name = user_provided_path;
              size_t last_slash = logical_name.find_last_of('/');
              if (last_slash != std::string::npos) {
                logical_name = logical_name.substr(last_slash + 1);
              }

              // Convert the user-provided path to an absolute path for local
              // storage.
              char resolved_path[PATH_MAX];
              if (realpath(user_provided_path.c_str(), resolved_path) == NULL) {
                log_error("Cannot find or resolve file path: ",
                            user_provided_path);
                continue;
              }
              std::string absolute_path = resolved_path;

              try {
                log_message("Sharing file: ", logical_name, "...");
                FileHashes hashes = compute_hashes(absolute_path);
                log_message("File size: ", hashes.file_size, " bytes");

                std::string upload_cmd = "upload_file " + group_id + " " +
                                         logical_name + " " +
                                         std::to_string(hashes.file_size) +
                                         " " + hashes.concatenated_hashes;

                if (!safe_send(sockfd, upload_cmd))
                  continue;
                if (!safe_recv(sockfd, response))
                  continue;
                log_tracker_response(response);
                if (response.rfind("SUCCESS", 0) == 0) {
                  std::lock_guard<std::mutex> lock(seeded_files_mutex);
                  seeded_files[logical_name] = {absolute_path,
                                                hashes.file_size};

                  // Save metadata LOCALLY with the ABSOLUTE path.
                  save_metadata_file(current_user_id, group_id, logical_name,
                                     absolute_path, hashes.file_size,
                                     hashes.concatenated_hashes);
                }
              } catch (const std::exception& e) {
                log_error("Hashing failed: ", e.what());
              }
            }
          } else if (command == "show_downloads") {
            should_send = false;
            std::lock_guard<std::mutex> lock(g_downloads_mutex);
            if (g_downloads.empty()) {
              log_message("No downloads to show.");
            } else {
              for (const auto& pair : g_downloads) {
                auto state = pair.second;
                if (state->download_complete.load()) {
                  if (state->download_successful.load()) {
                    log_message("[C] [", state->group_id, "] ",
                                state->file_name);
                  } else {
                    log_message("[E] [", state->group_id, "] ",
                                state->file_name, " (Verification Failed)");
                  }
                } else {
                  log_message("[D] [", state->group_id, "] ", state->file_name,
                              " (", state->downloaded_piece_count.load(), "/",
                              state->num_pieces, ")");
                }
              }
            }
          } else if (command == "download_file") {
            should_send = false;
            if (!is_logged_in.load()) {
              log_error("You must be logged in to download a file.");
            } else if (tokens.size() != 4 && tokens.size() != 5) {
              log_warn(
                    "Usage: download_file <group_id> <file_name> "
                    "<destination_path> [algorithm]");
            } else {
              std::string tracker_command =
                  tokens[0] + " " + tokens[1] + " " + tokens[2];
              if (!safe_send(sockfd, tracker_command))
                continue;
              if (!safe_recv(sockfd, response))
                continue;

              if (response.rfind("SUCCESS", 0) == 0) {
                // Parse the response without stream-extraction on the whole
                // string: the hash blob can legitimately contain embedded
                // characters that confuse >> tokenization. Format is
                // "SUCCESS: <file_size> <hashes> [peer ...]".
                size_t body_start = response.find(':');
                body_start = (body_start == std::string::npos)
                                 ? std::string::npos
                                 : response.find_first_not_of(' ',
                                                              body_start + 1);
                size_t size_end = (body_start == std::string::npos)
                                      ? std::string::npos
                                      : response.find(' ', body_start);
                size_t hashes_start = (size_end == std::string::npos)
                                          ? std::string::npos
                                          : size_end + 1;
                size_t hashes_end = (hashes_start == std::string::npos)
                                        ? std::string::npos
                                        : response.find(' ', hashes_start);

                long long file_size = -1;
                std::string all_hashes;
                bool parse_ok = false;
                if (body_start != std::string::npos &&
                    size_end != std::string::npos &&
                    hashes_end != std::string::npos) {
                  try {
                    file_size = std::stoll(
                        response.substr(body_start, size_end - body_start));
                    all_hashes = response.substr(hashes_start,
                                                 hashes_end - hashes_start);
                    parse_ok = (file_size >= 0 &&
                                all_hashes.length() % 40 == 0);
                  } catch (const std::exception&) {
                    parse_ok = false;
                  }
                }

                if (!parse_ok) {
                  log_error("Malformed download response from tracker: ", response);
                  continue;
                }

                std::vector<std::string> peers_list;
                std::string peer_addr;
                std::stringstream peers_ss(response.substr(hashes_end + 1));
                while (peers_ss >> peer_addr) {
                  peers_list.push_back(peer_addr);
                }
                if (peers_list.empty()) {
                  log_warn("No seeders found for this file.");
                } else {
                  std::string group_id = tokens[1];
                  std::string file_name = tokens[2];
                  std::string destination_path = tokens[3];
                  std::string algorithm = "rarest";

                  // Refuse a second concurrent download of the same file:
                  // the g_downloads map is keyed by file name, so a new
                  // state would orphan the old one (both writing to the
                  // same destination path).
                  {
                    std::lock_guard<std::mutex> lock(g_downloads_mutex);
                    auto existing = g_downloads.find(file_name);
                    if (existing != g_downloads.end() &&
                        !existing->second->download_complete.load()) {
                      log_error("A download of '", file_name,
                        "' is already in progress on this client.");
                      continue;
                    }
                  }

                  if (tokens.size() == 5) {
                    algorithm = tokens[4];
                  }

                  auto state = std::make_shared<DownloadState>();
                  state->group_id = group_id;
                  state->file_name = file_name;
                  state->destination_path = destination_path;
                  state->file_size = file_size;
                  state->concatenated_piece_hashes = all_hashes;
                  state->algorithm = algorithm;
                  state->num_pieces =
                      std::ceil(static_cast<double>(file_size) / PIECE_SIZE);

                  for (size_t i = 0; i < all_hashes.length(); i += 40) {
                    state->piece_hashes.push_back(all_hashes.substr(i, 40));
                  }

                  // Cross-check the two independent sources for the piece
                  // count: file_size/PIECE_SIZE vs. hash entries (40 hex
                  // chars each). A mismatch means the tracker response was
                  // truncated or corrupted; refuse the download rather than
                  // risk an out-of-bounds piece index later.
                  if (state->piece_hashes.size() !=
                      static_cast<size_t>(state->num_pieces)) {
                    log_error(
                      "Hash count mismatch for ", file_name, " (",
                      state->piece_hashes.size(), " hashes for ",
                      state->num_pieces,
                      " pieces). Tracker metadata is corrupt; aborting.");
                    continue;
                  }

                  for (const auto& peer_addr : peers_list) {
                    state->peers.emplace_back(peer_addr);
                  }
                  state->pieces_we_have.assign(state->num_pieces, false);
                  state->pieces_in_progress.assign(state->num_pieces, false);
                  state->pieces_exhausted.assign(state->num_pieces, false);
                  state->piece_failures.assign(state->num_pieces, 0);
                  state->piece_rarity.assign(state->num_pieces, 0);

                  {
                    std::lock_guard<std::mutex> lock(g_downloads_mutex);
                    g_downloads[file_name] = state;
                  }

                  save_metadata_file(current_user_id, group_id, file_name,
                                     destination_path, file_size, all_hashes);

                  std::string start_leeching_cmd =
                      "start_downloading " + group_id + " " + file_name;
                  log_message(
                      "[Client] Notifying tracker: starting download of ",
                      file_name);

                  safe_send(sockfd, start_leeching_cmd);

                  std::string start_leeching_response;
                  if (safe_recv(sockfd, start_leeching_response)) {
                    log_tracker_response(start_leeching_response);
                  }

                  std::thread download_thread(start_download, state);
                  download_thread.detach();
                }
              } else {
                // The command failed, just log the tracker's error message.
                log_tracker_response(response);
              }
            }
          }

          // Default path for all other commands
          if (should_send) {
            if (!safe_send(sockfd, command_to_process))
              continue;
            if (!safe_recv(sockfd, response))
              continue;
            log_tracker_response(response);

            if (command == "login" && response.rfind("SUCCESS", 0) == 0) {
              if (!has_args(2)) {
                continue;  // Tracker accepted but our usage is wrong.
              }
              is_logged_in.store(true);
              current_user_id = tokens[1];

              std::string app_home = get_application_home();
              std::string p2pclient = app_home + "/.p2p_client";
              std::string p2pclientUser = p2pclient + "/users";
              std::string user_dir =
                  app_home + "/.p2p_client/users/" + current_user_id;
              mkdir(p2pclient.c_str(), 0755);
              mkdir(p2pclientUser.c_str(), 0755);
              mkdir(user_dir.c_str(), 0755);

              load_state_from_disk(current_user_id);

              if (!is_peer_server_running) {
                std::thread peer_server_thread(run_peer_server_loop,
                                               peer_server_fd);
                peer_server_thread.detach();
                is_peer_server_running = true;
              }
            } else if (command == "logout" &&
                       response.rfind("SUCCESS", 0) == 0) {
              is_logged_in.store(false);
              std::lock_guard<std::mutex> lock(seeded_files_mutex);
              seeded_files.clear();
              log_debug("[Seeder Sync] Cleared seeded file state on logout");
            } else if (command == "stop_share" &&
                       response.rfind("SUCCESS", 0) == 0) {
              if (!has_args(3)) {
                continue;
              }
              std::string group_id = tokens[1];
              std::string file_name_to_stop = tokens[2];
              std::string app_home = get_application_home();
              if (!app_home.empty()) {
                std::string meta_file_path = app_home + "/.p2p_client/users/" +
                                             current_user_id + "/" + group_id +
                                             "_" + file_name_to_stop + ".meta";
                if (std::remove(meta_file_path.c_str()) != 0) {
                  log_error("Could not delete metadata file: ",
                              meta_file_path);
                } else {
                  log_debug("[State] Deleted metadata for ",
                              file_name_to_stop);
                }
              }
              std::lock_guard<std::mutex> lock(seeded_files_mutex);
              seeded_files.erase(file_name_to_stop);
            }
          }
        } else if (c == 127) {  // Backspace
        if (cursor_pos > 0) {
          current_line.erase(cursor_pos - 1, 1);
          cursor_pos--;
          std::lock_guard<std::mutex> lock(cout_mutex);
          redraw_line();
        }
      } else if (c == 27) {  // ESC: escape sequence (arrows, Home/End, Del)
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;

        if (seq[0] == '[') {
          std::lock_guard<std::mutex> lock(cout_mutex);
          if (seq[1] == 'A') {  // Up: older history entry.
            if (!input_history.empty() && history_nav > 0) {
              if (history_nav == input_history.size())
                nav_snapshot = current_line;  // Remember in-progress line.
              history_nav--;
              current_line = input_history[history_nav];
              cursor_pos = current_line.size();
              redraw_line();
            }
          } else if (seq[1] == 'B') {  // Down: newer history entry.
            if (history_nav < input_history.size()) {
              history_nav++;
              if (history_nav == input_history.size()) {
                current_line = nav_snapshot;  // Restore in-progress line.
              } else {
                current_line = input_history[history_nav];
              }
              cursor_pos = current_line.size();
              redraw_line();
            }
          } else if (seq[1] == 'C') {  // Right.
            if (cursor_pos < current_line.size()) {
              cursor_pos++;
              redraw_line();
            }
          } else if (seq[1] == 'D') {  // Left.
            if (cursor_pos > 0) {
              cursor_pos--;
              redraw_line();
            }
          } else if (seq[1] == 'H') {  // Home.
            cursor_pos = 0;
            redraw_line();
          } else if (seq[1] == 'F') {  // End.
            cursor_pos = current_line.size();
            redraw_line();
          } else if (seq[1] == '3') {  // Delete key: '3' + '~'.
            char tilde;
            if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == '~') {
              if (cursor_pos < current_line.size()) {
                current_line.erase(cursor_pos, 1);
                redraw_line();
              }
            }
          } else if (seq[1] == '1') {  // Home on some terminals: '1' + '~'.
            char tilde;
            if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == '~') {
              cursor_pos = 0;
              redraw_line();
            }
          } else if (seq[1] == '4') {  // End on some terminals: '4' + '~'.
            char tilde;
            if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == '~') {
              cursor_pos = current_line.size();
              redraw_line();
            }
          }
        }
        // Any other escape sequence: ignored (never crashes, never leaks
        // bytes into the command line).
      } else if (c == 9) {  // Tab: complete commands / file paths.
        std::lock_guard<std::mutex> lock(cout_mutex);
        tab_complete();
      } else if (c == 1) {  // Ctrl+A: home.
        cursor_pos = 0;
        std::lock_guard<std::mutex> lock(cout_mutex);
        redraw_line();
      } else if (c == 5) {  // Ctrl+E: end.
        cursor_pos = current_line.size();
        std::lock_guard<std::mutex> lock(cout_mutex);
        redraw_line();
      } else if (c == 21) {  // Ctrl+U: clear whole line.
        current_line.clear();
        cursor_pos = 0;
        std::lock_guard<std::mutex> lock(cout_mutex);
        redraw_line();
      } else if (c == 11) {  // Ctrl+K: kill to end of line.
        current_line.erase(cursor_pos);
        std::lock_guard<std::mutex> lock(cout_mutex);
        redraw_line();
      } else if (c == 23) {  // Ctrl+W: delete the word before the cursor.
        size_t end = cursor_pos;
        while (end > 0 && isspace((unsigned char)current_line[end - 1])) end--;
        size_t start = end;
        while (start > 0 && !isspace((unsigned char)current_line[start - 1]))
          start--;
        if (start < end) {
          current_line.erase(start, end - start);
          cursor_pos = start;
        }
        std::lock_guard<std::mutex> lock(cout_mutex);
        redraw_line();
      } else if (c == 12) {  // Ctrl+L: clear screen and repaint.
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "\x1b[2J\x1b[3J\x1b[H" << std::flush;
        redraw_line();
      } else if (!iscntrl(c)) {
        current_line.insert(cursor_pos, 1, c);
        cursor_pos++;
        std::lock_guard<std::mutex> lock(cout_mutex);
        redraw_line();
      } else {
        // Any other control byte (Ctrl+C/Ctrl+D flow control, tab, etc.):
        // swallowed here so it can neither crash the loop nor corrupt the
        // line buffer.
        continue;
      }
    }
  }

  if (is_logged_in.load()) {
    log_message("[Client] Cleaning up active downloads before exit...");
    std::lock_guard<std::mutex> lock(g_downloads_mutex);
    for (const auto& pair : g_downloads) {
      auto state = pair.second;
      // We only care about downloads that are not yet complete
      if (!state->download_complete.load()) {
        log_message("[Client] Notifying tracker: stopping leech of ",
                    state->file_name);
        std::string stop_leeching_cmd =
            "stop_leeching " + state->group_id + " " + state->file_name;
        sendMessage(sockfd, stop_leeching_cmd);
      }
    }
  }

  disable_raw_mode();
  std::cout << "\r\x1b[K";
  if (!safe_exit) {
    std::cout << "[Connection Error] : Connection lost with Primary. Please "
                 "restart client to connect again]"
              << std::endl;
  }
  std::cout << "[Client shutting down.]" << std::endl;
  if (sockfd >= 0)
    close(sockfd);
  if (peer_server_fd >= 0)
    close(peer_server_fd);
  return 0;
}