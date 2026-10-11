#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "piece_selector.h"

// A peer is dropped after this many consecutive pieces that fail hash
// verification, so one bad peer cannot waste a whole download's bandwidth.
constexpr int MAX_PEER_STRIKES = 3;

// Forward declaration
class PieceSelector;

struct PeerState {
  std::string peer_address;
  std::vector<bool> bitfield;  // Pieces this peer can still usefully serve.
  bool is_active = false;      // Handshake done and worker still running.

  PeerState(const std::string& addr) : peer_address(addr) {}
};

struct DownloadState {
  std::string group_id;
  std::string file_name;
  std::string destination_path;
  long long file_size;
  std::string concatenated_piece_hashes;  // 40 hex chars per piece.
  int num_pieces;
  std::string algorithm = "rarest";

  // Everything below up to the atomics is guarded by state_mutex.
  std::vector<PeerState> peers;
  std::vector<bool> pieces_we_have;
  std::vector<bool> pieces_in_progress;
  std::vector<int> piece_rarity;  // Active peers holding each piece.

  std::atomic<int> downloaded_piece_count{0};
  std::atomic<bool> download_complete{false};
  std::atomic<bool> download_successful{false};

  // Set by the client's main thread once it has told the tracker about the
  // outcome. Only that thread touches it.
  bool finalized = false;

  std::mutex state_mutex;
  std::condition_variable cv;  // Signalled whenever a piece or peer changes.

  std::unique_ptr<PieceSelector> piece_selector;
};

void start_download(std::shared_ptr<DownloadState> state);
