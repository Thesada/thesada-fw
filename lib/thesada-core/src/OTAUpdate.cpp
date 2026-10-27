// thesada-fw - OTAUpdate.cpp
//
// Manifest format (JSON at a known URL):
//   {
//     "version": "1.x",
//     "url": "https://example.com/firmware.bin",
//     "sha256": "e3b0c44298fc1c149afbf4c8996fb924..."
//   }
//
// TLS: uses /ca.crt from LittleFS (same file as MQTTClient and Cellular).
// If /ca.crt is not present, falls back to setInsecure().
//
// SPDX-License-Identifier: GPL-3.0-only

#include "OTAUpdate.h"
#include "Config.h"
#include "WiFiManager.h"
#include "MQTTClient.h"
#include "SleepManager.h"
#include "Log.h"
#include "Shell.h"
#include "ota_ca_progmem.h"
#include <thesada_config.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <functional>
#include <mbedtls/sha256.h>
#include "ota_verify_policy.h"
#include "ota_health_policy.h"
#include <esp_ota_ops.h>
#include <esp_image_format.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <Preferences.h>

#ifdef ENABLE_CELLULAR
#include "Cellular.h"
#endif

static const char* TAG = "OTA";

// Survives a software reset, not a power-on. recordBootStreak reads and clears it.
RTC_NOINIT_ATTR static uint32_t _otaPlannedRestart;
// The rescue flag and pending state as initArduino saw them, before it confirms.
static uint8_t _otaBootRescue = 0;
static bool _otaBootPending = false;

// in: wanted state. out: true when the running image reads it; unreadable is false.
static bool runningImageIs(esp_ota_img_states_t want) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  return running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == want;
}

static_assert(OTA_IMG_PENDING_VERIFY == ESP_OTA_IMG_PENDING_VERIFY &&
                  OTA_IMG_VALID == ESP_OTA_IMG_VALID && OTA_IMG_UNDEFINED == ESP_OTA_IMG_UNDEFINED,
              "ota_health_policy.h image states drifted from esp_ota_img_states_t");

// Only a confirmed image may hold a planned restart; unknown counts.
// in: none. out: true when the running image is valid, or undefined after a USB flash.
static bool runningImageConfirmed() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
         otaHealthStateConfirmed((uint32_t)state);
}

// initArduino calls this before setup. Rescue confirms only after the flag
// is cleared: a flag left set would skip the health gate on the next OTA.
// in: none. out: false confirms in the core; true leaves the health gate.
extern "C" bool verifyRollbackLater() {
  // Unreadable NVS defers. Confirming would skip the health gate.
  if (nvs_flash_init() != ESP_OK) return true;
  nvs_handle_t handle;
  if (nvs_open("boot", NVS_READWRITE, &handle) != ESP_OK) return true;
  uint8_t stored = 0;
  nvs_get_u8(handle, "rescue", &stored);
  _otaBootRescue = otaHealthRescue(stored, _otaPlannedRestart);
  _otaBootPending = runningImageIs(ESP_OTA_IMG_PENDING_VERIFY);
  bool defer = true;
  if (stored &&
      nvs_set_u8(handle, "rescue", 0) == ESP_OK &&
      nvs_commit(handle) == ESP_OK) {
    defer = _otaBootRescue == 0;
  }
  nvs_close(handle);
  return defer;
}

static bool _otaMqttUp = false;
static bool _otaPending = false;
static bool _otaStateKnown = false;
static uint32_t _otaRunState = 0;  // running slot's otadata state, once _otaStateKnown
static bool _otaImageSettled = false;
static uint32_t _otaCrashStreak = 0;
static bool _otaStreakKnown = false;
static uint64_t _otaSettleFailMs = 0;  // uptime of the last failed clear or USB mark, 0 = none
// The running image's proven record is stored, or this boot already tried it.
static bool _otaProvenDone = false;

void OTAUpdate::noteMqttUp() { _otaMqttUp = true; }

void OTAUpdate::plannedRestart() {
  _otaPlannedRestart = OTA_PLANNED_RESTART_MAGIC;
  esp_restart();
}

void OTAUpdate::brokerRestart() {
  _otaPlannedRestart = OTA_PLANNED_BROKER_MAGIC;
  esp_restart();
}

// This slot and its ELF hash. in: id out, OTA_IMAGE_ID_LEN bytes.
// out: false when the running image or its hash cannot be read.
static bool runningImageId(uint8_t* id) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_app_desc_t desc;
  if (!running || esp_ota_get_partition_description(running, &desc) != ESP_OK) return false;
  return otaHealthImageId(running->address, desc.app_elf_sha256, id);
}

// Store this image's id on its first broker session, once per image.
// in: none. out: none. A failed write is tried again on the next boot.
static void markProvenOnBroker() {
  if (_otaProvenDone || !_otaMqttUp) return;
  _otaProvenDone = true;
  uint8_t id[OTA_IMAGE_ID_LEN];
  if (!runningImageId(id)) {
    Log::kvfe(TAG, "ota.image_proven_unrecorded reason=no_image_id");
    return;
  }
  Preferences prefs;
  if (!prefs.begin("boot", false)) {
    Log::kvfe(TAG, "ota.image_proven_unrecorded reason=nvs");
    return;
  }
  size_t wrote = prefs.putBytes("proven_id", id, sizeof(id));
  prefs.end();
  if (wrote != sizeof(id)) {
    Log::kvfe(TAG, "ota.image_proven_unrecorded reason=nvs_write");
    return;
  }
  Log::info(TAG, "ota.image_proven reason=mqtt_up");
}

// Zero the streak the switch reset adds to, then arm an immediate confirm of
// the slot about to boot. out: true when both landed; the switch needs both.
static bool armRescue() {
  Preferences prefs;
  if (!prefs.begin("boot", false)) {
    Log::kvfe(TAG, "ota.image_rollback_failed reason=nvs");
    return false;
  }
  size_t crash = prefs.putUInt("crash_n", 0);
  size_t rescue = crash ? prefs.putUChar("rescue", OTA_RESCUE_ID) : 0;
  prefs.end();
  if (crash != 0) _otaCrashStreak = 0;
  if (rescue == 0) {
    Log::kvfe(TAG, "ota.image_rollback_failed reason=nvs_write");
    return false;
  }
  return true;
}

// A flag left by a failed switch would confirm the next written image unchecked.
// in: none. out: none.
static void disarmRescue() {
  Preferences prefs;
  bool cleared = prefs.begin("boot", false) && prefs.putUChar("rescue", 0) != 0;
  prefs.end();
  if (!cleared) Log::kvfe(TAG, "ota.rescue_left_set reason=nvs_write");
}

// Survives a software reset and deep sleep, not a power-on: fled-skip seconds
// not yet saved to NVS, then their complement.
RTC_NOINIT_ATTR static uint32_t _otaFledUnsaved[2];

// The fled record and its skip age, read once a boot.
static struct {
  bool loaded;
  bool savedThisBoot;
  char ver[32];
  OtaFledAge saved;
  uint32_t carry;   // unsaved seconds an earlier boot left in RTC memory
  uint32_t markMs;  // millis at the last save this boot
} _fled = {};

