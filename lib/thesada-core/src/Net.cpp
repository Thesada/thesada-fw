// thesada-fw - Net.cpp
// See Net.h. Holds the single process-wide CellularProvider slot.
// SPDX-License-Identifier: GPL-3.0-only
#include "Net.h"

namespace Net {

static CellularProvider s_provider   = {};
static bool             s_registered = false;

// Store the cellular module's provider table.
// In:  provider - function-pointer table; copied by value.
// Out: subsequent cellular() calls return a pointer to the stored copy.
void setCellularProvider(const CellularProvider& provider) {
  s_provider   = provider;
  s_registered = true;
}

// Return the registered provider.
// In:  none
// Out: pointer to the provider, or nullptr when no cellular module is in
//      the build (or it has not called setCellularProvider yet).
const CellularProvider* cellular() {
  return s_registered ? &s_provider : nullptr;
}

}  // namespace Net
