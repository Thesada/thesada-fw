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
- Authentication and authorization logic (admin pairing, captive portal)
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
- A compromised OTA update being accepted without signature verification (no firmware signing is currently implemented)
- Credential leakage via the config interface if the admin pairing is handled carelessly

Public exploits against this codebase are unlikely to have widespread impact, but fixes will still be taken seriously and addressed.

## Supported Versions

Only the latest release on the `main` branch is supported. No backport patches are planned.
