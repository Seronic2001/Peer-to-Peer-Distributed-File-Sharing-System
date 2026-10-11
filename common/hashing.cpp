#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "hashing.h"

// modern, non-deprecated EVP API for hashing
#include <openssl/evp.h>

std::string to_hex_string(const unsigned char* digest, int len) {
  std::stringstream ss;
  ss << std::hex << std::setfill('0');
  for (int i = 0; i < len; ++i) {
    ss << std::setw(2) << static_cast<int>(digest[i]);
  }
  return ss.str();
}

std::string hash_buffer(const char* data, size_t len) {
  EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
  if (!mdctx) {
    throw std::runtime_error("Failed to create EVP_MD_CTX for buffer hashing");
  }
  const EVP_MD* md = EVP_sha1();
  unsigned char md_value[EVP_MAX_MD_SIZE];
  unsigned int md_len;

  EVP_DigestInit_ex(mdctx, md, NULL);
  EVP_DigestUpdate(mdctx, data, len);
  EVP_DigestFinal_ex(mdctx, md_value, &md_len);
  EVP_MD_CTX_free(mdctx);

  return to_hex_string(md_value, md_len);
}

FileHashes compute_hashes(const std::string& file_path) {
  int fd = open(file_path.c_str(), O_RDONLY);
  if (fd < 0) {
    throw std::runtime_error("Could not open file " + file_path);
  }

  FileHashes result;
  result.file_size = 0;
  std::vector<char> buffer(PIECE_SIZE);

  // Read the file piece by piece. A piece is only short at end of file, so
  // keep reading until the buffer is full or read() reports EOF.
  while (true) {
    size_t filled = 0;
    while (filled < buffer.size()) {
      ssize_t n = read(fd, buffer.data() + filled, buffer.size() - filled);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        close(fd);
        throw std::runtime_error("Could not read file " + file_path);
      }
      if (n == 0)
        break;
      filled += static_cast<size_t>(n);
    }
    if (filled == 0)
      break;

    std::string piece_hash_str = hash_buffer(buffer.data(), filled);
    result.piece_hashes.push_back(piece_hash_str);
    result.concatenated_hashes += piece_hash_str;
    result.file_size += static_cast<long long>(filled);
    if (filled < buffer.size())
      break;
  }

  close(fd);
  return result;
}
