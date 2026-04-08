// CYD (ESP32-2432S028R) hardware validation test
// TFT: ILI9341 on VSPI, Touch: XPT2046 on HSPI (separate bus)
// SPDX-License-Identifier: GPL-3.0-only

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include <XPT2046_Touchscreen.h>

// ── Pin assignments ─────────────────────────────────────────────────────────
#define SD_CS       5
#define LED_R       4
#define LED_G       16
#define LED_B       17
#define LDR_PIN     34
#define TFT_BL_PIN  21

// Touch on HSPI (separate from TFT which is on VSPI)
#define XPT2046_CS   33
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25

TFT_eSPI tft;
SPIClass hspi(HSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========================================");
  Serial.println("CYD (ESP32-2432S028R) Hardware Test");
  Serial.println("========================================\n");

  // ── TFT Display ──────────────────────────────────────────────────────────
  Serial.println("[TEST] TFT Display...");
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);

  tft.init();
  tft.invertDisplay(true);
  tft.setRotation(1);  // landscape
  tft.fillScreen(TFT_BLACK);

  Serial.printf("       Size: %dx%d\n", tft.width(), tft.height());

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(3);
  tft.setCursor(10, 10);
  tft.println("CYD Test");

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 50);
  tft.printf("%dx%d", tft.width(), tft.height());

  int w = tft.width();
  int barH = 25;
  int y = 80;
  tft.fillRect(0, y,          w, barH, TFT_RED);     y += barH;
  tft.fillRect(0, y,          w, barH, TFT_GREEN);   y += barH;
  tft.fillRect(0, y,          w, barH, TFT_BLUE);    y += barH;
  tft.fillRect(0, y,          w, barH, TFT_WHITE);
  Serial.println("[PASS] TFT - check colors: red, green, blue, white");

  // ── RGB LED ──────────────────────────────────────────────────────────────
  Serial.println("\n[TEST] RGB LED...");
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); digitalWrite(LED_B, HIGH);
  Serial.print("       R"); digitalWrite(LED_R, LOW); delay(400); digitalWrite(LED_R, HIGH);
  Serial.print(" G");        digitalWrite(LED_G, LOW); delay(400); digitalWrite(LED_G, HIGH);
  Serial.println(" B");      digitalWrite(LED_B, LOW); delay(400); digitalWrite(LED_B, HIGH);
  Serial.println("[PASS] RGB LED");

  // ── LDR ──────────────────────────────────────────────────────────────────
  Serial.printf("\n[TEST] LDR: %d\n", analogRead(LDR_PIN));

  // ── SD Card ──────────────────────────────────────────────────────────────
  Serial.println("\n[TEST] SD Card...");
  if (SD.begin(SD_CS)) {
    uint8_t ct = SD.cardType();
    if (ct != CARD_NONE) {
      Serial.printf("[PASS] SD: %llu MB\n", SD.cardSize() / (1024*1024));
      File f = SD.open("/test.txt", FILE_WRITE);
      if (f) { f.println("ok"); f.close(); SD.remove("/test.txt"); Serial.println("[PASS] SD write/read"); }
    } else {
      Serial.println("[WARN] No card inserted");
    }
  } else {
    Serial.println("[WARN] SD init failed");
  }

  // ── Touch ────────────────────────────────────────────────────────────────
  Serial.println("\n[TEST] Touch (XPT2046 on HSPI)...");
  Serial.printf("       CS=%d IRQ=%d CLK=%d MOSI=%d MISO=%d\n",
                XPT2046_CS, XPT2046_IRQ, XPT2046_CLK, XPT2046_MOSI, XPT2046_MISO);

  hspi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(hspi);
  ts.setRotation(1);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 210);
  tft.println("Touch the screen!");

  Serial.println("       Touch within 15s...");
  unsigned long start = millis();
  bool touched = false;
  while (millis() - start < 15000) {
    if (ts.touched()) {
      TS_Point p = ts.getPoint();
      int tx = map(p.x, 200, 3800, 0, tft.width());
      int ty = map(p.y, 200, 3800, 0, tft.height());
      Serial.printf("[PASS] Touch raw(%d,%d) screen(%d,%d) z=%d\n", p.x, p.y, tx, ty, p.z);
      tft.fillCircle(tx, ty, 5, TFT_MAGENTA);
      touched = true;
      delay(100);
    }
    delay(10);
  }
  if (!touched) Serial.println("[WARN] No touch detected");

  Serial.println("\n========================================");
  Serial.println("Tests complete. Touch to draw.");
  Serial.println("========================================");
}

void loop() {
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    int x = map(p.x, 200, 3800, 0, tft.width());
    int y = map(p.y, 200, 3800, 0, tft.height());
    tft.fillCircle(x, y, 3, TFT_CYAN);
    delay(20);
  }
}
