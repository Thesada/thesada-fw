// thesada-fw - SecretJson.h
// JsonDocument allocator for documents that may hold key material: every
// block is wiped before it goes back to the heap (secret_heap.h).
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>

// out: process-wide allocator, for JsonDocument doc(secretJsonAllocator()).
ArduinoJson::Allocator* secretJsonAllocator();