static void putFledUnsaved(uint32_t s) {
  _otaFledUnsaved[0] = s;
  _otaFledUnsaved[1] = ~s;
}

// Written after the boot slot is set, so a failed switch leaves no skip. A
// lost write costs one more download of this release. in: none. out: none.
static void recordFledRelease() {
  Preferences prefs;
  // Age first: a lost version write then leaves the old release a fresh day.
  bool wrote = prefs.begin("boot", false) &&
               prefs.putULong64("fled_age", otaFledPack({0, 0, 0})) != 0 &&
               prefs.putString("fled_ver", FIRMWARE_VERSION) != 0;
  prefs.end();
  putFledUnsaved(0);
  if (wrote) Log::kvfw(TAG, "ota.fled_release_recorded version=%s", FIRMWARE_VERSION);
  else Log::kvfe(TAG, "ota.fled_release_unrecorded reason=nvs_write");
}

// An unread record skips nothing: it must not hold back every update. The
// RTC carry is taken once and zeroed. in: none. out: none.
static void loadFled() {
  if (_fled.loaded) return;
  _fled.loaded = true;
  _fled.carry = otaFledCarry(_otaFledUnsaved[0], _otaFledUnsaved[1]);
  putFledUnsaved(0);
  Preferences prefs;
  if (!prefs.begin("boot", true)) {
    Log::kvfw(TAG, "ota.fled_release_unread reason=nvs");
    return;
  }
  if (prefs.isKey("fled_ver")) prefs.getString("fled_ver", _fled.ver, sizeof(_fled.ver));
  if (prefs.isKey("fled_age")) _fled.saved = otaFledUnpack(prefs.getULong64("fled_age", 0));
  prefs.end();
}

// in: none. out: the clock as an epoch once SNTP synced it, else 0.
static uint32_t syncedEpoch() {
  return WiFiManager::ntpSynced() ? (uint32_t)time(nullptr) : 0u;
}

static uint32_t fledUnsavedS() { return _fled.carry + (millis() - _fled.markMs) / 1000u; }

// in: none. out: the saved age plus the time since that save.
static uint32_t fledAgeNow() {
  uint32_t elapsed = otaFledElapsed(fledUnsavedS(), !_fled.savedThisBoot,
                                    _fled.saved.seenEpoch, syncedEpoch());
  return otaFledAdvance(_fled.saved.ageS, elapsed);
}

// A failed write still moves the count on in RAM, so it is not retried every
// turn. in: age, installs since expiry. out: true when NVS took it.
static bool saveFled(uint32_t ageS, uint8_t tries) {
  const OtaFledAge next = {ageS, tries, syncedEpoch()};
  Preferences prefs;
  bool wrote = prefs.begin("boot", false) && prefs.putULong64("fled_age", otaFledPack(next)) != 0;
  prefs.end();
  if (!wrote) Log::kvfe(TAG, "ota.fled_age_unsaved reason=nvs_write");
  _fled.saved = next;
  _fled.savedThisBoot = true;
  _fled.carry = 0;
  _fled.markMs = millis();
  putFledUnsaved(0);
  return wrote;
}

// RTC memory holds the unsaved seconds each turn and NVS gets them every 10
// min, so a reset loses none and a power-on under 10 min. in: none. out: none.
static void ageFledRelease() {
  loadFled();
  if (!otaFledAges(_fled.ver, FIRMWARE_VERSION, _fled.saved.ageS)) return;
  uint32_t unsaved = fledUnsavedS();
  if (!otaFledSaveDue(unsaved, false)) {
    putFledUnsaved(unsaved);
    return;
  }
  saveFled(fledAgeNow(), _fled.saved.tries);
}

// An unreadable description or NVS must not look like a free slot.
// in: candidate partition. out: true if it is the image a switch came from.
static bool imageAbandoned(const esp_partition_t* part) {
  esp_app_desc_t desc;
  if (!part || esp_ota_get_partition_description(part, &desc) != ESP_OK) {
    Log::kvfe(TAG, "ota.image_rollback_failed reason=no_description");
    return true;
  }
  Preferences prefs;
  if (!prefs.begin("boot", true)) {
    Log::kvfe(TAG, "ota.image_rollback_failed reason=nvs");
    return true;
  }
  // left_ver/left_date/left_time are not read: they hold prebuilt IDF strings
  // that match every build. Kept for the older image in the other slot.
  uint8_t left[OTA_IMAGE_ID_LEN] = {0};
  size_t leftLen = prefs.isKey("left_id") ? prefs.getBytes("left_id", left, sizeof(left)) : 0;
  prefs.end();
  return otaHealthIsAbandoned(left, leftLen, part->address, desc.app_elf_sha256);
}

// otadata keeps a slot VALID while a download overwrites it, and its new
// descriptor reads early. in: candidate slot. out: true when the image verifies.
static bool imageVerifies(const esp_partition_t* part) {
  const esp_partition_pos_t pos = {part->address, part->size};
  esp_image_metadata_t meta;
  if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK) return true;
  Log::kvfe(TAG, "ota.image_rollback_failed reason=image_invalid");
  return false;
}

// The other slot is a target only when it is a different valid image that
// verifies and is not recorded as left. in: running. out: that slot, or null.
static const esp_partition_t* rollbackTarget(const esp_partition_t* running) {
  const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
  esp_ota_img_states_t otherState = ESP_OTA_IMG_UNDEFINED;
  if (!other || other == running ||
      esp_ota_get_state_partition(other, &otherState) != ESP_OK ||
      otherState != ESP_OTA_IMG_VALID || imageAbandoned(other) || !imageVerifies(other)) {
    return nullptr;
  }
  return other;
}

// in: two app descriptors. out: true when version, date and time all match.
static bool sameDescStrings(const esp_app_desc_t& a, const esp_app_desc_t& b) {
  return strncmp(a.version, b.version, sizeof(a.version)) == 0 &&
         strncmp(a.date, b.date, sizeof(a.date)) == 0 &&
         strncmp(a.time, b.time, sizeof(a.time)) == 0;
}

// Drop a record that names the running slot, then on a switch arrival record
// the slot it came from. in: open boot prefs. out: none.
static void updateLeftRecord(Preferences& prefs) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  uint8_t stored[OTA_IMAGE_ID_LEN] = {0};
  size_t len = prefs.isKey("left_id") ? prefs.getBytes("left_id", stored, sizeof(stored)) : 0;
  if (running && otaHealthLeftIsStale(stored, len, running->address)) {
    if (prefs.remove("left_id")) Log::info(TAG, "ota.left_image_cleared reason=slot_running");
    else Log::kvfe(TAG, "ota.left_image_kept reason=nvs_write");
  }
  if (!otaHealthIsArrival(_otaBootRescue, _otaBootPending)) return;
  const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
  esp_app_desc_t from, self;
  uint8_t id[OTA_IMAGE_ID_LEN];
  if (!running || !other || other == running ||
      esp_ota_get_partition_description(other, &from) != ESP_OK ||
      !otaHealthImageId(other->address, from.app_elf_sha256, id)) {
    Log::kvfe(TAG, "ota.left_image_unrecorded reason=no_image_id");
    return;
  }
  bool same = esp_ota_get_partition_description(running, &self) == ESP_OK &&
              sameDescStrings(from, self);
  if (!otaHealthRecordArrival(_otaBootRescue, same)) {
    Log::info(TAG, "ota.left_image_unrecorded reason=older_guard");
    return;
  }
  if (prefs.putBytes("left_id", id, sizeof(id)) != sizeof(id)) {
    Log::kvfe(TAG, "ota.left_image_unrecorded reason=nvs_write");
    return;
  }
  Log::kvf(TAG, "ota.left_image_recorded rescue=%u", (unsigned)_otaBootRescue);
}

