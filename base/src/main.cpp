#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("Thesada hello-world");
}

void loop() {
  Serial.println("Thesada hello-world");
  delay(1000);
}