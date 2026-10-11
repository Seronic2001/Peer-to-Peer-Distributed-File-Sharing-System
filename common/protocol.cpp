#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#include "protocol.h"

// Helper: receive exactly `count` bytes into buffer, looping on partial reads.
// Returns true on success, false on failure or peer closed connection.
static bool recvAll(int sockfd, void* buf, size_t count) {
  uint8_t* ptr = reinterpret_cast<uint8_t*>(buf);
  size_t total_recv = 0;
  while (total_recv < count) {
    ssize_t n = recv(sockfd, ptr + total_recv, count - total_recv, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;  // interrupted, retry
      return false;
    }
    if (n == 0) {
      // peer closed connection
      return false;
    }
    total_recv += static_cast<size_t>(n);
  }
  return true;
}

// Implementation of the sendMessage function
bool sendMessage(int sockfd, const std::string& message) {
  uint32_t net_len = htonl(static_cast<uint32_t>(message.length()));

  // The length prefix and the payload go out in one sendmsg() call. Sending
  // them separately makes every request wait ~40ms on the receiver's delayed
  // ACK (Nagle holds the second small write back).
  struct iovec iov[2];
  iov[0].iov_base = &net_len;
  iov[0].iov_len = sizeof(net_len);
  iov[1].iov_base = const_cast<char*>(message.data());
  iov[1].iov_len = message.length();

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;

  size_t remaining = sizeof(net_len) + message.length();
  while (remaining > 0) {
    ssize_t n = sendmsg(sockfd, &msg, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR)
      continue;  // interrupted, retry
    if (n <= 0)
      return false;
    remaining -= static_cast<size_t>(n);

    // Partial send: step past the bytes that went out.
    size_t sent = static_cast<size_t>(n);
    while (sent > 0 && msg.msg_iovlen > 0) {
      if (sent >= msg.msg_iov->iov_len) {
        sent -= msg.msg_iov->iov_len;
        msg.msg_iov++;
        msg.msg_iovlen--;
      } else {
        msg.msg_iov->iov_base = static_cast<char*>(msg.msg_iov->iov_base) + sent;
        msg.msg_iov->iov_len -= sent;
        sent = 0;
      }
    }
  }
  return true;
}

// Implementation of the receiveMessage function
bool receiveMessage(int sockfd, std::string& message) {
  // Read 4-byte length prefix
  uint32_t net_len;
  if (!recvAll(sockfd, &net_len, sizeof(net_len))) {
    return false;
  }

  // Convert to host order
  uint32_t len = ntohl(net_len);

  // Sanity check
  const uint32_t MAX_LEN = 10 * 1024 * 1024;  // 10 MB
  if (len > MAX_LEN) {
    return false;
  }

  // Read the payload straight into the caller's string (no intermediate copy).
  message.resize(len);
  return len == 0 || recvAll(sockfd, &message[0], len);
}
