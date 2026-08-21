// thesada-fw - cli_authz_policy.h
// Pure decision logic for whether an inbound MQTT CLI command may run.
// Host-unit-testable. See docs/invariants.md for the policy and why.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <string.h>

// How the current broker session authenticated.
enum CliAuthMode { CLI_AUTH_PASSWORD = 0, CLI_AUTH_MTLS = 1 };

// The one config key a password-auth session may write. Leaving broker_url
// off is the point - it is the repoint-to-another-broker path.
#define CLI_AUTHZ_PW_CONFIG_KEY "mqtt.port"

// Is the first whitespace-delimited token of payload exactly key?
// in: payload, key. out: true on an exact token match.
inline bool cliAuthzFirstTokenIs(const char* payload, const char* key) {
  if (!payload || !key) return false;
  while (*payload == ' ' || *payload == '\t') payload++;
  size_t klen = strlen(key);
  if (klen == 0) return false;
  if (strncmp(payload, key, klen) != 0) return false;
  // The key must end at a separator: "mqtt.portx" must not pass as "mqtt.port".
  char after = payload[klen];
  return after == '\0' || after == ' '  || after == '\t' ||
         after == '\n' || after == '\r';
}

// Commands a password-auth session may run: what pairing and recovery need,
// nothing that reads the filesystem, dumps config or runs code.
// in: cmd, storedCertBroken (stored client cert would not load or validate).
inline bool cliAuthzPasswordCmdAllowed(const char* cmd, bool storedCertBroken) {
  if (!cmd) return false;
  // A cert that will not load or validate is worthless, so clearing it costs
  // nothing and unstrands the device. No cert stored is not broken.
  if (storedCertBroken && strcmp(cmd, "cert.clear") == 0) return true;
  static const char* const kAllowed[] = {
    "cert.set", "cert.apply", "cert.info",  // provisioning
    "secret.set",                           // pair pushes NVS secrets here
    "restart",                              // pair + recovery both reboot
    "version", "chip.info", "heap",         // read-only, identifies the board
  };
  for (size_t i = 0; i < sizeof(kAllowed) / sizeof(kAllowed[0]); i++) {
    if (strcmp(cmd, kAllowed[i]) == 0) return true;
  }
  return false;
}

// Is the token after the key a plain decimal port in 1..65535? config.set
// stores a junk value verbatim, which strands the device at next reload.
inline bool cliAuthzPortValueOk(const char* payload, const char* key) {
  if (!cliAuthzFirstTokenIs(payload, key)) return false;
  while (*payload == ' ' || *payload == '\t') payload++;
  const char* v = payload + strlen(key);
  while (*v == ' ' || *v == '\t') v++;
  if (*v < '0' || *v > '9') return false;   // empty or non-numeric
  long port = 0;
  int digits = 0;
  while (*v >= '0' && *v <= '9') {
    port = port * 10 + (*v - '0');
    v++;
    if (++digits > 5) return false;         // cannot be a port, stop early
  }
  // Anything trailing the number is not a port: "8884}" must not pass.
  while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
  if (*v != '\0') return false;
  return port >= 1 && port <= 65535;
}

// Does this config.set carry a port value the device cannot come back from?
//
// Not an authorization question: "8884}" is stored verbatim and strands the
// device at the next reload whoever sent it. A bare key with no value is left
// alone - config.set answers with its usage line and writes nothing.
// in: payload after envelope unwrap. out: true when the write must be refused.
inline bool cliAuthzPortValueBad(const char* payload) {
  if (!cliAuthzFirstTokenIs(payload, CLI_AUTHZ_PW_CONFIG_KEY)) return false;
  const char* v = payload;
  while (*v == ' ' || *v == '\t') v++;
  v += strlen(CLI_AUTHZ_PW_CONFIG_KEY);
  while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
  if (!*v) return false;
  return !cliAuthzPortValueOk(payload, CLI_AUTHZ_PW_CONFIG_KEY);
}

// The gate. in: auth mode, command name sliced off the topic, payload after
// envelope unwrap, whether the stored client cert is broken. out: true to
// dispatch.
inline bool cliAuthzAllowed(CliAuthMode mode, const char* cmd,
                            const char* payload, bool storedCertBroken) {
  if (!cmd || !*cmd) return false;
  bool isConfigSet = strcmp(cmd, "config.set") == 0;
  // Runs on both paths. What each mode may WRITE stays asymmetric below.
  if (isConfigSet && cliAuthzPortValueBad(payload)) return false;
  if (mode == CLI_AUTH_MTLS) return true;
  if (isConfigSet) {
    return cliAuthzPortValueOk(payload, CLI_AUTHZ_PW_CONFIG_KEY);
  }
  return cliAuthzPasswordCmdAllowed(cmd, storedCertBroken);
}
