// ConcreSense — Phase 1 hardware bring-up
//
// Purpose: prove the board and every driver, and report exactly which
// subsystems are physically present. Every sensor is treated as optional, so
// this runs correctly on a bare ESP32 with nothing wired and stays useful as
// each module is added. Nothing here blocks on missing hardware.
//
// Phase 2 adds feature extraction and calibration; Phase 3 adds the FreeRTOS
// task split and the classifier.

#include <Wire.h>

#include "src/config.h"
#include "src/display/oled_ssd1306.h"
#include "src/sensor_status.h"
#include "src/sensors/gps_neo6m.h"
#include "src/sensors/imu_mpu6050.h"
#include "src/sensors/loadcell_hx711.h"
#include "src/sensors/moisture_cap.h"
#include "src/sensors/temp_ds18b20.h"

OledDisplay oled;
MoistureCapacitive moisture;
TempDS18B20 temp;
LoadCellHX711 loadcell;
ImuMPU6050 imu;
GpsNeo6M gps;

struct Subsystem {
  const char* name;
  SensorStatus status;
  const char* note;
};

static Subsystem report[6];
static uint8_t reportCount = 0;

static void record(const char* name, SensorStatus s, const char* note) {
  if (reportCount < 6) report[reportCount++] = {name, s, note};
}

// ---------------------------------------------------------------- I2C scan
static void scanI2C() {
  Serial.println(F("\n--- I2C bus scan (SDA=21 SCL=22 @400kHz) ---"));
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  0x%02X  ", addr);
      switch (addr) {
        case 0x3C:
        case 0x3D: Serial.println(F("SSD1306 OLED")); break;
        case 0x68:
        case 0x69: Serial.println(F("MPU-6050 IMU")); break;
        default: Serial.println(F("(unrecognised)")); break;
      }
      found++;
    }
  }
  if (found == 0) {
    Serial.println(F("  no devices."));
    Serial.println(F("  If modules ARE wired: check 3V3/GND before suspecting"
                     " the pins - both parts are"));
    Serial.println(F("  bus-silent when unpowered, which looks identical to"
                     " not being connected."));
  }
  Serial.printf("--- %u device(s) ---\n\n", found);
}

