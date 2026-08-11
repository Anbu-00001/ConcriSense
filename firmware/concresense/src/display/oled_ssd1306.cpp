#include "oled_ssd1306.h"

#include <Wire.h>
#include <math.h>

#include "../config.h"

SensorStatus OledDisplay::begin() {
  // Probe before constructing: Adafruit_SSD1306::begin() with SSD1306_SWITCHCAPVCC
  // on an absent panel can sit in retry loops, and Phase 1 must never hang on a
  // part that has not been bought yet.
  Wire.beginTransmission(ADDR_SSD1306);
  if (Wire.endTransmission() != 0) {
    status_ = SensorStatus::ABSENT;
    return status_;
  }

  d_ = new Adafruit_SSD1306(128, 64, &Wire, -1);
  if (!d_->begin(SSD1306_SWITCHCAPVCC, ADDR_SSD1306, false, false)) {
    delete d_;
    d_ = nullptr;
    status_ = SensorStatus::ERROR;
    return status_;
  }

  d_->clearDisplay();
  d_->setTextColor(SSD1306_WHITE);
  d_->setTextSize(1);
  d_->display();

  status_ = SensorStatus::OK;
  return status_;
}

void OledDisplay::centered(const char* text, int16_t y, uint8_t size) {
  int16_t x1, y1;
  uint16_t w, h;
  d_->setTextSize(size);
  d_->getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  d_->setCursor((128 - (int16_t)w) / 2, y);
  d_->print(text);
}

void OledDisplay::splash(const char* fwVersion) {
  if (status_ != SensorStatus::OK) return;

  // Scan-line reveal -- accumulating horizontal lines top to bottom, like a
  // CRT power-on. Purely cosmetic and bounded (~300ms), runs once at boot.
  d_->clearDisplay();
  for (int16_t y = 0; y <= 63; y += 4) {
    d_->drawFastHLine(0, y, 128, SSD1306_WHITE);
    d_->display();
  }
  delay(120);

  d_->clearDisplay();
  d_->drawRect(0, 0, 128, 64, SSD1306_WHITE);
  d_->drawRect(2, 2, 124, 60, SSD1306_WHITE);
  centered("ConcreSense", 9, 2);
  centered("IS 456:2000 SCREENING", 32, 1);
  d_->drawLine(14, 44, 114, 44, SSD1306_WHITE);
  char fwLine[24];
  snprintf(fwLine, sizeof(fwLine), "fw %s", fwVersion);
  centered(fwLine, 51, 1);
  d_->display();
}

void OledDisplay::showBringupLine(const char* label, const char* status) {
  if (status_ != SensorStatus::OK) return;
  d_->clearDisplay();
  d_->setTextSize(1);
  d_->setCursor(0, 0);
  d_->println(F("Bring-up"));
  d_->setCursor(0, 20);
  d_->println(label);
  d_->setTextSize(2);
  d_->setCursor(0, 36);
  d_->println(status);
  d_->display();
}

void OledDisplay::showStatusGrid(const char* lines[], uint8_t n) {
  if (status_ != SensorStatus::OK) return;
  d_->clearDisplay();
  d_->setTextSize(1);
  for (uint8_t i = 0; i < n && i < 8; i++) {
    d_->setCursor(0, i * 8);
    d_->println(lines[i]);
  }
  d_->display();
}

void OledDisplay::message(const char* title, const char* body) {
  if (status_ != SensorStatus::OK) return;
  d_->clearDisplay();
  d_->setTextSize(1);
  d_->setCursor(0, 0);
  d_->println(title);
  d_->setCursor(0, 16);
  d_->println(body);
  d_->display();
}

