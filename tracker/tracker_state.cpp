#include <sstream>

#include "include/tracker_logger.h"
#include "include/tracker_state.h"

// User Management

std::string TrackerState::handle_create_user(const std::string& user_id,
                                             const std::string& password) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (users.count(user_id)) {
    return "ERROR: User already exists.";
  }
  users[user_id] = {password, false, ""};
  return "SUCCESS: User account created.";
}

std::string TrackerState::handle_login(const std::string& user_id,
                                       const std::string& password) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!users.count(user_id)) {
    return "ERROR: User does not exist.";
  }
  if (users[user_id].password != password) {
    return "ERROR: Invalid password.";
  }
  if (users[user_id].is_logged_in) {
    return "ERROR: User already logged in.";
  }
  users[user_id].is_logged_in = true;
  return "SUCCESS: Login successful.";
}

std::string TrackerState::handle_logout(const std::string& user_id) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (users.count(user_id)) {
    users[user_id].is_logged_in = false;
    users[user_id].peer_address.clear();
  }
  return "SUCCESS: Logout successful.";
}

void TrackerState::add_peer_address(const std::string& user_id,
                                    const std::string& ip_port) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (users.count(user_id)) {
    users[user_id].peer_address = ip_port;
  }
}

// Group Management
std::string TrackerState::handle_create_group(const std::string& group_id,
                                              const std::string& owner_id) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (groups.count(group_id)) {
    return "ERROR: Group already exists.";
  }
  groups[group_id] = {owner_id, {owner_id}, {}, {}};
  return "SUCCESS: Group created.";
}

std::string TrackerState::handle_list_groups() {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (groups.empty()) {
    return "SUCCESS: No groups available.";
  }
  std::stringstream ss;
  ss << "SUCCESS:";
  for (const auto& pair : groups) {
    ss << " " << pair.first;
  }
  return ss.str();
}

std::string TrackerState::handle_join_group(const std::string& group_id,
                                            const std::string& user_id) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!groups.count(group_id)) {
    return "ERROR: Group does not exist.";
  }
  // Check if user is already a member
  if (groups[group_id].members.count(user_id)) {
    return "ERROR: You are already a member of this group.";
  }
  groups[group_id].pending_requests.insert(user_id);
  return "SUCCESS: Request to join group sent.";
}

std::string TrackerState::handle_leave_group(const std::string& group_id,
                                             const std::string& user_id) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!groups.count(group_id)) {
    return "ERROR: Group does not exist.";
  }
  auto& group = groups[group_id];
  if (!groups[group_id].members.count(user_id)) {
    return "ERROR: You are not a member of this group.";
  }

  // Remove the user from the members set first.
  group.members.erase(user_id);

  // Also remove the user as a seeder from all files within this group.
  for (auto& file_pair : group.files) {
    file_pair.second.seeders.erase(user_id);
  }

  // Check if the group is now empty.
  if (group.members.empty()) {
    groups.erase(group_id);
    log_tracker("[State] Group '", group_id, "' deleted as last member left.");
    return "SUCCESS: You have left the group. The group has been deleted.";
  }

  // If the group is not empty, check if the owner was the one who left.
  if (group.owner_id == user_id) {
    // Promote the next member to be the new owner.
    std::string new_owner_id = *group.members.begin();
    group.owner_id = new_owner_id;
    log_tracker("[State] Owner of group '", group_id, "' left. New owner is '",
                new_owner_id, "'.");
    return "SUCCESS: You have left the group. Ownership has been transferred.";
  }
  // Default case: a regular member left and the group is not empty.
  return "SUCCESS: You have left the group.";
}

std::string TrackerState::handle_list_requests(const std::string& group_id,
                                               const std::string& user_id) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!groups.count(group_id)) {
    return "ERROR: Group does not exist.";
  }
  if (groups[group_id].owner_id != user_id) {
    return "ERROR: You are not the owner of this group.";
  }
  if (groups[group_id].pending_requests.empty()) {
    return "SUCCESS: No pending join requests.";
  }

  std::stringstream ss;
  ss << "SUCCESS:";
  for (const auto& member_id : groups[group_id].pending_requests) {
    ss << " " << member_id;
  }
  return ss.str();
}

std::string TrackerState::handle_accept_request(const std::string& group_id,
                                                const std::string& user_to_add,
                                                const std::string& owner_id) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!groups.count(group_id)) {
    return "ERROR: Group does not exist.";
  }
  if (groups[group_id].owner_id != owner_id) {
    return "ERROR: You are not the owner of this group.";
  }
  if (!groups[group_id].pending_requests.count(user_to_add)) {
    return "ERROR: This user has not requested to join the group.";
  }

  groups[group_id].pending_requests.erase(user_to_add);
  groups[group_id].members.insert(user_to_add);
  return "SUCCESS: User added to group.";
}