// ---------------------------------------------------------------- helpers
static void banner() {
  Serial.println(F("\n\n=============================================="));
  Serial.println(F("  ConcreSense - Phase 1 Hardware Bring-up"));
  Serial.printf("  fw %s   device %s\n", FW_VERSION, DEVICE_ID);
  Serial.printf("  chip %s rev %d   %d MHz   %d core(s)\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                getCpuFrequencyMhz(), ESP.getChipCores());
  Serial.printf("  flash %u MB   free heap %u B\n",
                ESP.getFlashChipSize() / (1024 * 1024), ESP.getFreeHeap());
  Serial.printf("  sketch running on core %d\n", xPortGetCoreID());
  Serial.println(F("=============================================="));
}

static void printReport() {
  Serial.println(F("\n=============== BRING-UP REPORT ==============="));
  uint8_t ok = 0;
  for (uint8_t i = 0; i < reportCount; i++) {
    Serial.printf("  %-16s %-13s %s\n", report[i].name,
                  statusName(report[i].status), report[i].note);
    if (report[i].status == SensorStatus::OK) ok++;
  }
  Serial.printf("\n  %u/%u subsystems present.\n", ok, reportCount);
  if (ok < reportCount) {
    Serial.println(F("  ABSENT is expected for parts not yet wired - the"));
    Serial.println(F("  firmware degrades cleanly and will pick them up on"));
    Serial.println(F("  the next boot once they are connected."));
  }
  Serial.println(F("==============================================\n"));
}

// ---------------------------------------------------------------- setup
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(600);  // let USB CDC settle so the banner is not truncated
  banner();

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);

  if (BRINGUP_I2C_SCAN) scanI2C();

  // --- OLED (first, so later stages can show progress if it exists)
  Serial.print(F("[1/6] SSD1306 OLED       ... "));
  SensorStatus s = oled.begin();
  Serial.println(statusName(s));
  record("SSD1306 OLED", s, s == SensorStatus::OK ? "0x3C, 128x64"
                                                  : "not on bus");
  if (s == SensorStatus::OK) oled.splash(FW_VERSION);

  // --- Capacitive moisture on ADC1
  Serial.print(F("[2/6] Moisture (cap v1.2)... "));
  s = moisture.begin();
  Serial.println(statusName(s));
  if (s == SensorStatus::OK) {
    Reading mv = moisture.readMilliVolts();
    Reading raw = moisture.readRawCounts();
    Serial.printf("       %.0f mV (calibrated via eFuse Vref), raw %.0f/4095\n",
                  mv.value, raw.value);
    record("Moisture(cap)", s, "GPIO34, 11dB atten");
  } else {
    // Report what the floating pin actually reads — it is the fastest way to
    // tell "nothing connected" from "connected but unpowered".
    analogSetPinAttenuation(PIN_MOISTURE_AOUT, MOISTURE_ADC_ATTEN);
    Serial.printf("       floating pin reads %u mV\n",
                  analogReadMilliVolts(PIN_MOISTURE_AOUT));
    record("Moisture(cap)", s, "GPIO34 floating");
  }

  // --- DS18B20
  Serial.print(F("[3/6] DS18B20 temp       ... "));
  s = temp.begin();
  Serial.println(statusName(s));
  if (s == SensorStatus::OK) {
    Reading t = temp.readCelsius();
    Serial.printf("       %u device(s), %.2f C\n", temp.deviceCount(), t.value);
    record("DS18B20", s, "OneWire GPIO4");
  } else {
    record("DS18B20", s, "no OneWire device (4.7k pull-up?)");
  }

  // --- HX711
  Serial.print(F("[4/6] HX711 load cell    ... "));
  s = loadcell.begin();
  Serial.println(statusName(s));
  if (s == SensorStatus::OK) {
    const float sps = loadcell.measureSampleRateHz();
    Serial.printf("       raw offset %ld, measured %.1f SPS\n",
                  (long)loadcell.offset(), sps);
    // Measured, not assumed: the docs' penetration-force curve needs far more
    // than 10Hz, and most breakouts strap RATE low for exactly 10.
    if (!isnan(sps) && sps < 20.0f) {
      Serial.println(F("       NOTE: ~10 SPS. Force-vs-depth curves will be"
                       " coarse; bridge the"));
      Serial.println(F("       RATE pad to VCC for 80 SPS if a curve is"
                       " needed."));
    }
    record("HX711", s, "GPIO18/19, portMUX-guarded");
  } else {
    record("HX711", s, "DOUT never went low");
  }

  // --- MPU-6050
  Serial.print(F("[5/6] MPU-6050 IMU       ... "));
  s = imu.begin();
  Serial.println(statusName(s));
  if (s == SensorStatus::OK) {
    float ax, ay, az;
    imu.readAccel(ax, ay, az);
    Serial.printf("       WHO_AM_I 0x%02X, accel %.3f %.3f %.3f g\n",
                  imu.whoAmI(), ax, ay, az);
    record("MPU-6050", s, "200Hz, +/-4g");
  } else {
    record("MPU-6050", s, "not at 0x68/0x69");
  }

  // --- NEO-6M
  Serial.print(F("[6/6] NEO-6M GPS         ... "));
  s = gps.begin(3000);
  Serial.println(statusName(s));
  if (s == SensorStatus::OK) {
    GpsFix f = gps.fix();
    Serial.printf("       NMEA flowing, %u sats, fix=%s\n", f.satellites,
                  f.valid ? "yes" : "no (normal indoors)");
    record("NEO-6M GPS", s, "UART2 16/17, NMEA seen");
  } else {
    Serial.println(F("       no NMEA on UART2. If wired: TX/RX are commonly"
                     " swapped -"));
    Serial.println(F("       GPS TX must go to GPIO16."));
    record("NEO-6M GPS", s, "silent on UART2");
  }

  printReport();

  if (oled.status() == SensorStatus::OK) {
    char l0[24], l1[24], l2[24];
    uint8_t ok = 0;
    for (uint8_t i = 0; i < reportCount; i++)
      if (report[i].status == SensorStatus::OK) ok++;
    snprintf(l0, sizeof(l0), "Phase 1 bring-up");
    snprintf(l1, sizeof(l1), "%u/%u present", ok, reportCount);
    snprintf(l2, sizeof(l2), "heap %uK", ESP.getFreeHeap() / 1024);
    const char* lines[] = {l0, l1, l2};
    oled.showStatusGrid(lines, 3);
  }

  Serial.println(F("Entering live loop (1 Hz). Ctrl-C to exit monitor.\n"));
}

// ---------------------------------------------------------------- loop
void loop() {
  static uint32_t n = 0;
  gps.poll();  // drain UART every pass so the FIFO never overflows

  Serial.printf("[%6lu] ", (unsigned long)++n);

  if (moisture.status() == SensorStatus::OK) {
    Serial.printf("moist=%.0fmV ", moisture.readMilliVolts().value);
  }
  if (temp.status() == SensorStatus::OK) {
    Serial.printf("T=%.2fC ", temp.readCelsius().value);
  }
  if (loadcell.status() == SensorStatus::OK) {
    Serial.printf("load=%.1f ", loadcell.read().value);
  }
  if (imu.status() == SensorStatus::OK) {
    float ax, ay, az;
    if (imu.readAccel(ax, ay, az)) {
      Serial.printf("|a|=%.3fg ", sqrtf(ax * ax + ay * ay + az * az));
    }
  }
  if (gps.status() == SensorStatus::OK) {
    GpsFix f = gps.fix();
    Serial.printf("sats=%u%s ", f.satellites, f.valid ? "*" : "");
  }

  // On a bare board none of the above print, so emit something that proves the
  // loop is alive rather than an empty line.
  Serial.printf("heap=%u\n", ESP.getFreeHeap());

  delay(1000);
}
