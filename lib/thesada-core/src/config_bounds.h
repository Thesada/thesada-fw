// thesada-fw - config_bounds.h
// The two bounds every config parse and writer holds. No deps.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>

// Largest /config.json any writer produces and load() parses, bytes.
static const size_t CONFIG_FILE_MAX = 16384;

// Nesting levels a config parse takes and config.set addresses.
#define CONFIG_MAX_DEPTH 8
