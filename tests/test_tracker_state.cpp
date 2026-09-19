// Unit tests for TrackerState — pure in-memory logic, no sockets involved.
// These pin the group/user/file semantics and the replication replay rules.
#include "test_assert.h"

#include <sstream>
#include <string>

#include "tracker_state.h"

// NOTE: TrackerState lives in tracker/; the test Makefile adds -Itracker.

namespace {

// The download_file response format is
//   "SUCCESS: <size> <hashes> [peer ...]"
// where peers are session-local state that replication intentionally does
// NOT mirror (addresses live only on the primary; sessions are invalidated
// on failover). These helpers compare the durable metadata prefix.
std::string metadata_prefix(const std::string& response) {
  std::istringstream ss(response);
  std::string tok, out;
  for (int i = 0; i < 3 && ss >> tok; ++i) {
    if (i > 0) out += " ";
    out += tok;
  }
  return out;
}

int token_count(const std::string& response) {
  std::istringstream ss(response);
  std::string tok;
  int n = 0;
  while (ss >> tok) ++n;
  return n;
}

}  // namespace

TEST(create_user_then_login_logout_roundtrip) {
  TrackerState s;
  TEST_ASSERT(s.handle_create_user("alice", "pw1").rfind("SUCCESS", 0) == 0);
  TEST_ASSERT(s.handle_login("alice", "pw1").rfind("SUCCESS", 0) == 0);
  TEST_ASSERT(s.handle_login("alice", "pw1").rfind("ERROR", 0) == 0);  // twice
  TEST_ASSERT(s.handle_logout("alice").rfind("SUCCESS", 0) == 0);
  TEST_ASSERT(s.handle_login("alice", "pw1").rfind("SUCCESS", 0) ==
              0);  // again OK after logout
}

TEST(login_rejects_wrong_password_and_unknown_user) {
  TrackerState s;
  s.handle_create_user("bob", "secret");
  TEST_ASSERT(s.handle_login("bob", "wrong").rfind("ERROR", 0) == 0);
  TEST_ASSERT(s.handle_login("nobody", "x").rfind("ERROR", 0) == 0);
}

TEST(create_user_rejects_duplicates) {
  TrackerState s;
  TEST_ASSERT(s.handle_create_user("carol", "a").rfind("SUCCESS", 0) == 0);
  TEST_ASSERT(s.handle_create_user("carol", "b").rfind("ERROR", 0) == 0);
}

TEST(group_lifecycle_join_accept_owner_transfer) {
  TrackerState s;
  s.handle_create_user("owner", "p");
  s.handle_create_user("member", "p");
  s.handle_login("owner", "p");
  s.handle_login("member", "p");

  TEST_ASSERT(s.handle_create_group("g1", "owner").rfind("SUCCESS", 0) == 0);
  TEST_ASSERT(s.handle_create_group("g1", "member").rfind("ERROR", 0) == 0);

  // Member requests to join; owner must approve.
  TEST_ASSERT(s.handle_join_group("g1", "member").rfind("SUCCESS", 0) == 0);
  TEST_ASSERT(s.handle_list_requests("g1", "owner").find("member") !=
              std::string::npos);
  TEST_ASSERT(s.handle_list_requests("g1", "member").rfind("ERROR", 0) ==
              0);  // non-owner cannot list
  TEST_ASSERT(s.handle_accept_request("g1", "member", "owner").rfind(
                  "SUCCESS", 0) == 0);

  // After acceptance, member sees the group's files (none yet).
  TEST_ASSERT(s.handle_list_files("g1").rfind("SUCCESS", 0) == 0);

  // Owner leaves -> deterministic ownership transfer to first member.
  TEST_ASSERT(s.handle_leave_group("g1", "owner").rfind("SUCCESS", 0) == 0);
  TEST_ASSERT(s.handle_list_requests("g1", "member").rfind("SUCCESS", 0) ==
              0);  // member is now owner
}

TEST(join_request_duplicates_and_nonexistent_group) {
  TrackerState s;
  s.handle_create_user("u1", "p");
  TEST_ASSERT(s.handle_join_group("missing", "u1").rfind("ERROR", 0) == 0);
  s.handle_create_user("u2", "p");
  s.handle_create_group("g", "u2");
  s.handle_join_group("g", "u1");
  TEST_ASSERT(s.handle_join_group("g", "u1").rfind("SUCCESS", 0) ==
              0);  // re-request overwrites pending (set semantics)
  TEST_ASSERT(s.handle_join_group("g", "u2").rfind("ERROR", 0) ==
              0);  // owner already member
}

