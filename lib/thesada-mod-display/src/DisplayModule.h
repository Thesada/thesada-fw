// thesada-fw - DisplayModule.h
// SSD1306 OLED display - hardware init only. All rendering via Lua bindings.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <Module.h>

class DisplayModule : public Module {
public:
  const char* name() override { return "display"; }
  void begin() override;
  void loop() override;
  void status(ShellOutput out) override;

  // Static methods called from Lua bindings
  static bool ready();
  static void clear();
  static void text(int x, int y, const char* str);
  static void line(int x1, int y1, int x2, int y2);
  static void rect(int x, int y, int w, int h);
  static void fillRect(int x, int y, int w, int h);
  static void show();
  static void setFont(const char* size);
};
