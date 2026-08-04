#pragma once
#include <DallasTemperature.h>
#include <OneWire.h>

#include "../sensor_status.h"

// DS18B20 on OneWire. Needs an external 4.7k pull-up from DQ to 3V3 — the ESP32
// internal pull-up is far too weak for OneWire timing and will give either no
// devices found or intermittent CRC failures.
class TempDS18B20 {
 public:
  SensorStatus begin();
  Reading readCelsius();

  uint8_t deviceCount() const { return count_; }
  SensorStatus status() const { return status_; }

 private:
  OneWire* wire_ = nullptr;
  DallasTemperature* dallas_ = nullptr;
  uint8_t count_ = 0;
  SensorStatus status_ = SensorStatus::ABSENT;
};
