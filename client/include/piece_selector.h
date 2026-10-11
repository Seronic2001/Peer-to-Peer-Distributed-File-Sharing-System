#pragma once
#include <random>

#include "download_manager.h"

// Forward declarations to avoid circular dependency issues.
struct DownloadState;
struct PeerState;

// Abstract base class for piece selection strategies.
class PieceSelector {
 public:
  virtual ~PieceSelector() = default;

  // Selects the next piece to request from `peer`: one we still need, that
  // nobody else is fetching, and that the peer has. Returns the piece index,
  // or -1 if the peer has nothing to offer right now.
  virtual int select_piece(const DownloadState& state,
                           const PeerState& peer) = 0;
};

// Implements the Rarest-First piece selection algorithm.
class RarestFirstSelector : public PieceSelector {
 public:
  int select_piece(const DownloadState& state, const PeerState& peer) override;
};

// Implements a simple sequential (0, 1, 2...) piece selection algorithm.
class SequentialSelector : public PieceSelector {
 public:
  int select_piece(const DownloadState& state, const PeerState& peer) override;
};

// Implements a random piece selection algorithm.
class RandomSelector : public PieceSelector {
 public:
  int select_piece(const DownloadState& state, const PeerState& peer) override;

 private:
  std::mt19937 rng{std::random_device{}()};
};