void OledDisplay::showVerdict(const char* verdict, const char* mlLine,
                               const char* wcLine, const char* slumpLine) {
  if (status_ != SensorStatus::OK) return;
  d_->clearDisplay();

  d_->setTextSize(1);
  d_->setCursor(0, 0);
  d_->print(F("CONCRESENSE VERDICT"));
  // Live dot -- tied to real millis(), not decorative-only.
  if ((millis() / 500) % 2 == 0) d_->fillCircle(124, 3, 2, SSD1306_WHITE);
  d_->drawLine(0, 9, 127, 9, SSD1306_WHITE);

  // Verdict word, large, with a glyph so GOOD/MARGINAL/REJECT is readable
  // from across a room on camera, not just up close.
  centered(verdict, 14, 2);

  const int16_t iconY = 20;
  if (strcmp(verdict, "GOOD") == 0) {
    // Checkmark, left of the word -- position is approximate since the
    // word width varies; a fixed left icon keeps layout simple and stable.
    d_->drawLine(4, iconY + 6, 8, iconY + 10, SSD1306_WHITE);
    d_->drawLine(8, iconY + 10, 15, iconY, SSD1306_WHITE);
  } else if (strcmp(verdict, "MARGINAL") == 0) {
    d_->drawTriangle(9, iconY, 3, iconY + 10, 15, iconY + 10, SSD1306_WHITE);
    d_->drawFastVLine(9, iconY + 3, 4, SSD1306_WHITE);
    d_->drawPixel(9, iconY + 8, SSD1306_WHITE);
  } else if (strcmp(verdict, "REJECT") == 0) {
    d_->drawLine(4, iconY, 14, iconY + 10, SSD1306_WHITE);
    d_->drawLine(4, iconY + 10, 14, iconY, SSD1306_WHITE);
  }

  d_->drawLine(0, 33, 127, 33, SSD1306_WHITE);

  d_->setTextSize(1);
  d_->setCursor(0, 37);
  d_->println(mlLine);
  d_->setCursor(0, 47);
  d_->println(wcLine);
  d_->setCursor(0, 56);
  d_->println(slumpLine);

  d_->display();
}

void OledDisplay::showLiveReadings(float tempC, bool tempValid,
                                    float loadCounts, bool loadValid) {
  if (status_ != SensorStatus::OK) return;
  d_->clearDisplay();

  d_->setTextSize(1);
  d_->setCursor(0, 0);
  d_->print(F("LIVE SENSORS"));
  if ((millis() / 500) % 2 == 0) d_->fillCircle(124, 3, 2, SSD1306_WHITE);
  d_->drawLine(0, 9, 127, 9, SSD1306_WHITE);

  d_->setCursor(0, 14);
  d_->print(F("temperature"));
  d_->setTextSize(2);
  d_->setCursor(0, 24);
  if (tempValid) {
    char t[12];
    snprintf(t, sizeof(t), "%.1fC", tempC);
    d_->print(t);
  } else {
    d_->print(F("--"));
  }

  d_->setTextSize(1);
  d_->setCursor(0, 44);
  d_->print(F("load cell (raw counts)"));
  d_->setTextSize(2);
  d_->setCursor(0, 54);
  if (loadValid) {
    char l[16];
    snprintf(l, sizeof(l), "%.0f", loadCounts);
    d_->print(l);
  } else {
    d_->print(F("--"));
  }

  d_->display();
}

void OledDisplay::showGpsSearching(uint8_t satsInView, uint32_t elapsedMs) {
  if (status_ != SensorStatus::OK) return;
  d_->clearDisplay();

  d_->setTextSize(1);
  d_->setCursor(0, 0);
  d_->print(F("GPS SEARCHING"));
  if ((millis() / 500) % 2 == 0) d_->fillCircle(124, 3, 2, SSD1306_WHITE);
  d_->drawLine(0, 9, 127, 9, SSD1306_WHITE);

  // Radar sweep -- a generic "searching" motif, not a sky plot: per-satellite
  // azimuth/elevation is not parsed, so no fabricated bearing is ever drawn.
  // The sweep angle is driven by real millis(), so it visibly moves while
  // waiting instead of sitting static.
  const int16_t cx = 22, cy = 41, r = 18;
  d_->drawCircle(cx, cy, r, SSD1306_WHITE);
  d_->drawCircle(cx, cy, r * 2 / 3, SSD1306_WHITE);
  d_->drawCircle(cx, cy, r / 3, SSD1306_WHITE);
  const float angle = (millis() % 2400) / 2400.0f * 2.0f * (float)PI;
  const int16_t ex = cx + (int16_t)(r * cosf(angle));
  const int16_t ey = cy + (int16_t)(r * sinf(angle));
  d_->drawLine(cx, cy, ex, ey, SSD1306_WHITE);

  // Real numbers, right side: GPGSV satellite-in-view count and elapsed
  // search time, both genuine telemetry from the module.
  d_->setTextSize(1);
  d_->setCursor(50, 16);
  d_->print(F("sats in view"));
  d_->setTextSize(2);
  d_->setCursor(50, 26);
  d_->print(satsInView);

  d_->setTextSize(1);
  d_->setCursor(50, 46);
  d_->print(F("searching"));
  d_->setCursor(50, 55);
  char e[16];
  const uint32_t s = elapsedMs / 1000;
  snprintf(e, sizeof(e), "%lum%02lus", (unsigned long)(s / 60),
           (unsigned long)(s % 60));
  d_->print(e);

  d_->display();
}
