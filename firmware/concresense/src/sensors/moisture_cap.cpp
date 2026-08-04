#include "moisture_cap.h"

#include "../config.h"

SensorStatus MoistureCapacitive::begin() {
  analogReadResolution(12);

  // Without this the ADC saturates at ~1.1V while the sensor swings to ~3.0V,
  // railing most of the useful moisture range to a constant 4095. This single
  // line is the difference between a working channel and a dead one.
  analogSetPinAttenuation(PIN_MOISTURE_AOUT, MOISTURE_ADC_ATTEN);

  delay(10);

  // GPIO34 is input-only with no internal pull-ups, so an unconnected pin
  // floats near rail and drifts. A sensor that is actually powered and driving
  // the pin parks somewhere in the middle of the range and stays put, so both
  // the level and its stability are checked.
  uint32_t mvSum = 0;
  uint32_t mvMin = UINT32_MAX, mvMax = 0;
  for (uint8_t i = 0; i < 16; i++) {
    const uint32_t mv = analogReadMilliVolts(PIN_MOISTURE_AOUT);
    mvSum += mv;
    if (mv < mvMin) mvMin = mv;
    if (mv > mvMax) mvMax = mv;
    delay(2);
  }
  const uint32_t mean = mvSum / 16;
  const uint32_t spread = mvMax - mvMin;

  // Pinned to either rail => nothing is driving the pin.
  if (mean < 60 || mean > 3200) {
    status_ = SensorStatus::ABSENT;
    return status_;
  }
  // Mid-scale but wandering hundreds of mV => floating pin picking up noise,
  // not a sensor holding a level.
  if (spread > 400) {
    status_ = SensorStatus::ABSENT;
    return status_;
  }

  status_ = SensorStatus::OK;
  return status_;
}

Reading MoistureCapacitive::readMilliVolts() {
  Reading r;
  if (status_ == SensorStatus::ABSENT) {
    r.status = SensorStatus::ABSENT;
    return r;
  }

  uint32_t sum = 0;
  for (uint16_t i = 0; i < MOISTURE_ADC_SAMPLES; i++) {
    sum += analogReadMilliVolts(PIN_MOISTURE_AOUT);
  }
  r.value = static_cast<float>(sum) / MOISTURE_ADC_SAMPLES;
  r.status = SensorStatus::OK;
  return r;
}

Reading MoistureCapacitive::readRawCounts() {
  Reading r;
  if (status_ == SensorStatus::ABSENT) {
    r.status = SensorStatus::ABSENT;
    return r;
  }

  uint32_t sum = 0;
  for (uint16_t i = 0; i < MOISTURE_ADC_SAMPLES; i++) {
    sum += analogRead(PIN_MOISTURE_AOUT);
  }
  r.value = static_cast<float>(sum) / MOISTURE_ADC_SAMPLES;
  r.status = SensorStatus::OK;
  return r;
}
