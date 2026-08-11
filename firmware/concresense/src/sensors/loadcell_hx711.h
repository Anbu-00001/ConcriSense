#pragma once
#include "../sensor_status.h"

// HX711 24-bit load cell amplifier, bit-banged.
//
// Written by hand rather than pulled from a library because the read loop needs
// a portMUX critical section: the HX711 powers down if SCK stays high for more
// than 60us, and a FreeRTOS task switch landing mid-read does exactly that.
// Most Arduino HX711 libraries do not guard against this.
class LoadCellHX711 {
 public:
  SensorStatus begin();

  // One raw 24-bit two's-complement conversion. Blocks until DOUT goes low or
  // HX711_READ_TIMEOUT_MS elapses.
  bool readRaw(int32_t& out);

  // Median-of-n raw reads. Median rather than mean: HX711 outliers from EMI are
  // large and one-sided, so a mean drags with them and a median does not.
  bool readRawMedian(uint8_t n, int32_t& out);

  // Zero the scale at the current resting load.
  SensorStatus tare(uint8_t samples = 15);

  // Calibrated force reading. Units follow whatever scaleFactor was set from,
  // so Phase 2 sets it in grams and converts to newtons for the physics model.
  Reading read();

  void setScale(float countsPerUnit) { scale_ = countsPerUnit; }
  float scale() const { return scale_; }
  int32_t offset() const { return offset_; }

  // Measured at bring-up rather than assumed. Most red HX711 breakouts strap
  // the RATE pin low for 10 SPS, but some clones ship at 80 SPS, and the
  // docs' "penetration force curve" assumes far more than 10 Hz.
  float measureSampleRateHz(uint16_t samples = 12);

  SensorStatus status() const { return status_; }

 private:
  int32_t offset_ = 0;
  float scale_ = 1.0f;
  SensorStatus status_ = SensorStatus::ABSENT;

  // Plausibility baseline for read(): an intermittent DT/SCK connection can
  // corrupt enough of a median-of-5 to produce a physically impossible jump
  // (see HX711_MAX_PLAUSIBLE_JUMP in config.h). Tracked separately from
  // status_ because the fault is transient and self-clearing, not a
  // permanent "wire fell off" condition.
  float lastGood_ = 0.0f;
  bool hasLastGood_ = false;

  // Consecutive-rejection counter. Comparing only against lastGood_ has no
  // way back if the true resting value genuinely moves (thermal settling, a
  // bump, or a connection fault that does NOT self-clear back to the old
  // baseline) -- every future reading would then look like a jump forever,
  // reporting the load cell as permanently invalid. After
  // HX711_MAX_REJECT_STREAK straight rejections, the latest reading is
  // accepted as a new baseline instead of staying stuck.
  uint8_t rejectStreak_ = 0;

  bool waitReady(uint32_t timeoutMs);
};
