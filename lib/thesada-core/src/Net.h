// thesada-fw - Net.h
// Transport-abstraction hook for the net.* shell commands. thesada-core
// owns net.ip / net.ping / net.ntp / net.mqtt / net.http but must not
// depend on the optional cellular module. The cellular module registers a
// CellularProvider in its begin(); net.* commands consult it when WiFi is
// down so they keep working on the cellular leg, and MQTTClient takes its
// modem socket from here.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <Client.h>
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

  // Emit one human-readable line per call describing the cellular link.
  // Best-effort - emits whatever it can read.
  // in: output sink. out: none.
  void (*linkInfo)(std::function<void(const char*)> emit);

  // Resolve host to a dotted-quad string via the modem DNS (AT+CDNSGIP).
  // in: host, output buffer, buffer length. out: true on success.
  bool (*resolve)(const char* host, char* out, size_t outLen);

  // Sync the ESP32 system clock via modem NTP (AT+CNTP + AT+CCLK).
  // in: NTP server, timeout ms. out: true once settimeofday() has been called.
  bool (*ntpSync)(const char* server, uint32_t timeoutMs);

  // HTTPS GET over TLS on a modem socket; callback receives body chunks.
  // in: host, path, port, CA PEM (nullptr = insecure), body chunk callback,
  //     http status output. out: true on success; httpStatus = wire status.
  bool (*httpsGet)(const char* host, const char* path, uint16_t port,
                   const char* caPem,
                   std::function<bool(const uint8_t*, size_t)> onBody,
                   int* httpStatus);

  // Plain modem TCP socket for a TLS session to ride on. Slot 0 is the
  // MQTT session. in: slot. out: client, or nullptr for an unknown slot.
  Client* (*tcp)(uint8_t slot);
};

// Register the provider. Call once from the cellular module's begin().
// in: populated CellularProvider. out: none.
void setCellularProvider(const CellularProvider& provider);

// Returns the registered provider, or nullptr when no cellular module is
// compiled in or has not registered yet.
const CellularProvider* cellular();

}  // namespace Net
