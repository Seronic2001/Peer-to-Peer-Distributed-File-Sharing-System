#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iomanip>
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

  // Get file size
  struct stat file_stat;
  if (fstat(fd, &file_stat) < 0) {
    close(fd);
    throw std::runtime_error("Could not get size of file " + file_path);
  }
  result.file_size = file_stat.st_size;

  // Context for the full file hash
  EVP_MD_CTX* md_ctx_full = EVP_MD_CTX_new();
  if (!md_ctx_full) {
    throw std::runtime_error("Failed to create EVP_MD_CTX for full file hash");
  }
  EVP_DigestInit_ex(md_ctx_full, EVP_sha1(), NULL);

  std::vector<char> buffer(PIECE_SIZE);
  ssize_t bytes_read;

  // Read the file piece by piece in a loop using read()
  while ((bytes_read = read(fd, buffer.data(), PIECE_SIZE)) > 0) {
    // Update the full file hash with the chunk we just read
    EVP_DigestUpdate(md_ctx_full, buffer.data(), bytes_read);

    // Compute the hash for this specific piece
    std::string piece_hash_str = hash_buffer(buffer.data(), bytes_read);
    result.piece_hashes.push_back(piece_hash_str);
    result.concatenated_hashes += piece_hash_str;
  }

  // Finalize the full file hash
  unsigned char full_digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len;
  EVP_DigestFinal_ex(md_ctx_full, full_digest, &digest_len);
  result.full_file_hash = to_hex_string(full_digest, digest_len);

  EVP_MD_CTX_free(md_ctx_full);
  close(fd);

  return result;
}
