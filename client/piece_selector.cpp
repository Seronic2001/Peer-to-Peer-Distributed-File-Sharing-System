#include <algorithm>
#include <chrono>
#include <random>
#include <vector>

#include "include/piece_selector.h"

int RarestFirstSelector::select_piece(DownloadState& state) {
  int best_piece = -1;
  // Initialize with a value higher than any possible rarity
  int min_rarity = state.peers.size() + 1;

  // Find the rarest piece that we don't have and isn't already in progress.
  for (int i = 0; i < state.num_pieces; ++i) {
    if (!state.pieces_we_have[i] && !state.pieces_in_progress[i]) {
      // Check if this piece is rarer than the best one we've found so far.
      if (state.piece_rarity[i] < min_rarity) {
        // Ensure at least one of our active peers actually has this piece.
        bool is_available = false;
        for (const auto& p : state.peers) {
          if (p.is_active && p.bitfield.size() > (size_t)i && p.bitfield[i]) {
            is_available = true;
            break;
          }
        }

        if (is_available) {
          min_rarity = state.piece_rarity[i];
          best_piece = i;
        }
      }
    }
  }
  return best_piece;
}

int SequentialSelector::select_piece(DownloadState& state) {
  // Find the first piece we don't have and that isn't in progress.
  for (int i = 0; i < state.num_pieces; ++i) {
    if (!state.pieces_we_have[i] && !state.pieces_in_progress[i]) {
      // Check if any of our active peers have this piece.
      for (const auto& p : state.peers) {
        if (p.is_active && p.bitfield.size() > (size_t)i && p.bitfield[i]) {
          // This is a valid piece to download.
          return i;
        }
      }
    }
  }
  // No suitable piece was found.
  return -1;
}

int RandomSelector::select_piece(DownloadState& state) {
  std::vector<int> available_pieces;
  // Find all pieces we need and that are available from at least one peer.
  for (int i = 0; i < state.num_pieces; ++i) {
    if (!state.pieces_we_have[i] && !state.pieces_in_progress[i]) {
      for (const auto& p : state.peers) {
        if (p.is_active && p.bitfield.size() > (size_t)i && p.bitfield[i]) {
          available_pieces.push_back(i);
          break;  // Move to the next piece once we know it's available
        }
      }
    }
  }

  if (available_pieces.empty()) {
    return -1;  // No pieces to download
  }

  // Shuffle the list of available pieces and return the first one.
  unsigned seed =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::shuffle(available_pieces.begin(), available_pieces.end(),
               std::default_random_engine(seed));

  return available_pieces[0];
}
