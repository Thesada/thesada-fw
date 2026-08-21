// thesada-fw - Identity.h
// First-boot device identity in the "thesada-ident" NVS namespace: a stable
// device_id derived from the base MAC, plus an Ed25519 keypair.
// Deliberately its OWN namespace - Secret's field map cannot address these
// keys, so no cli secret.* command can read or overwrite them.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "device_identity_policy.h"
#include <stddef.h>
#include <stdint.h>

class Identity {
 public:
  // Load what NVS holds, minting on first boot when this build can. Reading
  // needs no libsodium, so it is compiled into every image, rescue included.
  // Safe to call repeatedly.
  // out: true when a usable identity is in NVS afterwards.
  static bool begin();

  // Stable device id, e.g. "thesada-dcb4d91acd28". Empty string until begin()
  // has succeeded.
  static const char* deviceId();

  // Public key as lowercase hex. Empty string until begin() has succeeded.
  static const char* publicKeyHex();

  // Is an mTLS client cert present? Delegates to MQTTClient::hasClientCert.
  // identity.info and chip.info report it as factory-provisioned: the cert is
  // the only durable trace a pairing leaves.
  static bool hasClientCert();

  // Sign len bytes of msg with the device key. sig must hold 64 bytes. The
  // private key is loaded, used and zeroed inside this call - it never sits in
  // a long-lived buffer. Always false where canMint() is false.
  // out: true on success.
  static bool sign(const uint8_t* msg, size_t len, uint8_t* sig);

  // Wipe identity from NVS. Used by the re-pair path, not by normal operation.
  static bool erase();

  // The name that must be unique per unit: config device.name when set,
  // otherwise the generated device_id. Never the shared literal default -
  // two units answering to one MQTT clientId evict each other.
  static const char* nodeName();

  // Can this build create or use a keypair? Rescue images drop the ~97 KB of
  // libsodium, so they read the stored id and key but never mint or sign.
  static bool canMint();
};
