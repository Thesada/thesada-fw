// thesada-fw - cmd_config_policy.h
// Accept/refuse predicate for a cmd/config payload. No Arduino or TLS deps,
// so the transport decision stays host-testable. The caller passes whether
// THIS broker session verified the server certificate. mqtt.allow_insecure
// is not a verified session.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>

// One byte under a 4 KB MQTT buffer, leaving room for a terminating NUL
// when the payload is copied out of the client callback.
#define CMD_CONFIG_MAX 4096

// Nesting past this is refused. A hostile document must not blow the stack
// inside the MQTT callback that runs this scan.
#define CMD_CONFIG_MAX_DEPTH 8

enum CmdConfigVerdict {
  CMD_CONFIG_OK = 0,
  CMD_CONFIG_REFUSE_TLS,
  CMD_CONFIG_REFUSE_SHAPE,
  CMD_CONFIG_REFUSE_BROKER,
};

struct CmdConfigScan {
  const char* s;
  size_t n;
  size_t i;
  int depth;
};

static inline bool cmdConfigWs(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static inline bool cmdConfigSkipWs(CmdConfigScan& p) {
  while (p.i < p.n && cmdConfigWs(p.s[p.i])) p.i++;
  return p.i < p.n;
}

static inline bool cmdConfigLiteral(CmdConfigScan& p, const char* lit) {
  for (size_t k = 0; lit[k]; k++) {
    if (p.i >= p.n || p.s[p.i] != lit[k]) return false;
    p.i++;
  }
  return true;
}

// in: p.i at the opening quote. out: true and p.i past the close; chars is
// the unescaped length when non-null.
static inline bool cmdConfigString(CmdConfigScan& p, size_t* chars) {
  if (p.i >= p.n || p.s[p.i] != '"') return false;
  p.i++;
  size_t nch = 0;
  while (p.i < p.n) {
    unsigned char c = (unsigned char)p.s[p.i++];
    if (c == '\\') {
      if (p.i >= p.n) return false;
      p.i++;
      nch++;
      continue;
    }
    if (c == '"') {
      if (chars) *chars = nch;
      return true;
    }
    if (c < 0x20) return false;
    nch++;
  }
  return false;
}

static inline bool cmdConfigKeyIs(const CmdConfigScan& p, size_t from, size_t to,
                                  const char* lit) {
  size_t k = 0;
  while (lit[k]) {
    if (from >= to || p.s[from] != lit[k]) return false;
    from++;
    k++;
  }
  return from == to;
}

static inline bool cmdConfigNumber(CmdConfigScan& p) {
  size_t start = p.i;
  if (p.i < p.n && p.s[p.i] == '-') p.i++;
  if (p.i >= p.n || p.s[p.i] < '0' || p.s[p.i] > '9') return false;
  if (p.s[p.i] == '0') p.i++;
  else while (p.i < p.n && p.s[p.i] >= '0' && p.s[p.i] <= '9') p.i++;
  if (p.i < p.n && p.s[p.i] == '.') {
    p.i++;
    if (p.i >= p.n || p.s[p.i] < '0' || p.s[p.i] > '9') return false;
    while (p.i < p.n && p.s[p.i] >= '0' && p.s[p.i] <= '9') p.i++;
  }
  if (p.i < p.n && (p.s[p.i] == 'e' || p.s[p.i] == 'E')) {
    p.i++;
    if (p.i < p.n && (p.s[p.i] == '+' || p.s[p.i] == '-')) p.i++;
    if (p.i >= p.n || p.s[p.i] < '0' || p.s[p.i] > '9') return false;
    while (p.i < p.n && p.s[p.i] >= '0' && p.s[p.i] <= '9') p.i++;
  }
  return p.i > start;
}

static inline bool cmdConfigValue(CmdConfigScan& p);

// Depth is capped by CMD_CONFIG_MAX_DEPTH. The cycle is the JSON walk.
// NOLINTNEXTLINE(misc-no-recursion)
static inline bool cmdConfigContainer(CmdConfigScan& p, char open, char close) {
  if (++p.depth > CMD_CONFIG_MAX_DEPTH) return false;
  p.i++;
  if (!cmdConfigSkipWs(p)) return false;
  if (p.s[p.i] == close) {
    p.i++;
    p.depth--;
    return true;
  }
  for (;;) {
    if (open == '{') {
      if (p.s[p.i] != '"') return false;
      if (!cmdConfigString(p, nullptr)) return false;
      if (!cmdConfigSkipWs(p) || p.s[p.i] != ':') return false;
      p.i++;
    }
    if (!cmdConfigValue(p)) return false;
    if (!cmdConfigSkipWs(p)) return false;
    if (p.s[p.i] == ',') {
      p.i++;
      if (!cmdConfigSkipWs(p)) return false;
      continue;
    }
    if (p.s[p.i] == close) {
      p.i++;
      p.depth--;
      return true;
    }
    return false;
  }
}

// NOLINTNEXTLINE(misc-no-recursion)
static inline bool cmdConfigValue(CmdConfigScan& p) {
  if (!cmdConfigSkipWs(p)) return false;
  char c = p.s[p.i];
  if (c == '"') return cmdConfigString(p, nullptr);
  if (c == '{') return cmdConfigContainer(p, '{', '}');
  if (c == '[') return cmdConfigContainer(p, '[', ']');
  if (c == 't') return cmdConfigLiteral(p, "true");
  if (c == 'f') return cmdConfigLiteral(p, "false");
  if (c == 'n') return cmdConfigLiteral(p, "null");
  return cmdConfigNumber(p);
}

// in: p.i at '{'. out: 1 nonempty broker string, 0 absent or empty, -1 bad.
static inline int cmdConfigMqttObject(CmdConfigScan& p) {
  if (++p.depth > CMD_CONFIG_MAX_DEPTH) return -1;
  p.i++;
  if (!cmdConfigSkipWs(p)) return -1;
  if (p.s[p.i] == '}') {
    p.i++;
    p.depth--;
    return 0;
  }
  int found = 0;
  bool saw = false;
  for (;;) {
    if (p.s[p.i] != '"') return -1;
    size_t ks = p.i + 1;
    if (!cmdConfigString(p, nullptr)) return -1;
    size_t ke = p.i - 1;
    bool isBroker = cmdConfigKeyIs(p, ks, ke, "broker");
    if (!cmdConfigSkipWs(p) || p.s[p.i] != ':') return -1;
    p.i++;
    if (isBroker) {
      if (saw) return -1;
      saw = true;
      if (!cmdConfigSkipWs(p) || p.s[p.i] != '"') return -1;
      size_t chars = 0;
      if (!cmdConfigString(p, &chars)) return -1;
      found = chars > 0 ? 1 : 0;
    } else if (!cmdConfigValue(p)) {
      return -1;
    }
    if (!cmdConfigSkipWs(p)) return -1;
    if (p.s[p.i] == ',') {
      p.i++;
      if (!cmdConfigSkipWs(p)) return -1;
      continue;
    }
    if (p.s[p.i] == '}') {
      p.i++;
      p.depth--;
      return found;
    }
    return -1;
  }
}

// in: full payload. out: 1 if the last-checked top-level mqtt object has a
// nonempty broker string, 0 if that key is missing or empty, -1 if the
// document is not one JSON object. A second top-level "mqtt" is refused.
static inline int cmdConfigScanBroker(CmdConfigScan& p) {
  if (!cmdConfigSkipWs(p) || p.s[p.i] != '{') return -1;
  p.depth = 1;
  p.i++;
  if (!cmdConfigSkipWs(p)) return -1;
  if (p.s[p.i] == '}') {
    p.i++;
    cmdConfigSkipWs(p);
    return p.i == p.n ? 0 : -1;
  }
  int broker = 0;
  bool sawMqtt = false;
  for (;;) {
    if (p.s[p.i] != '"') return -1;
    size_t ks = p.i + 1;
    if (!cmdConfigString(p, nullptr)) return -1;
    size_t ke = p.i - 1;
    bool isMqtt = cmdConfigKeyIs(p, ks, ke, "mqtt");
    if (!cmdConfigSkipWs(p) || p.s[p.i] != ':') return -1;
    p.i++;
    if (isMqtt) {
      if (sawMqtt) return -1;
      sawMqtt = true;
      if (!cmdConfigSkipWs(p) || p.s[p.i] != '{') return -1;
      int b = cmdConfigMqttObject(p);
      if (b < 0) return -1;
      broker = b;
    } else if (!cmdConfigValue(p)) {
      return -1;
    }
    if (!cmdConfigSkipWs(p)) return -1;
    if (p.s[p.i] == ',') {
      p.i++;
      if (!cmdConfigSkipWs(p)) return -1;
      continue;
    }
    if (p.s[p.i] == '}') {
      p.i++;
      cmdConfigSkipWs(p);
      if (p.i != p.n) return -1;
      return broker;
    }
    return -1;
  }
}

// May this payload be applied? in: tlsVerified (CA checked on the session
// that delivered this message, not merely encrypted), payload, byte length.
// out: OK only for one JSON object with a non-empty mqtt.broker, under the
// size cap, on a verified TLS session.
inline CmdConfigVerdict cmdConfigVerdict(bool tlsVerified, const char* payload, size_t len) {
  if (!payload || len == 0 || len >= CMD_CONFIG_MAX) return CMD_CONFIG_REFUSE_SHAPE;
  if (!tlsVerified) return CMD_CONFIG_REFUSE_TLS;
  CmdConfigScan scan = {payload, len, 0, 0};
  int broker = cmdConfigScanBroker(scan);
  if (broker < 0) return CMD_CONFIG_REFUSE_SHAPE;
  if (broker == 0) return CMD_CONFIG_REFUSE_BROKER;
  return CMD_CONFIG_OK;
}
