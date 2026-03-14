// thesada-fw — ModuleRegistry.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "ModuleRegistry.h"
#include "../../config.h"

#ifdef ENABLE_TEMPERATURE
#include "../modules/temperature/TemperatureModule.h"
#endif
#ifdef ENABLE_CURRENT
#include "../modules/current/CurrentModule.h"
#endif
#ifdef ENABLE_PWM
#include "../modules/pwm/PWMModule.h"
#endif
#ifdef ENABLE_CELLULAR
#include "../modules/cellular/CellularModule.h"
#endif

std::vector<Module*> ModuleRegistry::_modules;

void ModuleRegistry::add(Module* module) {
  _modules.push_back(module);
}

void ModuleRegistry::begin() {
#ifdef ENABLE_TEMPERATURE
  add(new TemperatureModule());
#endif
#ifdef ENABLE_CURRENT
  add(new CurrentModule());
#endif
#ifdef ENABLE_PWM
  add(new PWMModule());
#endif
#ifdef ENABLE_CELLULAR
  add(new CellularModule());
#endif
  for (auto* m : _modules) m->begin();
}

void ModuleRegistry::loop() {
  for (auto* m : _modules) m->loop();
}
