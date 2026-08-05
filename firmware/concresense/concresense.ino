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
#include "src/features/fft_features.h"
#include "src/physics/anchors_store.h"
#include "src/physics/calibration.h"
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

MoistureAnchors moistureAnchors;
LoadCellAnchors loadAnchors;

// ---------------------------------------------------------------------------
// Wokwi simulator add-on: verdict LEDs (optional, small, harmless on real HW)
//
// Not part of the verified hardware design in config.h/PROJECT_ANALYSIS.md.
// Added only so the wokwi/ simulation has a visible pass/fail readout without
// needing the OLED text to be legible in a screenshot. Three plain GPIO
// outputs, no PWM, no change to any sensor/physics/classification logic.
// Wired in wokwi/diagram.json to D25 (green), D26 (yellow), D27 (red) via
// 220-ohm series resistors. Safe to leave unwired on the real board.
#define PIN_LED_GOOD 25
#define PIN_LED_MARGINAL 26
#define PIN_LED_REJECT 27

static void updateVerdictLeds(QualityClass q) {
  digitalWrite(PIN_LED_GOOD, q == QualityClass::GOOD ? HIGH : LOW);
  digitalWrite(PIN_LED_MARGINAL, q == QualityClass::MARGINAL ? HIGH : LOW);
  digitalWrite(PIN_LED_REJECT, q == QualityClass::REJECT ? HIGH : LOW);
  // UNKNOWN (not yet calibrated / incomplete reading): all three off, rather
  // than guessing a verdict LED for it.
}
// ---------------------------------------------------------- end Wokwi add-on

// Burst buffer, static so a measurement never allocates mid-cycle.
static float gBurst[IMU_BURST_SAMPLES];

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

// ------------------------------------------------- Phase 2: measurement
//
// One full cycle: sample every channel, extract vibration features, run the
// physics chain, and apply the IS 456 rule engine. Phase 3 replaces the rule
// engine with the TinyML classifier and keeps the rules as a cross-check.
static void runMeasurementCycle(bool csv) {
  // Quiesce the OLED for the duration of the burst. The display and the IMU
  // share one I2C bus, and a full 128x64 frame push is ~20ms -- enough to drop
  // four samples out of a 200Hz window and skew the FFT bin spacing.
  const bool haveImu = imu.status() == SensorStatus::OK;

  float achievedHz = NAN;
  uint16_t got = 0;
  VibrationFeatures vf;

  if (haveImu) {
    got = imu.captureBurst(gBurst, IMU_BURST_SAMPLES, achievedHz);
    if (got == IMU_BURST_SAMPLES) {
      // Deliberately uses the ACHIEVED rate, not IMU_SAMPLE_RATE_HZ: bus
      // contention makes them differ, and using the nominal rate would put the
      // reported dominant frequency systematically off.
      vf = extractVibrationFeatures(gBurst, got, achievedHz);
    }
  }

  Reading mv = moisture.readMilliVolts();
  Reading tc = temp.readCelsius();
  Reading load = loadcell.read();

  const float tempC = tc.valid() ? tc.value : NAN;
  DerivedProperties d = deriveAll(mv.valid() ? mv.value : NAN, tempC,
                                  load.valid() ? load.value : NAN,
                                  moistureAnchors, loadAnchors);

  QualityClass q = QualityClass::UNKNOWN;
  if (d.wcValid && d.slumpValid) {
    q = classifyIS456(d.wcRatio, d.slumpMm, tempC);
  }
  updateVerdictLeds(q);  // Wokwi add-on, see definition above

  if (csv) {
    // Matches FEATURE_COLUMNS in tinyml_model/dataset_generator.py so real
    // readings can be merged with --merge-real. Blank fields stay blank rather
    // than becoming zero: a zero would be indistinguishable from a real value
    // and would poison the training set.
    Serial.print(F("CSV,"));
    if (mv.valid()) Serial.print(mv.value, 1); Serial.print(',');
    if (tc.valid()) Serial.print(tc.value, 4); Serial.print(',');
    if (load.valid()) Serial.print(load.value, 4); Serial.print(',');
    if (vf.valid) Serial.print(vf.rms, 5); Serial.print(',');
    if (vf.valid) Serial.print(vf.dominantFreqHz, 3); Serial.print(',');
    if (vf.valid) Serial.print(vf.spectralEntropy, 5); Serial.print(',');
    if (vf.valid) Serial.print(vf.dampingRatio, 5);
    Serial.println();
    return;
  }

  Serial.println(F("\n----------- measurement cycle -----------"));
  Serial.print(F("  moisture   : "));
  if (mv.valid()) Serial.printf("%.0f mV\n", mv.value);
  else Serial.println(statusName(moisture.status()));

  Serial.print(F("  temperature: "));
  if (tc.valid()) Serial.printf("%.2f C\n", tc.value);
  else Serial.println(statusName(temp.status()));

  Serial.print(F("  load       : "));
  if (load.valid()) Serial.printf("%.2f (raw units)\n", load.value);
  else Serial.println(statusName(loadcell.status()));

  Serial.print(F("  vibration  : "));
  if (vf.valid) {
    Serial.printf("%u samples @ %.1f Hz\n", got, achievedHz);
    Serial.printf("               rms=%.4f g  dom=%.2f Hz  entropy=%.3f  "
                  "damping=%.4f\n",
                  vf.rms, vf.dominantFreqHz, vf.spectralEntropy, vf.dampingRatio);
    if (vf.dominantFreqHz > 40.0f) {
      Serial.println(F("               (near the 44Hz DLPF corner - this peak"
                       " is filter-shaped)"));
    }
  } else {
    Serial.println(haveImu ? F("burst incomplete") : F("ABSENT"));
  }

  Serial.println(F("  --- derived ---"));
  if (d.wcValid) {
    Serial.printf("  eps_mix=%.2f  v_water=%.4f  w/c=%.3f\n", d.epsilonMix,
                  d.waterVolFrac, d.wcRatio);
  } else {
    Serial.print(F("  w/c        : unavailable ("));
    Serial.println(moistureAnchors.calibrated ? F("sensor/range)")
                                              : F("NOT CALIBRATED - run 'cm')"));
  }
  if (d.slumpValid) {
    Serial.printf("  tau_0=%.0f Pa  slump=%.1f mm\n", d.yieldStressPa, d.slumpMm);
  } else {
    Serial.print(F("  slump      : unavailable ("));
    Serial.println(loadAnchors.calibrated ? F("no load reading)")
                                          : F("NOT CALIBRATED - run 'cl')"));
  }

  Serial.printf("  IS 456     : %s\n", qualityName(q));
  if (q == QualityClass::UNKNOWN) {
    Serial.println(F("  (UNKNOWN is correct here - the device refuses to"));
    Serial.println(F("   classify without calibrated inputs rather than"));
    Serial.println(F("   emitting a confident-looking guess.)"));
  }
  Serial.println(F("-----------------------------------------\n"));

  if (oled.status() == SensorStatus::OK) {
    char l0[24], l1[24], l2[24];
    snprintf(l0, sizeof(l0), "IS456: %s", qualityName(q));
    if (d.wcValid) snprintf(l1, sizeof(l1), "w/c  %.2f", d.wcRatio);
    else snprintf(l1, sizeof(l1), "w/c  --");
    if (d.slumpValid) snprintf(l2, sizeof(l2), "slump %.0fmm", d.slumpMm);
    else snprintf(l2, sizeof(l2), "slump --");
    const char* lines[] = {l0, l1, l2};
    oled.showStatusGrid(lines, 3);
  }
}

