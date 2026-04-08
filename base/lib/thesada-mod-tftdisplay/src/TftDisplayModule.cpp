// thesada-fw - TftDisplayModule.cpp
// ILI9341 TFT + XPT2046 touch. Hardware init only, all rendering via Lua.
// SPDX-License-Identifier: GPL-3.0-only

#include <thesada_config.h>
#ifdef ENABLE_TFT

#include "TftDisplayModule.h"
#include <Config.h>
#include <Log.h>
#include <ModuleRegistry.h>
#ifdef ENABLE_SCRIPTENGINE
#include <ScriptEngine.h>
#endif

#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

extern "C" {
#include <lua/lua.h>
#include <lua/lauxlib.h>
}
extern lua_State* gL;

static const char* TAG = "TFT";

// CYD pin assignments
#define CYD_TFT_BL    21
#define CYD_TOUCH_CS   33
#define CYD_TOUCH_IRQ  36
#define CYD_TOUCH_CLK  25
#define CYD_TOUCH_MOSI 32
#define CYD_TOUCH_MISO 39
#define CYD_LED_R      4
#define CYD_LED_G      16
#define CYD_LED_B      17
#define CYD_LDR        34

static TFT_eSPI _tftInstance;
static TFT_eSPI* _tft = &_tftInstance;
static SPIClass* _touchSpi = nullptr;
static XPT2046_Touchscreen* _ts = nullptr;

static uint16_t _fgColor = TFT_WHITE;
static uint16_t _bgColor = TFT_BLACK;
static uint8_t  _currentFont = 2;
static bool _initialized = false;