TEST(upload_file_requires_membership_and_peer_address) {
  TrackerState s;
  s.handle_create_user("seeder", "p");
  s.handle_create_user("outsider", "p");
  s.handle_login("seeder", "p");
  s.handle_create_group("g", "seeder");

  // Not a member yet: upload must fail even with an address.
  s.add_peer_address("outsider", "1.2.3.4:1000");
  TEST_ASSERT(s.handle_upload_file("g", "f.bin", 10, std::string(40, '0'),
                                   "outsider")
                  .rfind("ERROR", 0) == 0);

  // Member but no peer address: upload must fail.
  TEST_ASSERT(s.handle_upload_file("g", "f.bin", 10, std::string(40, '0'),
                                   "seeder")
                  .rfind("ERROR", 0) == 0);

  s.add_peer_address("seeder", "5.6.7.8:2000");
  TEST_ASSERT(s.handle_upload_file("g", "f.bin", 10, std::string(40, '0'),
                                   "seeder")
                  .rfind("SUCCESS", 0) == 0);
}

TEST(leecher_lifecycle_and_download_peer_list) {
  TrackerState s;
  s.handle_create_user("a", "p");
  s.handle_create_user("b", "p");
  s.handle_login("a", "p");
  s.handle_login("b", "p");
  s.handle_create_group("g", "a");
  s.add_peer_address("a", "10.0.0.1:1111");
  s.add_peer_address("b", "10.0.0.2:2222");
  s.handle_join_group("g", "b");
  s.handle_accept_request("g", "b", "a");

  TEST_ASSERT(s.handle_upload_file("g", "file.bin", 1000, std::string(40, '1'),
                                   "a")
                  .rfind("SUCCESS", 0) == 0);

  // b starts leeching: appears as peer for a, is excluded from its own list.
  TEST_ASSERT(s.handle_start_downloading("g", "file.bin", "b").rfind(
                  "SUCCESS", 0) == 0);
  std::string resp_a = s.handle_download_file("g", "file.bin", "a");
  TEST_ASSERT(resp_a.find("10.0.0.2:2222") != std::string::npos);

  std::string resp_b = s.handle_download_file("g", "file.bin", "b");
  TEST_ASSERT(resp_b.find("10.0.0.1:1111") != std::string::npos);
  TEST_ASSERT(resp_b.find("10.0.0.2:2222") == std::string::npos);  // no self

  // Response shape: "SUCCESS: <size> <hashes> <peer...>"
  TEST_ASSERT(resp_b.rfind("SUCCESS: 1000 ", 0) == 0);
  TEST_ASSERT(resp_b.find(std::string(40, '1')) != std::string::npos);

  TEST_ASSERT(s.handle_stop_leeching("g", "file.bin", "b").rfind(
                  "SUCCESS", 0) == 0);
  // After stopping, a has nobody to download from.
  TEST_ASSERT(s.handle_download_file("g", "file.bin", "a").rfind("ERROR", 0) ==
              0);
}

TEST(download_file_errors_for_missing_group_file_or_no_seeders) {
  TrackerState s;
  s.handle_create_user("u", "p");
  s.handle_login("u", "p");
  TEST_ASSERT(s.handle_download_file("nogroup", "f", "u").rfind("ERROR", 0) ==
              0);
  s.handle_create_group("g", "u");
  TEST_ASSERT(s.handle_download_file("g", "nofile", "u").rfind("ERROR", 0) ==
              0);
}

TEST(leave_group_removes_seeder_entries) {
  TrackerState s;
  s.handle_create_user("a", "p");
  s.handle_create_user("b", "p");
  s.handle_login("a", "p");
  s.handle_login("b", "p");
  s.handle_create_group("g", "a");
  s.add_peer_address("a", "1.1.1.1:1");
  s.add_peer_address("b", "2.2.2.2:2");
  s.handle_join_group("g", "b");
  s.handle_accept_request("g", "b", "a");
  s.handle_upload_file("g", "f", 5, std::string(40, '2'), "a");
  s.handle_upload_file("g", "f", 5, std::string(40, '2'), "b");  // b seeder too

  s.handle_leave_group("g", "b");
  // b's seeder entry must be gone: a's download list has no peers.
  TEST_ASSERT(s.handle_download_file("g", "f", "a").rfind("ERROR", 0) == 0);
}

