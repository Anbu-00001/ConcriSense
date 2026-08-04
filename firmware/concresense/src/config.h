// ConcreSense — central hardware configuration
//
// Board verified over USB on 2026-08-04:
//   ESP32-D0WD-V3 rev v3.1 | dual core 240MHz | 4MB flash | no PSRAM
//   MAC c0:cd:d6:ce:4a:50  | "Vref calibration in eFuse" present
//
// The eFuse Vref matters: it lets analogReadMilliVolts() return a properly
// calibrated voltage instead of assuming a nominal 1100mV reference that can
// really be anywhere from 1000-1200mV part to part.
//
// No PSRAM matters too: GPIO16/17 are only free for UART2 because this is a
// WROOM. On a WROVER those pins belong to the PSRAM and the GPS dies silently.

#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------- identity
#define DEVICE_ID "CONCRESENSE_ESP32_001"
#define FW_VERSION "0.1.0-phase1"
#define COMPLIANCE_STANDARD "IS 456:2000"

// ------------------------------------------------------------------- pins
// I2C bus 0 — shared by SSD1306 OLED and MPU-6050 (distinct addresses)
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define I2C_FREQ_HZ 400000UL  // 400kHz: the 128x64 frame push is ~1KB, and at
                              // 100kHz it would stall IMU sampling badly

#define ADDR_SSD1306 0x3C
#define ADDR_MPU6050 0x68  // 0x69 if AD0 is strapped high

// Capacitive soil-moisture sensor v1.2 -> ADC1 (input-only pin, no pull-ups)
//
// NOTE: the project docs call this an "FC-28". That is wrong and it matters.
// FC-28 is the RESISTIVE two-prong module; it measures ionic conductivity,
// corrodes within minutes in cement pore solution, and is NOT described by the
// Lichtenecker dielectric model the docs cite. Use the capacitive v1.2 board.
#define PIN_MOISTURE_AOUT 34

// HX711 load cell amplifier (bit-banged 2-wire protocol)
#define PIN_HX711_DOUT 18
#define PIN_HX711_SCK 19

// DS18B20 on OneWire — needs an external 4.7k pull-up to 3V3
#define PIN_DS18B20_DQ 4

// NEO-6M GPS on hardware UART2
#define PIN_GPS_RX 16  // ESP32 receives  <- GPS TX
#define PIN_GPS_TX 17  // ESP32 transmits -> GPS RX
#define GPS_BAUD 9600

// ---------------------------------------------------------------- ADC setup
// The capacitive v1.2 sensor swings roughly 1.0V (wet) to 3.0V (dry).
// ESP32's default 0dB attenuation saturates at ~1.1V, which would rail almost
// the entire useful range to 4095. 11dB (12dB on newer cores) gives ~0-3.1V.
#define MOISTURE_ADC_ATTEN ADC_11db
#define MOISTURE_ADC_SAMPLES 64  // averaged to suppress ESP32 ADC noise

// ---------------------------------------------------------------- HX711
// Datasheet constraint: holding SCK high for >60us puts the HX711 into power
// down mode. Under FreeRTOS a task switch mid-read does exactly that, which is
// why the read is wrapped in a portMUX critical section.
#define HX711_READ_TIMEOUT_MS 200
#define HX711_SCK_SETTLE_US 1  // ESP32 at 240MHz toggles faster than the
                               // HX711's minimum clock high/low time

// Placeholder — overwritten by the tare/calibration routine in Phase 2.
#define HX711_DEFAULT_SCALE 1.0f

// ---------------------------------------------------------------- IMU
#define IMU_SAMPLE_RATE_HZ 200
#define IMU_BURST_SAMPLES 256  // power of two, required by the radix-2 FFT
                               // 256 @ 200Hz = 1.28s window, 0.78Hz bin width

// ---------------------------------------------------------------- core split
//
// The project docs assign sensors to Core 0 and networking to Core 1. That is
// backwards: on Arduino-ESP32 the Wi-Fi and TCP/IP stacks are pinned to Core 0
// by default. Following the docs would put 200Hz IMU sampling and a 60us-
// critical HX711 read on the same core as the Wi-Fi driver, turning every
// beacon and TCP retransmit into sample jitter and corrupted weight readings.
//
// So: timing-critical sampling on Core 1, jitter-tolerant inference and
// networking on Core 0 alongside the Wi-Fi stack already living there.
// Phase 1 is single-threaded; these take effect when tasks are created.
#define CORE_SAMPLING 1
#define CORE_INFERENCE_NET 0

// ---------------------------------------------------------------- bring-up
#define SERIAL_BAUD 115200
#define BRINGUP_I2C_SCAN true
