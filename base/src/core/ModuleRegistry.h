// thesada-fw — ModuleRegistry.h
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Module.h"
#include <vector>

class ModuleRegistry {
public:
  static void add(Module* module);
  static void begin();
  static void loop();
private:
  static std::vector<Module*> _modules;
};