// ------------------------------------------------- Phase 2: calibration
//
// Range anchoring, stated for what it is. Dipping the probe in air and water
// establishes the ADC span and offset of THIS board's sensor. It does not
// calibrate against concrete and does not pretend to: it makes the dielectric
// index meaningful on this hardware, and the physics chain does the rest.
static void calibrateMoisture() {
  if (moisture.status() != SensorStatus::OK) {
    Serial.println(F("moisture sensor ABSENT - cannot calibrate."));
    return;
  }
  Serial.println(F("\n=== moisture range anchoring ==="));
  Serial.println(F("This sets the ADC span for THIS board. It is NOT a"));
  Serial.println(F("calibration against concrete.\n"));

  Serial.println(F("1) Hold the probe in DRY AIR, then send any key."));
  while (!Serial.available()) delay(50);
  while (Serial.available()) Serial.read();
  delay(300);
  const float dry = moisture.readMilliVolts().value;
  Serial.printf("   dry anchor  = %.0f mV\n", dry);

  Serial.println(F("2) Immerse the probe in WATER to its marked line, then"
                   " send any key."));
  while (!Serial.available()) delay(50);
  while (Serial.available()) Serial.read();
  delay(300);
  const float sat = moisture.readMilliVolts().value;
  Serial.printf("   wet anchor  = %.0f mV\n", sat);

  if (dry - sat < 200.0f) {
    Serial.println(F("\nREJECTED: span < 200mV. A working v1.2 sensor should"));
    Serial.println(F("show ~1000-1600mV between air and water. Check that the"));
    Serial.println(F("board is powered from 3V3 and that AOUT is on GPIO34."));
    return;
  }

  moistureAnchors.mvDry = dry;
  moistureAnchors.mvSat = sat;
  Reading t = temp.readCelsius();
  moistureAnchors.refTempC = t.valid() ? t.value : 25.0f;
  moistureAnchors.calibrated = true;
  anchors::saveMoisture(moistureAnchors);

  Serial.printf("\nSaved. span=%.0f mV at %.1f C\n", dry - sat,
                moistureAnchors.refTempC);
}

