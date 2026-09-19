#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
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

// Make this file aware of the global map of files this client is sharing.
extern std::unordered_map<std::string, SeededFileInfo> seeded_files;
extern std::mutex seeded_files_mutex;
extern std::unordered_map<std::string, std::shared_ptr<DownloadState>>
    g_downloads;
extern std::mutex g_downloads_mutex;

// Seeder State Helpers
bool is_file_seeded(const std::string& file_name) {
  // First, check if it's a fully seeded file.
  {
    std::lock_guard<std::mutex> lock(seeded_files_mutex);
    if (seeded_files.count(file_name) > 0) {
      return true;
    }
  }

  // If not, check if it's an active (partial) download.
  {
    std::lock_guard<std::mutex> lock(g_downloads_mutex);
    if (g_downloads.count(file_name) > 0) {
      return true;
    }
  }

  // If it's in neither map, we don't have it.
  return false;
}

void handle_peer_connection(int peer_sock) {
  log_debug("[Peer Thread] New peer connected on socket ", peer_sock);

  std::string handshake_message;
  if (!receiveMessage(peer_sock, handshake_message)) {
    close(peer_sock);
    return;
  }

  log_debug("[Peer Thread] Received from peer: ", handshake_message);

  std::stringstream ss(handshake_message);
  std::string command, file_name;
  ss >> command >> file_name;

  if (command != "HANDSHAKE" || file_name.empty()) {
    sendMessage(peer_sock, "HANDSHAKE_FAIL: Invalid handshake");
    close(peer_sock);
    return;
  }

  if (!is_file_seeded(file_name)) {
    sendMessage(peer_sock, "HANDSHAKE_FAIL: File not found");
    close(peer_sock);
    return;
  }

  std::string local_path;
  long long file_size = 0;
  bool is_partial = false;
  std::shared_ptr<DownloadState> download_state = nullptr;

  // First, check if it's an active download
  {
    std::lock_guard<std::mutex> lock(g_downloads_mutex);
    auto it = g_downloads.find(file_name);
    if (it != g_downloads.end()) {
      download_state = it->second;
      // Lock the specific download's state to safely read from it
      std::lock_guard<std::mutex> state_lock(download_state->state_mutex);
      local_path = download_state->destination_path;
      file_size = download_state->file_size;
      is_partial = true;
    }
  }

  // If it wasn't a partial download, check if it's a fully seeded file
  if (!is_partial) {
    std::lock_guard<std::mutex> lock(seeded_files_mutex);
    auto it = seeded_files.find(file_name);
    if (it != seeded_files.end()) {
      local_path = it->second.path;
      file_size = it->second.file_size;
    }
  }

  // If we couldn't find the file in either map, we don't have it.
  if (local_path.empty()) {
    sendMessage(peer_sock, "HANDSHAKE_FAIL: File not found in any state");
    close(peer_sock);
    return;
  }

  sendMessage(peer_sock, "HANDSHAKE_OK");

  // Logic to Send Bitfield
  try {
    int num_pieces = std::ceil(static_cast<double>(file_size) / PIECE_SIZE);
    std::string bitfield_to_send;

    if (is_partial && download_state) {
      // CASE 1: The file is currently being downloaded. We are a partial
      // seeder. Construct the bitfield based on the pieces we actually have.
      std::lock_guard<std::mutex> state_lock(download_state->state_mutex);
      bitfield_to_send.reserve(num_pieces);
      for (bool have_piece : download_state->pieces_we_have) {
        bitfield_to_send += (have_piece ? '1' : '0');
      }
      log_debug("[Peer Thread] Sending PARTIAL bitfield for ", file_name);
    } else {
      // CASE 2: The file is not being downloaded, so it must be a fully seeded
      // file. Construct a bitfield of all '1's.
      bitfield_to_send = std::string(num_pieces, '1');
      log_debug("[Peer Thread] Sending FULL bitfield for ", file_name);
    }
    // Now, send the correctly constructed bitfield to the peer.
    sendMessage(peer_sock, "BITFIELD " + bitfield_to_send);
  } catch (const std::exception& e) {
    // This block will now catch potential issues, like the file path missing
    // for a seeder.
    log_error("[Peer Thread] Error during bitfield generation: ", e.what());
    // It's best to close the connection if we can't send a valid bitfield.
    close(peer_sock);
    return;
  }

  // Handle Piece Requests
  std::string request_message;
  while (receiveMessage(peer_sock, request_message)) {
    std::stringstream request_ss(request_message);
    std::vector<std::string> request_tokens;
    std::string request_token;
    while (request_ss >> request_token) {
      request_tokens.push_back(request_token);
    }

    if (request_tokens.empty() || request_tokens[0] != "REQUEST_PIECE")
      continue;

    if (request_tokens.size() == 2) {
      int piece_index = -1;
      try {
        piece_index = std::stoi(request_tokens[1]);
      } catch (const std::exception&) {
        continue;  // Malformed index; ignore the request.
      }
      long long max_pieces = (file_size + PIECE_SIZE - 1) / PIECE_SIZE;
      if (piece_index < 0 || piece_index >= max_pieces) {
        log_debug("[Peer Thread] Rejecting out-of-range piece request ",
                    piece_index, " for ", file_name);
        continue;
      }
      int fd = open(local_path.c_str(), O_RDONLY);
      if (fd < 0)
        continue;

      // Seek to the correct offset with lseek()
      long long offset = (long long)piece_index * PIECE_SIZE;
      lseek(fd, offset, SEEK_SET);

      std::vector<char> buffer(PIECE_SIZE);
      // Read data with read()
      ssize_t bytes_read = read(fd, buffer.data(), PIECE_SIZE);

      if (bytes_read > 0) {
        std::string header = "PIECE " + std::to_string(piece_index) + " ";
        std::string piece_message;
        piece_message.reserve(header.length() + bytes_read);
        piece_message.append(header);
        piece_message.append(buffer.data(), bytes_read);

        sendMessage(peer_sock, piece_message);
      }
      close(fd);
    }
  }

  close(peer_sock);
  log_debug("[Peer Thread] Peer ", peer_sock, " disconnected.");
}

void run_peer_server_loop(int server_fd) {
  if (listen(server_fd, 10) < 0) {
    perror("listen (peer server)");
    return;
  }

  struct sockaddr_in address;
  socklen_t addrlen = sizeof(address);
  getsockname(server_fd, (struct sockaddr*)&address, &addrlen);
  int port = ntohs(address.sin_port);
  log_message("Peer server listening on port ", port);

  while (true) {
    int new_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen);
    if (new_socket < 0) {
      log_message("ERROR: accept (peer server) failed.");
      continue;
    }

    std::thread peer_handler_thread(handle_peer_connection, new_socket);
    peer_handler_thread.detach();
  }

  close(server_fd);
}
