#pragma once
#include "../sensor_status.h"

// Capacitive soil-moisture sensor v1.2 on ADC1.
//
// Deliberately NOT called "FC-28". FC-28 is the resistive two-prong module: it
// measures ionic conductivity, electrolyses and corrodes within minutes in the
// Ca2+/OH- rich pore solution of fresh cement, and is not described by the
// Lichtenecker dielectric mixture rule the project docs invoke. The capacitive
// v1.2 board (555-based, plates sealed under solder mask) is the correct part
// and the only one for which the docs' physics actually holds.
class MoistureCapacitive {
 public:
  SensorStatus begin();

  // Averaged calibrated reading. Uses analogReadMilliVolts(), which applies
  // this chip's eFuse-burned Vref instead of assuming a nominal 1100mV.
  Reading readMilliVolts();

  // Raw averaged 12-bit counts, kept for dataset logging: the TinyML model is
  // trained on whatever the board actually produces, so the uncorrected value
  // is worth recording alongside the calibrated one.
  Reading readRawCounts();

  SensorStatus status() const { return status_; }

 private:
  SensorStatus status_ = SensorStatus::ABSENT;
};
