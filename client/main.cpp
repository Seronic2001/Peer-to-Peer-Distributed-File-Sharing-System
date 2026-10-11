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
#include <cerrno>
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
// Both maps are keyed by file_key(group_id, file_name).
std::unordered_map<std::string, SeededFileInfo> seeded_files;
std::mutex seeded_files_mutex;
std::unordered_map<std::string, std::shared_ptr<DownloadState>> g_downloads;
std::mutex g_downloads_mutex;
std::atomic<bool> exit_program(false);
std::atomic_bool is_logged_in(false);  // Client-side authentication state
// The authenticated user's id. Logically session-global: the prompt builder
// (which runs on the main thread) and login/logout handlers all use it.
std::string current_user_id = "";

// State for Raw Mode Line Editor. Background threads redraw the prompt when
// they log, so current_line, cursor_pos and current_user_id are only modified
// with cout_mutex held.
std::string current_line;
size_t cursor_pos = 0;
std::mutex cout_mutex;
struct termios orig_termios;

// Tracker connection and this peer's listening socket. Used by the main
// thread only.
static int g_tracker_fd = -1;
static bool g_tracker_lost = false;
static int g_peer_server_fd = -1;
static std::string g_self_port;

// Command history for Up/Down arrow navigation.
static std::vector<std::string> input_history;
static size_t history_nav = 0;        // Current position while navigating.
static std::string nav_snapshot;      // Line being typed when nav started.

// Directory that holds the client's private state (history, .meta files).
std::string get_application_home() {
  const char* home_dir = getenv("HOME");
  return home_dir ? home_dir : ".";
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

// ----------------------------------------------------------------------------
// Tracker communication
// ----------------------------------------------------------------------------

// One request/response round trip with the tracker. The tracker answers every
// command with exactly one message, so all traffic goes through here to keep
// the stream in step. A failure means the tracker is gone: the client exits.
bool tracker_request(const std::string& command, std::string& response) {
  if (sendMessage(g_tracker_fd, command) &&
      receiveMessage(g_tracker_fd, response)) {
    return true;
  }
  g_tracker_lost = true;
  exit_program = true;
  return false;
}

bool is_success(const std::string& response) {
  return response.rfind("SUCCESS", 0) == 0;
}

// Connects to the first tracker in the list that reports itself PRIMARY.
// Returns the connected socket, or -1.
int connect_to_primary_tracker(const std::string& tracker_info_path) {
  std::ifstream info(tracker_info_path);
  if (!info) {
    log_error("Could not open tracker info file: ", tracker_info_path);
    return -1;
  }

  std::string addr;
  bool any_address = false;
  while (std::getline(info, addr)) {
    if (addr.empty())
      continue;
    any_address = true;

    size_t colon_pos = addr.find(':');
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    try {
      if (colon_pos == std::string::npos)
        throw std::invalid_argument("missing port");
      serv_addr.sin_port = htons(std::stoi(addr.substr(colon_pos + 1)));
    } catch (const std::exception&) {
      log_warn("Skipping malformed tracker address: ", addr);
      continue;
    }
    if (inet_pton(AF_INET, addr.substr(0, colon_pos).c_str(),
                  &serv_addr.sin_addr) <= 0) {
      log_warn("Skipping malformed tracker address: ", addr);
      continue;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
      perror("socket creation failed");
      continue;
    }
    struct timeval timeout;
    timeout.tv_sec = 2;  // Bounds the connect and the role exchange.
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

    std::string role_response;
    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) != 0) {
      log_message("Could not connect to tracker at ", addr, ". Trying next...");
    } else if (!receiveMessage(sockfd, role_response) ||
               role_response != "ROLE PRIMARY") {
      log_message("Could not get primary role from ", addr,
                  ". Trying next...");
    } else {
      log_message("Successfully connected to PRIMARY tracker at ", addr);
      timeout.tv_sec = 5;  // 5-second timeout for commands
      setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
      setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
      return sockfd;
    }
    close(sockfd);
  }

  if (!any_address) {
    log_error("No tracker addresses found in ", tracker_info_path);
  } else {
    log_error("Failed to connect to any available tracker.");
  }
  return -1;
}

// ----------------------------------------------------------------------------
// Persistent seeding state (.meta files)
// ----------------------------------------------------------------------------

