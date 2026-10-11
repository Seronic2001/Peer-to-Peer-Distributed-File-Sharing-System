#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#include "hashing.h"
#include "include/client_peer.h"
#include "include/download_manager.h"
#include "include/ui.h"
#include "protocol.h"

// Make this file aware of the global maps of files this client is sharing.
// Both are keyed by file_key(group_id, file_name).
extern std::unordered_map<std::string, SeededFileInfo> seeded_files;
extern std::mutex seeded_files_mutex;
extern std::unordered_map<std::string, std::shared_ptr<DownloadState>>
    g_downloads;
extern std::mutex g_downloads_mutex;

namespace {

struct ServedFile {
  std::string path;
  long long file_size = 0;
  // Set while the file is still being downloaded (we are a partial seeder).
  std::shared_ptr<DownloadState> download;
};

// Looks up a file we can serve: an in-progress download first, then the
// fully seeded files.
bool find_served_file(const std::string& key, ServedFile& out) {
  {
    std::lock_guard<std::mutex> lock(g_downloads_mutex);
    auto it = g_downloads.find(key);
    if (it != g_downloads.end() && !it->second->download_complete.load()) {
      out.download = it->second;
      out.path = it->second->destination_path;
      out.file_size = it->second->file_size;
      return true;
    }
  }
  std::lock_guard<std::mutex> lock(seeded_files_mutex);
  auto it = seeded_files.find(key);
  if (it == seeded_files.end())
    return false;
  out.path = it->second.path;
  out.file_size = it->second.file_size;
  return true;
}

}  // namespace

void handle_peer_connection(int peer_sock) {
  log_debug("[Peer Thread] New peer connected on socket ", peer_sock);

  std::string handshake_message;
  if (!receiveMessage(peer_sock, handshake_message)) {
    close(peer_sock);
    return;
  }

  log_debug("[Peer Thread] Received from peer: ", handshake_message);

  std::stringstream ss(handshake_message);
  std::string command, group_id, file_name;
  ss >> command >> group_id >> file_name;

  if (command != "HANDSHAKE" || file_name.empty()) {
    sendMessage(peer_sock, "HANDSHAKE_FAIL: Invalid handshake");
    close(peer_sock);
    return;
  }

  ServedFile file;
  if (!find_served_file(file_key(group_id, file_name), file)) {
    sendMessage(peer_sock, "HANDSHAKE_FAIL: File not found");
    close(peer_sock);
    return;
  }

  sendMessage(peer_sock, "HANDSHAKE_OK");

  const long long num_pieces = piece_count(file.file_size);
  std::string bitfield(num_pieces, '1');
  if (file.download) {
    // Partial seeder: advertise only the pieces we actually have.
    std::lock_guard<std::mutex> lock(file.download->state_mutex);
    for (long long i = 0; i < num_pieces; ++i) {
      if (!file.download->pieces_we_have[i])
        bitfield[i] = '0';
    }
  }
  sendMessage(peer_sock, "BITFIELD " + bitfield);

  // Handle Piece Requests. The file is opened on the first request and kept
  // open for the rest of the connection.
  int fd = -1;
  std::string request_message;
  while (receiveMessage(peer_sock, request_message)) {
    std::istringstream request_ss(request_message);
    std::string request;
    long long piece_index = -1;
    if (!(request_ss >> request >> piece_index) ||
        request != "REQUEST_PIECE") {
      continue;  // Not a well-formed piece request; ignore it.
    }
    if (piece_index < 0 || piece_index >= num_pieces) {
      log_debug("[Peer Thread] Rejecting out-of-range piece request ",
                piece_index, " for ", file_name);
      continue;
    }
    if (file.download) {
      std::lock_guard<std::mutex> lock(file.download->state_mutex);
      if (!file.download->pieces_we_have[piece_index])
        continue;  // Not downloaded yet.
    }
    if (fd < 0 && (fd = open(file.path.c_str(), O_RDONLY)) < 0)
      continue;

    // Read the piece directly behind the header to avoid an extra copy.
    std::string piece_message = "PIECE " + std::to_string(piece_index) + " ";
    const size_t header_len = piece_message.size();
    piece_message.resize(header_len + PIECE_SIZE);
    ssize_t bytes_read = pread(fd, &piece_message[header_len], PIECE_SIZE,
                               static_cast<off_t>(piece_index) * PIECE_SIZE);
    if (bytes_read <= 0)
      continue;
    piece_message.resize(header_len + bytes_read);
    if (!sendMessage(peer_sock, piece_message))
      break;
  }

  if (fd >= 0)
    close(fd);
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
