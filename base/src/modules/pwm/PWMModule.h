// thesada-fw — PWMModule.h
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../../core/Module.h"

class PWMModule : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "PWMModule"; }
};
