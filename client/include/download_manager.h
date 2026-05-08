#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "piece_selector.h"

// Forward declaration
class PieceSelector;

struct PeerState {
  int sock = -1;
  std::string peer_address;
  std::vector<bool> bitfield;
  bool is_active = false;
  int assigned_piece = -1;

  PeerState(const std::string& addr) : peer_address(addr) {}
};

struct DownloadState {
  std::string group_id;
  std::string file_name;
  std::string destination_path;
  long long file_size;
  std::vector<std::string> piece_hashes;
  std::string concatenated_piece_hashes;
  int num_pieces;
  std::string algorithm = "rarest";

  std::vector<PeerState> peers;
  std::vector<bool> pieces_we_have;
  std::vector<bool> pieces_in_progress;
  std::vector<int> piece_rarity;

  std::atomic<int> peers_ready{0};
  std::atomic<int> downloaded_piece_count{0};
  std::atomic<bool> download_complete{false};
  std::atomic<bool> download_successful{false};

  std::mutex state_mutex;
  std::mutex file_mutex;
  std::condition_variable cv;

  std::unique_ptr<PieceSelector> piece_selector;
};

void start_download(std::shared_ptr<DownloadState> state);
