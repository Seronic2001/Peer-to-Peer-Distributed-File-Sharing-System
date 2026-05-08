#pragma once

#include <string>
#include <vector>

// constexpr for compile-time constant,
constexpr int PIECE_SIZE = 512 * 1024;  // 512 KB

// hold the results of the hashing process.
struct FileHashes {
  long long file_size;  // The total size of the file in bytes
  std::string full_file_hash;
  std::vector<std::string> piece_hashes;
  std::string concatenated_hashes;
};

// Computes the SHA1 hash for the entire file and for each 512KB piece.
// This function reads the file incrementally to keep memory usage low,
// regardless of the file's size. It uses the modern OpenSSL EVP API.
FileHashes compute_hashes(const std::string& file_path);

// Computes the SHA1 hash of an in-memory data buffer.
std::string hash_buffer(const char* data, size_t len);
