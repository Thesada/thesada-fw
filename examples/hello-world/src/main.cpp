// thesada-fw — hello-world example
// Minimal PlatformIO project to verify toolchain
// SPDX-License-Identifier: GPL-3.0-only

#include <Arduino.h>

void setup() {
  Serial.begin(115200);
}

void loop() {
  Serial.println("thesada-fw hello-world");
  delay(1000);
}
