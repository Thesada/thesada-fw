// thesada-fw — Module.h
// Base class for all modules
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

class Module {
public:
  virtual void begin() = 0;
  virtual void loop() = 0;
  virtual const char* name() = 0;
  virtual ~Module() {}
};
