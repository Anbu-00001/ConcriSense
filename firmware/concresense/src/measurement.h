#pragma once
#include <Arduino.h>

#include "features/fft_features.h"
#include "physics/calibration.h"
#include "sensors/gps_neo6m.h"

// One complete measurement, produced on the sampling core and consumed on the
// inference core.
//
// Deliberately a flat, POD-ish struct with no pointers or heap members: it is
// passed BY VALUE through a FreeRTOS queue, which memcpy's it. A pointer here
// would cross a core boundary and become a lifetime bug that only shows up
// under load.
struct MeasurementRecord {
  uint32_t seq = 0;
  uint32_t uptimeMs = 0;

  // True when the sensor values were synthesised on-device rather than read
  // from hardware (the 'sim' console command). Travels all the way into the
  // published MQTT payload as data_source=simulated_onboard, so a simulated
  // reading can never be mistaken for a measured one downstream -- in the
  // dashboard, in the history, or in the audit PDF.
  bool simulated = false;

  // raw / measured
  float moistureMv = NAN;
  float tempC = NAN;
  float loadCounts = NAN;

  bool moistureValid = false;
  bool tempValid = false;
  bool loadValid = false;

  VibrationFeatures vib;
  DerivedProperties derived;

  GpsFix gps;

  // Assemble the model input vector in MODEL_FEATURE_ORDER.
  //
  // This ordering is duplicated from tinyml_model/train_classifier.py's
  // FEATURE_COLUMNS. There is no way to enforce that at compile time across
  // two languages, so it is asserted in the host tests instead -- a silent
  // reorder here would produce confident, completely wrong classifications.
  //
  // Returns false if any channel is missing, so the caller skips inference
  // rather than feeding NaN or zero into the network.
  bool toFeatureVector(float* out7) const {
    if (!moistureValid || !tempValid || !loadValid || !vib.valid) return false;
    if (isnan(derived.forceN)) return false;

    out7[0] = moistureMv;
    out7[1] = tempC;
    out7[2] = derived.forceN;
    out7[3] = vib.rms;
    out7[4] = vib.dominantFreqHz;
    out7[5] = vib.spectralEntropy;
    out7[6] = vib.dampingRatio;
    return true;
  }
};
