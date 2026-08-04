// Shared status vocabulary for every ConcreSense driver.
//
// Phase 1 runs on a board where most sensors are not wired yet, so "absent" has
// to be a first-class, non-fatal outcome. Every begin() reports which of these
// happened instead of blocking or faulting, and the bring-up report prints the
// result per subsystem. That makes the board incrementally testable as parts
// arrive rather than all-or-nothing.

#pragma once
#include <Arduino.h>

enum class SensorStatus : uint8_t {
  OK = 0,        // present and returning plausible data
  ABSENT,        // did not respond — almost always "not wired up yet"
  TIMEOUT,       // responded once but stopped (loose wire, brownout)
  OUT_OF_RANGE,  // responding, but the value is not physically believable
  ERROR          // present and actively reporting a fault
};

inline const char* statusName(SensorStatus s) {
  switch (s) {
    case SensorStatus::OK: return "OK";
    case SensorStatus::ABSENT: return "ABSENT";
    case SensorStatus::TIMEOUT: return "TIMEOUT";
    case SensorStatus::OUT_OF_RANGE: return "OUT_OF_RANGE";
    case SensorStatus::ERROR: return "ERROR";
  }
  return "?";
}

// A reading is only meaningful when status == OK. Callers must check, and the
// feature extractor in Phase 2 refuses to build a vector from non-OK channels
// rather than silently feeding zeros into the classifier.
struct Reading {
  float value = NAN;
  SensorStatus status = SensorStatus::ABSENT;

  bool valid() const { return status == SensorStatus::OK && !isnan(value); }
};