uint32_t OTAUpdate::recordBootStreak(int resetReason, uint32_t* brownoutsOut) {
  OtaPlanned planned = otaHealthTakePlanned(&_otaPlannedRestart);
  // Only a confirmed image may hold a planned restart; unknown counts.
  bool confirmed = planned != OTA_PLANNED_NONE && runningImageConfirmed();
  uint8_t id[OTA_IMAGE_ID_LEN];
  bool haveId = runningImageId(id);
  Preferences prefs;
  if (!prefs.begin("boot", false)) {
    // Unread streak must not look like zero: confirm would keep a bad image.
    Log::kvfe(TAG, "ota.crash_streak_unread");
    _otaStreakKnown = false;
    _otaCrashStreak = OTA_HEALTH_CRASH_LIMIT;
    if (brownoutsOut) *brownoutsOut = 0;
    return _otaCrashStreak;
  }
  updateLeftRecord(prefs);
  uint32_t brownouts = prefs.getUInt("brownout_n", 0);
  if (resetReason == OTA_RST_BROWNOUT) {
    brownouts++;
    if (prefs.putUInt("brownout_n", brownouts) == 0) {
      prefs.end();
      Log::kvfe(TAG, "ota.crash_streak_unread reason=nvs_write");
      _otaStreakKnown = false;
      _otaCrashStreak = OTA_HEALTH_CRASH_LIMIT;
      if (brownoutsOut) *brownoutsOut = 0;
      return _otaCrashStreak;
    }
  }
  uint8_t stored[OTA_IMAGE_ID_LEN] = {0};
  size_t storedLen =
      prefs.isKey("proven_id") ? prefs.getBytes("proven_id", stored, sizeof(stored)) : 0;
  bool proven = haveId && otaHealthImageProven(stored, storedLen, id);
  _otaProvenDone = proven;
  OtaResetClass kind = otaHealthBootClass(resetReason, planned, confirmed, proven);
  uint32_t crashes = otaHealthNextCrashStreak(prefs.getUInt("crash_n", 0), kind);
  // A failed write must not look stored. Confirm would keep a bad image.
  if (prefs.putUInt("crash_n", crashes) == 0) {
    prefs.end();
    Log::kvfe(TAG, "ota.crash_streak_unread reason=nvs_write");
    _otaStreakKnown = false;
    _otaCrashStreak = OTA_HEALTH_CRASH_LIMIT;
    if (brownoutsOut) *brownoutsOut = 0;
    return _otaCrashStreak;
  }
  prefs.end();
  _otaStreakKnown = true;
  _otaCrashStreak = crashes;
  if (brownoutsOut) *brownoutsOut = brownouts;
  if (resetReason == OTA_RST_SW && confirmed && kind == OTA_RESET_HOLD) {
    Log::kvf(TAG, "ota.crash_streak_held reason=%s streak=%lu",
             planned == OTA_PLANNED_BROKER ? "broker_restart" : "planned_restart",
             (unsigned long)crashes);
  } else if (resetReason == OTA_RST_SW && confirmed && planned == OTA_PLANNED_BROKER) {
    Log::kvfw(TAG, "ota.crash_streak_counted reason=broker_restart_unproven streak=%lu",
              (unsigned long)crashes);
  }
  return crashes;
}

// A software reset counts as a crash, so the next image must not inherit
// a streak already at the limit. out: true when crash_n was stored as zero.
static bool clearCrashStreak() {
  Preferences prefs;
  if (!prefs.begin("boot", false)) {
    Log::kvfe(TAG, "ota.crash_streak_unread reason=nvs");
    return false;
  }
  size_t wrote = prefs.putUInt("crash_n", 0);
  prefs.end();
  if (wrote == 0) {
    Log::kvfe(TAG, "ota.crash_streak_unread reason=nvs_write");
    return false;
  }
  return true;
}

void OTAUpdate::rollbackIfCrashLoop() {
  if (!_otaStreakKnown || !otaHealthShouldRollback(_otaCrashStreak)) return;
  const unsigned long streak = _otaCrashStreak;
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) {
    Log::kvfe(TAG, "ota.image_rollback_failed reason=state_unknown streak=%lu",
              streak);
    return;
  }
  if (state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (!rollbackTarget(running)) {
      // Leaving it pending lets the bootloader boot the slot just refused.
      Log::kvfe(TAG, "ota.image_rollback_failed reason=no_other_image state=pending streak=%lu",
                streak);
      // Confirm even if the clear fails. This image has no safe other slot,
      // and leaving it pending lets the bootloader boot the one just refused.
      if (clearCrashStreak()) _otaCrashStreak = 0;
      if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
        Log::kvfe(TAG, "ota.image_rollback_failed reason=cancel_failed state=pending streak=%lu",
                  streak);
        return;
      }
      Log::kvfw(TAG, "ota.state_change from=pending to=valid reason=no_other_image streak=%lu",
                streak);
      return;
    }
    Log::kvfw(TAG, "ota.state_change from=pending to=previous reason=crash_streak streak=%lu",
              streak);
    if (!clearCrashStreak()) return;
    if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) {
      Log::kvfe(TAG, "ota.image_rollback_failed state=pending streak=%lu",
                streak);
    }
    return;
  }
  if (state != ESP_OTA_IMG_VALID) {
    Log::kvfe(TAG, "ota.image_rollback_failed reason=state_not_valid state=%d streak=%lu",
              (int)state, streak);
    return;
  }
  const esp_partition_t* other = rollbackTarget(running);
  if (!other) {
    Log::kvfe(TAG, "ota.image_rollback_failed reason=no_other_image streak=%lu",
              streak);
    return;
  }
  // The image booted next records this one by its id, so it needs one.
  uint8_t id[OTA_IMAGE_ID_LEN];
  if (!runningImageId(id)) {
    Log::kvfe(TAG, "ota.image_rollback_failed reason=no_image_id streak=%lu", streak);
    return;
  }
  if (!armRescue()) return;
  Log::kvfw(TAG, "ota.state_change from=valid to=other reason=crash_streak streak=%lu",
            streak);
  if (esp_ota_set_boot_partition(other) != ESP_OK) {
    Log::kvfe(TAG, "ota.image_rollback_failed state=valid streak=%lu",
              streak);
    disarmRescue();
    return;
  }
  recordFledRelease();
  ESP.restart();
}

