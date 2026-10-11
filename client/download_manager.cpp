#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "hashing.h"
#include "include/download_manager.h"
#include "include/ui.h"
#include "protocol.h"

namespace {

// Helper to connect to a single peer
int connect_to_peer(const std::string& peer_addr) {
  size_t colon_pos = peer_addr.find(':');
  if (colon_pos == std::string::npos)
    return -1;
  std::string ip = peer_addr.substr(0, colon_pos);
  int port = 0;
  try {
    port = std::stoi(peer_addr.substr(colon_pos + 1));
  } catch (const std::exception&) {
    return -1;
  }

  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    return -1;
  }

  // Bound connect and request/response I/O so a stalled peer (e.g. one that
  // accepts the connection but never replies) cannot hang a download worker
  // forever.
  struct timeval io_timeout;
  io_timeout.tv_sec = 10;
  io_timeout.tv_usec = 0;
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
  setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));

  struct sockaddr_in serv_addr;
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &serv_addr.sin_addr) <= 0 ||
      connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
    close(sockfd);
    return -1;
  }
  return sockfd;
}

// Connects to the peer, performs the handshake and records its bitfield.
// Returns the connected socket, or -1 if the peer cannot be used.
int open_peer_session(DownloadState& state, PeerState& self) {
  int sock = connect_to_peer(self.peer_address);
  if (sock < 0) {
    log_warn("[Peer ", self.peer_address, "] could not connect.");
    return -1;
  }
  log_debug("[Peer ", self.peer_address, "] Connected.");

  const std::string bitfield_prefix = "BITFIELD ";
  std::string response, bitfield_msg;
  if (!sendMessage(sock,
                   "HANDSHAKE " + state.group_id + " " + state.file_name) ||
      !receiveMessage(sock, response) || response != "HANDSHAKE_OK" ||
      !receiveMessage(sock, bitfield_msg) ||
      bitfield_msg.compare(0, bitfield_prefix.size(), bitfield_prefix) != 0 ||
      bitfield_msg.size() - bitfield_prefix.size() !=
          static_cast<size_t>(state.num_pieces)) {
    log_debug("[Peer ", self.peer_address, "] Handshake or BITFIELD failed.");
    close(sock);
    return -1;
  }

  std::lock_guard<std::mutex> lock(state.state_mutex);
  self.bitfield.assign(state.num_pieces, false);
  for (int i = 0; i < state.num_pieces; ++i) {
    if (bitfield_msg[bitfield_prefix.size() + i] == '1') {
      self.bitfield[i] = true;
      state.piece_rarity[i]++;
    }
  }
  self.is_active = true;
  log_debug("[Peer ", self.peer_address, "] BITFIELD received. Active and has ",
            std::count(self.bitfield.begin(), self.bitfield.end(), true),
            " pieces.");
  return sock;
}

enum class FetchResult {
  Ok,
  BadData,     // Peer answered, but the bytes failed hash verification.
  PeerFailed,  // Connection or protocol failure: the peer is unusable.
};

// Requests one piece, verifies it and writes it to disk. Called without
// state_mutex held; pwrite() is positional, so workers need no file lock.
FetchResult fetch_piece(const DownloadState& state,
                        const PeerState& self,
                        int sock,
                        int piece,
                        int output_fd) {
  std::string response;
  if (!sendMessage(sock, "REQUEST_PIECE " + std::to_string(piece)) ||
      !receiveMessage(sock, response)) {
    log_debug("[Peer ", self.peer_address, "] Failed to receive piece ",
              piece);
    return FetchResult::PeerFailed;
  }

  const std::string header = "PIECE " + std::to_string(piece) + " ";
  if (response.compare(0, header.size(), header) != 0) {
    log_debug("[Peer ", self.peer_address,
              "] Malformed PIECE response for piece ", piece);
    return FetchResult::PeerFailed;
  }
  const char* piece_data = response.data() + header.size();
  const size_t data_len = response.size() - header.size();

  if (state.concatenated_piece_hashes.compare(
          static_cast<size_t>(piece) * 40, 40,
          hash_buffer(piece_data, data_len)) != 0) {
    log_debug("[Peer ", self.peer_address, "] Hash mismatch for piece ",
              piece);
    return FetchResult::BadData;
  }

  // The piece only counts as downloaded if the full payload reached the disk.
  const off_t offset = static_cast<off_t>(piece) * PIECE_SIZE;
  if (pwrite(output_fd, piece_data, data_len, offset) != (ssize_t)data_len) {
    log_error("[Peer ", self.peer_address, "] Could not write piece ", piece,
              " to disk.");
    return FetchResult::PeerFailed;
  }
  return FetchResult::Ok;
}

