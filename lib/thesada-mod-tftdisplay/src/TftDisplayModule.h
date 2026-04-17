// thesada-fw - TftDisplayModule.h
// ILI9341 TFT + XPT2046 touch display. All rendering via Lua bindings.
// Provides the same Display.* Lua API as the OLED module plus color and touch.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <Module.h>

class TftDisplayModule : public Module {
public:
  const char* name() override { return "tftdisplay"; }
  void begin() override;
  void loop() override;
  void status(ShellOutput out) override;

  // Static methods called from Lua bindings (same API as DisplayModule)
  static bool ready();
  static void clear();
  static void text(int x, int y, const char* str);
  static void line(int x1, int y1, int x2, int y2);
  static void rect(int x, int y, int w, int h);
  static void fillRect(int x, int y, int w, int h);
  static void show();
  static void setFont(const char* size);

  // TFT-specific (color, touch)
  static void setColor(uint8_t r, uint8_t g, uint8_t b);
  static void setBgColor(uint8_t r, uint8_t g, uint8_t b);
  static int  width();
  static int  height();
  static bool touched(int* x, int* y);
  static void centerText(int y, const char* str);
  static int  textWidth(const char* str);

  // Touch callback (called from loop when IRQ fires)
  static void setTouchCallback(int luaRef);

  // Backlight control
  static void setBacklight(bool on);

  // RGB LED
  static void setLed(uint8_t r, uint8_t g, uint8_t b);
};
