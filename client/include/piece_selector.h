#pragma once
#include "download_manager.h"

// Forward declaration to avoid circular dependency issues.
struct DownloadState;

// Abstract base class for piece selection strategies.
class PieceSelector {
 public:
  virtual ~PieceSelector() = default;

  // Selects the next piece to download based on the strategy.
  // The index of the piece to download, or -1 if no piece can be
  // selected.
  virtual int select_piece(DownloadState& state) = 0;
};

// Implements the Rarest-First piece selection algorithm.
class RarestFirstSelector : public PieceSelector {
 public:
  int select_piece(DownloadState& state) override;
};

// Implements a simple sequential (0, 1, 2...) piece selection algorithm.
class SequentialSelector : public PieceSelector {
 public:
  int select_piece(DownloadState& state) override;
};

// Implements a random piece selection algorithm.

class RandomSelector : public PieceSelector {
 public:
  int select_piece(DownloadState& state) override;
};
