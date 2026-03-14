# base

Full Thesada firmware base. Copy and adapt for each node deployment.

## Quick Start

1. Edit `config.h` to enable required modules
2. Edit `data/config.json` with runtime values
3. `pio run -e esp32dev` — compile
4. `pio run -e esp32dev --target upload` — flash via USB
5. `pio run -e esp32dev --target uploadfs` — upload LittleFS (config + scripts)
6. Subsequent updates via OTA through web UI
