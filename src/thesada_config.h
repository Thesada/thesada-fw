// thesada-fw - compile-time module enables
// Uncomment a define to include the corresponding module in the build.
// Runtime values (pins, intervals, thresholds) live in data/config.json.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#define FIRMWARE_VERSION "1.3.6"  // bump on each release

// ── Sensor modules ──────────────────────────────────────────────────────────
#define ENABLE_TEMPERATURE   // DS18B20 one-wire temperature sensors
// #define ENABLE_SHT31      // SHT31 I2C temperature + humidity sensor
#define ENABLE_ADS1115       // ADS1115 differential current sensing
#define ENABLE_BATTERY       // AXP2101 battery monitoring (via PowerManager)
#define ENABLE_PMU           // AXP2101 power management (charging, LED, voltage rails)
// #define ENABLE_PWM        // PWM output (e.g. fan control)
// #define ENABLE_DISPLAY    // SSD1306 OLED display (I2C, 128x64)
// #define ENABLE_TFT        // ILI9341 TFT + XPT2046 touch (CYD board)
#define ENABLE_SD            // SD card data logger (SD_MMC 1-bit)

// ── Connectivity modules ─────────────────────────────────────────────────────
#define ENABLE_CELLULAR      // SIM7080G LTE-M/NB-IoT fallback
// #define ENABLE_ETH        // LAN8720A Ethernet (WT32-ETH01 and similar)
// #define ENABLE_LORA       // LoRa/Meshtastic (future hardware)

// ── Notification / alerting ──────────────────────────────────────────────────
#define ENABLE_TELEGRAM      // Telegram Bot API (direct alerts + Lua Telegram.send)

// ── Optional core services ──────────────────────────────────────────────────
#define ENABLE_WEBSERVER     // Web UI, REST API, dashboard, OTA upload
// #define ENABLE_LITESERVER // Lightweight HTTP (OTA + config, for heap-limited boards)
#define ENABLE_SCRIPTENGINE  // Lua scripting (alerts.lua, rules.lua)

// ── Dependency warnings ──────────────────────────────────────────────────────
#if defined(ENABLE_SCRIPTENGINE) && !defined(ENABLE_TELEGRAM)
  #warning "Lua Telegram.send() unavailable: ENABLE_TELEGRAM is off"
#endif
#if defined(ENABLE_TELEGRAM) && !defined(ENABLE_SCRIPTENGINE)
  #warning "Lua alert scripts unavailable: ENABLE_SCRIPTENGINE is off"
#endif
#if defined(ENABLE_BATTERY) && !defined(ENABLE_PMU)
  #error "ENABLE_BATTERY requires ENABLE_PMU"
#endif
#if defined(ENABLE_WEBSERVER) && defined(ENABLE_LITESERVER)
  #error "ENABLE_WEBSERVER and ENABLE_LITESERVER are mutually exclusive"
#endif

// ── Board pinout ─────────────────────────────────────────────────────────────
// Board is set via platformio.ini build_flags (-DBOARD_WROOM32 etc).
// Default is LILYGO if no board flag is set.
#if !defined(BOARD_WROOM32) && !defined(BOARD_S3_BARE) && !defined(BOARD_ETH)
  #define BOARD_LILYGO_T_SIM7080_S3
#endif

// ── Board-specific module overrides ─────────────────────────────────────────
// Bare ESP32-S3 devkit (Freenove, etc) - no LILYGO hardware.
#ifdef BOARD_S3_BARE
  #undef ENABLE_CELLULAR
  #undef ENABLE_PMU
  #undef ENABLE_BATTERY
  #undef ENABLE_SD
  #undef ENABLE_ADS1115
  #undef ENABLE_TEMPERATURE
  #ifndef ENABLE_SHT31
    #define ENABLE_SHT31
  #endif
  #undef BOARD_LILYGO_T_SIM7080_S3
#endif

// WROOM32 has no cellular, PMU, battery, SD, or LILYGO-specific hardware.
#ifdef BOARD_WROOM32
  #undef ENABLE_CELLULAR
  #undef ENABLE_PMU
  #undef ENABLE_BATTERY
  #undef ENABLE_SD
  #undef ENABLE_TEMPERATURE
  #undef ENABLE_ADS1115
  #undef BOARD_LILYGO_T_SIM7080_S3
#endif

// WT32-ETH01 (or similar ESP32 + LAN8720A) - Ethernet primary, WiFi fallback.
// Sensors via remaining GPIOs (IO4=1-Wire, IO14/IO15=I2C).
#ifdef BOARD_ETH
  #undef ENABLE_CELLULAR
  #undef ENABLE_PMU
  #undef ENABLE_BATTERY
  #undef ENABLE_DISPLAY
  #undef ENABLE_TFT
  #ifndef ENABLE_ETH
    #define ENABLE_ETH
  #endif
  #undef BOARD_LILYGO_T_SIM7080_S3
#endif

// OWB rescue build - minimal firmware for remote unbricking via OTA on a
// weak link. Keeps PMU (mandatory for VBUS accept) + core (WiFi/MQTT/OTA).
// Strips every optional module to shrink the binary so it has a fighting
// chance to complete download before a readBytes() stall trips the TWDT.
// Inherits LILYGO_T_SIM7080_S3 pinout (no board flag set, default applies).
#ifdef BOARD_OWB_RESCUE
  #undef ENABLE_TEMPERATURE
  #undef ENABLE_SHT31
  #undef ENABLE_ADS1115
  #undef ENABLE_BATTERY
  #undef ENABLE_SD
  #undef ENABLE_CELLULAR
  #undef ENABLE_TELEGRAM
  #undef ENABLE_WEBSERVER
  #undef ENABLE_LITESERVER
  #undef ENABLE_SCRIPTENGINE
  #undef ENABLE_PWM
  #undef ENABLE_DISPLAY
  #undef ENABLE_TFT
  // ENABLE_PMU stays - AXP2101 must be initialized or board rejects VBUS
#endif

// CYD is a display-only node with TFT touch, RGB LED, LDR.
// LiteServer replaces full WebServer (not enough heap for AsyncTCP on WROOM-32).
#ifdef BOARD_CYD
  #undef ENABLE_CELLULAR
  #undef ENABLE_PMU
  #undef ENABLE_BATTERY
  #undef ENABLE_TEMPERATURE
  #undef ENABLE_ADS1115
  #undef ENABLE_DISPLAY
  #undef ENABLE_WEBSERVER
  #ifndef ENABLE_LITESERVER
    #define ENABLE_LITESERVER
  #endif
  // SD stays enabled - CYD has SPI SD slot (sd.mode: "spi", sd.pin_cs: 5)
  #undef BOARD_LILYGO_T_SIM7080_S3
  #ifndef ENABLE_TFT
    #define ENABLE_TFT
  #endif
#endif

// ── MQTT transport ───────────────────────────────────────────────────────────
// Port is read from config.json mqtt.port (default 8883).
// MQTT_TLS controls whether WiFiClientSecure is compiled in.
// Set to false only for local unencrypted testing brokers.
#define MQTT_TLS  true

// ── Build-time debug options ─────────────────────────────────────────────────
// Enabling these increases binary size and serial output.
// #define DEBUG_AT_COMMANDS  // dump raw AT traffic to Serial (TinyGSM)
// #define DEBUG_VERBOSE      // extra detail in WiFi scan, MQTT, sensor reads