// Returns the per-user state directory, creating it if needed.
std::string user_state_dir(const std::string& user_id) {
  std::string dir = get_application_home() + "/.p2p_client";
  mkdir(dir.c_str(), 0755);
  dir += "/users";
  mkdir(dir.c_str(), 0755);
  dir += "/" + user_id;
  mkdir(dir.c_str(), 0755);
  return dir;
}

std::string meta_file_path(const std::string& user_id,
                           const std::string& group_id,
                           const std::string& file_name) {
  return user_state_dir(user_id) + "/" + group_id + "_" + file_name + ".meta";
}

// Records a fully available file so it is seeded again after a restart.
void save_metadata_file(const std::string& user_id,
                        const std::string& group_id,
                        const std::string& file_name,
                        const std::string& local_path,
                        long long file_size) {
  const std::string meta_path = meta_file_path(user_id, group_id, file_name);
  std::ofstream out(meta_path, std::ios::trunc);
  out << "group_id:" << group_id << "\n"
      << "logical_name:" << file_name << "\n"
      << "local_path:" << local_path << "\n"
      << "file_size:" << file_size << "\n";
  if (!out) {
    log_error("Could not save metadata for '", file_name, "': ", meta_path);
  }
}

// Rebuilds seeded_files from the user's .meta files. Unreadable or malformed
// entries are skipped.
void load_state_from_disk(const std::string& user_id) {
  const std::string user_dir = user_state_dir(user_id);
  DIR* dir = opendir(user_dir.c_str());

  std::lock_guard<std::mutex> lock(seeded_files_mutex);
  seeded_files.clear();  // Clear any old state from a previous user
  if (!dir)
    return;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    const std::string filename = entry->d_name;
    if (filename.size() <= 5 ||
        filename.compare(filename.size() - 5, 5, ".meta") != 0) {
      continue;
    }

    std::ifstream in(user_dir + "/" + filename);
    std::string line, group_id, logical_name, local_path;
    long long file_size = -1;
    while (std::getline(in, line)) {
      const size_t colon_pos = line.find(':');
      if (colon_pos == std::string::npos)
        continue;
      const std::string key = line.substr(0, colon_pos);
      const std::string value = line.substr(colon_pos + 1);
      if (key == "group_id") {
        group_id = value;
      } else if (key == "logical_name") {
        logical_name = value;
      } else if (key == "local_path") {
        local_path = value;
      } else if (key == "file_size") {
        try {
          file_size = std::stoll(value);
        } catch (const std::exception&) {
          file_size = -1;
        }
      }
    }
    if (group_id.empty() || logical_name.empty() || local_path.empty() ||
        file_size < 0) {
      log_warn("[State] Ignoring malformed metadata file: ", filename);
      continue;
    }

    // Check if the actual data file still exists on disk
    struct stat file_stat_buffer;
    if (stat(local_path.c_str(), &file_stat_buffer) == 0) {
      seeded_files[file_key(group_id, logical_name)] = {local_path, file_size};
    } else {
      log_message("[State] Metadata found for '", logical_name,
                  "' but local file is missing.");
    }
  }
  closedir(dir);
  log_debug("[State] Loaded ", seeded_files.size(),
            " seeded files from disk for user ", user_id);
}

// ----------------------------------------------------------------------------
// Commands
// ----------------------------------------------------------------------------

// Tells the tracker we seed a complete file and, once it agrees, records the
// file locally. An empty file has no piece hashes; "-" stands in for them so
// the command keeps its token count.
void announce_seed(const std::string& group_id,
                   const std::string& file_name,
                   const std::string& absolute_path,
                   long long file_size,
                   const std::string& hashes) {
  std::string response;
  if (!tracker_request("upload_file " + group_id + " " + file_name + " " +
                           std::to_string(file_size) + " " +
                           (hashes.empty() ? "-" : hashes),
                       response)) {
    return;
  }
  log_tracker_response(response);
  if (!is_success(response))
    return;
  {
    std::lock_guard<std::mutex> lock(seeded_files_mutex);
    seeded_files[file_key(group_id, file_name)] = {absolute_path, file_size};
  }
  save_metadata_file(current_user_id, group_id, file_name, absolute_path,
                     file_size);
}