// Convert RGB to 565 color
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Initialize TFT display, touch, and RGB LED
void TftDisplayModule::begin() {
  // Backlight on
  pinMode(CYD_TFT_BL, OUTPUT);
  digitalWrite(CYD_TFT_BL, HIGH);

  // RGB LED off (active low)
  pinMode(CYD_LED_R, OUTPUT); digitalWrite(CYD_LED_R, HIGH);
  pinMode(CYD_LED_G, OUTPUT); digitalWrite(CYD_LED_G, HIGH);
  pinMode(CYD_LED_B, OUTPUT); digitalWrite(CYD_LED_B, HIGH);

  // TFT init (global static instance, same as test sketch)
  JsonObject cfg = Config::get();
  uint8_t rotation = cfg["display"]["rotation"] | 1;  // 0-3, default 1 (landscape, USB left)

  _tft->init();
  _tft->invertDisplay(true);
  _tft->setRotation(rotation);
  _tft->fillScreen(TFT_BLACK);

  // Splash screen - centered
  int cx = _tft->width() / 2;
  _tft->setTextDatum(TC_DATUM);
  _tft->setTextColor(TFT_GREEN, TFT_BLACK);
  _tft->setTextSize(3);
  _tft->drawString("thesada-fw", cx, 50);
  _tft->setTextSize(2);
  _tft->setTextColor(TFT_WHITE, TFT_BLACK);
  _tft->drawString(FIRMWARE_VERSION, cx, 100);
  _tft->setTextSize(1);
  _tft->setTextColor(TFT_DARKGREY, TFT_BLACK);
  _tft->drawString("CYD Display Node", cx, 140);
  _tft->setTextDatum(TL_DATUM);

  // Touch init on HSPI
  _touchSpi = new SPIClass(HSPI);
  _touchSpi->begin(CYD_TOUCH_CLK, CYD_TOUCH_MISO, CYD_TOUCH_MOSI, CYD_TOUCH_CS);
  _ts = new XPT2046_Touchscreen(CYD_TOUCH_CS, CYD_TOUCH_IRQ);
  _ts->begin(*_touchSpi);
  _ts->setRotation(rotation);

  _initialized = true;

  char msg[64];
  snprintf(msg, sizeof(msg), "Ready - %dx%d, touch on HSPI", _tft->width(), _tft->height());
  Log::info(TAG, msg);

  // Register Lua Display.* bindings for TFT (same global name as OLED)
  #ifdef ENABLE_SCRIPTENGINE
  ScriptEngine::addBindings([](lua_State* L) {
    static const luaL_Reg lib[] = {
      {"clear",     [](lua_State* L) -> int { TftDisplayModule::clear(); return 0; }},
      {"show",      [](lua_State* L) -> int { return 0; }}, // no-op for TFT
      {"ready",     [](lua_State* L) -> int { lua_pushboolean(L, TftDisplayModule::ready()); return 1; }},
      {"text",      [](lua_State* L) -> int {
        TftDisplayModule::text((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2), luaL_checkstring(L, 3));
        return 0;
      }},
      {"line",      [](lua_State* L) -> int {
        TftDisplayModule::line((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2),
                               (int)luaL_checknumber(L, 3), (int)luaL_checknumber(L, 4));
        return 0;
      }},
      {"rect",      [](lua_State* L) -> int {
        TftDisplayModule::rect((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2),
                               (int)luaL_checknumber(L, 3), (int)luaL_checknumber(L, 4));
        return 0;
      }},
      {"fill",      [](lua_State* L) -> int {
        TftDisplayModule::fillRect((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2),
                                   (int)luaL_checknumber(L, 3), (int)luaL_checknumber(L, 4));
        return 0;
      }},
      {"font",      [](lua_State* L) -> int { TftDisplayModule::setFont(luaL_checkstring(L, 1)); return 0; }},
      {"color",     [](lua_State* L) -> int {
        TftDisplayModule::setColor((uint8_t)luaL_checknumber(L, 1), (uint8_t)luaL_checknumber(L, 2), (uint8_t)luaL_checknumber(L, 3));
        return 0;
      }},
      {"bgcolor",   [](lua_State* L) -> int {
        TftDisplayModule::setBgColor((uint8_t)luaL_checknumber(L, 1), (uint8_t)luaL_checknumber(L, 2), (uint8_t)luaL_checknumber(L, 3));
        return 0;
      }},
      {"width",     [](lua_State* L) -> int { lua_pushinteger(L, TftDisplayModule::width()); return 1; }},
      {"height",    [](lua_State* L) -> int { lua_pushinteger(L, TftDisplayModule::height()); return 1; }},
      {"touched",   [](lua_State* L) -> int {
        int x, y;
        if (TftDisplayModule::touched(&x, &y)) { lua_pushinteger(L, x); lua_pushinteger(L, y); return 2; }
        lua_pushnil(L); return 1;
      }},
      {"center",    [](lua_State* L) -> int {
        TftDisplayModule::centerText((int)luaL_checknumber(L, 1), luaL_checkstring(L, 2)); return 0;
      }},
      {"textWidth", [](lua_State* L) -> int {
        lua_pushinteger(L, TftDisplayModule::textWidth(luaL_checkstring(L, 1))); return 1;
      }},
      {"onTouch",   [](lua_State* L) -> int {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        TftDisplayModule::setTouchCallback(ref);
        return 0;
      }},
      {"backlight", [](lua_State* L) -> int { TftDisplayModule::setBacklight(lua_toboolean(L, 1)); return 0; }},
      {"led",       [](lua_State* L) -> int {
        TftDisplayModule::setLed((uint8_t)luaL_checknumber(L, 1), (uint8_t)luaL_checknumber(L, 2), (uint8_t)luaL_checknumber(L, 3));
        return 0;
      }},
      {nullptr, nullptr}
    };
    luaL_newlib(L, lib);
    lua_setglobal(L, "Display");  // same global name - scripts work on either display
  });
  #endif
}

// Lua touch callback reference
static int _touchCallbackRef = LUA_NOREF;
static uint32_t _lastTouchMs = 0;

