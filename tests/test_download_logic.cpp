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

// Recompute rarity the way the download manager does: one count per active
// peer holding the piece.
void compute_rarity(DownloadState& s) {
  for (const auto& p : s.peers) {
    if (!p.is_active) continue;
    for (int i = 0; i < s.num_pieces; ++i) {
      if (p.bitfield[i]) s.piece_rarity[i]++;
    }
  }
}

}  // namespace

TEST(rarest_selects_piece_available_on_fewest_peers) {
  DownloadState s;
  init_state(s, 4, 3);
  // Piece 0: on all 3 peers. Piece 1: on 1 peer. Piece 2: on 2. Piece 3: on 2.
  set_bitfield(s.peers[0], {0, 1, 2, 3}, 4);
  set_bitfield(s.peers[1], {0, 2, 3}, 4);
  set_bitfield(s.peers[2], {0, 3}, 4);
  compute_rarity(s);

  RarestFirstSelector sel;
  TEST_ASSERT_EQ(sel.select_piece(s, s.peers[0]), 1);  // Rarity 1 beats 2, 3.
}

TEST(rarest_only_offers_pieces_the_asking_peer_has) {
  DownloadState s;
  init_state(s, 4, 3);
  set_bitfield(s.peers[0], {0, 1, 2, 3}, 4);
  set_bitfield(s.peers[1], {0, 2, 3}, 4);
  set_bitfield(s.peers[2], {0, 3}, 4);
  compute_rarity(s);

  // Piece 1 is the rarest overall, but only peer 0 can serve it. The others
  // get the rarest piece out of what they hold.
  RarestFirstSelector sel;
  TEST_ASSERT_EQ(sel.select_piece(s, s.peers[1]), 2);
  // Pieces 0 and 3 are equally common: the tie goes to the lower index.
  TEST_ASSERT_EQ(sel.select_piece(s, s.peers[2]), 0);
}

TEST(rarest_never_returns_owned_or_in_progress_pieces) {
  DownloadState s;
  init_state(s, 4, 1);
  set_bitfield(s.peers[0], {0, 1, 2, 3}, 4);
  s.pieces_we_have[0] = true;
  s.pieces_in_progress[1] = true;

  RarestFirstSelector sel;
  int choice = sel.select_piece(s, s.peers[0]);
  TEST_ASSERT(choice == 2 || choice == 3);
}

TEST(rarest_returns_minus_one_when_nothing_is_left) {
  DownloadState s;
  init_state(s, 2, 1);
  set_bitfield(s.peers[0], {0, 1}, 2);
  s.pieces_we_have[0] = true;
  s.pieces_we_have[1] = true;
  RarestFirstSelector sel;
  TEST_ASSERT_EQ(sel.select_piece(s, s.peers[0]), -1);
}

TEST(selectors_offer_nothing_for_inactive_peer) {
  DownloadState s;
  init_state(s, 2, 2);
  set_bitfield(s.peers[0], {0, 1}, 2);
  // peers[1] never completed its handshake: inactive, empty bitfield.
  compute_rarity(s);
  RarestFirstSelector r;
  SequentialSelector q;
  RandomSelector d;
  TEST_ASSERT_EQ(r.select_piece(s, s.peers[1]), -1);
  TEST_ASSERT_EQ(q.select_piece(s, s.peers[1]), -1);
  TEST_ASSERT_EQ(d.select_piece(s, s.peers[1]), -1);
  TEST_ASSERT(r.select_piece(s, s.peers[0]) != -1);
}

TEST(sequential_prefers_lowest_index) {
  DownloadState s;
  init_state(s, 5, 1);
  set_bitfield(s.peers[0], {1, 3, 4}, 5);  // Piece 0 unavailable.
  SequentialSelector sel;
  TEST_ASSERT_EQ(sel.select_piece(s, s.peers[0]), 1);
}

TEST(sequential_skips_owned_and_unavailable) {
  DownloadState s;
  init_state(s, 4, 1);
  set_bitfield(s.peers[0], {2, 3}, 4);
  s.pieces_we_have[2] = true;
  SequentialSelector sel;
  TEST_ASSERT_EQ(sel.select_piece(s, s.peers[0]), 3);
}

TEST(sequential_returns_minus_one_when_nothing_available) {
  DownloadState s;
  init_state(s, 3, 1);
  set_bitfield(s.peers[0], {}, 3);
  SequentialSelector sel;
  TEST_ASSERT_EQ(sel.select_piece(s, s.peers[0]), -1);
}

TEST(random_returns_valid_available_piece) {
  DownloadState s;
  init_state(s, 10, 2);
  set_bitfield(s.peers[0], {0, 1, 2, 3}, 10);
  set_bitfield(s.peers[1], {2, 3, 4}, 10);
  s.pieces_we_have[2] = true;
  s.pieces_in_progress[3] = true;
  RandomSelector sel;
  std::set<int> seen;
  for (int trial = 0; trial < 200; ++trial) {
    int choice = sel.select_piece(s, s.peers[0]);
    TEST_ASSERT(choice == 0 || choice == 1);
    seen.insert(choice);
    TEST_ASSERT_EQ(sel.select_piece(s, s.peers[1]), 4);
  }
  TEST_ASSERT_EQ(seen.size(), (size_t)2);  // Both candidates get picked.
}

TEST(random_returns_minus_one_when_no_pieces_available) {
  DownloadState s;
  init_state(s, 3, 1);
  set_bitfield(s.peers[0], {}, 3);
  RandomSelector sel;
  TEST_ASSERT_EQ(sel.select_piece(s, s.peers[0]), -1);
}

TEST(all_selectors_agree_on_empty_file) {
  // Zero pieces: every strategy must decline politely.
  DownloadState s;
  init_state(s, 0, 1);
  set_bitfield(s.peers[0], {}, 0);
  RarestFirstSelector r;
  SequentialSelector q;
  RandomSelector d;
  TEST_ASSERT_EQ(r.select_piece(s, s.peers[0]), -1);
  TEST_ASSERT_EQ(q.select_piece(s, s.peers[0]), -1);
  TEST_ASSERT_EQ(d.select_piece(s, s.peers[0]), -1);
}

TEST_MAIN()
