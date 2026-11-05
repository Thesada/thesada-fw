// thesada-fw - TlsClient.h
// mbedTLS client over any Arduino Client byte stream. One TLS stack for
// every transport (WiFi socket, modem TCP socket), so certificate chain,
// hostname check and client-cert auth behave the same on all of them.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <Client.h>
#include <IPAddress.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>

class TlsClient : public Client {
public:
  TlsClient();
  ~TlsClient() override;

  // Lower byte stream the TLS records travel over. Not owned. Changing it
  // drops any open session.
  // in: transport client, or nullptr. out: none.
  void setTransport(Client* transport);
  Client* transport() const { return _transport; }

  // Trust anchors as a PEM bundle. The pointer is kept, not copied, and
  // must stay valid while the client is in use. nullptr clears it.
  // in: PEM string. out: none.
  void setCACert(const char* pem);

  // Encrypt without checking the server. Only for an explicit opt-in
  // (allow_insecure). Cleared by setCACert with a non-null bundle.
  void setInsecure();

  // Client certificate and key for mutual TLS. Pointers are kept, both
  // or neither. nullptr clears.
  // in: PEM strings. out: none.
  void setCertificate(const char* pem);
  void setPrivateKey(const char* pem);

  // Upper bound for TCP dial plus handshake. Default 20 s.
  void setHandshakeTimeout(uint32_t ms) { _handshakeTimeoutMs = ms; }

  // Dial host:port on the transport and run the handshake. The host name
  // is sent as SNI and checked against the server certificate.
  // in: host, port. out: 1 on an established session, 0 otherwise.
  int connect(const char* host, uint16_t port) override;

  // Dial an address already resolved, verify against hostname.
  // in: ip, port, hostname (required unless insecure). out: 1 / 0.
  int connect(IPAddress ip, uint16_t port, const char* hostname);

  // Refused without a hostname: there is nothing to verify against.
  int connect(IPAddress ip, uint16_t port) override;

  size_t  write(uint8_t b) override;
  size_t  write(const uint8_t* buf, size_t size) override;
  int     available() override;
  int     read() override;
  int     read(uint8_t* buf, size_t size) override;
  int     peek() override;
  void    flush() override {}
  void    stop() override;
  uint8_t connected() override;
  operator bool() override { return connected(); }

  // True when the last handshake checked chain and name.
  bool verified() const { return _verified; }

  // mbedTLS error and verify flags of the last failure (0 when none).
  int      lastError() const { return _lastError; }
  uint32_t verifyFlags() const { return _verifyFlags; }

private:
  int  openSession(const char* hostname, bool dialByName, const char* host,
                   IPAddress ip, uint16_t port);
  bool setupContexts(const char* hostname);
  void freeContexts();
  bool fillPeek();
  void markClosed();

  static int bioSend(void* ctx, const unsigned char* buf, size_t len);
  static int bioRecv(void* ctx, unsigned char* buf, size_t len);

  Client*      _transport = nullptr;
  const char*  _caPem     = nullptr;
  const char*  _certPem   = nullptr;
  const char*  _keyPem    = nullptr;
  bool         _insecure  = false;
  uint32_t     _handshakeTimeoutMs = 20000;

  bool         _ctxLive   = false;
  bool         _connected = false;
  bool         _verified  = false;
  int          _peek      = -1;
  int          _lastError = 0;
  uint32_t     _verifyFlags = 0;

  mbedtls_ssl_context      _ssl;
  mbedtls_ssl_config       _conf;
  mbedtls_x509_crt         _caChain;
  mbedtls_x509_crt         _ownCert;
  mbedtls_pk_context       _ownKey;
  mbedtls_ctr_drbg_context _drbg;
  mbedtls_entropy_context  _entropy;
};