// The IDF marks the active otadata entry, which is the staged image once an
// update is written, so this waits while the boot slot is not the running one.
// in: none. out: true when the running USB image now reads VALID.
static bool markUsbImageValid() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || esp_ota_get_boot_partition() != running) {
    Log::kvfw(TAG, "ota.image_mark_failed reason=boot_slot_changed");
    return false;
  }
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK) {
    Log::kvfe(TAG, "ota.image_mark_failed reason=usb_flash err=0x%x", (unsigned)err);
    return false;
  }
  Log::kvf(TAG, "ota.image_marked_valid reason=usb_flash");
  return true;
}

// Confirm a pending image once, and retry when the IDF call fails.
// An unread streak leaves a pending image pending.
// in: none. out: none. A non-pending image clears its streak after an hour up,
// and a USB-flashed one is then marked valid.
void OTAUpdate::confirmIfHealthy() {
  ageFledRelease();
  markProvenOnBroker();
  if (_otaImageSettled) return;
  if (!_otaStateKnown) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    _otaPending = state == ESP_OTA_IMG_PENDING_VERIFY;
    _otaRunState = (uint32_t)state;
    _otaStateKnown = true;
  }
  if (!_otaPending) {
    const bool usb = _otaRunState == ESP_OTA_IMG_UNDEFINED;
    if (!_otaStreakKnown || (_otaCrashStreak == 0 && !usb)) { _otaImageSettled = true; return; }
    const uint64_t upMs = (uint64_t)esp_timer_get_time() / 1000;
    if (_otaCrashStreak != 0) {
      if (!otaHealthShouldClearValid(false, upMs, _otaCrashStreak, _otaSettleFailMs)) return;
      if (!clearCrashStreak()) {
        _otaSettleFailMs = upMs;
        return;
      }
      Log::kvf(TAG, "ota.crash_streak_cleared reason=healthy_uptime streak=%lu",
               (unsigned long)_otaCrashStreak);
      _otaCrashStreak = 0;
    }
    if (usb) {
      if (!otaHealthShouldMarkUsbValid(_otaRunState, _otaStreakKnown, upMs, _otaSettleFailMs)) return;
      if (!markUsbImageValid()) {
        _otaSettleFailMs = upMs;
        return;
      }
    }
    _otaImageSettled = true;
    return;
  }
  if (!_otaStreakKnown) return;
  if (!otaHealthShouldMark(true, _otaMqttUp, millis(), _otaCrashStreak)) return;
  if (!clearCrashStreak()) return;
  if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
    Log::warn(TAG, "ota.image_confirm_failed");
    return;
  }
  _otaImageSettled = true;
  Log::info(TAG, "ota.state_change from=pending to=valid reason=healthy");
}

uint32_t OTAUpdate::_lastCheck       = 0;
uint32_t OTAUpdate::_checkIntervalMs = 21600000;  // 6 hours
bool     OTAUpdate::_enabled         = true;
bool     OTAUpdate::_checkRequested  = false;
bool     OTAUpdate::_forceRequested  = false;
String   OTAUpdate::_pendingManifestUrl;

static String _otaCaCert;
static bool   _otaCaCertLoaded = false;

static void loadCaCert() {
  if (_otaCaCertLoaded) return;
  _otaCaCertLoaded = true;
  if (LittleFS.exists("/ca.crt")) {
    File cf = LittleFS.open("/ca.crt", "r");
    if (cf) {
      _otaCaCert = cf.readString();
      cf.close();
      Log::info(TAG, "ota.ca_loaded path=/ca.crt");
    }
  }
  if (_otaCaCert.isEmpty()) {
    // Fallback to baked PROGMEM bundle. Prevents silent OTA disable when
    // LittleFS is fresh/wiped (firmware-only reflash without uploadfs).
    // FPSTR + assignment copies flash->RAM as a one-shot at boot; OK
    // because _otaCaCert is then reused for every subsequent fetch.
    _otaCaCert = String(FPSTR(OTA_CA_PROGMEM));
    if (!_otaCaCert.isEmpty()) {
      Log::warn(TAG, "ota.ca_fallback source=progmem");
    } else {
      Log::kvfw(TAG, "ota.tls_insecure reason=no_ca");
    }
  }
}

// Without this, every silent bailout looks identical to "device offline"
// from the broker side. publish() queues on the WiFi ring (or routes to
// the cellular forwarder) when the broker is down, so a refusal raised
// while disconnected still reaches the operator on reconnect - do not
// pre-empt that with a connected() guard here.
// in:  reason (short kebab-case identifier, e.g. "no-ca", "heap-low").
// out: none.
static void publishOtaRefusal(const char* reason) {
  JsonObject  cfg    = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char topic[MQTT_TOPIC_CAP];
  snprintf(topic, sizeof(topic), "%s/status/ota", prefix);
  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"state\":\"refused\",\"reason\":\"%s\",\"version\":\"%s\"}",
           reason, FIRMWARE_VERSION);
  MQTTClient::publish(topic, payload);
}

// Short socket + handshake timeouts so a flaky upstream cannot block
// readBytes() long enough to trip the task watchdog. WiFiClientSecure
// defaults to ~60s read timeout, far past the ~5s TWDT window.
//
// When /ca.crt is missing OTAUpdate::begin() refuses to enable OTA unless
// ota.allow_insecure is set in config.json, so by the time we get here we
// already know either the CA cert is loaded or the operator has opted in.
static void configureSecureClient(WiFiClientSecure& client) {
  loadCaCert();
  if (_otaCaCert.isEmpty()) {
    client.setInsecure();
  } else {
    client.setCACert(_otaCaCert.c_str());
  }
  client.setTimeout(10);            // arduino-esp32 v2.x: seconds (current platform)
  client.setHandshakeTimeout(10);   // ssl handshake, always seconds
}

// ---------------------------------------------------------------------------
// Transport-agnostic HTTP GET
//
// OTA fallback over cellular when WiFi is down. The two
// underlying paths are very different (Arduino HTTPClient/WiFiClientSecure
// over WiFi vs SIM7080G modem-native SSL socket via TinyGsmClientSecure),
// so we wrap both behind a single byte-stream callback API. Same /ca.crt
// validates the manifest origin on either path (public TLS roots covering
// the common backends - GitHub Releases, Let's Encrypt fronted origins,
// Sectigo, etc - are baked into the firmware via ota_ca_progmem.h).
// ---------------------------------------------------------------------------