void cmd_upload_file(const std::vector<std::string>& tokens) {
  if (tokens.size() != 3) {
    log_warn("Usage: upload_file <group_id> <file_path>");
    return;
  }
  const std::string& group_id = tokens[1];
  const std::string& user_provided_path = tokens[2];

  // The logical name is the part of the path after the last '/'
  std::string logical_name = user_provided_path;
  size_t last_slash = logical_name.find_last_of('/');
  if (last_slash != std::string::npos) {
    logical_name = logical_name.substr(last_slash + 1);
  }

  // Convert the user-provided path to an absolute path for local storage.
  char resolved_path[PATH_MAX];
  if (realpath(user_provided_path.c_str(), resolved_path) == NULL) {
    log_error("Cannot find or resolve file path: ", user_provided_path);
    return;
  }

  try {
    log_message("Sharing file: ", logical_name, "...");
    FileHashes hashes = compute_hashes(resolved_path);
    log_message("File size: ", hashes.file_size, " bytes");
    announce_seed(group_id, logical_name, resolved_path, hashes.file_size,
                  hashes.concatenated_hashes);
  } catch (const std::exception& e) {
    log_error("Hashing failed: ", e.what());
  }
}

void cmd_show_downloads() {
  std::lock_guard<std::mutex> lock(g_downloads_mutex);
  if (g_downloads.empty()) {
    log_message("No downloads to show.");
    return;
  }
  for (const auto& pair : g_downloads) {
    const auto& state = pair.second;
    if (!state->download_complete.load()) {
      log_message("[D] [", state->group_id, "] ", state->file_name, " (",
                  state->downloaded_piece_count.load(), "/", state->num_pieces,
                  ")");
    } else if (state->download_successful.load()) {
      log_message("[C] [", state->group_id, "] ", state->file_name);
    } else {
      log_message("[E] [", state->group_id, "] ", state->file_name,
                  " (Failed)");
    }
  }
}

void cmd_download_file(const std::vector<std::string>& tokens) {
  if (tokens.size() != 4 && tokens.size() != 5) {
    log_warn(
        "Usage: download_file <group_id> <file_name> "
        "<destination_path> [algorithm]");
    return;
  }
  const std::string& group_id = tokens[1];
  const std::string& file_name = tokens[2];
  const std::string key = file_key(group_id, file_name);

  // Refuse a second concurrent download of the same file: both would write
  // to the same map slot. A finished entry is simply replaced.
  {
    std::lock_guard<std::mutex> lock(g_downloads_mutex);
    auto existing = g_downloads.find(key);
    if (existing != g_downloads.end() &&
        !existing->second->download_complete.load()) {
      log_error("A download of '", file_name,
                "' is already in progress on this client.");
      return;
    }
  }

  std::string response;
  if (!tracker_request("download_file " + group_id + " " + file_name,
                       response)) {
    return;
  }
  if (!is_success(response)) {
    log_tracker_response(response);
    return;
  }

  // Format is "SUCCESS: <file_size> <hashes> [peer ...]".
  std::istringstream response_ss(response);
  std::string status, all_hashes, peer_addr;
  long long file_size = -1;
  response_ss >> status >> file_size >> all_hashes;
  if (all_hashes == "-")
    all_hashes.clear();  // Empty file: no pieces.

  // Cross-check the two independent sources for the piece count (file size
  // vs. 40 hex chars per hash). A mismatch means the metadata is corrupt;
  // refuse the download rather than risk an out-of-bounds piece index later.
  if (!response_ss || file_size < 0 || all_hashes.length() % 40 != 0 ||
      static_cast<long long>(all_hashes.length() / 40) !=
          piece_count(file_size)) {
    log_error("Malformed download response from tracker for ", file_name,
              ". Aborting.");
    return;
  }

  auto state = std::make_shared<DownloadState>();
  while (response_ss >> peer_addr) {
    state->peers.emplace_back(peer_addr);
  }
  if (state->peers.empty()) {
    log_warn("No seeders found for this file.");
    return;
  }

  // A directory as destination means "put the file in there".
  std::string destination_path = tokens[3];
  struct stat dest_stat;
  if (destination_path.back() == '/') {
    destination_path += file_name;
  } else if (stat(destination_path.c_str(), &dest_stat) == 0 &&
             S_ISDIR(dest_stat.st_mode)) {
    destination_path += "/" + file_name;
  }

  state->group_id = group_id;
  state->file_name = file_name;
  state->destination_path = destination_path;
  state->file_size = file_size;
  state->concatenated_piece_hashes = all_hashes;
  state->algorithm = tokens.size() == 5 ? tokens[4] : "rarest";
  state->num_pieces = static_cast<int>(piece_count(file_size));
  state->pieces_we_have.assign(state->num_pieces, false);
  state->pieces_in_progress.assign(state->num_pieces, false);
  state->piece_rarity.assign(state->num_pieces, 0);

  {
    std::lock_guard<std::mutex> lock(g_downloads_mutex);
    g_downloads[key] = state;
  }

  log_message("[Client] Notifying tracker: starting download of ", file_name);
  if (tracker_request("start_downloading " + group_id + " " + file_name,
                      response)) {
    log_tracker_response(response);
  }

  std::thread download_thread(start_download, state);
  download_thread.detach();
}