// True if a piece this peer has is currently being fetched by another
// worker. That fetch may still fail, so the peer should wait rather than quit.
bool may_get_work_later(const DownloadState& state, const PeerState& self) {
  for (int i = 0; i < state.num_pieces; ++i) {
    if (!state.pieces_we_have[i] && state.pieces_in_progress[i] &&
        self.bitfield[i]) {
      return true;
    }
  }
  return false;
}

// One worker per peer. Each worker pulls its own next piece under the lock,
// fetches it without the lock, and exits once the peer has nothing left to
// offer or has proven unusable.
void peer_worker(DownloadState& state, PeerState& self, int output_fd) {
  int sock = open_peer_session(state, self);
  if (sock < 0)
    return;

  int strikes = 0;
  std::unique_lock<std::mutex> lock(state.state_mutex);
  while (true) {
    int piece = -1;
    state.cv.wait(lock, [&] {
      piece = state.piece_selector->select_piece(state, self);
      return piece != -1 || !may_get_work_later(state, self);
    });
    if (piece == -1)
      break;

    state.pieces_in_progress[piece] = true;
    lock.unlock();
    FetchResult result = fetch_piece(state, self, sock, piece, output_fd);
    lock.lock();
    state.pieces_in_progress[piece] = false;

    if (result == FetchResult::Ok) {
      state.pieces_we_have[piece] = true;
      state.downloaded_piece_count++;
      strikes = 0;
      log_debug("[Peer ", self.peer_address, "] Downloaded piece ", piece,
                " (", state.downloaded_piece_count.load(), "/",
                state.num_pieces, ")");
    } else if (result == FetchResult::BadData) {
      // This peer's copy of the piece is bad: never ask it again, and let
      // another peer supply it.
      self.bitfield[piece] = false;
      state.piece_rarity[piece]--;
      strikes++;
    }
    state.cv.notify_all();

    if (result == FetchResult::PeerFailed || strikes >= MAX_PEER_STRIKES) {
      log_warn("[Peer ", self.peer_address, "] dropped from download of ",
               state.file_name, ".");
      break;
    }
  }

  // Retire: this peer no longer contributes to availability.
  for (int i = 0; i < state.num_pieces; ++i) {
    if (self.bitfield[i])
      state.piece_rarity[i]--;
  }
  self.is_active = false;
  lock.unlock();
  state.cv.notify_all();
  close(sock);
}

std::unique_ptr<PieceSelector> make_piece_selector(
    const std::string& algorithm) {
  if (algorithm == "sequential")
    return std::make_unique<SequentialSelector>();
  if (algorithm == "random")
    return std::make_unique<RandomSelector>();
  if (algorithm != "rarest") {
    log_warn("[Download Manager] Unknown algorithm '", algorithm,
             "'. Defaulting to 'rarest'.");
  }
  return std::make_unique<RarestFirstSelector>();
}

}  // namespace

// Main download orchestrator
void start_download(std::shared_ptr<DownloadState> state) {
  state->piece_selector = make_piece_selector(state->algorithm);

  size_t last_slash = state->destination_path.find_last_of('/');
  if (last_slash != std::string::npos) {
    mkdir(state->destination_path.substr(0, last_slash).c_str(), 0777);
  }

  bool success = false;
  int output_fd =
      open(state->destination_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (output_fd < 0) {
    log_error("Could not open destination file: ", state->destination_path);
  } else {
    std::vector<std::thread> peer_threads;
    for (auto& peer : state->peers) {
      peer_threads.emplace_back(peer_worker, std::ref(*state), std::ref(peer),
                                output_fd);
    }
    // Workers exit on their own once no peer can supply a missing piece.
    for (auto& t : peer_threads) {
      t.join();
    }
    close(output_fd);

    // Every piece was hash-verified before it was written.
    success = state->downloaded_piece_count == state->num_pieces;
    if (!success) {
      log_warn("[Download Manager] No peer could supply the remaining pieces (",
               state->downloaded_piece_count.load(), "/", state->num_pieces,
               " downloaded).");
    }
  }

  // The client's main loop picks up the outcome and informs the tracker.
  state->download_successful = success;
  state->download_complete = true;
  log_message("[Download Manager] Download process finished for ",
              state->file_name);
}
