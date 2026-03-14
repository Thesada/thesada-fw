// thesada-fw — WiFiManager.h
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

class WiFiManager {
public:
  static void begin();
  static void loop();
  static bool connected();
};
