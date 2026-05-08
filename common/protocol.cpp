#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "protocol.h"

// Helper: send exactly `count` bytes from buffer, looping on partial sends.
// Returns true on success, false on failure.
static bool sendAll(int sockfd, const void* buf, size_t count) {
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(buf);
  size_t total_sent = 0;
  while (total_sent < count) {
    ssize_t n =
        send(sockfd, ptr + total_sent, count - total_sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;  // interrupted, retry
      return false;
    }
    if (n == 0) {
      // unexpected, treat as failure
      return false;
    }
    total_sent += static_cast<size_t>(n);
  }
  return true;
}

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
  // Length prefix
  uint32_t len = static_cast<uint32_t>(message.length());
  uint32_t net_len = htonl(len);

  // Send the 4-byte length prefix (ensure all 4 bytes are sent)
  if (!sendAll(sockfd, &net_len, sizeof(net_len))) {
    return false;
  }

  // Send the payload (may be zero-length)
  if (len > 0) {
    if (!sendAll(sockfd, message.data(), len)) {
      return false;
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

  // Read payload
  if (len == 0) {
    message.clear();
    return true;
  }

  std::vector<char> buffer(len);
  if (!recvAll(sockfd, buffer.data(), len)) {
    return false;
  }

  message.assign(buffer.begin(), buffer.end());
  return true;
}
