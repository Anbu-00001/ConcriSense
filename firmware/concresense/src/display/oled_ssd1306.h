#pragma once
#include <Arduino.h>
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

  // Phase 3: post-measurement verdict view. `verdict` is the IS456 rule
  // result (compliance-authoritative -- see reportMeasurement()) and is
  // rendered large; `mlLine`/`wcLine`/`slumpLine` are pre-formatted detail
  // rows shown below it.
  void showVerdict(const char* verdict, const char* mlLine,
                    const char* wcLine, const char* slumpLine);

  // Shown instead of showVerdict() when calibration is incomplete, so the
  // "not classified yet" state is a live readout rather than a dead-end
  // "UNKNOWN" label repeating every cycle.
  void showLiveReadings(float tempC, bool tempValid, float loadCounts,
                        bool loadValid);

  // Live "searching" view for the fix-not-yet-acquired state -- the normal
  // indoor case, not an error. satsInView is real GPGSV telemetry (satellites
  // the module can see, reported well before a fix locks); elapsedMs is real
  // time since the GPS module started searching. No fabricated position or
  // bearing is drawn -- the radar sweep is a generic "searching" motif, not a
  // sky plot, since per-satellite azimuth/elevation is not parsed.
  void showGpsSearching(uint8_t satsInView, uint32_t elapsedMs);

  SensorStatus status() const { return status_; }

 private:
  void centered(const char* text, int16_t y, uint8_t size);

  Adafruit_SSD1306* d_ = nullptr;
  SensorStatus status_ = SensorStatus::ABSENT;
};
