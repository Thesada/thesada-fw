// thesada-fw - TlsClient.cpp
// SPDX-License-Identifier: GPL-3.0-only

#include "TlsClient.h"
#include "Log.h"
#include <mbedtls/net_sockets.h>
#include <mbedtls/error.h>
#include <esp_task_wdt.h>

static const char* TAG = "TLS";

// Both directions block in bounded loops; the task watchdog is fed on
// every retry so a slow modem link does not trip it.
static constexpr uint32_t WRITE_TIMEOUT_MS = 10000;

TlsClient::TlsClient() {}

TlsClient::~TlsClient() {
  stop();
}

void TlsClient::setTransport(Client* transport) {
  if (transport != _transport) stop();
  _transport = transport;
}

void TlsClient::setCACert(const char* pem) {
  _caPem = pem;
  if (pem) _insecure = false;
}

void TlsClient::setInsecure() {
  _caPem    = nullptr;
  _insecure = true;
}

void TlsClient::setCertificate(const char* pem) { _certPem = pem; }
void TlsClient::setPrivateKey(const char* pem)  { _keyPem  = pem; }

// ---------------------------------------------------------------------------

int TlsClient::bioSend(void* ctx, const unsigned char* buf, size_t len) {
  Client* t = static_cast<Client*>(ctx);
  if (!t->connected()) return MBEDTLS_ERR_NET_CONN_RESET;
  size_t n = t->write(buf, len);
  return n > 0 ? (int)n : MBEDTLS_ERR_SSL_WANT_WRITE;
}

int TlsClient::bioRecv(void* ctx, unsigned char* buf, size_t len) {
  Client* t = static_cast<Client*>(ctx);
  int a = t->available();
  if (a <= 0) return t->connected() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_CONN_RESET;
  int n = t->read(buf, len < (size_t)a ? len : (size_t)a);
  return n > 0 ? n : MBEDTLS_ERR_SSL_WANT_READ;
}

// ---------------------------------------------------------------------------

