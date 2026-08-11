#pragma once
#include <TinyGPS++.h>

#include "../sensor_status.h"

struct GpsFix {
  double latitude = 0.0;
  double longitude = 0.0;
  float hdop = 0.0f;
  uint8_t satellites = 0;
  bool valid = false;      // a real 3D fix with plausible HDOP
  char isoTime[25] = "";   // UTC, ISO-8601

  // Real telemetry that exists even with no fix: the NEO-6M reports GPGSV
  // (satellites in view/tracked) the moment it sees any sky at all, well
  // before it can lock a usable position. Indoors this is usually 0, but
  // near a window it is often 2-4 -- true signal, not a fabricated number.
  uint8_t satellitesInView = 0;
  uint32_t searchElapsedMs = 0;  // time since the module started searching
};

// NEO-6M on hardware UART2.
//
// Two facts drive this design: the module needs 27s+ for a cold start, and it
// will never get a fix indoors. So "no fix" is the normal indoor case, not an
// error, and the rest of the pipeline must not block waiting for one. The
// driver distinguishes "module absent" (no NMEA bytes at all) from "module
// present, searching" (NMEA flowing, no fix yet) — those need different fixes
// from whoever is holding the board.
class GpsNeo6M {
 public:
  SensorStatus begin(uint32_t detectTimeoutMs = 3000);

  // Non-blocking: drains whatever UART bytes are waiting into the parser.
  // Call frequently; the NEO-6M emits a burst every second at 9600 baud and
  // the hardware FIFO will overflow if it is left unattended.
  void poll();

  // Non-const because TinyGPS++'s accessors are themselves non-const: reading
  // a field clears its "updated" flag, so querying mutates the parser.
  GpsFix fix();

  bool moduleDetected() const { return sawNmea_; }
  uint32_t sentencesParsed() { return gps_.sentencesWithFix(); }
  uint32_t checksumErrors() { return gps_.failedChecksum(); }
  SensorStatus status() const { return status_; }

 private:
  TinyGPSPlus gps_;
  TinyGPSCustom gsvSatsInView_;  // GPGSV field 3: total satellites in view
  uint32_t beginMs_ = 0;
  bool sawNmea_ = false;
  SensorStatus status_ = SensorStatus::ABSENT;
};
