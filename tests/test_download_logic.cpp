// Unit tests for client piece-selection strategies (piece_selector.cpp).
// These exercise the real selectors against synthetic DownloadState fixtures.
#include "test_assert.h"

#include <algorithm>
#include <set>
#include <vector>

#include "piece_selector.h"
#include "hashing.h"

namespace {

// Initialize an empty download state with `num_pieces` and `num_peers` peers
// (all inactive, empty bitfields). DownloadState is non-copyable (atomics +
// mutexes), so callers own the instance and pass it by reference.
void init_state(DownloadState& s, int num_pieces, int num_peers) {
  s.file_size = (long long)num_pieces * PIECE_SIZE;
  s.num_pieces = num_pieces;
  s.pieces_we_have.assign(num_pieces, false);
  s.pieces_in_progress.assign(num_pieces, false);
  s.pieces_exhausted.assign(num_pieces, false);
  s.piece_failures.assign(num_pieces, 0);
  s.piece_rarity.assign(num_pieces, 0);
  for (int i = 0; i < num_peers; ++i) {
    s.peers.emplace_back("10.0.0." + std::to_string(i + 1) + ":1000");
  }
}

// Mark peer `p` as active and having exactly the pieces in `bits`.
void set_bitfield(PeerState& p, const std::vector<int>& bits, int num_pieces) {
  p.bitfield.assign(num_pieces, false);
  for (int b : bits) p.bitfield[b] = true;
  p.is_active = true;
}

}  // namespace

TEST(rarest_selects_piece_available_on_fewest_peers) {
  DownloadState s;
  init_state(s, 4, 3);
  // Piece 0: on all 3 peers. Piece 1: on 1 peer. Piece 2: on 2. Piece 3: on 2.
  set_bitfield(s.peers[0], {0, 1, 2, 3}, 4);
  set_bitfield(s.peers[1], {0, 2, 3}, 4);
  set_bitfield(s.peers[2], {0, 3}, 4);

  // Recompute rarity the way the download manager does.
  for (const auto& p : s.peers) {
    for (int i = 0; i < s.num_pieces; ++i) {
      if (p.bitfield[i]) s.piece_rarity[i]++;
    }
  }

  RarestFirstSelector sel;
  int choice = sel.select_piece(s);
  TEST_ASSERT_EQ(choice, 1);  // Rarity 1 beats 2 and 3.
}

TEST(rarest_skips_pieces_owned_by_no_active_peer) {
  DownloadState s;
  init_state(s, 3, 2);
  set_bitfield(s.peers[0], {0, 2}, 3);
  set_bitfield(s.peers[1], {0, 2}, 3);
  for (const auto& p : s.peers) {
    for (int i = 0; i < s.num_pieces; ++i) {
      if (p.bitfield[i]) s.piece_rarity[i]++;
    }
  }
  // Piece 1 is "rarest" (count 0) but nobody active has it: must not pick it.
  RarestFirstSelector sel;
  int choice = sel.select_piece(s);
  TEST_ASSERT(choice == 0 || choice == 2);
  TEST_ASSERT(choice != 1);
}

TEST(rarest_never_returns_owned_or_in_progress_pieces) {
  DownloadState s;
  init_state(s, 4, 1);
  set_bitfield(s.peers[0], {0, 1, 2, 3}, 4);
  s.pieces_we_have[0] = true;
  s.pieces_in_progress[1] = true;

  RarestFirstSelector sel;
  int choice = sel.select_piece(s);
  TEST_ASSERT(choice == 2 || choice == 3);
}

TEST(rarest_returns_minus_one_when_everything_is_available_everywhere) {
  DownloadState s;
  init_state(s, 2, 1);
  set_bitfield(s.peers[0], {0, 1}, 2);
  s.pieces_we_have[0] = true;
  s.pieces_we_have[1] = true;
  RarestFirstSelector sel;
  TEST_ASSERT_EQ(sel.select_piece(s), -1);
}

