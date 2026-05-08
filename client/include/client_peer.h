#pragma once

#include <string>

struct SeededFileInfo {
  std::string path;
  long long file_size;
};

// Handles a single peer connection.
// This function is spawned in a new thread for each connecting peer.
void handle_peer_connection(int peer_socket);

// This function listens for and accepts incoming connections from other
// clients. It is intended to be run in its own detached thread.
void run_peer_server_loop(int server_fd);
