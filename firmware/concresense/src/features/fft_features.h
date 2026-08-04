#pragma once
#include <Arduino.h>

// Vibration feature extraction for the MPU-6050 burst.
//
// Radix-2 Cooley-Tukey, in-place, float. N is fixed at compile time and must be
// a power of two (IMU_BURST_SAMPLES = 256). At 200Hz that is a 1.28s window
// with 0.78Hz bins and a 100Hz Nyquist limit.
//
// Caveat worth stating plainly: the MPU-6050's digital low-pass is configured
// at 44Hz, so real spectral content above ~44Hz is attenuated in hardware
// before it is ever sampled. Any "dominant frequency" reported near or above
// that corner is shaped by the filter, not just the mix.

struct VibrationFeatures {
  float rms = NAN;             // RMS of the AC component, g
  float peakToPeak = NAN;      // g
  float dominantFreqHz = NAN;  // strongest non-DC bin
  float spectralEntropy = NAN; // 0 = pure tone, 1 = white noise
  float spectralCentroidHz = NAN;
  float dampingRatio = NAN;    // log-decrement estimate over the window
  bool valid = false;
};

// buf is modified in place (mean removed, then windowed).
// sampleRateHz should be the rate actually achieved by captureBurst(), not the
// nominal one — I2C contention makes them differ.
VibrationFeatures extractVibrationFeatures(float* buf, uint16_t n,
                                           float sampleRateHz);
