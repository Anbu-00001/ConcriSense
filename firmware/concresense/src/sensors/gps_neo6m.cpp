#include "gps_neo6m.h"

#include "../config.h"

SensorStatus GpsNeo6M::begin(uint32_t detectTimeoutMs) {
  Serial2.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  gsvSatsInView_.begin(gps_, "GPGSV", 3);
  beginMs_ = millis();

  // Presence is decided purely on whether NMEA bytes arrive, never on whether a
  // fix is acquired. The module talks immediately on power-up even with no sky
  // view, so bytes-but-no-fix is a working module indoors, while silence means
  // it is unpowered or the TX/RX pair is swapped.
  const uint32_t start = millis();
  while (millis() - start < detectTimeoutMs) {
    while (Serial2.available()) {
      gps_.encode(Serial2.read());
      sawNmea_ = true;
    }
    delay(10);
  }

  status_ = sawNmea_ ? SensorStatus::OK : SensorStatus::ABSENT;
  return status_;
}

void GpsNeo6M::poll() {
  while (Serial2.available()) {
    gps_.encode(Serial2.read());
    sawNmea_ = true;
  }
}

GpsFix GpsNeo6M::fix() {
  GpsFix f;
  if (!sawNmea_) return f;

  f.satellites = gps_.satellites.isValid() ? gps_.satellites.value() : 0;
  f.hdop = gps_.hdop.isValid() ? gps_.hdop.hdop() : 0.0f;
  f.satellitesInView = gsvSatsInView_.isValid() ? atoi(gsvSatsInView_.value()) : 0;
  f.searchElapsedMs = millis() - beginMs_;

  if (gps_.location.isValid()) {
    f.latitude = gps_.location.lat();
    f.longitude = gps_.location.lng();

    // TinyGPS++ keeps reporting the last known location after the fix is lost,
    // so age is checked explicitly. HDOP above ~20 is a fix too poor to
    // geo-tag a compliance record with.
    const bool fresh = gps_.location.age() < 5000;
    const bool usable = f.hdop > 0.0f && f.hdop < 20.0f;
    f.valid = fresh && usable;
  }

  if (gps_.date.isValid() && gps_.time.isValid() && gps_.date.year() > 2000) {
    snprintf(f.isoTime, sizeof(f.isoTime), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             gps_.date.year(), gps_.date.month(), gps_.date.day(),
             gps_.time.hour(), gps_.time.minute(), gps_.time.second());
  }

  return f;
}
