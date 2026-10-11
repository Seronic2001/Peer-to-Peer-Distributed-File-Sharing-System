#include <algorithm>
#include <random>

#include "include/piece_selector.h"

namespace {

// Number of piece indices that are safe to inspect for this peer.
int candidate_range(const DownloadState& state, const PeerState& peer) {
  if (!peer.is_active)
    return 0;
  return std::min<size_t>(state.num_pieces, peer.bitfield.size());
}

// A piece is a candidate if we still need it, no other worker is fetching
// it, and this peer has it.
bool is_candidate(const DownloadState& state, const PeerState& peer, int i) {
  return !state.pieces_we_have[i] && !state.pieces_in_progress[i] &&
         peer.bitfield[i];
}

}  // namespace

int RarestFirstSelector::select_piece(const DownloadState& state,
                                      const PeerState& peer) {
  int best_piece = -1;
  const int n = candidate_range(state, peer);
  for (int i = 0; i < n; ++i) {
    if (is_candidate(state, peer, i) &&
        (best_piece == -1 ||
         state.piece_rarity[i] < state.piece_rarity[best_piece])) {
      best_piece = i;
    }
  }
  return best_piece;
}

int SequentialSelector::select_piece(const DownloadState& state,
                                     const PeerState& peer) {
  const int n = candidate_range(state, peer);
  for (int i = 0; i < n; ++i) {
    if (is_candidate(state, peer, i))
      return i;
  }
  return -1;
}

int RandomSelector::select_piece(const DownloadState& state,
                                 const PeerState& peer) {
  // Reservoir sampling: one pass, uniform over the candidates, no allocation.
  int chosen = -1;
  int seen = 0;
  const int n = candidate_range(state, peer);
  for (int i = 0; i < n; ++i) {
    if (!is_candidate(state, peer, i))
      continue;
    ++seen;
    if (std::uniform_int_distribution<int>(1, seen)(rng) == 1)
      chosen = i;
  }
  return chosen;
}
