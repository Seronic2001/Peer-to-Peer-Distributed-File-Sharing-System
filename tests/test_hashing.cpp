// Unit tests for common/hashing.cpp — SHA-1 full-file + per-piece hashing.
#include "test_assert.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hashing.h"

namespace {

// RAII temp-file helper: writes `content` to a unique temp path and removes
// the file (even on assertion failure) when the scope exits.
class TempFile {
 public:
  explicit TempFile(const std::string& content) {
    path_ = "/tmp/p2p_test_file_XXXXXX";
    std::vector<char> buf(path_.begin(), path_.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd < 0) {
      TEST_FAIL_MSG("mkstemp failed");
    }
    path_ = buf.data();
    size_t written = 0;
    while (written < content.size()) {
      ssize_t n = write(fd, content.data() + written, content.size() - written);
      if (n <= 0) {
        close(fd);
        TEST_FAIL_MSG("short write in TempFile");
      }
      written += static_cast<size_t>(n);
    }
    close(fd);
  }
  ~TempFile() { unlink(path_.c_str()); }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace

TEST(hash_buffer_known_sha1_vectors) {
  // Reference SHA-1 values (well-known test vectors).
  TEST_ASSERT_EQ(hash_buffer("", 0),
                 "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  TEST_ASSERT_EQ(hash_buffer("abc", 3),
                 "a9993e364706816aba3e25717850c26c9cd0d89d");
  // 56-byte message spans the padding boundary of SHA-1's block size.
  std::string m56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  TEST_ASSERT_EQ(m56.size(), (size_t)56);
  TEST_ASSERT_EQ(hash_buffer(m56.data(), m56.size()),
                 "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  // One million 'a' characters exercises multi-block hashing.
  std::string million(1000000, 'a');
  TEST_ASSERT_EQ(hash_buffer(million.data(), million.size()),
                 "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST(hash_buffer_is_deterministic) {
  std::string data = "the same bytes hashed twice";
  TEST_ASSERT_EQ(hash_buffer(data.data(), data.size()),
                 hash_buffer(data.data(), data.size()));
}

TEST(hash_buffer_binary_data_with_nul_bytes) {
  char binary[] = {'\0', '\x01', '\xff', 'a', '\0'};
  std::string expected = hash_buffer(binary, sizeof(binary));
  // Recompute via std::string carrying embedded NULs.
  std::string s(binary, sizeof(binary));
  TEST_ASSERT_EQ(hash_buffer(s.data(), s.size()), expected);
}

TEST(compute_hashes_small_file_single_piece) {
  TempFile file("hello p2p world");
  FileHashes h = compute_hashes(file.path());

  TEST_ASSERT_EQ(h.file_size, (long long)15);
  TEST_ASSERT_EQ(h.piece_hashes.size(), (size_t)1);
  // The single piece hash equals the full-file hash for files < PIECE_SIZE.
  TEST_ASSERT_EQ(h.piece_hashes[0], h.full_file_hash);
  TEST_ASSERT_EQ(h.concatenated_hashes, h.full_file_hash);
}

TEST(compute_hashes_exact_multiple_of_piece_size) {
  // A file of exactly 2 * PIECE_SIZE must produce exactly 2 pieces, with no
  // trailing empty piece (the read() loop returns 0 and terminates).
  std::string content(2 * PIECE_SIZE, 'A');
  content[PIECE_SIZE / 2] = 'B';  // Non-uniform so hashes differ.
  TempFile file(content);
  FileHashes h = compute_hashes(file.path());

  TEST_ASSERT_EQ(h.file_size, (long long)(2 * PIECE_SIZE));
  TEST_ASSERT_EQ(h.piece_hashes.size(), (size_t)2);
  TEST_ASSERT_EQ(h.concatenated_hashes.size(), (size_t)80);  // 2 * 40 hex

  // Full-file hash must match an in-memory hash of the same bytes.
  TEST_ASSERT_EQ(h.full_file_hash, hash_buffer(content.data(), content.size()));

  // Piece i must equal the hash of that slice of the content.
  TEST_ASSERT_EQ(h.piece_hashes[0],
                 hash_buffer(content.data(), PIECE_SIZE));
  TEST_ASSERT_EQ(h.piece_hashes[1],
                 hash_buffer(content.data() + PIECE_SIZE, PIECE_SIZE));
}

TEST(compute_hashes_partial_final_piece) {
  // 1.5 pieces: second piece is half-full. The final piece hash must cover
  // only the actual bytes, not the padded buffer.
  std::string content(PIECE_SIZE + PIECE_SIZE / 2, 'C');
  TempFile file(content);
  FileHashes h = compute_hashes(file.path());

  TEST_ASSERT_EQ(h.piece_hashes.size(), (size_t)2);
  TEST_ASSERT_EQ(h.piece_hashes[1],
                 hash_buffer(content.data() + PIECE_SIZE, PIECE_SIZE / 2));
  TEST_ASSERT_EQ(h.full_file_hash, hash_buffer(content.data(), content.size()));
}

TEST(compute_hashes_empty_file) {
  TempFile file("");
  FileHashes h = compute_hashes(file.path());

  TEST_ASSERT_EQ(h.file_size, (long long)0);
  TEST_ASSERT(h.piece_hashes.empty());
  TEST_ASSERT(h.concatenated_hashes.empty());
  TEST_ASSERT_EQ(h.full_file_hash,
                 "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST(compute_hashes_missing_file_throws) {
  bool threw = false;
  try {
    compute_hashes("/tmp/p2p_test_does_not_exist_9f3a7c");
  } catch (const std::exception&) {
    threw = true;
  }
  TEST_ASSERT(threw);
}

TEST_MAIN()
