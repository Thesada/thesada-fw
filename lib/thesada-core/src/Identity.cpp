// thesada-fw - Identity.cpp
// SPDX-License-Identifier: GPL-3.0-only
//
// Raw IDF nvs API for the same reason Secret.cpp uses it: an absent namespace
// is the normal first-boot path, and Preferences ERROR-logs every miss under
// an un-maskable tag.
#include "Identity.h"
#include "Config.h"
#include "Log.h"
#include "MQTTClient.h"
#include <thesada_config.h>
#include <string.h>

#include <nvs.h>
#include <esp_system.h>

#ifdef ENABLE_IDENTITY
#include <sodium.h>
#include <mbedtls/platform_util.h>
#endif

static const char* TAG = "Identity";

// Shared across units by construction, so it is only ever reached when NVS
// holds no identity AND device.name is unset.
static const char* FALLBACK_NAME = "thesada-node";

static char _deviceId[IDENTITY_ID_CAP]       = {0};
static char _pubHex[IDENTITY_PUBKEY_HEX_CAP] = {0};
static bool _ready                           = false;

// --- read path: no libsodium, compiled into every image ----------------------

// 15 chars is the NVS namespace limit, so this cannot grow.
static const char* IDENT_NS = "thesada-ident";
static const char* K_ID     = "device_id";
static const char* K_PK     = "ed25519_pk";

const char* Identity::deviceId()     { return _deviceId; }
const char* Identity::publicKeyHex() { return _pubHex; }

bool Identity::hasClientCert() { return MQTTClient::hasClientCert(); }

// config device.name when set, else the generated id. Never the shared
// literal when an id exists - two units on one MQTT clientId evict each other.
const char* Identity::nodeName() {
  JsonObject  cfg  = Config::get();
  const char* name = cfg["device"]["name"] | "";
  if (name && *name) return name;
  if (_deviceId[0])  return _deviceId;
  return FALLBACK_NAME;
}

// out: true only when id and public key are both present and the id still has
// the shape this firmware generates.
static bool loadExisting() {
  nvs_handle_t h;
  if (nvs_open(IDENT_NS, NVS_READONLY, &h) != ESP_OK) return false;

  size_t    idLen = sizeof(_deviceId);
  esp_err_t e     = nvs_get_str(h, K_ID, _deviceId, &idLen);
  if (e != ESP_OK || !identityDeviceIdValid(_deviceId)) {
    nvs_close(h);
    _deviceId[0] = '\0';
    return false;
  }

  uint8_t pk[IDENTITY_PUBKEY_LEN];
  size_t  pkLen = sizeof(pk);
  e = nvs_get_blob(h, K_PK, pk, &pkLen);
  nvs_close(h);
  if (e != ESP_OK || pkLen != IDENTITY_PUBKEY_LEN ||
      !identityKeyMaterialPresent(pk, pkLen)) {
    _deviceId[0] = '\0';
    return false;
  }
  return identityHexEncode(pk, pkLen, _pubHex, sizeof(_pubHex));
}

bool Identity::erase() {
  nvs_handle_t h;
  if (nvs_open(IDENT_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  nvs_erase_all(h);
  esp_err_t e = nvs_commit(h);
  nvs_close(h);
  _deviceId[0] = '\0';
  _pubHex[0]   = '\0';
  _ready       = false;
  return e == ESP_OK;
}

#ifdef ENABLE_IDENTITY

// Only the minting half touches the private key.
static const char* K_SK = "ed25519_sk";

// Writes secret key first and the id last. loadExisting() gates on the id, so
// a write interrupted midway reads back as absent and regenerates cleanly.
static bool generate() {
  uint8_t mac[IDENTITY_MAC_LEN] = {0};
  if (esp_efuse_mac_get_default(mac) != ESP_OK) {
    Log::warn(TAG, "identity.mac_read_failed");
    return false;
  }
  char id[IDENTITY_ID_CAP];
  if (!identityDeviceIdFromMac(mac, id, sizeof(id))) return false;

  if (sodium_init() < 0) {
    Log::warn(TAG, "identity.sodium_init_failed");
    return false;
  }

  uint8_t pk[IDENTITY_PUBKEY_LEN];
  uint8_t sk[crypto_sign_ed25519_SECRETKEYBYTES];
  if (crypto_sign_ed25519_keypair(pk, sk) != 0) {
    mbedtls_platform_zeroize(sk, sizeof(sk));
    Log::warn(TAG, "identity.keygen_failed");
    return false;
  }

  nvs_handle_t h;
  if (nvs_open(IDENT_NS, NVS_READWRITE, &h) != ESP_OK) {
    mbedtls_platform_zeroize(sk, sizeof(sk));
    return false;
  }
  esp_err_t e = nvs_set_blob(h, K_SK, sk, sizeof(sk));
  mbedtls_platform_zeroize(sk, sizeof(sk));
  if (e == ESP_OK) e = nvs_set_blob(h, K_PK, pk, sizeof(pk));
  if (e == ESP_OK) e = nvs_set_str(h, K_ID, id);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  if (e != ESP_OK) {
    Log::kvfw(TAG, "identity.persist_failed err=%d", (int)e);
    return false;
  }

  strncpy(_deviceId, id, sizeof(_deviceId) - 1);
  _deviceId[sizeof(_deviceId) - 1] = '\0';
  return identityHexEncode(pk, sizeof(pk), _pubHex, sizeof(_pubHex));
}

bool Identity::canMint() { return true; }

// The private key is loaded, used and zeroed inside this call - it never sits
// in a long-lived buffer.
bool Identity::sign(const uint8_t* msg, size_t len, uint8_t* sig) {
  if (!msg || !sig || !_ready) return false;
  if (sodium_init() < 0) return false;

  nvs_handle_t h;
  if (nvs_open(IDENT_NS, NVS_READONLY, &h) != ESP_OK) return false;
  uint8_t   sk[crypto_sign_ed25519_SECRETKEYBYTES];
  size_t    skLen = sizeof(sk);
  esp_err_t e     = nvs_get_blob(h, K_SK, sk, &skLen);
  nvs_close(h);
  if (e != ESP_OK || skLen != sizeof(sk) ||
      !identityKeyMaterialPresent(sk, skLen)) {
    mbedtls_platform_zeroize(sk, sizeof(sk));
    return false;
  }

  int rc = crypto_sign_ed25519_detached(sig, nullptr, msg, len, sk);
  mbedtls_platform_zeroize(sk, sizeof(sk));
  return rc == 0;
}

#else  // !ENABLE_IDENTITY - minting only. Reading above stays compiled in.

bool Identity::canMint() { return false; }
bool Identity::sign(const uint8_t*, size_t, uint8_t*) { return false; }

// Nothing to mint with, so an absent identity stays absent.
static bool generate() { return false; }

#endif

bool Identity::begin() {
  if (_ready) return true;
  if (loadExisting()) {
    _ready = true;
    Log::kvf(TAG, "identity.loaded device_id=%s", _deviceId);
    return true;
  }
  if (!generate()) {
    if (!Identity::canMint()) Log::warn(TAG, "identity.mint_unavailable");
    return false;
  }
  _ready = true;
  Log::kvf(TAG, "identity.generated device_id=%s", _deviceId);
  return true;
}
