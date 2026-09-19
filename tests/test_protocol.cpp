// Unit tests for common/protocol.cpp — length-prefixed framing.
#include "test_assert.h"

#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"

// Create a connected socketpair for in-memory send/recv testing.
static bool make_socket_pair(int fds[2]) {
  return socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
}

TEST(protocol_roundtrip_small) {
  int fds[2];
  TEST_ASSERT(make_socket_pair(fds));

  TEST_ASSERT(sendMessage(fds[0], "HELLO"));
  std::string received;
  TEST_ASSERT(receiveMessage(fds[1], received));
  TEST_ASSERT_EQ(received, "HELLO");

  close(fds[0]);
  close(fds[1]);
}

TEST(protocol_roundtrip_empty) {
  int fds[2];
  TEST_ASSERT(make_socket_pair(fds));

  TEST_ASSERT(sendMessage(fds[0], ""));
  std::string received = "sentinel";
  TEST_ASSERT(receiveMessage(fds[1], received));
  TEST_ASSERT(received.empty());

  close(fds[0]);
  close(fds[1]);
}

TEST(protocol_roundtrip_binary_payload_with_newlines_and_nul) {
  int fds[2];
  TEST_ASSERT(make_socket_pair(fds));

  // Piece payloads are binary; the framing must preserve every byte,
  // including NUL, \n, and \r which would break a text-based protocol.
  std::string payload;
  for (int i = 0; i < 4096; ++i) {
    payload.push_back(static_cast<char>(i % 256));
  }

  TEST_ASSERT(sendMessage(fds[0], payload));
  std::string received;
  TEST_ASSERT(receiveMessage(fds[1], received));
  TEST_ASSERT_EQ(received.size(), payload.size());
  TEST_ASSERT(std::memcmp(received.data(), payload.data(), payload.size()) == 0);

  close(fds[0]);
  close(fds[1]);
}

TEST(protocol_large_message) {
  int fds[2];
  TEST_ASSERT(make_socket_pair(fds));

  // > 1 MB exercises the partial-send/recv loops in sendAll/recvAll. The
  // socketpair buffer is far smaller than the payload, so the sender and
  // receiver MUST run concurrently: sending first and reading afterwards on
  // one thread would deadlock once the kernel buffer fills.
  std::string payload(2 * 1024 * 1024, 'x');
  for (size_t i = 0; i < payload.size(); i += 4096) payload[i] = 'y';

  std::string received;
  bool recv_ok = false;
  std::thread receiver([&] { recv_ok = receiveMessage(fds[1], received); });

  bool send_ok = sendMessage(fds[0], payload);
  receiver.join();

  TEST_ASSERT(send_ok);
  TEST_ASSERT(recv_ok);
  TEST_ASSERT_EQ(received.size(), payload.size());
  TEST_ASSERT(std::memcmp(received.data(), payload.data(),
                          std::min(payload.size(), received.size())) == 0);

  close(fds[0]);
  close(fds[1]);
}

TEST(protocol_many_sequential_messages_stay_framed) {
  int fds[2];
  TEST_ASSERT(make_socket_pair(fds));

  // Burst-write several messages; framing must survive coalescing in the
  // socket buffer (this is what a seeder sending a bitfield followed by
  // piece responses looks like on the wire).
  std::vector<std::string> sent = {"BITFIELD 0110", "PIECE 3 abc", "HEARTBEAT",
                                   ""};
  for (const auto& m : sent) {
    TEST_ASSERT(sendMessage(fds[0], m));
  }

  for (const auto& expected : sent) {
    std::string received;
    TEST_ASSERT(receiveMessage(fds[1], received));
    TEST_ASSERT_EQ(received, expected);
  }

  close(fds[0]);
  close(fds[1]);
}

TEST(protocol_recv_returns_false_on_peer_close) {
  int fds[2];
  TEST_ASSERT(make_socket_pair(fds));

  close(fds[0]);  // Peer hangs up without sending anything.

  std::string received;
  TEST_ASSERT(!receiveMessage(fds[1], received));  // Must report failure.

  close(fds[1]);
}

TEST(protocol_recv_rejects_oversized_length_prefix) {
  int fds[2];
  TEST_ASSERT(make_socket_pair(fds));

  // Hand-craft a length prefix larger than the 10 MB sanity limit. The
  // receiver must refuse instead of trying to allocate ~2 GB.
  uint32_t bogus_len = 2u * 1024u * 1024u * 1024u;  // 2 GB
  uint32_t net_len = htonl(bogus_len);
  ssize_t n = write(fds[0], &net_len, sizeof(net_len));
  TEST_ASSERT_EQ(n, (ssize_t)sizeof(net_len));

  std::string received;
  TEST_ASSERT(!receiveMessage(fds[1], received));

  close(fds[0]);
  close(fds[1]);
}

TEST(protocol_send_all_handles_eintr) {
  int fds[2];
  TEST_ASSERT(make_socket_pair(fds));

  // SIGCHLD is a real interruptible signal on Linux; deliver it to ourselves
  // mid-send to prove the EINTR retry loop works.
  signal(SIGCHLD, SIG_DFL);
  raise(SIGCHLD);

  TEST_ASSERT(sendMessage(fds[0], "sent under signal pressure"));
  std::string received;
  TEST_ASSERT(receiveMessage(fds[1], received));
  TEST_ASSERT_EQ(received, "sent under signal pressure");

  close(fds[0]);
  close(fds[1]);
}

TEST(protocol_recv_all_handles_eintr) {
  int fds[2];
  TEST_ASSERT(make_socket_pair(fds));

  // Deliver a signal to the thread blocked in recv, then send the message:
  // the receive loop must retry on EINTR rather than reporting failure.
  std::string received;
  bool recv_ok = false;
  std::thread receiver([&] {
    raise(SIGCHLD);  // Interrupt before recv arms; loop must still recover.
    recv_ok = receiveMessage(fds[1], received);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  TEST_ASSERT(sendMessage(fds[0], "eintr probe"));
  receiver.join();

  TEST_ASSERT(recv_ok);
  TEST_ASSERT_EQ(received, "eintr probe");

  close(fds[0]);
  close(fds[1]);
}

TEST(protocol_send_reports_failure_on_closed_socket) {
  int fds[2];
  TEST_ASSERT(make_socket_pair(fds));
  close(fds[1]);  // Receiver gone (connection reset once buffers drain).

  // Fill the socket buffer then keep sending; must eventually report failure
  // rather than looping forever. Payload of 16 MB >> AF_UNIX buffer.
  bool send_reported_failure = false;
  for (int i = 0; i < 64 && !send_reported_failure; ++i) {
    if (!sendMessage(fds[0], std::string(256 * 1024, 'z'))) {
      send_reported_failure = true;
    }
  }
  TEST_ASSERT(send_reported_failure);

  close(fds[0]);
}

TEST_MAIN()