// in:  url, writeCallback. out: outStatus (200/404/...), outLen (bytes).
static bool otaHttpGetWiFi(const char* url,
                           std::function<bool(const uint8_t*, size_t)> writeCallback,
                           int* outStatus, size_t* outLen) {
  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient       plainClient;
  bool https = strncmp(url, "https://", 8) == 0;
  if (https) {
    configureSecureClient(secureClient);
    http.begin(secureClient, url);
  } else {
    http.begin(plainClient, url);
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setConnectTimeout(10000);
  http.setTimeout(10000);

  yield(); esp_task_wdt_reset();
  int code = http.GET();
  yield(); esp_task_wdt_reset();
  if (outStatus) *outStatus = code;
  if (code != 200) {
    http.end();
    return code > 0;  // negative = transport error; positive = HTTP-level error (still "transport ok")
  }

  WiFiClient* stream = http.getStreamPtr();
  int contentLength  = http.getSize();
  size_t total = 0;
  uint8_t buf[1024];
  uint32_t deadline = millis() + 5UL * 60UL * 1000UL;  // 5 min cap
  while (millis() < deadline) {
    yield(); esp_task_wdt_reset();
    Shell::pumpConsole();
    if (!stream->connected() && !stream->available()) break;
    int avail = stream->available();
    if (avail <= 0) { delay(10); continue; }
    int n = stream->readBytes(buf, std::min((size_t)avail, sizeof(buf)));
    if (n <= 0) break;
    total += n;
    if (writeCallback && !writeCallback(buf, n)) {
      http.end();
      if (outLen) *outLen = total;
      return false;
    }
    if (contentLength > 0 && (int)total >= contentLength) break;
  }
  http.end();
  if (outLen) *outLen = total;
  return true;
}

#ifdef ENABLE_CELLULAR
// in:  url, writeCallback. out: outStatus, outLen.
static bool otaHttpGetCellular(const char* url,
                               std::function<bool(const uint8_t*, size_t)> writeCallback,
                               int* outStatus, size_t* outLen) {
  const char* p = url;
  uint16_t port = 443;
  if (strncmp(p, "https://", 8) == 0)      { p += 8; port = 443; }
  else if (strncmp(p, "http://",  7) == 0) { p += 7; port = 80;  }
  char host[128];
  const char* slash   = strchr(p, '/');
  const char* hostEnd = slash ? slash : p + strlen(p);
  size_t hostLen      = hostEnd - p;
  if (hostLen == 0 || hostLen >= sizeof(host)) return false;
  memcpy(host, p, hostLen);
  host[hostLen] = '\0';
  char* colon = strchr(host, ':');
  if (colon) { *colon = '\0'; port = atoi(colon + 1); }
  const char* path = slash ? slash : "/";

  size_t total = 0;
  auto wrap = [&](const uint8_t* buf, size_t len) -> bool {
    total += len;
    return writeCallback ? writeCallback(buf, len) : true;
  };
  bool ok = Cellular::httpsGet(host, path, port, wrap, outStatus);
  if (outLen) *outLen = total;
  return ok;
}
#endif

#ifdef ENABLE_CELLULAR
static bool parseUrl(const char* url, char* host, size_t hostCap,
                     const char** pathOut, uint16_t* portOut) {
  const char* p = url;
  uint16_t port = 443;
  if (strncmp(p, "https://", 8) == 0)      { p += 8; port = 443; }
  else if (strncmp(p, "http://",  7) == 0) { p += 7; port = 80;  }
  else return false;
  const char* slash   = strchr(p, '/');
  const char* hostEnd = slash ? slash : p + strlen(p);
  size_t hostLen      = hostEnd - p;
  if (hostLen == 0 || hostLen >= hostCap) return false;
  memcpy(host, p, hostLen);
  host[hostLen] = '\0';
  char* colon = strchr(host, ':');
  if (colon) { *colon = '\0'; port = atoi(colon + 1); }
  *pathOut = slash ? slash : "/";
  *portOut = port;
  return true;
}

// Download a binary in 64 KB chunks via HTTP Range requests. Each chunk
// is its own short-lived TLS session, which works around SIM7080G HTTP
// session degradation past ~500-900 KB (observed on fw 1951B17). The
// WiFi transport does not need this; HTTPClient handles 1+ MB downloads
// in one socket fine.
//
// expectedSize MUST be > 0 - it is the loop's termination condition.
// Comes from the manifest's `size` field; without it this function
// refuses to run and the caller falls back to a single GET.
static bool downloadBinaryChunkedCellular(
    const char* url, size_t expectedSize,
    std::function<bool(const uint8_t*, size_t)> writeCallback,
    size_t* totalOut) {
  if (totalOut) *totalOut = 0;
  if (expectedSize == 0) return false;

  char host[128];
  const char* path = nullptr;
  uint16_t port = 443;
  if (!parseUrl(url, host, sizeof(host), &path, &port)) {
    Log::error(TAG, "ota.download_failed reason=bad_url");
    return false;
  }

  // MQTT (SMCONN) runs inside the modem fw and competes with our HTTPS
  // chunks for the modem's TCP socket slots and the PDP context's
  // bandwidth, even though our ATGuard blocks our own SMPUB traffic.
  // Drop the modem-native MQTT session before the download so the chunks
  // get the full bandwidth budget. Cellular::loop() detects the dropped
  // session on its next tick after ATGuard releases and re-establishes
  // MQTT automatically (existing recovery path), so no explicit SMCONN
  // is needed here. On OTA success the device reboots and MQTT comes up
  // fresh on the new fw anyway.
  Cellular::atPassthrough("+SMDISC", 5000UL, [](const char*){});

  // 64 KB chunk size. At ~8.4 KB/s LTE-M that is ~8 s per chunk, well
  // under the empirically observed session-degradation threshold. For
  // a 1.48 MB binary this is ~23 round trips.
  static constexpr size_t   CHUNK             = 64UL * 1024UL;
  // Inter-chunk pacing: lets the modem's internal TCP slots clear
  // TIME_WAIT and the carrier's per-source-port rate-counter decay
  // before the next handshake. Bench-tuned on LTE-M; trimmable if a
  // future carrier handles back-to-back sockets cleanly.
  static constexpr uint32_t CHUNK_GAP_MS      = 5000UL;
  // Reset the PDP context every Nth chunk as a heavier escape hatch
  // for the same problem class (modem-internal state accumulation).
  // 8 chunks * 64 KB = 512 KB which is where the empirical stall
  // showed up most consistently.
  static constexpr int      CHUNK_BOUNCE_EVERY = 8;

  size_t offset    = 0;
  int    chunkIdx  = 0;
  while (offset < expectedSize) {
    size_t end = std::min(offset + CHUNK - 1, expectedSize - 1);
    int status = 0;
    bool ok = Cellular::httpsGet(host, path, port, writeCallback, &status,
                                 offset, end);
    if (!ok || (status != 206 && status != 200)) {
      Log::kvfe(TAG, "ota.chunk_failed offset=%u status=%d ok=%d",
                (unsigned)offset, status, (int)ok);
      return false;
    }
    offset = end + 1;
    chunkIdx++;
    if (totalOut) *totalOut = offset;

    if (offset >= expectedSize) break;

    // Periodic CNACT bounce. Deactivate + reactivate the PDP context
    // so the modem releases every TCP slot still in TIME_WAIT and the
    // operator-side flow counters reset. Cheap (~1-2 s) compared to
    // the recovery cost when a chunk eventually stalls without it.
    if (chunkIdx % CHUNK_BOUNCE_EVERY == 0) {
      Log::info(TAG, "ota.pdp_bounce method=cnact_cycle");
      Cellular::atPassthrough("+CNACT=0,0", 5000UL, [](const char*){});
      delay(500);
      Cellular::atPassthrough("+CNACT=0,1", 10000UL, [](const char*){});
      // CNACT activation completes asynchronously via +APP PDP URC;
      // a short settle window before the next handshake is reliable.
      for (int i = 0; i < 20; ++i) {
        Shell::pumpConsole();
        delay(100);
        esp_task_wdt_reset();
      }
    }

    uint32_t paceUntil = millis() + CHUNK_GAP_MS;
    while (millis() < paceUntil) {
      Shell::pumpConsole();
      esp_task_wdt_reset();
      delay(200);
    }
  }
  return true;
}
#endif // ENABLE_CELLULAR

// WiFi wins when both are up (cheaper than waking + holding cellular TX).
// in:  url, writeCallback. out: outStatus, outLen.
static bool otaHttpGet(const char* url,
                      std::function<bool(const uint8_t*, size_t)> writeCallback,
                      int* outStatus, size_t* outLen) {
  if (outStatus) *outStatus = 0;
  if (outLen)    *outLen    = 0;
  if (WiFiManager::connected()) {
    return otaHttpGetWiFi(url, writeCallback, outStatus, outLen);
  }
#ifdef ENABLE_CELLULAR
  if (Cellular::connected()) {
    return otaHttpGetCellular(url, writeCallback, outStatus, outLen);
  }
#endif
  Log::warn(TAG, "ota.http_get_failed reason=no_transport");
  return false;
}

static bool anyTransportUp() {
  if (WiFiManager::connected()) return true;
#ifdef ENABLE_CELLULAR
  if (Cellular::connected())    return true;
#endif
  return false;
}

// ---------------------------------------------------------------------------

void OTAUpdate::begin() {
  JsonObject cfg = Config::get();

  _enabled = cfg["ota"]["enabled"] | true;
  uint32_t intervalS = cfg["ota"]["check_interval_s"] | 21600;
  _checkIntervalMs = intervalS * 1000UL;

  const char* manifestUrl = cfg["ota"]["manifest_url"] | "";
  if (strlen(manifestUrl) == 0) {
    Log::warn(TAG, "ota.disabled reason=no_manifest_url");
    _enabled = false;
  }

  // Refuse to bring OTA up without a CA cert unless ota.allow_insecure is
  // explicitly set in config.json. With no cert, both manifest and binary
  // come over an unverified HTTPS channel - the SHA256 in the manifest is
  // not MITM protection because an attacker who can MITM controls both.
  // Dev workflows that legitimately need to bypass set the opt-in.
  if (_enabled) {
    loadCaCert();
    bool allowInsecure = cfg["ota"]["allow_insecure"] | false;
    switch (otaTlsMode(!_otaCaCert.isEmpty(), allowInsecure)) {
      case OTA_TLS_REFUSED:
        Log::error(TAG, "ota.disabled reason=no_ca allow_insecure=unset");
        _enabled = false;
        break;
      case OTA_TLS_INSECURE:
        Log::kvfw(TAG, "ota.tls_insecure reason=allow_insecure_set");
        break;
      case OTA_TLS_VERIFIED:
        break;
    }
  }

  if (_enabled) {
    Log::kvf(TAG, "ota.enabled interval_s=%lu manifest=%.80s",
             intervalS, manifestUrl);
  }

  // On-demand checks come from the `ota.check` shell command (serial or
  // the cli/ MQTT path, both via triggerCheck) and from publishing to
  // ota.cmd_topic.
  registerCommandTopic();

  // Run first check ~30s after boot to let things settle. Guard the
  // unsigned subtraction: a sub-30s interval would set _lastCheck in the
  // future, underflowing the first elapsed compare and firing at once.
  const uint32_t settleMs = 30000;
  _lastCheck = (_checkIntervalMs > settleMs)
                 ? millis() - (_checkIntervalMs - settleMs)
                 : millis();
}

// ---------------------------------------------------------------------------

// Subscribe the optional ota.cmd_topic. Split out of begin() because
// MQTTClient::begin() resets the subscription table, and on the boot path it
// runs after OTAUpdate::begin() (heap order, main.cpp) - so MQTT re-registers
// the topic itself once its own table is clean.
void OTAUpdate::registerCommandTopic() {
  JsonObject cfg = Config::get();
  // Re-check the config gate, not the _enabled flag: this now runs from
  // MQTTClient::begin() too, and a device with ota.enabled=false must not
  // gain a remote trigger that the old single call site never gave it.
  if (!(cfg["ota"]["enabled"] | true)) return;

  const char* customTopic = cfg["ota"]["cmd_topic"] | "";
  if (strlen(customTopic) == 0) return;

  char topic[MQTT_TOPIC_CAP];
  strncpy(topic, customTopic, sizeof(topic) - 1);
  topic[sizeof(topic) - 1] = '\0';

  MQTTClient::subscribe(topic, [](const char* topic, const char* payload) {
    Log::info(TAG, "ota.trigger source=mqtt");
    _checkRequested = true;
    if (payload && strlen(payload) > 8) {
      _pendingManifestUrl = payload;
    } else {
      _pendingManifestUrl = "";
    }
  });
}

// ---------------------------------------------------------------------------

void OTAUpdate::checkNow() {
  if (!_enabled) return;
  if (!anyTransportUp()) return;
  Log::info(TAG, "ota.boot_check heap=clean");
  check();
  _lastCheck = millis();
}

void OTAUpdate::triggerCheck(const char* manifestOverride, bool force) {
  if (manifestOverride && strlen(manifestOverride) > 0) {
    _pendingManifestUrl = manifestOverride;
  } else {
    _pendingManifestUrl = "";
  }
  _forceRequested = force;
  _checkRequested = true;
}

void OTAUpdate::loop() {
  confirmIfHealthy();
  // Deferred MQTT-triggered check: runs outside MQTT callback context.
  if (_checkRequested) {
    _checkRequested = false;
    bool force = _forceRequested;
    _forceRequested = false;
    const char* override = _pendingManifestUrl.length() > 0
                             ? _pendingManifestUrl.c_str() : nullptr;
    check(override, force);
    _pendingManifestUrl = "";
    return;
  }

  if (!_enabled) return;
  if (!anyTransportUp()) return;

  // When deep sleep is enabled, millis() resets every wake cycle.
  // Use wall clock (NTP) + RTC-persisted last check time instead.
  if (SleepManager::enabled()) {
    time_t now = time(nullptr);
    if (now > 1700000000) {  // NTP synced
      time_t last = SleepManager::lastOtaCheck();
      if (last == 0 || (now - last) >= (time_t)(_checkIntervalMs / 1000)) {
        SleepManager::setLastOtaCheck(now);
        check(nullptr);
      }
    }
    return;
  }

  uint32_t now = millis();
  if (now - _lastCheck >= _checkIntervalMs) {
    _lastCheck = now;
    check(nullptr);
  }
}

// ---------------------------------------------------------------------------

void OTAUpdate::check(const char* manifestOverride, bool force) {
  if (!anyTransportUp()) {
    Log::warn(TAG, "ota.check_skipped reason=no_transport");
    publishOtaRefusal("no-transport");
    return;
  }

  JsonObject cfg = Config::get();
  const char* url = manifestOverride
                      ? manifestOverride
                      : (cfg["ota"]["manifest_url"] | "");

  if (strlen(url) == 0) {
    Log::error(TAG, "ota.check_refused reason=no_manifest_url");
    publishOtaRefusal("no-manifest-url");
    return;
  }

  // Mirror the begin()-time refusal so a triggered check (cli/ota.check or
  // cmd_topic publish) cannot bypass it. begin() may have set _enabled=false,
  // but triggerCheck() -> loop() runs check() regardless of _enabled to allow
  // operator-driven retries; without this gate, an attacker who can publish
  // to the OTA cmd_topic could still force an insecure HTTPS fetch even
  // though begin() refused at boot.
  //
  // With the PROGMEM CA fallback in loadCaCert this gate effectively only
  // fires when the PROGMEM bundle itself is empty (build misconfig). Kept
  // anyway so a future allow_insecure code path stays consistent.
  loadCaCert();
  bool allowInsecure = cfg["ota"]["allow_insecure"] | false;
  if (_otaCaCert.isEmpty() && !allowInsecure) {
    Log::kvfw(TAG, "ota.check_refused reason=no_ca");
    publishOtaRefusal("no-ca");
    return;
  }

  Log::kvf(TAG, "ota.phase_change from=idle to=check force=%d", (int)force);

  // On heap-constrained boards (CYD/WROOM), MQTT's TLS session fragments
  // heap so a second TLS connection for the manifest fetch will OOM.
  // Skip periodic checks when heap is too low - boot-time checkNow()
  // handles OTA with clean heap before MQTT connects.
  // Exception: --force means user explicitly wants to flash. Disconnect
  // MQTT to free TLS buffers, letting the manifest fetch succeed on a
  // clean heap. MQTT reconnect happens post-reboot if update applies, or
  // in loop() if we bail out.
  // Only applies to the WiFi transport: cellular MQTT TLS runs inside
  // the modem so it does not fragment ESP heap, and the manifest fetch
  // goes through the modem's separate SSL socket layer regardless.
  if (WiFiManager::connected() &&
      MQTTClient::connected() && ESP.getMaxAllocHeap() < 40000) {
    if (!force) {
      Log::kvfw(TAG, "ota.check_skipped reason=heap_low");
      publishOtaRefusal("heap-low");
      return;
    }
    Log::kvf(TAG, "ota.heap_low action=mqtt_disconnect");
    MQTTClient::_client.disconnect();
    MQTTClient::_wifiClient.stop();
    delay(200);  // let TCP teardown + free mbedtls context
  }

  Log::kvf(TAG, "ota.phase_change from=check to=fetch_manifest");
  String remoteVersion, binUrl, sha256;
  size_t binSize = 0;
  if (!fetchManifest(url, remoteVersion, binUrl, sha256, binSize)) {
    Log::kvfe(TAG, "ota.phase_change from=fetch_manifest to=idle reason=fetch_failed");
    publishOtaRefusal("manifest-fetch-failed");
    return;
  }

  Log::kvf(TAG, "ota.version_check remote=%s local=%s",
           remoteVersion.c_str(), FIRMWARE_VERSION);

  if (!otaShouldUpdate(remoteVersion.c_str(), FIRMWARE_VERSION, force)) {
    Log::kvf(TAG, "ota.phase_change from=fetch_manifest to=idle reason=up_to_date");
    // Publish so operators see the check completed and the device is on
    // the latest. Without this an up-to-date check looks identical to
    // "device offline" or "silent refusal" from the broker side.
    const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
    char topic[MQTT_TOPIC_CAP];
    snprintf(topic, sizeof(topic), "%s/status/ota", prefix);
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"state\":\"up-to-date\",\"version\":\"%s\",\"remote\":\"%s\"}",
             FIRMWARE_VERSION, remoteVersion.c_str());
    MQTTClient::publish(topic, payload);
    return;
  }
  // Without this the image a switch booted pulls the fled build straight back.
  loadFled();
  const uint32_t fledAge = fledAgeNow();
  const uint8_t tries = _fled.saved.tries;
  const OtaFledAction fled =
      otaFledAction(remoteVersion.c_str(), _fled.ver, fledAge, tries, force);
  if (fled == OTA_FLED_SKIP) {
    if (otaFledSaveDue(fledUnsavedS(), !_fled.savedThisBoot)) saveFled(fledAge, tries);
    Log::kvfw(TAG, "ota.phase_change from=fetch_manifest to=idle reason=fled_release version=%s age_s=%lu",
              remoteVersion.c_str(), (unsigned long)fledAge);
    publishOtaRefusal("fled-release");
    return;
  }
  if (fled == OTA_FLED_RETRY || fled == OTA_FLED_LAST) {
    // Saved before the download, so a try that never boots still counts.
    const OtaFledAge spent = otaFledAfterTry(fled, fledAge, tries, true);
    const bool saved = saveFled(spent.ageS, spent.tries);
    if (!otaFledMayDownload(fled, saved)) {
      // saveFled moved RAM on; the try NVS did not take stays unspent.
      const OtaFledAge kept = otaFledAfterTry(fled, fledAge, tries, false);
      _fled.saved.ageS = kept.ageS;
      _fled.saved.tries = kept.tries;
      Log::kvfe(TAG, "ota.phase_change from=fetch_manifest to=idle reason=fled_try_unsaved version=%s",
                remoteVersion.c_str());
      publishOtaRefusal("fled-release");
      return;
    }
    Log::kvfw(TAG, "ota.fled_release_retry version=%s try=%u", remoteVersion.c_str(),
              (unsigned)(tries + 1));
  } else if (fled == OTA_FLED_FORCED) {
    Log::warn(TAG, "ota.force_reflash version=fled");
  } else if (force && !isNewer(remoteVersion.c_str(), FIRMWARE_VERSION)) {
    Log::warn(TAG, "ota.force_reflash version=same_or_older");
  }

  Log::kvf(TAG, "ota.phase_change from=fetch_manifest to=download version=%s url=%.80s",
           remoteVersion.c_str(), binUrl.c_str());

  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char statusTopic[MQTT_TOPIC_CAP];
  snprintf(statusTopic, sizeof(statusTopic), "%s/status/ota", prefix);
  char statusPayload[128];
  snprintf(statusPayload, sizeof(statusPayload),
           "{\"state\":\"updating\",\"from\":\"%s\",\"to\":\"%s\"}",
           FIRMWARE_VERSION, remoteVersion.c_str());
  MQTTClient::publish(statusTopic, statusPayload);
  delay(100);

  if (applyUpdate(binUrl, sha256, binSize)) {
    Log::kvf(TAG, "ota.phase_change from=apply to=reboot version=%s", remoteVersion.c_str());
    delay(500);
    plannedRestart();
  } else {
    Log::error(TAG, "ota.update_failed");
    snprintf(statusPayload, sizeof(statusPayload),
             "{\"state\":\"failed\",\"version\":\"%s\"}", FIRMWARE_VERSION);
    MQTTClient::publish(statusTopic, statusPayload);
  }
}

