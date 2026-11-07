// thesada-fw - SecretJson.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "SecretJson.h"
#include "secret_heap.h"
#include <mbedtls/platform_util.h>

void secretWipe(void* p, size_t n) noexcept { mbedtls_platform_zeroize(p, n); }

namespace {
class SecretJsonAllocator : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override { return secretAlloc(size); }
  void deallocate(void* ptr) override { secretFree(ptr); }
  void* reallocate(void* ptr, size_t size) override { return secretRealloc(ptr, size); }
};
}  // namespace

ArduinoJson::Allocator* secretJsonAllocator() {
  static SecretJsonAllocator a;
  return &a;
}