bool TlsClient::setupContexts(const char* hostname) {
  mbedtls_ssl_init(&_ssl);
  mbedtls_ssl_config_init(&_conf);
  mbedtls_x509_crt_init(&_caChain);
  mbedtls_x509_crt_init(&_ownCert);
  mbedtls_pk_init(&_ownKey);
  mbedtls_ctr_drbg_init(&_drbg);
  mbedtls_entropy_init(&_entropy);
  _ctxLive = true;

  int rc = mbedtls_ctr_drbg_seed(&_drbg, mbedtls_entropy_func, &_entropy,
                                 (const unsigned char*)"thesada-tls", 11);
  if (rc == 0) {
    rc = mbedtls_ssl_config_defaults(&_conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
  }
  if (rc != 0) { _lastError = rc; return false; }
  mbedtls_ssl_conf_rng(&_conf, mbedtls_ctr_drbg_random, &_drbg);

  if (_caPem) {
    rc = mbedtls_x509_crt_parse(&_caChain, (const unsigned char*)_caPem, strlen(_caPem) + 1);
    if (rc < 0) { _lastError = rc; return false; }
    mbedtls_ssl_conf_ca_chain(&_conf, &_caChain, nullptr);
    mbedtls_ssl_conf_authmode(&_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  } else {
    mbedtls_ssl_conf_authmode(&_conf, MBEDTLS_SSL_VERIFY_NONE);
  }

  if (_certPem && _keyPem) {
    rc = mbedtls_x509_crt_parse(&_ownCert, (const unsigned char*)_certPem, strlen(_certPem) + 1);
    if (rc == 0) {
#if MBEDTLS_VERSION_MAJOR >= 3
      rc = mbedtls_pk_parse_key(&_ownKey, (const unsigned char*)_keyPem, strlen(_keyPem) + 1,
                                nullptr, 0, mbedtls_ctr_drbg_random, &_drbg);
#else
      rc = mbedtls_pk_parse_key(&_ownKey, (const unsigned char*)_keyPem, strlen(_keyPem) + 1,
                                nullptr, 0);
#endif
    }
    if (rc == 0) rc = mbedtls_ssl_conf_own_cert(&_conf, &_ownCert, &_ownKey);
    if (rc != 0) { _lastError = rc; return false; }
  }

  rc = mbedtls_ssl_setup(&_ssl, &_conf);
  if (rc == 0 && hostname && *hostname) rc = mbedtls_ssl_set_hostname(&_ssl, hostname);
  if (rc != 0) { _lastError = rc; return false; }
  mbedtls_ssl_set_bio(&_ssl, _transport, bioSend, bioRecv, nullptr);
  return true;
}

void TlsClient::freeContexts() {
  if (!_ctxLive) return;
  mbedtls_ssl_free(&_ssl);
  mbedtls_ssl_config_free(&_conf);
  mbedtls_x509_crt_free(&_caChain);
  mbedtls_x509_crt_free(&_ownCert);
  mbedtls_pk_free(&_ownKey);
  mbedtls_ctr_drbg_free(&_drbg);
  mbedtls_entropy_free(&_entropy);
  _ctxLive = false;
}

// ---------------------------------------------------------------------------

int TlsClient::openSession(const char* hostname, bool dialByName, const char* host,
                           IPAddress ip, uint16_t port) {
  stop();
  _lastError   = 0;
  _verifyFlags = 0;
  _verified    = false;

  if (!_transport) { _lastError = -1; return 0; }
  // Without a bundle a session is only allowed as an explicit opt-in, and
  // a check without a name proves nothing about who answered.
  if (!_caPem && !_insecure) {
    Log::kvfe(TAG, "tls.refused reason=no_ca host=%s", hostname ? hostname : "");
    _lastError = -1;
    return 0;
  }
  if (_caPem && (!hostname || !*hostname)) {
    Log::kvfe(TAG, "tls.refused reason=no_hostname");
    _lastError = -1;
    return 0;
  }

  if (!setupContexts(hostname)) {
    char err[64];
    mbedtls_strerror(_lastError, err, sizeof(err));
    Log::kvfe(TAG, "tls.setup_failed rc=-0x%04x err=\"%s\"", -_lastError, err);
    freeContexts();
    return 0;
  }

  esp_task_wdt_reset();
  int dialed = dialByName ? _transport->connect(host, port) : _transport->connect(ip, port);
  esp_task_wdt_reset();
  if (!dialed) {
    Log::kvfw(TAG, "tls.dial_failed host=%s port=%u", hostname ? hostname : host, (unsigned)port);
    _lastError = MBEDTLS_ERR_NET_CONNECT_FAILED;
    freeContexts();
    return 0;
  }

  uint32_t t0 = millis();
  int rc;
  do {
    rc = mbedtls_ssl_handshake(&_ssl);
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      esp_task_wdt_reset();
      delay(5);
    }
  } while ((rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) &&
           millis() - t0 < _handshakeTimeoutMs);

  _verifyFlags = mbedtls_ssl_get_verify_result(&_ssl);
  if (rc != 0) {
    _lastError = rc;
    char err[64];
    mbedtls_strerror(rc, err, sizeof(err));
    char vinfo[96] = "";
    if (_caPem && _verifyFlags && _verifyFlags != (uint32_t)-1) {
      mbedtls_x509_crt_verify_info(vinfo, sizeof(vinfo), "", _verifyFlags);
      for (char* p = vinfo; *p; ++p) if (*p == '\n') *p = ' ';
    }
    Log::kvfe(TAG, "tls.handshake_failed host=%s rc=-0x%04x ms=%lu verify=0x%08lx err=\"%s\" cause=\"%s\"",
              hostname ? hostname : "", -rc, (unsigned long)(millis() - t0),
              (unsigned long)_verifyFlags, err, vinfo);
    _transport->stop();
    freeContexts();
    return 0;
  }

  _connected = true;
  _verified  = _caPem != nullptr;
  Log::kvf(TAG, "tls.handshake_ok host=%s ms=%lu verified=%d mtls=%d cipher=%s",
           hostname ? hostname : "", (unsigned long)(millis() - t0), _verified ? 1 : 0,
           (_certPem && _keyPem) ? 1 : 0, mbedtls_ssl_get_ciphersuite(&_ssl));
  return 1;
}

int TlsClient::connect(const char* host, uint16_t port) {
  return openSession(host, true, host, IPAddress(), port);
}

int TlsClient::connect(IPAddress ip, uint16_t port, const char* hostname) {
  return openSession(hostname, false, nullptr, ip, port);
}

int TlsClient::connect(IPAddress ip, uint16_t port) {
  return openSession(nullptr, false, nullptr, ip, port);
}

// ---------------------------------------------------------------------------

void TlsClient::markClosed() {
  _connected = false;
  _peek      = -1;
}

size_t TlsClient::write(uint8_t b) {
  return write(&b, 1);
}

size_t TlsClient::write(const uint8_t* buf, size_t size) {
  if (!_connected) return 0;
  size_t   done = 0;
  uint32_t t0   = millis();
  while (done < size) {
    int rc = mbedtls_ssl_write(&_ssl, buf + done, size - done);
    if (rc > 0) { done += rc; continue; }
    if (rc != MBEDTLS_ERR_SSL_WANT_WRITE && rc != MBEDTLS_ERR_SSL_WANT_READ) {
      _lastError = rc;
      markClosed();
      break;
    }
    if (millis() - t0 > WRITE_TIMEOUT_MS) break;
    esp_task_wdt_reset();
    delay(2);
  }
  return done;
}

// Pulls one decrypted byte into _peek when records are waiting on the
// transport, so available() can answer without the caller reading.
bool TlsClient::fillPeek() {
  if (_peek >= 0) return true;
  if (_transport->available() <= 0) return false;
  unsigned char c;
  int rc = mbedtls_ssl_read(&_ssl, &c, 1);
  if (rc == 1) { _peek = c; return true; }
  if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return false;
  if (rc != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY && rc != 0) _lastError = rc;
  markClosed();
  return false;
}

int TlsClient::available() {
  if (!_connected) return 0;
  size_t n = mbedtls_ssl_get_bytes_avail(&_ssl);
  if (_peek >= 0) return (int)n + 1;
  if (n > 0) return (int)n;
  return fillPeek() ? 1 : 0;
}

int TlsClient::read() {
  uint8_t c;
  return read(&c, 1) == 1 ? c : -1;
}

int TlsClient::read(uint8_t* buf, size_t size) {
  if (size == 0) return 0;
  size_t got = 0;
  if (_peek >= 0) {
    buf[got++] = (uint8_t)_peek;
    _peek = -1;
  }
  if (!_connected || got == size) return got ? (int)got : -1;
  if (mbedtls_ssl_get_bytes_avail(&_ssl) == 0 && _transport->available() <= 0) {
    return got ? (int)got : -1;
  }
  int rc = mbedtls_ssl_read(&_ssl, buf + got, size - got);
  if (rc > 0) return (int)got + rc;
  if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
    if (rc != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY && rc != 0) _lastError = rc;
    markClosed();
  }
  return got ? (int)got : -1;
}

int TlsClient::peek() {
  if (_peek < 0 && _connected && mbedtls_ssl_get_bytes_avail(&_ssl) > 0) {
    unsigned char c;
    if (mbedtls_ssl_read(&_ssl, &c, 1) == 1) _peek = c;
  }
  if (_peek < 0 && _connected) fillPeek();
  return _peek;
}

uint8_t TlsClient::connected() {
  if (_peek >= 0) return 1;
  if (_connected && mbedtls_ssl_get_bytes_avail(&_ssl) > 0) return 1;
  if (_connected && !_transport->connected()) markClosed();
  return _connected ? 1 : 0;
}

void TlsClient::stop() {
  if (_connected) mbedtls_ssl_close_notify(&_ssl);
  markClosed();
  if (_ctxLive && _transport) _transport->stop();
  freeContexts();
  _verified = false;
}