// ---------------------------------------------------------------------------

// Belt-and-braces watchdog feeding + tight timeouts
// so a stall here (TLS handshake, hung socket read on a flaky upstream)
// cannot block this task long enough to trip TWDT. Goes through the
// unified otaHttpGet helper so the same call site works over WiFi or
// the SIM7080G modem-native SSL socket.
bool OTAUpdate::fetchManifest(const char* url,
                               String& version, String& binUrl, String& sha256,
                               size_t& size) {
  size = 0;
  String body;
  // Cap the manifest at 8 KB - real manifests are ~200 B; anything
  // larger is either a misconfig or a malicious upstream.
  auto cb = [&](const uint8_t* buf, size_t len) -> bool {
    if (!otaManifestSizeOk(body.length(), len)) return false;
    body.concat(reinterpret_cast<const char*>(buf), len);
    return true;
  };

  int    status = 0;
  size_t total  = 0;
  bool transportOk = otaHttpGet(url, cb, &status, &total);

  if (!transportOk || status != 200) {
    Log::kvfe(TAG, "ota.manifest_fetch_failed status=%d bytes=%u",
              status, (unsigned)total);
    return false;
  }

  yield();
  esp_task_wdt_reset();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Log::error(TAG, "ota.manifest_parse_failed");
    return false;
  }

  const char* v = doc["version"] | "";
  const char* u = doc["url"]     | "";
  const char* s = doc["sha256"]  | "";
  // size: optional. Required for the cellular chunked Range path; the
  // WiFi path falls back to Content-Length from the binary GET if absent.
  size_t      sz = doc["size"]   | (size_t)0;

  if (!otaManifestValid(v, u, s)) {
    Log::error(TAG, "ota.manifest_invalid missing=version_url_sha256");
    return false;
  }

  version = v;
  binUrl  = u;
  sha256  = s;
  size    = sz;
  return true;
}

