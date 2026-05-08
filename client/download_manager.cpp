#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include "hashing.h"
#include "include/download_manager.h"
#include "include/ui.h"
#include "protocol.h"

// Helper to connect to a single peer
int connect_to_peer(const std::string& peer_addr) {
  size_t colon_pos = peer_addr.find(':');
  if (colon_pos == std::string::npos)
    return -1;
  std::string ip = peer_addr.substr(0, colon_pos);
  int port = std::stoi(peer_addr.substr(colon_pos + 1));

  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    return -1;
  }
  struct sockaddr_in serv_addr;
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &serv_addr.sin_addr) <= 0) {
    close(sockfd);
    return -1;
  }
  if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
    close(sockfd);
    return -1;
  }
  return sockfd;
}

void peer_worker(DownloadState& state, PeerState& self, int output_fd) {
  self.sock = connect_to_peer(self.peer_address);
  if (self.sock < 0) {
    log_message("[Peer ", self.peer_address, "] Could not connect.");
    self.is_active = false;
    state.peers_ready++;
    return;
  }

  log_message("[Peer ", self.peer_address, "] Connected.");
  std::string handshake_msg = "HANDSHAKE " + state.file_name;
  if (!sendMessage(self.sock, handshake_msg)) {
    log_message("[Peer ", self.peer_address, "] Handshake send failed.");
    close(self.sock);
    self.is_active = false;
    state.peers_ready++;
    return;
  }

  std::string response;
  if (!receiveMessage(self.sock, response) || response != "HANDSHAKE_OK") {
    log_message("[Peer ", self.peer_address,
                "] Handshake failed. Resp: ", response);
    close(self.sock);
    self.is_active = false;
    state.peers_ready++;
    return;
  }

  std::string bitfield_msg;
  if (receiveMessage(self.sock, bitfield_msg)) {
    std::stringstream ss(bitfield_msg);
    std::string command, data;
    ss >> command >> data;

    if (command == "BITFIELD" &&
        data.length() == static_cast<size_t>(state.num_pieces)) {
      std::lock_guard<std::mutex> lock(state.state_mutex);
      self.bitfield.resize(state.num_pieces);
      for (int i = 0; i < state.num_pieces; ++i) {
        if (data[i] == '1') {
          self.bitfield[i] = true;
          state.piece_rarity[i]++;
        } else {
          self.bitfield[i] = false;
        }
      }
      self.is_active = true;
      log_message("[Peer ", self.peer_address,
                  "] BITFIELD received. Active and has ",
                  std::count(self.bitfield.begin(), self.bitfield.end(), true),
                  " pieces.");
    } else {
      log_message("[Peer ", self.peer_address,
                  "] BITFIELD malformed or wrong length: ", data.length());
      self.is_active = false;
    }
  } else {
    log_message("[Peer ", self.peer_address, "] No BITFIELD received.");
    self.is_active = false;
  }

  state.peers_ready++;

  while (!state.download_complete) {
    int piece_to_download = -1;
    {
      std::unique_lock<std::mutex> lock(state.state_mutex);
      state.cv.wait(lock, [&] {
        return (self.assigned_piece != -1) || state.download_complete;
      });

      if (state.download_complete)
        break;
      piece_to_download = self.assigned_piece;
    }

    bool success = false;
    std::string request_msg =
        "REQUEST_PIECE " + std::to_string(piece_to_download);
    sendMessage(self.sock, request_msg);

    std::string piece_response;
    if (receiveMessage(self.sock, piece_response)) {
      std::stringstream piece_ss(piece_response);
      std::string header, piece_idx_str;
      piece_ss >> header >> piece_idx_str;

      if (header == "PIECE") {
        size_t data_start = header.length() + piece_idx_str.length() + 2;
        const char* piece_data = piece_response.data() + data_start;
        size_t data_len = piece_response.length() - data_start;

        std::string received_hash = hash_buffer(piece_data, data_len);
        if (received_hash == state.piece_hashes[piece_to_download]) {
          {
            std::lock_guard<std::mutex> file_lock(state.file_mutex);
            long long offset = (long long)piece_to_download * PIECE_SIZE;
            //  Using pwrite for atomic seek-and-write
            if (pwrite(output_fd, piece_data, data_len, offset) ==
                (ssize_t)data_len) {
              success = true;
            }
          }
          success = true;
        } else {
          log_message("[Peer ", self.peer_address, "] Hash mismatch for piece ",
                      piece_to_download, ". Retrying...");
        }
      }
    } else {
      log_message("[Peer ", self.peer_address, "] Failed to receive piece ",
                  piece_to_download);
    }

    {
      std::lock_guard<std::mutex> lock(state.state_mutex);
      if (success) {
        state.pieces_we_have[piece_to_download] = true;
        state.downloaded_piece_count++;
        log_message("[Peer ", self.peer_address, "] Downloaded piece ",
                    piece_to_download, " (",
                    state.downloaded_piece_count.load(), "/", state.num_pieces,
                    ")");
      } else {
        // Mark as not in progress so another peer can try again
        state.pieces_in_progress[piece_to_download] = false;
      }
      self.assigned_piece = -1;
    }
  }
  close(self.sock);
  self.is_active = false;
}