// Reports downloads that finished since the last call to the tracker: a
// successful one turns us into a seeder, a failed one ends our leecher entry.
// Finished downloads stay in g_downloads so show_downloads can list them.
void finalize_finished_downloads() {
  std::vector<std::shared_ptr<DownloadState>> finished;
  {
    std::lock_guard<std::mutex> lock(g_downloads_mutex);
    for (auto& pair : g_downloads) {
      auto& state = pair.second;
      if (state->download_complete.load() && !state->finalized) {
        state->finalized = true;
        finished.push_back(state);
      }
    }
  }

  // Tracker round trips happen without g_downloads_mutex held, so the peer
  // server is never blocked behind the network.
  for (const auto& state : finished) {
    char resolved_path[PATH_MAX];
    if (state->download_successful.load() &&
        realpath(state->destination_path.c_str(), resolved_path) != NULL) {
      log_message("Download of '", state->file_name,
                  "' complete. Notifying tracker...");
      announce_seed(state->group_id, state->file_name, resolved_path,
                    state->file_size, state->concatenated_piece_hashes);
    } else {
      log_error("Download of '", state->file_name, "' failed.");
      std::string response;
      tracker_request(
          "stop_leeching " + state->group_id + " " + state->file_name,
          response);
    }
  }
}

// Commands the tracker handles on its own, plus the local bookkeeping that
// follows a successful login, logout or stop_share.
void cmd_tracker_passthrough(const std::string& line,
                             const std::vector<std::string>& tokens) {
  const std::string& command = tokens[0];
  // The tracker learns our peer port from the login command.
  const std::string wire =
      command == "login" ? line + " " + g_self_port : line;

  std::string response;
  if (!tracker_request(wire, response))
    return;
  log_tracker_response(response);
  if (!is_success(response))
    return;

  // The tracker validated the argument count, so tokens can be indexed.
  if (command == "login") {
    {
      std::lock_guard<std::mutex> lock(cout_mutex);
      current_user_id = tokens[1];
    }
    is_logged_in.store(true);
    load_state_from_disk(current_user_id);

    static bool is_peer_server_running = false;
    if (!is_peer_server_running) {
      std::thread(run_peer_server_loop, g_peer_server_fd).detach();
      is_peer_server_running = true;
    }
  } else if (command == "logout") {
    is_logged_in.store(false);
    std::lock_guard<std::mutex> lock(seeded_files_mutex);
    seeded_files.clear();
    log_debug("[Seeder Sync] Cleared seeded file state on logout");
  } else if (command == "stop_share") {
    const std::string meta_path =
        meta_file_path(current_user_id, tokens[1], tokens[2]);
    if (std::remove(meta_path.c_str()) != 0) {
      log_error("Could not delete metadata file: ", meta_path);
    }
    std::lock_guard<std::mutex> lock(seeded_files_mutex);
    seeded_files.erase(file_key(tokens[1], tokens[2]));
  }
}