// File Management
std::string TrackerState::handle_upload_file(const std::string& group_id,
                                             const std::string& file_name,
                                             long long file_size,
                                             const std::string& hashes,
                                             const std::string& user_id) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!groups.count(group_id) || !groups[group_id].members.count(user_id)) {
    return "ERROR: You are not a member of this group.";
  }

  if (!users.count(user_id) || users[user_id].peer_address.empty()) {
    return "ERROR: Could not find your peer address. Please log in again.";
  }

  auto& group_files = groups[group_id].files;

  // When a user becomes a seeder, they are no longer a leecher.
  if (group_files.count(file_name)) {
    group_files[file_name].leechers.erase(user_id);
    group_files[file_name].seeders.insert(user_id);
  } else {
    FileMetadata meta;
    meta.file_size = file_size;
    meta.concatenated_hashes = hashes;
    meta.seeders.insert(user_id);
    group_files[file_name] = meta;
  }
  return "SUCCESS: File shared successfully.";
}

std::string TrackerState::handle_list_files(const std::string& group_id) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!groups.count(group_id)) {
    return "ERROR: Group does not exist.";
  }
  const auto& group_files = groups[group_id].files;
  if (group_files.empty()) {
    return "SUCCESS: No files in this group.";
  }
  std::stringstream ss;
  ss << "SUCCESS:";
  for (const auto& pair : group_files) {
    ss << " " << pair.first;
  }
  return ss.str();
}

std::string TrackerState::handle_download_file(const std::string& group_id,
                                               const std::string& file_name,
                                               const std::string& user_id) {
  std::lock_guard<std::mutex> file_lock(state_mutex);
  if (!groups.count(group_id) || !groups[group_id].members.count(user_id)) {
    return "ERROR: You are not a member of this group.";
  }

  auto& group = groups[group_id];
  if (!group.files.count(file_name)) {
    return "ERROR: File not found in this group.";
  }

  const auto& file_info = group.files[file_name];
  if (file_info.seeders.empty()) {
    return "ERROR: No seeders currently available for this file.";
  }

  const auto& file_meta = groups[group_id].files[file_name];
  if (file_meta.seeders.empty() && file_meta.leechers.empty()) {
    return "ERROR: No peers available for this file.";
  }

  // Combine both seeders and leechers into one list for the client.
  std::set<std::string> all_peers = file_meta.seeders;
  all_peers.insert(file_meta.leechers.begin(), file_meta.leechers.end());
  // Remove the requesting user from the list to not connect to self
  all_peers.erase(user_id);

  if (all_peers.empty()) {
    return "ERROR: No other peers available for this file.";
  }

  std::stringstream ss;
  ss << "SUCCESS: " << file_meta.file_size << " "
     << file_meta.concatenated_hashes;
  for (const auto& peer_id : all_peers) {
    if (users.count(peer_id) && !users[peer_id].peer_address.empty()) {
      ss << " " << users[peer_id].peer_address;
    }
  }
  return ss.str();
}

std::string TrackerState::handle_stop_share(const std::string& group_id,
                                            const std::string& file_name,
                                            const std::string& user_id) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!groups.count(group_id)) {
    return "ERROR: Group does not exist.";
  }
  auto& group = groups[group_id];
  if (!group.files.count(file_name)) {
    return "ERROR: You were not sharing this file in this group.";
  }
  if (group.files[file_name].seeders.erase(user_id) > 0) {
    return "SUCCESS: You are no longer sharing this file.";
  }
  return "ERROR: You were not sharing this file in this group.";
}

std::string TrackerState::handle_start_downloading(const std::string& group_id,
                                                   const std::string& file_name,
                                                   const std::string& user_id) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!groups.count(group_id) || !groups[group_id].files.count(file_name)) {
    return "ERROR: File or group does not exist.";
  }
  groups[group_id].files[file_name].leechers.insert(user_id);
  return "SUCCESS: You are now listed as a leecher for this file.";
}

std::string TrackerState::handle_stop_leeching(const std::string& group_id,
                                               const std::string& file_name,
                                               const std::string& user_id) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!groups.count(group_id) || !groups[group_id].files.count(file_name)) {
    return "ERROR: File or group does not exist.";
  }
  groups[group_id].files[file_name].leechers.erase(user_id);
  return "SUCCESS: You are no longer listed as a leecher for this file.";
}

void TrackerState::clear_sessions() {
  std::lock_guard<std::mutex> lock(state_mutex);
  for (auto& pair : users) {
    pair.second.is_logged_in = false;
    pair.second.peer_address.clear();
  }
  log_tracker("[State] Cleared all user sessions.");
}

// REPLICATION LOGIC
/*
  Serializes the current state into a multi-line string.
   Format:
   USER <user_id> <password>
   GROUP <group_id> <owner_id>
   MEMBER <group_id> <member_id>
   ... and so on for all state data.
*/

