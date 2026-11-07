// thesada-fw - secret_heap.h
// Heap copies of key material that are wiped before they go back to the heap.
// secretWipe is the one wipe primitive: mbedtls_platform_zeroize on the board
// (SecretJson.cpp), a recording double on the host. No Arduino deps.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <cstddef>
#include <string>

// Must not be elided: a memset on a block about to be freed may be.
void secretWipe(void* p, size_t n) noexcept;

// Each block carries its capacity in a header, so a free wipes all of it
// without asking the heap. Keeps malloc's alignment.
constexpr size_t SECRET_BLOCK_HDR = alignof(std::max_align_t);
static_assert(SECRET_BLOCK_HDR >= sizeof(size_t), "header holds the capacity");

// in: bytes. out: block, or nullptr on overflow or a failed malloc.
inline void* secretAlloc(size_t n) {
  if (n > SIZE_MAX - SECRET_BLOCK_HDR) return nullptr;
  unsigned char* b = static_cast<unsigned char*>(malloc(SECRET_BLOCK_HDR + n));
  if (!b) return nullptr;
  memcpy(b, &n, sizeof(n));
  return b + SECRET_BLOCK_HDR;
}

// in: a block from secretAlloc. out: its capacity.
inline size_t secretCapacity(const void* p) {
  size_t n = 0;
  memcpy(&n, static_cast<const unsigned char*>(p) - SECRET_BLOCK_HDR, sizeof(n));
  return n;
}

// Wipes header and capacity, then frees. in: block or nullptr.
inline void secretFree(void* p) {
  if (!p) return;
  unsigned char* b = static_cast<unsigned char*>(p) - SECRET_BLOCK_HDR;
  secretWipe(b, SECRET_BLOCK_HDR + secretCapacity(p));
  free(b);
}

// A grow copies, so the old block is wiped before it is freed. A shrink stays
// in place and wipes the tail, so it never fails.
// in: block or nullptr, new size. out: block, or nullptr with p untouched.
inline void* secretRealloc(void* p, size_t n) {
  if (!p) return secretAlloc(n);
  const size_t cap = secretCapacity(p);
  if (n <= cap) {
    secretWipe(static_cast<unsigned char*>(p) + n, cap - n);
    return p;
  }
  void* q = secretAlloc(n);
  if (!q) return nullptr;
  memcpy(q, p, cap);
  secretFree(p);
  return q;
}

// A heap copy of a payload that may carry a key, wiped when destroyed.
// Copyable for std::function: every copy wipes its own bytes.
class SecretString {
 public:
  SecretString(const char* p, size_t n) : s_(p ? p : "", p ? n : 0) {}
  SecretString(const SecretString&) = default;
  SecretString(SecretString&&) noexcept = default;
  SecretString& operator=(const SecretString&) = delete;
  SecretString& operator=(SecretString&&) = delete;
  ~SecretString() {
    if (!s_.empty()) secretWipe(&s_[0], s_.size());
  }

  const char* c_str() const { return s_.c_str(); }
  size_t size() const { return s_.size(); }
  bool empty() const { return s_.empty(); }

 private:
  std::string s_;
};