static void calibrateLoadCell() {
  if (loadcell.status() != SensorStatus::OK) {
    Serial.println(F("load cell ABSENT - cannot calibrate."));
    return;
  }
  Serial.println(F("\n=== load cell calibration ==="));
  Serial.println(F("1) Remove all load, then send any key to tare."));
  while (!Serial.available()) delay(50);
  while (Serial.available()) Serial.read();
  loadcell.tare(20);
  Serial.printf("   tared, offset = %ld\n", (long)loadcell.offset());

  Serial.println(F("2) Place a KNOWN mass on the plunger. Type its mass in"));
  Serial.println(F("   grams and press enter (e.g. 500)."));
  while (!Serial.available()) delay(50);
  const float grams = Serial.parseFloat();
  while (Serial.available()) Serial.read();

  if (grams <= 0.0f) {
    Serial.println(F("   invalid mass - aborted."));
    return;
  }

  int32_t raw;
  if (!loadcell.readRawMedian(15, raw)) {
    Serial.println(F("   read failed - aborted."));
    return;
  }
  const float counts = (float)(raw - loadcell.offset());
  const float newtons = grams * 0.00980665f;  // gram-force -> N
  if (fabsf(counts) < 100.0f) {
    Serial.println(F("   REJECTED: mass produced almost no change. Check the"));
    Serial.println(F("   cell wiring and that the mass is on the plunger."));
    return;
  }

  loadAnchors.countsPerNewton = counts / newtons;
  loadAnchors.calibrated = true;
  anchors::saveLoadCell(loadAnchors);
  Serial.printf("   %.1f g = %.3f N -> %.1f counts/N. Saved.\n", grams, newtons,
                loadAnchors.countsPerNewton);
}

static void printHelp() {
  Serial.println(F("\ncommands:"));
  Serial.println(F("  m  - run one measurement cycle"));
  Serial.println(F("  c  - run measurement, print as CSV (for --merge-real)"));
  Serial.println(F("  cm - calibrate moisture range anchors"));
  Serial.println(F("  cl - calibrate load cell"));
  Serial.println(F("  s  - show calibration state"));
  Serial.println(F("  x  - erase stored calibration"));
  Serial.println(F("  h  - this help\n"));
}

static void printCalState() {
  Serial.println(F("\ncalibration state:"));
  Serial.printf("  moisture : %s (dry=%.0fmV sat=%.0fmV @%.1fC)\n",
                moistureAnchors.calibrated ? "CALIBRATED" : "not calibrated",
                moistureAnchors.mvDry, moistureAnchors.mvSat,
                moistureAnchors.refTempC);
  Serial.printf("  load cell: %s (%.1f counts/N, plunger %.2f mm^2)\n",
                loadAnchors.calibrated ? "CALIBRATED" : "not calibrated",
                loadAnchors.countsPerNewton, loadAnchors.plungerAreaM2 * 1e6f);
  if (!moistureAnchors.calibrated || !loadAnchors.calibrated) {
    Serial.println(F("  -> derived w/c and slump stay suppressed until both"));
    Serial.println(F("     are calibrated. This is intentional."));
  }
  Serial.println();
}

static void handleCommand() {
  if (!Serial.available()) return;
  const String cmd = Serial.readStringUntil('\n');
  String c = cmd;
  c.trim();

  if (c == "m") runMeasurementCycle(false);
  else if (c == "c") runMeasurementCycle(true);
  else if (c == "cm") calibrateMoisture();
  else if (c == "cl") calibrateLoadCell();
  else if (c == "s") printCalState();
  else if (c == "x") { anchors::clear(); Serial.println(F("calibration erased (reboot to apply)")); }
  else if (c == "h") printHelp();
  else if (c.length()) Serial.println(F("unknown command - 'h' for help"));
}

// ---------------------------------------------------------------- setup
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(600);  // let USB CDC settle so the banner is not truncated
  banner();

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);

  // Wokwi add-on: verdict LEDs, see definition above. All off until the
  // first measurement cycle.
  pinMode(PIN_LED_GOOD, OUTPUT);
  pinMode(PIN_LED_MARGINAL, OUTPUT);
  pinMode(PIN_LED_REJECT, OUTPUT);
  digitalWrite(PIN_LED_GOOD, LOW);
  digitalWrite(PIN_LED_MARGINAL, LOW);
  digitalWrite(PIN_LED_REJECT, LOW);

  // Restore per-board calibration before any driver runs, so the bring-up
  // report can state whether this board is calibrated.
  anchors::load(moistureAnchors, loadAnchors);

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
  } else if (s == SensorStatus::OUT_OF_RANGE) {
    // Distinct from ABSENT: the pin did clock out a word, but a railed or zero
    // value means a floating DOUT or a disconnected/shorted bridge, not a
    // missing chip. Different fault, different fix.
    Serial.println(F("       clocked a word but it was railed - floating DOUT,"
                     " or the load"));
    Serial.println(F("       cell bridge (E+/E-/A+/A-) is not connected."));
    record("HX711", s, "railed reading, check bridge");
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

  printCalState();
  printHelp();
  Serial.println(F("Entering live loop (1 Hz telemetry).\n"));
}

// ---------------------------------------------------------------- loop
void loop() {
  static uint32_t n = 0;
  gps.poll();  // drain UART every pass so the FIFO never overflows

  handleCommand();

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