// Main download orchestrator
void start_download(std::shared_ptr<DownloadState> state) {
  // Instantiate the correct piece selection algorithm
  if (state->algorithm == "sequential") {
    state->piece_selector = std::make_unique<SequentialSelector>();
    log_message(
        "[Download Manager] Using 'sequential' piece selection algorithm.");
  } else if (state->algorithm == "random") {
    state->piece_selector = std::make_unique<RandomSelector>();
    log_message("[Download Manager] Using 'random' piece selection algorithm.");
  } else {
    state->piece_selector = std::make_unique<RarestFirstSelector>();
    if (state->algorithm != "rarest") {
      log_message("[Download Manager] Unknown algorithm '", state->algorithm,
                  "'. Defaulting to 'rarest'.");
    } else {
      log_message(
          "[Download Manager] Using 'rarest' piece selection algorithm.");
    }
  }

  std::string dir_path;
  size_t last_slash = state->destination_path.find_last_of('/');
  if (last_slash != std::string::npos) {
    dir_path = state->destination_path.substr(0, last_slash);
    // The 0777 permissions mean read/write/execute for all users.
    mkdir(dir_path.c_str(), 0777);
  }

  int output_fd =
      open(state->destination_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (output_fd < 0) {
    log_message("Error: Could not open destination file: ",
                state->destination_path);
    return;
  }

  std::vector<std::thread> peer_threads;
  for (size_t i = 0; i < state->peers.size(); ++i) {
    peer_threads.emplace_back(peer_worker, std::ref(*state),
                              std::ref(state->peers[i]), output_fd);
  }

  while (static_cast<size_t>(state->peers_ready.load()) < state->peers.size()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  int active_count = 0;
  for (auto& p : state->peers) {
    if (p.is_active)
      active_count++;
  }

  if (active_count == 0) {
    log_message(
        "[Download Manager] ERROR: No active peers with pieces. Aborting "
        "download.");
    state->download_complete = true;  // Ensure we signal main thread on abort
    state->cv.notify_all();
    for (auto& t : peer_threads) {
      if (t.joinable()) {
        t.join();
      }
    }
    close(output_fd);
    return;
  }

  log_message(
      "[Download Manager] Peer connection attempts finished. Active peers: ",
      active_count, "/", state->peers.size(), ". Starting piece selection.");

  while (state->downloaded_piece_count < state->num_pieces) {
    std::unique_lock<std::mutex> lock(state->state_mutex);

    bool work_assigned = false;
    for (auto& p : state->peers) {
      if (p.is_active && p.assigned_piece == -1) {
        int piece_idx = state->piece_selector->select_piece(*state);

        if (piece_idx != -1) {
          if (p.bitfield.size() > (size_t)piece_idx && p.bitfield[piece_idx]) {
            state->pieces_in_progress[piece_idx] = true;
            p.assigned_piece = piece_idx;
            work_assigned = true;
          } else {
            log_message("[Download Manager] Peer ", p.peer_address,
                        " does not have piece ", piece_idx,
                        " or bitfield too short.");
          }
        } else {
          log_message(
              "[Download Manager] select_piece() returned -1 (no piece "
              "available now).");
          break;
        }
      }
    }

    if (work_assigned) {
      lock.unlock();
      state->cv.notify_all();
    } else {
      lock.unlock();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  bool success = false;
  if (state->downloaded_piece_count == state->num_pieces) {
    try {
      FileHashes downloaded_hashes = compute_hashes(state->destination_path);
      if (downloaded_hashes.concatenated_hashes ==
          state->concatenated_piece_hashes) {
        log_message("[Download Manager] Final file verification successful.");
        success = true;
      } else {
        log_message("[Download Manager] FINAL HASH MISMATCH! File is corrupt.");
      }
    } catch (...) {
      log_message(
          "[Download Manager] Exception during final hash verification.");
    }
  } else {
    log_message("[Download Manager] Download did not complete all pieces.");
  }

  state->download_successful = success;
  state->download_complete = true;

  // Wake up any sleeping worker threads. They will see download_complete is
  // true and exit.
  state->cv.notify_all();
  for (auto& t : peer_threads) {
    t.join();
  }

  close(output_fd);

  log_message("[Download Manager] Download process finished for ",
              state->file_name);
}