// Runs one submitted input line.
void process_command(const std::string& line) {
  std::stringstream ss(line);
  std::vector<std::string> tokens;
  std::string token;
  while (ss >> token) {
    tokens.push_back(token);
  }
  if (tokens.empty()) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    redraw_line();
    return;  // Whitespace-only input.
  }

  record_command(line);
  const std::string& command = tokens[0];

  if (command == "clear" || command == "cls") {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << "\x1b[2J\x1b[3J\x1b[H" << std::flush;
    redraw_line();
    return;
  }

  log_message(ui::styled(ui::DIM, "ran: "), line);

  if (command == "quit" || command == "exit") {
    exit_program = true;
  } else if (command == "help" || command == "?") {
    print_help();
  } else if (command != "create_user" && command != "login" &&
             !is_logged_in.load()) {
    log_error("You must be logged in to use this command.");
  } else if (command == "upload_file") {
    cmd_upload_file(tokens);
  } else if (command == "download_file") {
    cmd_download_file(tokens);
  } else if (command == "show_downloads") {
    cmd_show_downloads();
  } else {
    cmd_tracker_passthrough(line, tokens);
  }
}

// ----------------------------------------------------------------------------
// Line editor
// ----------------------------------------------------------------------------

// Waits up to timeout_ms for one byte from stdin.
// Returns 1 if a byte was read, 0 on timeout, -1 on EOF or error.
int read_key(char& c, int timeout_ms) {
  fd_set read_fds;
  FD_ZERO(&read_fds);
  FD_SET(STDIN_FILENO, &read_fds);
  struct timeval timeout;
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;

  int activity = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout);
  if (activity < 0)
    return errno == EINTR ? 0 : -1;
  if (activity == 0)
    return 0;
  return read(STDIN_FILENO, &c, 1) == 1 ? 1 : -1;
}

// Applies one keypress to the line editor. Returns true when Enter was
// pressed, with the finished line in `submitted`.
bool handle_key(char c, std::string& submitted) {
  // Escape sequences (arrows, Home/End, Del) are read in full before taking
  // cout_mutex, and with a short timeout so a bare ESC cannot stall the loop.
  char seq[3] = {0, 0, 0};
  if (c == 27) {
    if (read_key(seq[0], 50) != 1 || read_key(seq[1], 50) != 1)
      return false;
    if (seq[0] != '[')
      return false;  // Unknown sequence: ignored, never leaks into the line.
    if (seq[1] >= '0' && seq[1] <= '9' &&
        (read_key(seq[2], 50) != 1 || seq[2] != '~')) {
      return false;
    }
  }

  std::lock_guard<std::mutex> lock(cout_mutex);
  if (c == '\n' || c == '\r') {
    submitted = current_line;
    current_line.clear();
    cursor_pos = 0;
    return true;
  }

  if (c == 27) {
    switch (seq[1]) {
      case 'A':  // Up: older history entry.
        if (history_nav > 0) {
          if (history_nav == input_history.size())
            nav_snapshot = current_line;  // Remember in-progress line.
          current_line = input_history[--history_nav];
          cursor_pos = current_line.size();
        }
        break;
      case 'B':  // Down: newer history entry.
        if (history_nav < input_history.size()) {
          history_nav++;
          current_line = history_nav == input_history.size()
                             ? nav_snapshot  // Restore in-progress line.
                             : input_history[history_nav];
          cursor_pos = current_line.size();
        }
        break;
      case 'C':  // Right.
        if (cursor_pos < current_line.size())
          cursor_pos++;
        break;
      case 'D':  // Left.
        if (cursor_pos > 0)
          cursor_pos--;
        break;
      case 'H':  // Home.
      case '1':  // Home on some terminals: "1~".
        cursor_pos = 0;
        break;
      case 'F':  // End.
      case '4':  // End on some terminals: "4~".
        cursor_pos = current_line.size();
        break;
      case '3':  // Delete key: "3~".
        if (cursor_pos < current_line.size())
          current_line.erase(cursor_pos, 1);
        break;
      default:
        return false;
    }
  } else if (c == 127) {  // Backspace
    if (cursor_pos > 0)
      current_line.erase(--cursor_pos, 1);
  } else if (c == 9) {  // Tab: complete commands / file paths.
    tab_complete();
    return false;  // tab_complete redraws when it changes anything.
  } else if (c == 1) {  // Ctrl+A: home.
    cursor_pos = 0;
  } else if (c == 5) {  // Ctrl+E: end.
    cursor_pos = current_line.size();
  } else if (c == 21) {  // Ctrl+U: clear whole line.
    current_line.clear();
    cursor_pos = 0;
  } else if (c == 11) {  // Ctrl+K: kill to end of line.
    current_line.erase(cursor_pos);
  } else if (c == 23) {  // Ctrl+W: delete the word before the cursor.
    size_t end = cursor_pos;
    while (end > 0 && isspace((unsigned char)current_line[end - 1]))
      end--;
    size_t start = end;
    while (start > 0 && !isspace((unsigned char)current_line[start - 1]))
      start--;
    current_line.erase(start, cursor_pos - start);
    cursor_pos = start;
  } else if (c == 12) {  // Ctrl+L: clear screen and repaint.
    std::cout << "\x1b[2J\x1b[3J\x1b[H";
  } else if (!iscntrl((unsigned char)c)) {
    current_line.insert(cursor_pos++, 1, c);
  } else {
    // Any other control byte (Ctrl+C, Ctrl+D, ...): swallowed so it can
    // neither crash the loop nor corrupt the line buffer.
    return false;
  }
  redraw_line();
  return false;
}