std::string TrackerState::serialize_state() const {
  std::lock_guard<std::mutex> lock(state_mutex);
  std::stringstream ss;
  for (const auto& pair : users) {
    ss << "user " << pair.first << " " << pair.second.password << "\n";
  }
  for (const auto& pair : groups) {
    const auto& group = pair.second;
    ss << "group " << pair.first << " " << group.owner_id << "\n";
    for (const auto& member_id : group.members) {
      ss << "member " << pair.first << " " << member_id << "\n";
    }
    for (const auto& pending_id : group.pending_requests) {
      ss << "pending " << pair.first << " " << pending_id << "\n";
    }
    for (const auto& file_pair : group.files) {
      const auto& file = file_pair.second;
      ss << "file " << pair.first << " " << file_pair.first << " "
         << file.file_size << " " << file.concatenated_hashes << "\n";
      for (const auto& seeder_id : file.seeders) {
        ss << "seeder " << pair.first << " " << file_pair.first << " "
           << seeder_id << "\n";
      }
      for (const auto& leecher_id : file.leechers) {
        ss << "leecher " << pair.first << " " << file_pair.first << " "
           << leecher_id << "\n";
      }
    }
  }
  return ss.str();
}

// Processes incoming commands from the primary tracker.
// This function can handle EITHER a single command OR a full state dump.

void TrackerState::process_replicated_command(const std::string& command_str) {
  std::stringstream ss(command_str);
  std::string first_line;
  std::getline(ss, first_line);

  if (first_line == "FULL_STATE_SYNC") {
    log_tracker("[Replication] Received full state sync. Applying...");
    std::lock_guard<std::mutex> lock(state_mutex);
    users.clear();
    groups.clear();

    std::string line;
    while (std::getline(ss, line)) {
      std::stringstream line_ss(line);
      std::string type;
      line_ss >> type;
      if (type == "user") {
        std::string user_id, password;
        line_ss >> user_id >> password;
        users[user_id] = {password, false, ""};
      } else if (type == "group") {
        std::string group_id, owner_id;
        line_ss >> group_id >> owner_id;
        groups[group_id].owner_id = owner_id;
      } else if (type == "member") {
        std::string group_id, user_id;
        line_ss >> group_id >> user_id;
        groups[group_id].members.insert(user_id);
      } else if (type == "pending") {
        std::string group_id, user_id;
        line_ss >> group_id >> user_id;
        groups[group_id].pending_requests.insert(user_id);
      } else if (type == "file") {
        std::string group_id, file_name, hashes;
        long long file_size;
        line_ss >> group_id >> file_name >> file_size >> hashes;
        groups[group_id].files[file_name] = {file_size, hashes, {}, {}};
      } else if (type == "seeder") {
        std::string group_id, file_name, user_id;
        line_ss >> group_id >> file_name >> user_id;
        groups[group_id].files[file_name].seeders.insert(user_id);
      } else if (type == "leecher") {
        std::string group_id, file_name, user_id;
        line_ss >> group_id >> file_name >> user_id;
        groups[group_id].files[file_name].leechers.insert(user_id);
      }
    }
    log_tracker("[Replication] Full state sync applied.");
    return;
  }

  // Process single command
  ss.clear();
  ss.str(command_str);
  std::vector<std::string> tokens;
  std::string token;
  while (ss >> token)
    tokens.push_back(token);
  if (tokens.empty())
    return;
  std::string command = tokens[0];

  if (command == "create_user" && tokens.size() == 3)
    handle_create_user(tokens[1], tokens[2]);
  else if (command == "login" && tokens.size() == 4) {
    handle_login(tokens[1], tokens[2]);
    add_peer_address(tokens[1], tokens[3]);
  } else if (command == "logout" && tokens.size() == 2)
    handle_logout(tokens[1]);
  else if (command == "create_group" && tokens.size() == 3)
    handle_create_group(tokens[1], tokens[2]);
  else if (command == "join_group" && tokens.size() == 3)
    handle_join_group(tokens[1], tokens[2]);
  else if (command == "leave_group" && tokens.size() == 3)
    handle_leave_group(tokens[1], tokens[2]);
  else if (command == "accept_request" && tokens.size() == 4)
    handle_accept_request(tokens[1], tokens[2], tokens[3]);
  else if (command == "upload_file" && tokens.size() == 6) {
    long long file_size = std::stoll(tokens[3]);
    handle_upload_file(tokens[1], tokens[2], file_size, tokens[4], tokens[5]);
  } else if (command == "stop_share" && tokens.size() == 4)
    handle_stop_share(tokens[1], tokens[2], tokens[3]);
  else if (command == "start_downloading" && tokens.size() == 4) {
    handle_start_downloading(tokens[1], tokens[2], tokens[3]);
  } else if (command == "stop_leeching" && tokens.size() == 4) {
    handle_stop_leeching(tokens[1], tokens[2], tokens[3]);
  }
}