# Security Policy

## Reporting a Vulnerability

Report security issues privately - please do not open a public issue.

**Preferred:** GitHub private vulnerability reporting. Open the **Security** tab of this repository and click **"Report a vulnerability"** (Security -> Advisories -> Report a vulnerability). The report stays private and threaded with the maintainer.

**Alternative:** email **info@hit-con.ca** with a description, steps to reproduce, and relevant version or board information.

Response target: acknowledgement within 5 business days.

## Coordinated Disclosure

This project follows a **90-day coordinated disclosure** policy. After reporting:

1. I will confirm receipt and assess severity.
2. We agree on a fix timeline (typically within 90 days).
3. A fix is released before public disclosure.

If a fix cannot land within 90 days I will communicate the reason and negotiate a reasonable extension.

## Scope

**In scope:**

- Firmware code in this repository (src/, lib/, data/)
- Authentication and authorization logic (admin pairing, captive portal, web dashboard)
- MQTT TLS handling and certificate loading
- OTA update mechanism
- Any logic that handles external input over WiFi, MQTT, or serial

**Out of scope:**

- Vulnerabilities in upstream libraries (PlatformIO packages, Arduino framework, Espressif IDF) - report these to their maintainers
- Physical access attacks (this is hobbyist sensor hardware deployed on private property)
- The self-hosted MQTT broker or supporting infrastructure - out of scope for this repo

## Threat Model (honest summary)

This is hobbyist property-monitoring firmware running on ESP32 hardware. The deployment is self-hosted on a private network. There is no multi-tenant user base, no payment handling, and no sensitive personal data beyond the owner's own sensor readings.

The primary realistic risks are:

- A malicious actor on the same local network hijacking MQTT or the captive portal
- A malicious actor on the same local network sniffing or hijacking the plaintext HTTP admin interface (see below)
- A compromised OTA update being accepted without signature verification (no firmware signing is currently implemented)
- Credential leakage via the config interface if the admin pairing is handled carelessly

Public exploits against this codebase are unlikely to have widespread impact, but fixes will still be taken seriously and addressed.

### Plaintext HTTP admin interface (port 80)

The optional web dashboard (`thesada-mod-httpserver`) speaks plain HTTP.
There is no TLS on port 80, so **everything the browser exchanges with the
device is readable and modifiable by anyone on the same network path**:

- Basic Auth credentials (`web.user` / `web.password`) on every
  authenticated request
- Bearer tokens (1 h TTL) issued by `/api/login`
- The full `config.json` via `/api/config` - secrets provisioned into NVS
  via `secret.set` are NOT in the file, but any credentials still stored
  inline in a legacy `config.json` (e.g. WiFi PSKs) transit in the clear
- Live shell input/output over the `/ws/serial` WebSocket

A LAN attacker who captures one authenticated exchange has the admin
password (or a token valid for up to an hour) and with it config write,
file I/O, and shell execution.

What limits the exposure today:

- The module is **off by default** - it only runs with `web.enabled: true`
- Default or empty `web.password` refuses the entire authenticated
  surface, so a fresh device never serves admin over the open default
- Rate limiting (5 fails, 30 s lockout) and one-time WebSocket tokens
- Provisioning secrets with `secret.set` keeps them out of `/api/config`
  responses

Deployment guidance: treat web-UI trust as network trust. Enable the
dashboard only on an isolated / trusted network segment (IoT VLAN with no
untrusted peers), or leave it disabled and administer over the MQTT CLI,
which runs on verified TLS with optional mTLS client identity.

Why not HTTPS: ESPAsyncWebServer has no maintained TLS support, and each
mbedtls session needs ~30 KB of contiguous heap - the MQTT TLS connection
already competes for that on this hardware (see the heap watchdog in
MQTTClient). If an
encrypted local admin channel becomes a hard requirement, the realistic
path is migrating the dashboard to `esp_https_server`; until then the
limitation stands documented here.

## Supported Versions

Only the latest release on the `main` branch is supported. No backport patches are planned.
