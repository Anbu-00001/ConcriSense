#pragma once
#include "../sensor_status.h"

// MPU-6050 driven through raw I2C register access rather than a vendor library.
//
// The 200Hz burst in Phase 2 reads 256 consecutive samples with a hard timing
// budget; going straight at the registers avoids the per-call float conversion
// and object overhead of the Adafruit stack, and lets the burst loop own its
// own timing. It also drops a dependency.
class ImuMPU6050 {
 public:
  SensorStatus begin();

  // Single 3-axis acceleration sample in g.
  bool readAccel(float& ax, float& ay, float& az);

  // Fills buf with `n` acceleration-magnitude samples at IMU_SAMPLE_RATE_HZ.
  // Returns samples actually captured. Reports the achieved rate so Phase 2's
  // FFT can use the true bin spacing instead of the nominal one — I2C
  // contention with the OLED on the shared bus makes these differ.
  uint16_t captureBurst(float* buf, uint16_t n, float& achievedHz);

  uint8_t whoAmI() const { return whoami_; }
  SensorStatus status() const { return status_; }

 private:
  uint8_t addr_ = 0;
  uint8_t whoami_ = 0;
  SensorStatus status_ = SensorStatus::ABSENT;

  bool writeReg(uint8_t reg, uint8_t val);
  bool readRegs(uint8_t reg, uint8_t* buf, uint8_t len);
};
