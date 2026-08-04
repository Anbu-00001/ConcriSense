#include "oled_ssd1306.h"

#include <Wire.h>

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

void OledDisplay::splash(const char* fwVersion) {
  if (status_ != SensorStatus::OK) return;
  d_->clearDisplay();
  d_->setTextSize(2);
  d_->setCursor(0, 8);
  d_->println(F("ConcreSense"));
  d_->setTextSize(1);
  d_->setCursor(0, 34);
  d_->print(F("fw "));
  d_->println(fwVersion);
  d_->setCursor(0, 46);
  d_->println(F("Phase 1 bring-up"));
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
