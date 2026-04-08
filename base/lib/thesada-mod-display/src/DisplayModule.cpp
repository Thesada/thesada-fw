// thesada-fw - DisplayModule.cpp
// SSD1306 OLED display - hardware init only. All rendering via Lua bindings.
// SPDX-License-Identifier: GPL-3.0-only

#include <thesada_config.h>
#ifdef ENABLE_DISPLAY

#include "DisplayModule.h"
#include <Config.h>
#include <Log.h>
#include <ModuleRegistry.h>
#ifdef ENABLE_SCRIPTENGINE
#include <ScriptEngine.h>
extern "C" {
#include <lua/lua.h>
#include <lua/lualib.h>
#include <lua/lauxlib.h>
}
#endif

#include <U8g2lib.h>
#include <Wire.h>
#include <esp_task_wdt.h>

static const char* TAG = "Display";

// Display instance
static U8G2_SSD1306_128X64_NONAME_F_SW_I2C* _u8g2 = nullptr;

// Initialize I2C display from config
void DisplayModule::begin() {
  JsonObject cfg = Config::get();
  int sda  = cfg["display"]["sda"]     | 21;
  int scl  = cfg["display"]["scl"]     | 22;
  int addr = cfg["display"]["address"] | 0x3C;

  // I2C scan to verify display is present
  Wire.begin(sda, scl);
  Wire.beginTransmission(addr);
  uint8_t scanResult = Wire.endTransmission();
  if (scanResult != 0) {
    char emsg[64];
    snprintf(emsg, sizeof(emsg), "No device at 0x%02X (SDA=%d SCL=%d) - err %d", addr, sda, scl, scanResult);
    Log::error(TAG, emsg);
    return;
  }
  Log::info(TAG, "I2C device found");

  // SW I2C for U8g2 - avoids conflicts with Wire used by other modules
  esp_task_wdt_reset();
  _u8g2 = new U8G2_SSD1306_128X64_NONAME_F_SW_I2C(U8G2_R0, scl, sda, U8X8_PIN_NONE);
  _u8g2->setI2CAddress(addr << 1);
  if (!_u8g2->begin()) {
    Log::error(TAG, "SSD1306 init failed");
    delete _u8g2;
    _u8g2 = nullptr;
    return;
  }
  esp_task_wdt_reset();

  // Show splash screen
  _u8g2->setFont(u8g2_font_6x10_tr);
  _u8g2->clearBuffer();
  _u8g2->drawStr(10, 30, "thesada-fw");
  _u8g2->drawStr(10, 45, FIRMWARE_VERSION);
  _u8g2->sendBuffer();

  char msg[48];
  snprintf(msg, sizeof(msg), "Ready - SDA=%d SCL=%d addr=0x%02X", sda, scl, addr);
  Log::info(TAG, msg);

  // Register Lua Display.* bindings for OLED
  #ifdef ENABLE_SCRIPTENGINE
  ScriptEngine::addBindings([](lua_State* L) {
    static const luaL_Reg lib[] = {
      {"clear", [](lua_State* L) -> int { DisplayModule::clear(); return 0; }},
      {"show",  [](lua_State* L) -> int { DisplayModule::show(); return 0; }},
      {"ready", [](lua_State* L) -> int { lua_pushboolean(L, DisplayModule::ready()); return 1; }},
      {"text",  [](lua_State* L) -> int {
        DisplayModule::text((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2), luaL_checkstring(L, 3));
        return 0;
      }},
      {"line",  [](lua_State* L) -> int {
        DisplayModule::line((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2),
                            (int)luaL_checknumber(L, 3), (int)luaL_checknumber(L, 4));
        return 0;
      }},
      {"rect",  [](lua_State* L) -> int {
        DisplayModule::rect((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2),
                            (int)luaL_checknumber(L, 3), (int)luaL_checknumber(L, 4));
        return 0;
      }},
      {"fill",  [](lua_State* L) -> int {
        DisplayModule::fillRect((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2),
                                (int)luaL_checknumber(L, 3), (int)luaL_checknumber(L, 4));
        return 0;
      }},
      {"font",  [](lua_State* L) -> int { DisplayModule::setFont(luaL_checkstring(L, 1)); return 0; }},
      {nullptr, nullptr}
    };
    luaL_newlib(L, lib);
    lua_setglobal(L, "Display");
  });
  #endif
}

// No loop work - all rendering driven by Lua
void DisplayModule::loop() {}

// Check if display is initialized
bool DisplayModule::ready() {
  return _u8g2 != nullptr;
}

// Clear the framebuffer
void DisplayModule::clear() {
  if (_u8g2) _u8g2->clearBuffer();
}

// Draw text at x,y using current font
void DisplayModule::text(int x, int y, const char* str) {
  if (_u8g2 && str) _u8g2->drawStr(x, y, str);
}

// Draw a line
void DisplayModule::line(int x1, int y1, int x2, int y2) {
  if (_u8g2) _u8g2->drawLine(x1, y1, x2, y2);
}

// Draw a rectangle outline
void DisplayModule::rect(int x, int y, int w, int h) {
  if (_u8g2) _u8g2->drawFrame(x, y, w, h);
}

// Draw a filled rectangle
void DisplayModule::fillRect(int x, int y, int w, int h) {
  if (_u8g2) _u8g2->drawBox(x, y, w, h);
}

// Send framebuffer to display
void DisplayModule::show() {
  if (_u8g2) _u8g2->sendBuffer();
}

// Set font size: "small" (5x7), "medium" (6x10), "large" (10x20)
void DisplayModule::setFont(const char* size) {
  if (!_u8g2 || !size) return;
  if (strcmp(size, "small") == 0)       _u8g2->setFont(u8g2_font_5x7_tr);
  else if (strcmp(size, "large") == 0)  _u8g2->setFont(u8g2_font_10x20_tr);
  else                                  _u8g2->setFont(u8g2_font_6x10_tr);
}

// Report display module status
void DisplayModule::status(ShellOutput out) {
  out(ready() ? "ready" : "not ready");
}

MODULE_REGISTER(DisplayModule, PRIORITY_SERVICE)  // before ScriptEngine(40) so Lua bindings are registered first

#endif // ENABLE_DISPLAY
