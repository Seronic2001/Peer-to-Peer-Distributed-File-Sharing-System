#pragma once

#include <string>

struct SeededFileInfo {
  std::string path;
  long long file_size;
};

// Key used by the seeded-file and download maps. The same file name may be
// shared in several groups, so the group is part of the identity. Neither id
// can contain whitespace, which makes the space a safe separator.
inline std::string file_key(const std::string& group_id,
                            const std::string& file_name) {
  return group_id + " " + file_name;
}

// Handles a single peer connection.
// This function is spawned in a new thread for each connecting peer.
void handle_peer_connection(int peer_socket);

// This function listens for and accepts incoming connections from other
// clients. It is intended to be run in its own detached thread.
void run_peer_server_loop(int server_fd);
