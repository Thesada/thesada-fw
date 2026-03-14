// thesada-fw — CellularModule.h
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../../core/Module.h"

class CellularModule : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "CellularModule"; }
};
