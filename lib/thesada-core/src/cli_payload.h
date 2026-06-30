// thesada-fw - cli_payload.h
// Pure parse for "<field>\n<value>" cli binary payloads (secret.set, cert.set).
// Splits on the first newline and rejects an over-long field rather than
// clipping its length - a clipped length makes the value length too large and
// reads past the payload end. memchr (not strchr) bounds the scan to plen, so a
// payload without a trailing NUL is safe. Host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <string.h>

enum class CliSplit { Ok, NoNewline, FieldTooLong };

// Split payload[0..plen) into field (copied into fieldOut, capacity fieldCap
// including the NUL) and value (a pointer into payload plus its length).
// Rejects a field that would not fit fieldCap. out: status.
inline CliSplit cliSplitFieldValue(const char* payload, size_t plen,
                                   char* fieldOut, size_t fieldCap,
                                   const char** valueOut, size_t* valueLenOut) {
  if (!payload || !fieldOut || fieldCap == 0 || !valueOut || !valueLenOut)
    return CliSplit::NoNewline;
  const char* nl = (const char*)memchr(payload, '\n', plen);
  if (!nl) return CliSplit::NoNewline;
  size_t fieldLen = (size_t)(nl - payload);
  if (fieldLen >= fieldCap) return CliSplit::FieldTooLong;
  memcpy(fieldOut, payload, fieldLen);
  fieldOut[fieldLen] = '\0';
  *valueOut = nl + 1;
  *valueLenOut = plen - fieldLen - 1;
  return CliSplit::Ok;
}