// ---------------------------------------------------------------------------

// Download firmware binary, verify SHA256, and flash it.
//
// WiFi transport: single GET through otaHttpGet (HTTPClient handles
// multi-MB downloads in one socket without session degradation).
// Cellular transport: 64 KB Range chunks via downloadBinaryChunkedCellular.
// Each chunk is its own short-lived TLS session, defeating the
// SIM7080G HTTP-session degradation observed past ~500-900 KB on fw
// 1951B17. Requires manifest `size` field; missing -> single-GET
// fallback with a warn (may stall on cellular).
//
// Both paths feed the same streaming callback (Update.write +
// mbedtls_sha256_update) so the binary is never buffered in heap.
//
// Update.begin(UPDATE_SIZE_UNKNOWN) keeps the streaming path agnostic
// of whether Content-Length was present on the wire. Trade-off: Update
// can't pre-check "fits in OTA partition"; instead it errors at the
// first Update.write() past the boundary, which we propagate.
bool OTAUpdate::applyUpdate(const String& binUrl, const String& expectedSha256,
                            size_t expectedSize) {
  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    Log::kvfe(TAG, "ota.phase_change from=download to=idle reason=update_begin_failed");
    return false;
  }

  mbedtls_sha256_context sha_ctx;
  mbedtls_sha256_init(&sha_ctx);
  mbedtls_sha256_starts(&sha_ctx, 0);  // 0 = SHA-256

  size_t   written       = 0;
  uint32_t lastProgress  = 0;
  bool     writeOk       = true;

  auto cb = [&](const uint8_t* buf, size_t len) -> bool {
    if (!writeOk) return false;
    size_t w = Update.write(const_cast<uint8_t*>(buf), len);
    if (w != len) {
      Log::error(TAG, "ota.flash_write_failed reason=short_write");
      writeOk = false;
      return false;
    }
    mbedtls_sha256_update(&sha_ctx, buf, len);
    written += len;
    uint32_t now = millis();
    if (now - lastProgress > 5000) {
      if (expectedSize > 0) {
        Log::kvf(TAG, "ota.download_progress bytes=%u total=%u pct=%u",
                 (unsigned)written, (unsigned)expectedSize,
                 (unsigned)((written * 100) / expectedSize));
      } else {
        Log::kvf(TAG, "ota.download_progress bytes=%u", (unsigned)written);
      }
      lastProgress = now;
    }
    esp_task_wdt_reset();
    yield();
    return true;
  };

  bool   transportOk = false;
  int    status      = 0;
  size_t total       = 0;

  if (WiFiManager::connected()) {
    transportOk = otaHttpGetWiFi(binUrl.c_str(), cb, &status, &total);
  }