// Poll touch IRQ in loop - only reads SPI when pin is LOW (touched)
void TftDisplayModule::loop() {
  if (!_initialized || !_ts || _touchCallbackRef == LUA_NOREF) return;

  // IRQ pin is LOW when touched - skip SPI read if not touched
  if (digitalRead(CYD_TOUCH_IRQ) == HIGH) return;

  // Debounce: 200ms between touch events
  uint32_t now = millis();
  if (now - _lastTouchMs < 200) return;

  if (_ts->touched()) {
    _lastTouchMs = now;
    TS_Point p = _ts->getPoint();
    int x = map(p.x, 200, 3800, 0, _tft ? _tft->width() : 320);
    int y = map(p.y, 200, 3800, 0, _tft ? _tft->height() : 240);

    // Call Lua callback
    extern lua_State* gL;
    if (gL && _touchCallbackRef != LUA_NOREF) {
      lua_rawgeti(gL, LUA_REGISTRYINDEX, _touchCallbackRef);
      lua_pushinteger(gL, x);
      lua_pushinteger(gL, y);
      if (lua_pcall(gL, 2, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(gL, -1);
        Log::error("TFT", "Touch callback error");
        lua_pop(gL, 1);
      }
    }
  }
}

// Check if TFT is initialized
bool TftDisplayModule::ready() {
  return _initialized;
}

// Clear screen with current background color
void TftDisplayModule::clear() {
  if (_tft) _tft->fillScreen(_bgColor);
}

// Draw text at position (transparent background - no black rectangles over cards)
void TftDisplayModule::text(int x, int y, const char* str) {
  if (_tft && str) {
    _tft->setTextColor(_fgColor);
    _tft->drawString(str, x, y, _currentFont);
  }
}

// Draw a line
void TftDisplayModule::line(int x1, int y1, int x2, int y2) {
  if (_tft) _tft->drawLine(x1, y1, x2, y2, _fgColor);
}

// Draw a rectangle outline
void TftDisplayModule::rect(int x, int y, int w, int h) {
  if (_tft) _tft->drawRect(x, y, w, h, _fgColor);
}

// Draw a filled rectangle
void TftDisplayModule::fillRect(int x, int y, int w, int h) {
  if (_tft) _tft->fillRect(x, y, w, h, _fgColor);
}

// No-op for TFT (draws are immediate, no framebuffer)
void TftDisplayModule::show() {}

// Set font: "small" (8px), "medium" (16px), "large" (26px)
void TftDisplayModule::setFont(const char* size) {
  if (!size) return;
  if (strcmp(size, "small") == 0)       _currentFont = 1;
  else if (strcmp(size, "large") == 0)  _currentFont = 4;
  else                                  _currentFont = 2;
}

// Set foreground draw color
void TftDisplayModule::setColor(uint8_t r, uint8_t g, uint8_t b) {
  _fgColor = rgb565(r, g, b);
}

// Set background color
void TftDisplayModule::setBgColor(uint8_t r, uint8_t g, uint8_t b) {
  _bgColor = rgb565(r, g, b);
}

// Get display width
int TftDisplayModule::width() {
  return _tft ? _tft->width() : 0;
}

// Get display height
int TftDisplayModule::height() {
  return _tft ? _tft->height() : 0;
}

// Check for touch and return mapped coordinates
bool TftDisplayModule::touched(int* x, int* y) {
  if (!_ts || !_ts->touched()) return false;
  TS_Point p = _ts->getPoint();
  if (x) *x = map(p.x, 200, 3800, 0, _tft ? _tft->width() : 320);
  if (y) *y = map(p.y, 200, 3800, 0, _tft ? _tft->height() : 240);
  return true;
}

// Register a Lua function to be called on touch events
void TftDisplayModule::setTouchCallback(int luaRef) {
  _touchCallbackRef = luaRef;
}

// Draw text centered horizontally at y position
void TftDisplayModule::centerText(int y, const char* str) {
  if (_tft && str) {
    _tft->setTextColor(_fgColor);
    _tft->setTextDatum(TC_DATUM);
    _tft->drawString(str, _tft->width() / 2, y, _currentFont);
    _tft->setTextDatum(TL_DATUM);
  }
}

// Return pixel width of a string in current font
int TftDisplayModule::textWidth(const char* str) {
  if (_tft && str) return _tft->textWidth(str, _currentFont);
  return 0;
}

// Control TFT backlight
void TftDisplayModule::setBacklight(bool on) {
  digitalWrite(CYD_TFT_BL, on ? HIGH : LOW);
}

// Set RGB LED (active low on CYD)
void TftDisplayModule::setLed(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(CYD_LED_R, 255 - r);
  analogWrite(CYD_LED_G, 255 - g);
  analogWrite(CYD_LED_B, 255 - b);
}

// Report TFT display module status
void TftDisplayModule::status(ShellOutput out) {
  char line[64];
  snprintf(line, sizeof(line), "%s  %dx%d", ready() ? "ready" : "not ready", width(), height());
  out(line);
}

MODULE_REGISTER(TftDisplayModule, PRIORITY_OUTPUT)  // before ScriptEngine(40) so Lua bindings are registered first

#endif // ENABLE_TFT
