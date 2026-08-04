#include "temp_ds18b20.h"

#include "../config.h"

SensorStatus TempDS18B20::begin() {
  wire_ = new OneWire(PIN_DS18B20_DQ);
  dallas_ = new DallasTemperature(wire_);
  dallas_->begin();

  count_ = dallas_->getDeviceCount();
  if (count_ == 0) {
    status_ = SensorStatus::ABSENT;
    return status_;
  }

  // 12-bit: 0.0625degC resolution, ~750ms conversion. Concrete hydration is
  // slow enough that resolution matters more than conversion speed here.
  dallas_->setResolution(12);

  // Blocking waits inside a conversion would stall the sampling task, so the
  // library is told not to block and the caller paces the requests instead.
  dallas_->setWaitForConversion(true);  // Phase 1 is single-threaded; Phase 3
                                        // switches this off and polls.

  status_ = SensorStatus::OK;
  return status_;
}

Reading TempDS18B20::readCelsius() {
  Reading r;
  if (status_ == SensorStatus::ABSENT || dallas_ == nullptr) {
    r.status = SensorStatus::ABSENT;
    return r;
  }

  dallas_->requestTemperatures();
  const float c = dallas_->getTempCByIndex(0);

  // The library returns -127 for a disconnected device and 85 is the sensor's
  // power-on reset value — 85 exactly, on the first read, means the conversion
  // never actually ran.
  if (c == DEVICE_DISCONNECTED_C) {
    r.status = SensorStatus::TIMEOUT;
    status_ = SensorStatus::TIMEOUT;
    return r;
  }
  if (c < -40.0f || c > 125.0f) {
    r.status = SensorStatus::OUT_OF_RANGE;
    return r;
  }

  r.value = c;
  r.status = SensorStatus::OK;
  return r;
}