#ifdef ENABLE_CELLULAR
  else if (Cellular::connected()) {
    if (expectedSize > 0) {
      transportOk = downloadBinaryChunkedCellular(binUrl.c_str(),
                                                  expectedSize, cb, &total);
      // The chunked path returns true only when every chunk hit 206/200,
      // so synthesize a 200 for the success-path branch below.
      if (transportOk) status = 200;
    } else {
      Log::kvfw(TAG, "ota.manifest_missing_size fallback=single_get");
      transportOk = otaHttpGetCellular(binUrl.c_str(), cb, &status, &total);
    }
  }
#endif
  else {
    Log::error(TAG, "ota.download_failed reason=no_transport");
  }

  if (!transportOk || status != 200 || !writeOk || written == 0 ||
      (expectedSize > 0 && written < expectedSize)) {
    Log::kvfe(TAG, "ota.phase_change from=download to=idle reason=download_failed transport=%d status=%d write_ok=%d bytes=%u expected=%u",
              (int)transportOk, status, (int)writeOk,
              (unsigned)written, (unsigned)expectedSize);
    mbedtls_sha256_free(&sha_ctx);
    Update.abort();
    return false;
  }

  Log::kvf(TAG, "ota.phase_change from=download to=verify bytes=%u", (unsigned)written);
  uint8_t hash[32];
  mbedtls_sha256_finish(&sha_ctx, hash);
  mbedtls_sha256_free(&sha_ctx);

  char computedHex[65];
  for (int i = 0; i < 32; i++) {
    snprintf(computedHex + (i * 2), 3, "%02x", hash[i]);
  }
  computedHex[64] = '\0';

  if (!otaShaMatches(computedHex, expectedSha256.c_str())) {
    Log::kvfe(TAG, "ota.phase_change from=verify to=idle reason=sha256_mismatch computed=%s", computedHex);
    Update.abort();
    return false;
  }

  Log::kvf(TAG, "ota.phase_change from=verify to=apply");

  if (!Update.end(true)) {
    Log::kvfe(TAG, "ota.phase_change from=apply to=idle reason=update_end_failed");
    return false;
  }

  Log::kvf(TAG, "ota.download_verified bytes=%u", (unsigned)written);
  return true;
}

// ---------------------------------------------------------------------------

bool OTAUpdate::isNewer(const char* remote, const char* local) {
  return otaIsNewer(remote, local);
}
