#pragma once

#include <string>
#include <vector>

// constexpr for compile-time constant,
constexpr int PIECE_SIZE = 512 * 1024;  // 512 KB

// hold the results of the hashing process.
struct FileHashes {
  long long file_size;  // The total size of the file in bytes
  std::vector<std::string> piece_hashes;
  std::string concatenated_hashes;
};

// Number of pieces a file of `file_size` bytes is split into.
inline long long piece_count(long long file_size) {
  return (file_size + PIECE_SIZE - 1) / PIECE_SIZE;
}

// Computes the SHA1 hash of each 512KB piece of the file.
// This function reads the file incrementally to keep memory usage low,
// regardless of the file's size. It uses the modern OpenSSL EVP API.
FileHashes compute_hashes(const std::string& file_path);

// Computes the SHA1 hash of an in-memory data buffer.
std::string hash_buffer(const char* data, size_t len);