TEST(stop_share_removes_seeder_not_file_entry) {
  TrackerState s;
  s.handle_create_user("a", "p");
  s.handle_login("a", "p");
  s.handle_create_group("g", "a");
  s.add_peer_address("a", "1.1.1.1:1");
  s.handle_upload_file("g", "f", 5, std::string(40, '3'), "a");

  // stop_share by a non-seeder fails and does not disturb state.
  TEST_ASSERT(s.handle_stop_share("g", "f", "ghost").rfind("ERROR", 0) == 0);
  // Real seeder stops: no seeders left for download.
  TEST_ASSERT(s.handle_stop_share("g", "f", "a").rfind("SUCCESS", 0) == 0);
  TEST_ASSERT(s.handle_download_file("g", "f", "a").rfind("ERROR", 0) == 0);
}

TEST(clear_sessions_invalidates_everything) {
  TrackerState s;
  s.handle_create_user("a", "p");
  s.handle_create_user("b", "p");
  s.handle_login("a", "p");
  s.handle_login("b", "p");
  s.clear_sessions();
  // Logging in again must succeed (sessions were reset, not stuck logged-in).
  TEST_ASSERT(s.handle_login("a", "p").rfind("SUCCESS", 0) == 0);
  TEST_ASSERT(s.handle_login("b", "p").rfind("SUCCESS", 0) == 0);
}

// ---- Replication replay ----

TEST(replication_replay_matches_primary_operations) {
  TrackerState primary;
  TrackerState backup;

  // Apply operations directly on primary; replay identical strings on backup.
  TEST_ASSERT(primary.handle_create_user("u1", "p1").rfind("SUCCESS", 0) == 0);
  backup.process_replicated_command("create_user u1 p1");

  TEST_ASSERT(primary.handle_login("u1", "p1").rfind("SUCCESS", 0) == 0);
  primary.add_peer_address("u1", "9.9.9.9:9000");
  // The live forwarder replicates logins with a placeholder port: the backup
  // has no client socket and cannot learn the peer IP. Addresses are
  // session-local by design and are wiped on promotion (clear_sessions).
  backup.process_replicated_command("login u1 p1 0");

  TEST_ASSERT(primary.handle_create_group("g1", "u1").rfind("SUCCESS", 0) == 0);
  backup.process_replicated_command("create_group g1 u1");

  TEST_ASSERT(primary.handle_join_group("g1", "u1").rfind("ERROR", 0) ==
              0);  // already member (owner)

  TEST_ASSERT(primary.handle_create_user("u2", "p2").rfind("SUCCESS", 0) == 0);
  backup.process_replicated_command("create_user u2 p2");
  TEST_ASSERT(primary.handle_join_group("g1", "u2").rfind("SUCCESS", 0) == 0);
  backup.process_replicated_command("join_group g1 u2");
  TEST_ASSERT(primary.handle_accept_request("g1", "u2", "u1").rfind(
                  "SUCCESS", 0) == 0);
  backup.process_replicated_command("accept_request g1 u2 u1");

  TEST_ASSERT(primary.handle_upload_file("g1", "f.bin", 123,
                                         std::string(40, 'a'), "u1")
                  .rfind("SUCCESS", 0) == 0);
  backup.process_replicated_command(
      "upload_file g1 f.bin 123 " + std::string(40, 'a') + " u1");

  TEST_ASSERT(primary.handle_start_downloading("g1", "f.bin", "u2").rfind(
                  "SUCCESS", 0) == 0);
  backup.process_replicated_command("start_downloading g1 f.bin u2");

  // Durable state (groups, files, membership, sizes, hashes) must be
  // identical after replay.
  TEST_ASSERT_EQ(primary.handle_list_groups(), backup.handle_list_groups());
  TEST_ASSERT_EQ(primary.handle_list_files("g1"),
                 backup.handle_list_files("g1"));
  TEST_ASSERT_EQ(metadata_prefix(primary.handle_download_file("g1", "f.bin", "u1")),
                 metadata_prefix(backup.handle_download_file("g1", "f.bin", "u1")));
  TEST_ASSERT_EQ(metadata_prefix(primary.handle_download_file("g1", "f.bin", "u2")),
                 metadata_prefix(backup.handle_download_file("g1", "f.bin", "u2")));

  // Session state must NOT be mirrored: the primary knows the real peer
  // address, the backup only has the placeholder.
  TEST_ASSERT(primary.handle_download_file("g1", "f.bin", "u2")
                  .find("9.9.9.9:9000") != std::string::npos);
  TEST_ASSERT(backup.handle_download_file("g1", "f.bin", "u2")
                  .find("9.9.9.9") == std::string::npos);
}

