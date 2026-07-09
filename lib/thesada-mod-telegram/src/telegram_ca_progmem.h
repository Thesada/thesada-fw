// thesada-fw - PROGMEM CA root for Telegram Bot API TLS validation.
// SPDX-License-Identifier: GPL-3.0-only
//
// The Telegram module previously called WiFiClientSecure::setInsecure(),
// so every Bot API request was sent over unverified TLS. The bot token
// sits in the request URL path, so a MITM could lift it and impersonate
// the device on Telegram - suppressing real alerts or sending false
// "all clear" messages. This baked root closes that hole.
//
// api.telegram.org's leaf certificate chains to:
//   leaf  -> Go Daddy Secure Certificate Authority - G2  (intermediate)
//         -> Go Daddy Root Certificate Authority - G2    (root, below)
// The server sends the full chain, so trusting the root alone validates
// it. The leaf rotates roughly yearly; the root is valid until 2037, so
// no maintenance is needed across routine leaf renewals.
//
// /telegram-ca.crt in LittleFS overrides this PROGMEM copy, so a CA
// change (Telegram switching issuer) can be handled with a cli/fs.write
// push instead of a reflash - same override pattern as OTA's /ca.crt.
// Cost: ~1.4 KB in the firmware binary (cert is in .rodata, not RAM).
//
// Go Daddy Root Certificate Authority - G2
//   SHA256: 45140B3247EB9CC8C5B4F0D7B53091F73292089E6E5A63E2749DD3ACA9198EDA
//   Valid:  2009-09-01 .. 2037-12-31
#pragma once

#include <Arduino.h>

// Bundled as one raw string. R"PEM(...)PEM" preserves PEM newlines
// verbatim. PROGMEM keeps it in flash .rodata - String(FPSTR(...))
// copies it to RAM once at module begin().
static const char TELEGRAM_CA_PROGMEM[] PROGMEM = R"PEM(-----BEGIN CERTIFICATE-----
MIIDxTCCAq2gAwIBAgIBADANBgkqhkiG9w0BAQsFADCBgzELMAkGA1UEBhMCVVMx
EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoT
EUdvRGFkZHkuY29tLCBJbmMuMTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRp
ZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5MDkwMTAwMDAwMFoXDTM3MTIzMTIz
NTk1OVowgYMxCzAJBgNVBAYTAlVTMRAwDgYDVQQIEwdBcml6b25hMRMwEQYDVQQH
EwpTY290dHNkYWxlMRowGAYDVQQKExFHb0RhZGR5LmNvbSwgSW5jLjExMC8GA1UE
AxMoR28gRGFkZHkgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIw
DQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAL9xYgjx+lk09xvJGKP3gElY6SKD
E6bFIEMBO4Tx5oVJnyfq9oQbTqC023CYxzIBsQU+B07u9PpPL1kwIuerGVZr4oAH
/PMWdYA5UXvl+TW2dE6pjYIT5LY/qQOD+qK+ihVqf94Lw7YZFAXK6sOoBJQ7Rnwy
DfMAZiLIjWltNowRGLfTshxgtDj6AozO091GB94KPutdfMh8+7ArU6SSYmlRJQVh
GkSBjCypQ5Yj36w6gZoOKcUcqeldHraenjAKOc7xiID7S13MMuyFYkMlNAJWJwGR
tDtwKj9useiciAF9n9T521NtYJ2/LOdYq7hfRvzOxBsDPAnrSTFcaUaz4EcCAwEA
AaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYE
FDqahQcQZyi27/a9BUFuIMGU2g/eMA0GCSqGSIb3DQEBCwUAA4IBAQCZ21151fmX
WWcDYfF+OwYxdS2hII5PZYe096acvNjpL9DbWu7PdIxztDhC2gV7+AJ1uP2lsdeu
9tfeE8tTEH6KRtGX+rcuKxGrkLAngPnon1rpN5+r5N9ss4UXnT3ZJE95kTXWXwTr
gIOrmgIttRD02JDHBHNA7XIloKmf7J6raBKZV8aPEjoJpL1E/QYVN8Gb5DKj7Tjo
2GTzLH4U/ALqn83/B2gX2yKQOC16jdFU8WnjXzPKej17CuPKf1855eJ1usV2GDPO
LPAvTK33sefOT6jEm0pUBsV/fdUID+Ic/n4XuKxe9tQWskMJDE32p2u0mYRlynqI
4uJEvlz36hz1
-----END CERTIFICATE-----
)PEM";