// ----------------------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------------------

int main(int argc, char const* argv[]) {
  if (argc != 3) {
    log_message("Usage: ", argv[0],
                " <YOUR_IP>:<YOUR_PORT> <tracker_info.txt>");
    return 1;
  }

  // Parse self listening address from argv[1]
  const std::string self_addr_str = argv[1];
  size_t colon_pos_self = self_addr_str.find(':');
  if (colon_pos_self == std::string::npos) {
    log_error("Invalid listening address format. Use IP:PORT");
    return 1;
  }
  const std::string self_ip = self_addr_str.substr(0, colon_pos_self);
  g_self_port = self_addr_str.substr(colon_pos_self + 1);
  int self_port = 0;
  try {
    self_port = std::stoi(g_self_port);
  } catch (const std::exception&) {
    log_error("Invalid port number: '", g_self_port, "'");
    return 1;
  }
  if (self_port <= 0 || self_port > 65535) {
    log_error("Port out of range (1-65535): ", self_port);
    return 1;
  }
  g_self_port = std::to_string(self_port);  // Canonical form for the tracker.

  g_tracker_fd = connect_to_primary_tracker(argv[2]);
  if (g_tracker_fd < 0) {
    return 1;
  }

  // Prepare Peer Server Socket with the static address
  g_peer_server_fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in peer_addr;
  peer_addr.sin_family = AF_INET;
  peer_addr.sin_port = htons(self_port);
  // Allow rebinding after a restart while old connections from this port
  // sit in TIME_WAIT (mirrors the tracker's socket setup).
  int peer_opt = 1;
  setsockopt(g_peer_server_fd, SOL_SOCKET, SO_REUSEADDR, &peer_opt,
             sizeof(peer_opt));
  if (inet_pton(AF_INET, self_ip.c_str(), &peer_addr.sin_addr) <= 0) {
    log_error("Invalid listening IP address provided.");
    return 1;
  }
  if (bind(g_peer_server_fd, (struct sockaddr*)&peer_addr, sizeof(peer_addr)) <
      0) {
    perror("bind (peer server) failed");
    log_error("Could not bind to ", self_addr_str,
              ". Is the port already in use?");
    return 1;
  }

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
  }

  while (!exit_program) {
    finalize_finished_downloads();

    char c;
    int got_key = read_key(c, 100);  // Poll every 100ms
    if (got_key < 0)
      break;  // stdin closed or failed.
    std::string line;
    if (got_key == 1 && handle_key(c, line)) {
      process_command(line);
    }
  }

  if (is_logged_in.load() && !g_tracker_lost) {
    log_message("[Client] Cleaning up active downloads before exit...");
    std::lock_guard<std::mutex> lock(g_downloads_mutex);
    for (const auto& pair : g_downloads) {
      const auto& state = pair.second;
      // We only care about downloads that are not yet complete
      if (!state->download_complete.load()) {
        log_message("[Client] Notifying tracker: stopping leech of ",
                    state->file_name);
        sendMessage(g_tracker_fd, "stop_leeching " + state->group_id + " " +
                                      state->file_name);
      }
    }
  }

  disable_raw_mode();
  std::cout << "\r\x1b[K";
  if (g_tracker_lost) {
    std::cout << "[Connection Error] : Connection lost with Primary. Please "
                 "restart client to connect again]"
              << std::endl;
  }
  std::cout << "[Client shutting down.]" << std::endl;
  close(g_tracker_fd);
  close(g_peer_server_fd);
  return 0;
}
