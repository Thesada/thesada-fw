// thesada-fw - cert_policy.h
// Pure helpers around client cert/key handling. No mbedtls, so host
// unit-testable.
//
// SCOPE, precisely: this does NOT validate a certificate. The cryptographic
// work - mbedtls_x509_crt_parse, mbedtls_pk_parse_key, mbedtls_pk_check_pair -
// stays in MQTTClient.cpp and is NOT covered by this unit. What lives here is
// (1) a structural pre-flight that rejects obviously-malformed PEM before a
// parser is handed attacker-influenced bytes, and (2) CN extraction, which is
// display metadata for `cert info`, not a security decision.
//
// certKeyInputsUsable is a DELIBERATE BEHAVIOUR CHANGE, not a refactor: the
// guard it fronts previously required only non-null and non-empty. It is
// strictly more rejecting. Both call sites already pass strlen()+1, so the
// parsers were PEM-only in practice.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <string.h>

#define PEM_CERT_BEGIN "-----BEGIN CERTIFICATE-----"
#define PEM_CERT_END   "-----END CERTIFICATE-----"

// Is there a well-ordered begin/end pair? End must follow begin, so a truncated
// or reversed blob is rejected before the parser sees it.
inline bool pemHasBlock(const char* s, const char* begin, const char* end) {
  if (!s || !begin || !end) return false;
  const char* b = strstr(s, begin);
  if (!b) return false;
  return strstr(b + strlen(begin), end) != nullptr;
}

inline bool pemLooksLikeCert(const char* s) {
  return pemHasBlock(s, PEM_CERT_BEGIN, PEM_CERT_END);
}

// The four private-key headers mbedtls accepts: PKCS#8, encrypted PKCS#8,
// PKCS#1 RSA and SEC1 EC. A PUBLIC KEY block deliberately does not match.
inline bool pemLooksLikeKey(const char* s) {
  static const char* const kBegins[] = {
      "-----BEGIN PRIVATE KEY-----",
      "-----BEGIN ENCRYPTED PRIVATE KEY-----",
      "-----BEGIN RSA PRIVATE KEY-----",
      "-----BEGIN EC PRIVATE KEY-----"};
  static const char* const kEnds[] = {
      "-----END PRIVATE KEY-----",
      "-----END ENCRYPTED PRIVATE KEY-----",
      "-----END RSA PRIVATE KEY-----",
      "-----END EC PRIVATE KEY-----"};
  for (size_t i = 0; i < sizeof(kBegins) / sizeof(kBegins[0]); i++) {
    if (pemHasBlock(s, kBegins[i], kEnds[i])) return true;
  }
  return false;
}

// Structural gate before mbedtls. Passing this does NOT mean the key matches
// the cert - that is mbedtls_pk_check_pair's job and is not tested here.
inline bool certKeyInputsUsable(const char* cert, const char* key) {
  if (!cert || !key || !*cert || !*key) return false;
  return pemLooksLikeCert(cert) && pemLooksLikeKey(key);
}

// Pull CN out of an mbedtls DN string ("C=CA, O=Thesada, CN=owb-debug").
//
// Returns true whenever a "CN=" marker was found, INCLUDING an empty value -
// the caller's "(no CN)" fallback must fire only when no marker exists, which
// is what the inline code this replaced did.
//
// Two weaknesses preserved from that inline code, both pinned by tests: strstr
// matches "CN=" anywhere, so an earlier field VALUE containing it wins; and the
// value is cut at the first comma, so an escaped comma truncates. out is always
// terminated.
inline bool certExtractCn(const char* dn, char* out, size_t outLen) {
  if (!out || outLen == 0) return false;
  out[0] = '\0';
  if (!dn) return false;
  const char* p = strstr(dn, "CN=");
  if (!p) return false;
  p += 3;
  size_t i = 0;
  while (*p && *p != ',' && i < outLen - 1) out[i++] = *p++;
  out[i] = '\0';
  return true;
}
