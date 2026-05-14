// thesada-fw - compile-time module enables
// Uncomment a define to include the corresponding module in the build.
// Runtime values (pins, intervals, thresholds) live in data/config.json.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#define FIRMWARE_VERSION "1.4.6"  // bump on each release

// ── Memory tuning defaults ──────────────────────────────────────────────────
#define LOG_RING_SIZE       50    // log replay lines for WS terminal
#define LOG_LINE_LEN        220   // max chars per log line
#define MQTT_QUEUE_SIZE     8     // offline publish queue depth
#define MQTT_RX_RING_SIZE   8     // debug RX topic ring

// Lua GC cadence. On low-alloc MCU workloads Lua's incremental GC is
// starved and dead short strings (MQTT payloads, JSON-decoded fields)
// accumulate until the system heap fragments and mbedtls can no longer
// find a contiguous TLS buffer. Periodic full collect prevents this.
// Set to 0 to disable. Override per-board below.
#define LUA_GC_INTERVAL_MS  30000

// ── Sensor modules ──────────────────────────────────────────────────────────
#define ENABLE_TEMPERATURE   // DS18B20 one-wire temperature sensors
// #define ENABLE_SHT31      // SHT31 I2C temperature + humidity sensor
#define ENABLE_ADS1115       // ADS1115 differential current sensing
#define ENABLE_BATTERY       // AXP2101 battery monitoring (via PowerManager)
#define ENABLE_PMU           // AXP2101 power management (charging, LED, voltage rails)
// #define ENABLE_PWM        // PWM output (e.g. fan control)
#define ENABLE_SD            // SD card data logger (SD_MMC 1-bit)

// ── Connectivity modules ─────────────────────────────────────────────────────
#define ENABLE_CELLULAR      // SIM7080G LTE-M/NB-IoT fallback
// #define ENABLE_GNSS       // SIM7080G built-in GNSS receiver (requires ENABLE_CELLULAR)
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
#if defined(ENABLE_GNSS) && !defined(ENABLE_CELLULAR)
  #error "ENABLE_GNSS requires ENABLE_CELLULAR (shares the SIM7080 modem core)"
#endif
#if defined(ENABLE_WEBSERVER) && defined(ENABLE_LITESERVER)
  #error "ENABLE_WEBSERVER and ENABLE_LITESERVER are mutually exclusive"
#endif

// ── Board pinout ─────────────────────────────────────────────────────────────
// Board is set via platformio.ini build_flags (-DBOARD_S3_BARE for bare S3
// devkit, otherwise defaults to LILYGO T-SIM7080G-S3).
#ifndef BOARD_S3_BARE
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
  // ENABLE_PMU stays - AXP2101 must be initialized or board rejects VBUS
#endif

// ── MQTT transport ───────────────────────────────────────────────────────────
// Port is read from config.json mqtt.port (default 8883).
// MQTT_TLS controls whether WiFiClientSecure is compiled in.
// Set to false only for local unencrypted testing brokers.
#define MQTT_TLS  true

// ── Phase 3 heap safeguards ───────────────────────────────────
// Preventive reboot when free heap stays below the floor for the hold
// window. Lands the device on a fresh, defragmented heap before mbedtls
// allocations start failing inside the TLS stack. Set FLOOR=0 to disable.
#ifndef HEAP_REBOOT_FLOOR_BYTES
  #define HEAP_REBOOT_FLOOR_BYTES   25000
#endif
#ifndef HEAP_REBOOT_HOLD_MS
  #define HEAP_REBOOT_HOLD_MS       60000
#endif

// Heap-aware skip in TelegramModule::sendTo. MQTT carries OTA so it has
// hard priority over Telegram. If free heap is below this floor we
// silently drop the Telegram POST instead of contending for the same
// TLS buffers as the MQTT client.
#ifndef TELEGRAM_HEAP_FLOOR_BYTES
  #define TELEGRAM_HEAP_FLOOR_BYTES 35000
#endif

// ── Build-time debug options ─────────────────────────────────────────────────
// Enabling these increases binary size and serial output.
// #define DEBUG_AT_COMMANDS  // dump raw AT traffic to Serial (TinyGSM)
// #define DEBUG_VERBOSE      // extra detail in WiFi scan, MQTT, sensor reads
