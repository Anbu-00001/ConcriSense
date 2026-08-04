#pragma once
#include <Adafruit_SSD1306.h>

#include "../sensor_status.h"

// SSD1306 128x64 on the shared I2C bus.
//
// Bus contention is a live concern: a full frame push is ~1KB over the same
// wires the MPU-6050 is sampled on. At 400kHz that is ~20ms, which would drop
// roughly four samples out of a 200Hz burst. Phase 2 therefore quiesces the
// display for the duration of a burst rather than interleaving.
class OledDisplay {
 public:
  SensorStatus begin();

  void splash(const char* fwVersion);
  void showBringupLine(const char* label, const char* status);

  // Phase 1 diagnostic view. The classification UI arrives in Phase 3.
  void showStatusGrid(const char* lines[], uint8_t n);

  void message(const char* title, const char* body);

  SensorStatus status() const { return status_; }

 private:
  Adafruit_SSD1306* d_ = nullptr;
  SensorStatus status_ = SensorStatus::ABSENT;
};