TEST(replication_ignores_malformed_commands_without_crashing) {
  TrackerState s;
  // None of these may throw or corrupt state.
  s.process_replicated_command("");
  s.process_replicated_command("   ");
  s.process_replicated_command("create_user");
  s.process_replicated_command("create_user onlyone");
  s.process_replicated_command("login u p");          // missing port
  s.process_replicated_command("upload_file g f x");  // too few tokens
  s.process_replicated_command(
      "upload_file g f NOT_A_NUMBER abc u");  // non-numeric size
  s.process_replicated_command("unknown_cmd a b c");
  // State must still be usable afterwards.
  TEST_ASSERT(s.handle_create_user("ok", "p").rfind("SUCCESS", 0) == 0);
}

TEST(replication_rejects_control_characters_in_args) {
  TrackerState s;
  // A replicated command smuggling a newline inside a token must not create
  // a user whose id contains control characters.
  std::string evil = std::string("create_user bad\nguy pw");
  s.process_replicated_command(evil);
  // "bad" was created as its own user (line split) — "guy" must not exist.
  TEST_ASSERT(s.handle_create_user("guy", "p").rfind("SUCCESS", 0) == 0);
}

TEST(full_state_sync_rebuilds_identical_state) {
  TrackerState primary;
  primary.handle_create_user("u1", "p1");
  primary.handle_create_user("u2", "p2");
  primary.handle_login("u1", "p1");
  primary.add_peer_address("u1", "1.1.1.1:100");
  primary.handle_create_group("g1", "u1");
  primary.handle_join_group("g1", "u2");
  primary.handle_accept_request("g1", "u2", "u1");
  primary.handle_upload_file("g1", "big.bin", 9999, std::string(40, 'f'),
                             "u1");
  primary.handle_start_downloading("g1", "big.bin", "u2");

  std::string dump = primary.serialize_state();

  TrackerState restored;
  restored.process_replicated_command("FULL_STATE_SYNC\n" + dump);

  // Durable metadata must be rebuilt exactly.
  TEST_ASSERT_EQ(primary.handle_list_groups(), restored.handle_list_groups());
  TEST_ASSERT_EQ(primary.handle_list_files("g1"),
                 restored.handle_list_files("g1"));
  TEST_ASSERT_EQ(metadata_prefix(primary.handle_download_file("g1", "big.bin", "u1")),
                 metadata_prefix(restored.handle_download_file("g1", "big.bin", "u1")));
  TEST_ASSERT_EQ(metadata_prefix(primary.handle_download_file("g1", "big.bin", "u2")),
                 metadata_prefix(restored.handle_download_file("g1", "big.bin", "u2")));

  // serialize_state() intentionally omits session peer addresses: the
  // restored state must know the file metadata but no peer addresses.
  // u2 is a leecher whose only eligible peer is u1 (a seeder with an
  // address), so the primary returns 4 tokens and the restored state 3.
  // (u1's own view lists no peers: it is the sole seeder and u2 has no
  // address, hence 3 tokens on both.)
  TEST_ASSERT_EQ(token_count(primary.handle_download_file("g1", "big.bin", "u1")), 3);
  TEST_ASSERT_EQ(token_count(restored.handle_download_file("g1", "big.bin", "u1")), 3);
  TEST_ASSERT_EQ(token_count(primary.handle_download_file("g1", "big.bin", "u2")), 4);
  TEST_ASSERT_EQ(token_count(restored.handle_download_file("g1", "big.bin", "u2")), 3);
  TEST_ASSERT(primary.handle_download_file("g1", "big.bin", "u2")
                  .find("1.1.1.1:100") != std::string::npos);
}

TEST(full_state_sync_clears_previous_state) {
  TrackerState s;
  s.handle_create_user("stale", "p");
  s.handle_create_group("stale_group", "stale");
  s.process_replicated_command("FULL_STATE_SYNC\n");
  // After a sync with an empty dump, prior state must be gone.
  TEST_ASSERT(s.handle_create_group("stale_group", "stale").rfind(
                  "SUCCESS", 0) == 0);  // would ERROR if it still existed
}

TEST_MAIN()
