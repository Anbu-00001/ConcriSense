#include "imu_mpu6050.h"

#include <Wire.h>

#include "../config.h"

namespace {
constexpr uint8_t REG_SMPLRT_DIV = 0x19;
constexpr uint8_t REG_CONFIG = 0x1A;
constexpr uint8_t REG_ACCEL_CONFIG = 0x1C;
constexpr uint8_t REG_ACCEL_XOUT_H = 0x3B;
constexpr uint8_t REG_PWR_MGMT_1 = 0x6B;
constexpr uint8_t REG_WHO_AM_I = 0x75;

// +/-4g full scale: fresh-concrete vibration rarely exceeds ~2g, and 4g keeps
// headroom for the transient when the probe first contacts the mix without
// throwing away resolution the way 8g or 16g would.
constexpr uint8_t ACCEL_FS_4G = 0x08;
constexpr float ACCEL_LSB_PER_G_4G = 8192.0f;
}  // namespace

bool ImuMPU6050::writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool ImuMPU6050::readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;  // repeated start
  if (Wire.requestFrom(addr_, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

SensorStatus ImuMPU6050::begin() {
  // Try the AD0-low address first, then AD0-high. Breakout boards differ in
  // whether AD0 is pulled down on the PCB, so probing both saves a support
  // round-trip when the scanner shows 0x69 instead of 0x68.
  for (uint8_t candidate : {(uint8_t)ADDR_MPU6050, (uint8_t)0x69}) {
    addr_ = candidate;
    Wire.beginTransmission(addr_);
    if (Wire.endTransmission() != 0) continue;

    uint8_t id = 0;
    if (!readRegs(REG_WHO_AM_I, &id, 1)) continue;

    // WHO_AM_I masks off bit0 and bit7, so a genuine MPU-6050 reports 0x68
    // regardless of the AD0 strap. Clones (MPU-6500/9250) report 0x70/0x71 and
    // are register-compatible enough for what this project does, so they are
    // accepted but recorded.
    whoami_ = id;
    if (id == 0x68 || id == 0x70 || id == 0x71 || id == 0x72 || id == 0x73) {
      if (!writeReg(REG_PWR_MGMT_1, 0x00)) continue;  // wake from sleep
      delay(50);
      // PLL with X-axis gyro reference is more stable than the internal 8MHz
      // oscillator, which matters for consistent FFT bin spacing.
      writeReg(REG_PWR_MGMT_1, 0x01);
      delay(10);

      // DLPF bandwidth 44Hz => internal sample rate 1kHz. With SMPLRT_DIV=4
      // that divides to 200Hz, matching IMU_SAMPLE_RATE_HZ.
      //
      // Note the Nyquist consequence: a 44Hz low-pass on a 200Hz sampler means
      // real content above 44Hz is attenuated before it is ever digitised. The
      // docs' example "dominant frequency 45.2Hz" sits right at that corner.
      // Phase 2 widens the DLPF when spectral content above 44Hz is wanted.
      writeReg(REG_CONFIG, 0x03);
      writeReg(REG_SMPLRT_DIV, 4);
      writeReg(REG_ACCEL_CONFIG, ACCEL_FS_4G);
      delay(10);

      status_ = SensorStatus::OK;
      return status_;
    }
  }

  status_ = SensorStatus::ABSENT;
  return status_;
}

bool ImuMPU6050::readAccel(float& ax, float& ay, float& az) {
  if (status_ != SensorStatus::OK) return false;

  uint8_t b[6];
  if (!readRegs(REG_ACCEL_XOUT_H, b, 6)) return false;

  const int16_t rx = (int16_t)((b[0] << 8) | b[1]);
  const int16_t ry = (int16_t)((b[2] << 8) | b[3]);
  const int16_t rz = (int16_t)((b[4] << 8) | b[5]);

  ax = rx / ACCEL_LSB_PER_G_4G;
  ay = ry / ACCEL_LSB_PER_G_4G;
  az = rz / ACCEL_LSB_PER_G_4G;
  return true;
}

uint16_t ImuMPU6050::captureBurst(float* buf, uint16_t n, float& achievedHz) {
  achievedHz = NAN;
  if (status_ != SensorStatus::OK || buf == nullptr || n == 0) return 0;

  const uint32_t periodUs = 1000000UL / IMU_SAMPLE_RATE_HZ;
  uint16_t got = 0;

  const uint32_t t0 = micros();
  uint32_t next = t0;

  while (got < n) {
    float ax, ay, az;
    if (!readAccel(ax, ay, az)) break;

    // Magnitude rather than a single axis: the probe's orientation in the mix
    // is not controlled, so any one axis would encode how the operator happened
    // to hold it. Magnitude is orientation-invariant.
    buf[got++] = sqrtf(ax * ax + ay * ay + az * az);

    next += periodUs;
    // Busy-wait on the remainder. Sub-millisecond delays cannot be done with
    // vTaskDelay, and the burst is short (1.28s) so holding the core is fine.
    while ((int32_t)(micros() - next) < 0) {
    }
  }

  const uint32_t elapsed = micros() - t0;
  if (got > 1 && elapsed > 0) {
    achievedHz = (1000000.0f * got) / elapsed;
  }
  return got;
}
