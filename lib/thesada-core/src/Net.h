// thesada-fw - Net.h
// Transport-abstraction hook for the net.* shell commands. thesada-core
// owns net.ip / net.ping / net.ntp / net.mqtt / net.http but must not
// depend on the optional cellular module. The cellular module registers a
// CellularProvider in its begin(); net.* commands consult it when WiFi is
// down so they keep working on the cellular leg. Mirrors the
// MQTTClient::setPublishForwarder pattern.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <functional>
#include <cstddef>

namespace Net {

// Function-pointer table the cellular module fills in. Any member may be
// null; callers must null-check before use. Members are plain function
// pointers (not std::function) so the cellular module can wire them with
// captureless lambdas and the table stays trivially copyable.
struct CellularProvider {
  // True when the modem is registered AND the data context (CNACT) is up -
  // the precondition for DNS / NTP / HTTPS over the modem.
  bool (*linkUp)();

  // Emit one human-readable line per call describing the cellular link:
  // operator, modem IP, signal, IMEI. Best-effort - emits whatever it can
  // read. Only meaningful when linkUp() is true.
  void (*linkInfo)(std::function<void(const char*)> emit);

  // Resolve host to a dotted-quad string via the modem DNS (AT+CDNSGIP).
  // Returns true and writes a NUL-terminated address into out on success.
  bool (*resolve)(const char* host, char* out, size_t outLen);

  // Sync the ESP32 system clock via modem NTP (AT+CNTP + AT+CCLK). Returns
  // true once settimeofday() has been called with a valid epoch.
  bool (*ntpSync)(const char* server, uint32_t timeoutMs);

  // HTTPS GET via the modem SSL socket. Mirrors Cellular::httpsGet: the
  // callback receives body chunks, httpStatus gets the wire status code.
  bool (*httpsGet)(const char* host, const char* path, uint16_t port,
                   std::function<bool(const uint8_t*, size_t)> onBody,
                   int* httpStatus);
};

// Register the provider. Called once from the cellular module's begin().
void setCellularProvider(const CellularProvider& provider);

// Returns the registered provider, or nullptr when no cellular module is
// compiled in / it has not registered yet.
const CellularProvider* cellular();

}  // namespace Net
