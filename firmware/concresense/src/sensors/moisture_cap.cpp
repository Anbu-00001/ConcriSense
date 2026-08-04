#include "moisture_cap.h"

#include "../config.h"

SensorStatus MoistureCapacitive::begin() {
  analogReadResolution(12);

  // Without this the ADC saturates at ~1.1V while the sensor swings to ~3.0V,
  // railing most of the useful moisture range to a constant 4095. This single
  // line is the difference between a working channel and a dead one.
  analogSetPinAttenuation(PIN_MOISTURE_AOUT, MOISTURE_ADC_ATTEN);

  delay(10);

  // GPIO34 is input-only with no internal pull-ups, so an unconnected pin sits
  // at a rail. A sensor that is actually powered and driving the pin parks
  // mid-range and holds there, so level and stability are both checked.
  //
  // Presence is decided on RAW COUNTS, not millivolts. This is not a style
  // choice: at 11dB attenuation the ESP32's calibrated conversion has a floor
  // of roughly 140mV, so a pin reading a hard 0 counts still reports ~142mV.
  // Measured on this board with nothing connected: raw 0 -> 142mV. A
  // millivolt-based floor can therefore never detect a grounded/floating-low
  // pin, and the channel would report OK while feeding a constant into the
  // classifier -- exactly the silent-garbage failure this check exists to stop.
  uint32_t rawSum = 0;
  uint16_t rawMin = 4095, rawMax = 0;
  for (uint8_t i = 0; i < 16; i++) {
    const uint16_t raw = analogRead(PIN_MOISTURE_AOUT);
    rawSum += raw;
    if (raw < rawMin) rawMin = raw;
    if (raw > rawMax) rawMax = raw;
    delay(2);
  }
  const uint16_t rawMean = rawSum / 16;
  const uint16_t rawSpread = rawMax - rawMin;

  // Pinned to either rail => nothing is driving the pin.
  if (rawMean <= 8 || rawMean >= 4087) {
    status_ = SensorStatus::ABSENT;
    return status_;
  }
  // Mid-scale but wandering => floating pin picking up noise, not a sensor
  // holding a level. ~500 counts at 12 bits is ~0.4V of wander.
  if (rawSpread > 500) {
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
