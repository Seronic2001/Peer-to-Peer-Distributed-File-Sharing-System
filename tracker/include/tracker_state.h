#pragma once

#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct UserInfo {
  std::string password;
  bool is_logged_in = false;
  std::string peer_address;  // e.g., "127.0.0.1:9090"
};

struct FileMetadata {
  long long file_size;
  std::string concatenated_hashes;
  std::set<std::string> seeders;   // User IDs of seeders
  std::set<std::string> leechers;  // User IDs of leechers
};
struct GroupInfo {
  std::string owner_id;
  std::set<std::string> members;
  std::set<std::string> pending_requests;
  std::unordered_map<std::string, FileMetadata> files;
};

class TrackerState {
 public:
  // State Modification Methods
  std::string handle_create_user(const std::string& user_id,
                                 const std::string& password);
  std::string handle_login(const std::string& user_id,
                           const std::string& password);
  std::string handle_logout(const std::string& user_id);
  std::string handle_create_group(const std::string& group_id,
                                  const std::string& owner_id);
  std::string handle_list_groups();
  std::string handle_join_group(const std::string& group_id,
                                const std::string& user_id);
  std::string handle_leave_group(const std::string& group_id,
                                 const std::string& user_id);
  std::string handle_list_requests(const std::string& group_id,
                                   const std::string& user_id);
  std::string handle_accept_request(const std::string& group_id,
                                    const std::string& user_to_add,
                                    const std::string& owner_id);
  std::string handle_upload_file(const std::string& group_id,
                                 const std::string& file_name,
                                 long long file_size,
                                 const std::string& hashes,
                                 const std::string& user_id);
  std::string handle_list_files(const std::string& group_id);
  std::string handle_download_file(const std::string& group_id,
                                   const std::string& file_name,
                                   const std::string& user_id);
  std::string handle_stop_share(const std::string& group_id,
                                const std::string& file_name,
                                const std::string& user_id);
  void add_peer_address(const std::string& user_id, const std::string& port);

  std::string handle_start_downloading(const std::string& group_id,
                                       const std::string& file_name,
                                       const std::string& user_id);
  std::string handle_stop_leeching(const std::string& group_id,
                                   const std::string& file_name,
                                   const std::string& user_id);

  // Methods for Replication and State Management
  void process_replicated_command(const std::string& command_str);
  void clear_sessions();
  std::string serialize_state() const;

 private:
  mutable std::mutex state_mutex;
  std::unordered_map<std::string, UserInfo> users;
  std::unordered_map<std::string, GroupInfo> groups;
};
