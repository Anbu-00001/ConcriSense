#include "loadcell_hx711.h"

#include "../config.h"

// Guards the 25-pulse shift-out. Without it a task switch can hold SCK high
// past the HX711's 60us power-down threshold and the conversion is lost or
// silently corrupted.
static portMUX_TYPE hx711Mux = portMUX_INITIALIZER_UNLOCKED;

SensorStatus LoadCellHX711::begin() {
  pinMode(PIN_HX711_SCK, OUTPUT);
  pinMode(PIN_HX711_DOUT, INPUT);
  digitalWrite(PIN_HX711_SCK, LOW);

  // Power-cycle: SCK high >60us powers down, dropping it low wakes and resets
  // the channel/gain selection to the default (channel A, gain 128).
  digitalWrite(PIN_HX711_SCK, HIGH);
  delayMicroseconds(80);
  digitalWrite(PIN_HX711_SCK, LOW);
  delay(10);

  // With no HX711 wired, DOUT floats. It usually floats high (which reads as
  // "busy forever") but can float low and mimic a ready chip, so a successful
  // read alone is not proof of presence — the value has to be believable too.
  int32_t probe;
  if (!readRaw(probe)) {
    status_ = SensorStatus::ABSENT;
    return status_;
  }

  // A real HX711 with a load cell attached sits well inside the 24-bit range.
  // Saturated rails mean a floating input or a disconnected/shorted bridge.
  if (probe == 0 || probe <= -8388600L || probe >= 8388600L) {
    status_ = SensorStatus::OUT_OF_RANGE;
    return status_;
  }

  offset_ = probe;
  scale_ = HX711_DEFAULT_SCALE;
  status_ = SensorStatus::OK;
  return status_;
}

bool LoadCellHX711::waitReady(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (digitalRead(PIN_HX711_DOUT) == HIGH) {
    if (millis() - start > timeoutMs) return false;
    delay(1);  // yields to the scheduler; safe because SCK is low here
  }
  return true;
}

bool LoadCellHX711::readRaw(int32_t& out) {
  if (!waitReady(HX711_READ_TIMEOUT_MS)) return false;

  uint32_t value = 0;

  // Everything from here to the end of the 25th pulse must not be preempted.
  taskENTER_CRITICAL(&hx711Mux);
  for (uint8_t i = 0; i < 24; i++) {
    digitalWrite(PIN_HX711_SCK, HIGH);
    delayMicroseconds(HX711_SCK_SETTLE_US);  // ESP32 toggles faster than the
                                             // HX711's minimum pulse width
    value = (value << 1) | (digitalRead(PIN_HX711_DOUT) == HIGH ? 1u : 0u);
    digitalWrite(PIN_HX711_SCK, LOW);
    delayMicroseconds(HX711_SCK_SETTLE_US);
  }
  // A 25th pulse selects channel A at gain 128 for the next conversion.
  digitalWrite(PIN_HX711_SCK, HIGH);
  delayMicroseconds(HX711_SCK_SETTLE_US);
  digitalWrite(PIN_HX711_SCK, LOW);
  taskEXIT_CRITICAL(&hx711Mux);

  // Sign-extend the 24-bit two's-complement result into 32 bits.
  if (value & 0x800000UL) value |= 0xFF000000UL;
  out = static_cast<int32_t>(value);
  return true;
}

bool LoadCellHX711::readRawMedian(uint8_t n, int32_t& out) {
  if (n == 0) return false;
  if (n > 31) n = 31;

  int32_t buf[31];
  uint8_t got = 0;
  for (uint8_t i = 0; i < n; i++) {
    int32_t v;
    if (readRaw(v)) buf[got++] = v;
  }
  if (got == 0) return false;

  // Insertion sort — n is tiny, so this beats anything cleverer.
  for (uint8_t i = 1; i < got; i++) {
    const int32_t key = buf[i];
    int8_t j = i - 1;
    while (j >= 0 && buf[j] > key) {
      buf[j + 1] = buf[j];
      j--;
    }
    buf[j + 1] = key;
  }
  out = buf[got / 2];
  return true;
}

SensorStatus LoadCellHX711::tare(uint8_t samples) {
  int32_t median;
  if (!readRawMedian(samples, median)) {
    status_ = SensorStatus::TIMEOUT;
    return status_;
  }
  offset_ = median;
  status_ = SensorStatus::OK;
  return status_;
}

Reading LoadCellHX711::read() {
  Reading r;
  // Gate on OK, not just on ABSENT. A chip in OUT_OF_RANGE clocks out words
  // happily, so an ABSENT-only check would return a confident 0.00 from a
  // disconnected bridge -- a value indistinguishable from a real zero load.
  if (status_ != SensorStatus::OK) {
    r.status = status_;
    return r;
  }

  int32_t raw;
  if (!readRawMedian(5, raw)) {
    r.status = SensorStatus::TIMEOUT;
    status_ = SensorStatus::TIMEOUT;
    return r;
  }

  r.value = static_cast<float>(raw - offset_) / scale_;
  r.status = SensorStatus::OK;
  return r;
}

float LoadCellHX711::measureSampleRateHz(uint16_t samples) {
  if (status_ == SensorStatus::ABSENT) return NAN;

  int32_t discard;
  if (!readRaw(discard)) return NAN;  // align to a conversion boundary

  const uint32_t start = millis();
  uint16_t got = 0;
  for (uint16_t i = 0; i < samples; i++) {
    if (!readRaw(discard)) break;
    got++;
  }
  const uint32_t elapsed = millis() - start;
  if (got == 0 || elapsed == 0) return NAN;

  return (1000.0f * got) / elapsed;
}