TEST(rarest_ignores_inactive_peers) {
  DownloadState s;
  init_state(s, 2, 2);
  set_bitfield(s.peers[0], {0, 1}, 2);
  s.peers[1].is_active = false;  // Connected but bitfield invalid.
  for (const auto& p : s.peers) {
    if (!p.is_active) continue;
    for (int i = 0; i < s.num_pieces; ++i) {
      if (p.bitfield[i]) s.piece_rarity[i]++;
    }
  }
  RarestFirstSelector sel;
  int choice = sel.select_piece(s);
  TEST_ASSERT(choice == 0 || choice == 1);  // Both servable by the active peer.
}

TEST(rarest_skips_exhausted_pieces) {
  DownloadState s;
  init_state(s, 3, 1);
  set_bitfield(s.peers[0], {0, 1, 2}, 3);
  s.pieces_exhausted[0] = true;  // Failed MAX_PIECE_ATTEMPTS times.
  RarestFirstSelector sel;
  int choice = sel.select_piece(s);
  TEST_ASSERT(choice == 1 || choice == 2);
}

TEST(rarest_all_pieces_exhausted_returns_minus_one) {
  DownloadState s;
  init_state(s, 2, 1);
  set_bitfield(s.peers[0], {0, 1}, 2);
  s.pieces_exhausted[0] = true;
  s.pieces_exhausted[1] = true;
  RarestFirstSelector sel;
  TEST_ASSERT_EQ(sel.select_piece(s), -1);
}

TEST(sequential_prefers_lowest_index) {
  DownloadState s;
  init_state(s, 5, 1);
  set_bitfield(s.peers[0], {1, 3, 4}, 5);  // Piece 0 unavailable.
  SequentialSelector sel;
  TEST_ASSERT_EQ(sel.select_piece(s), 1);
}

TEST(sequential_skips_owned_and_unavailable) {
  DownloadState s;
  init_state(s, 4, 1);
  set_bitfield(s.peers[0], {2, 3}, 4);
  s.pieces_we_have[2] = true;
  SequentialSelector sel;
  TEST_ASSERT_EQ(sel.select_piece(s), 3);
}

TEST(sequential_returns_minus_one_when_nothing_available) {
  DownloadState s;
  init_state(s, 3, 1);
  set_bitfield(s.peers[0], {}, 3);
  SequentialSelector sel;
  TEST_ASSERT_EQ(sel.select_piece(s), -1);
}

TEST(random_returns_valid_available_piece) {
  DownloadState s;
  init_state(s, 10, 2);
  set_bitfield(s.peers[0], {0, 1, 2, 3}, 10);
  set_bitfield(s.peers[1], {2, 3, 4}, 10);
  RandomSelector sel;
  for (int trial = 0; trial < 50; ++trial) {
    int choice = sel.select_piece(s);
    TEST_ASSERT(choice >= 0 && choice < 10);
    TEST_ASSERT(s.peers[0].bitfield[choice] || s.peers[1].bitfield[choice]);
    TEST_ASSERT(!s.pieces_we_have[choice]);
    TEST_ASSERT(!s.pieces_in_progress[choice]);
  }
}

TEST(random_returns_minus_one_when_no_pieces_available) {
  DownloadState s;
  init_state(s, 3, 1);
  set_bitfield(s.peers[0], {}, 3);
  RandomSelector sel;
  TEST_ASSERT_EQ(sel.select_piece(s), -1);
}

TEST(random_respects_exhausted_pieces) {
  DownloadState s;
  init_state(s, 5, 1);
  set_bitfield(s.peers[0], {0, 1, 2, 3, 4}, 5);
  s.pieces_exhausted[2] = true;
  RandomSelector sel;
  for (int trial = 0; trial < 50; ++trial) {
    TEST_ASSERT(sel.select_piece(s) != 2);
  }
}

TEST(all_selectors_agree_on_empty_state) {
  // No peers, no pieces: every strategy must decline politely.
  DownloadState s;
  init_state(s, 0, 0);
  RarestFirstSelector r;
  SequentialSelector q;
  RandomSelector d;
  TEST_ASSERT_EQ(r.select_piece(s), -1);
  TEST_ASSERT_EQ(q.select_piece(s), -1);
  TEST_ASSERT_EQ(d.select_piece(s), -1);
}

TEST_MAIN()
