// ConcreSense — Phase 1 hardware bring-up
//
// Purpose: prove the board and every driver, and report exactly which
// subsystems are physically present. Every sensor is treated as optional, so
// this runs correctly on a bare ESP32 with nothing wired and stays useful as
// each module is added. Nothing here blocks on missing hardware.
//
// Phase 2 adds feature extraction and calibration; Phase 3 adds the FreeRTOS
// task split and the classifier.

#include <WiFi.h>
#include <Wire.h>

#include "src/config.h"
#include "src/display/oled_ssd1306.h"
#include "src/features/fft_features.h"
#include "src/measurement.h"
#include "src/network/net_client.h"
#include "src/physics/anchors_store.h"
#include "src/physics/calibration.h"
#include "src/sensor_status.h"
#include "src/tinyml/model_infer.h"
#include "src/tinyml/model_selftest.h"
#include "src/tinyml/model_weights.h"
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
NetConfig netConfig;

// --- Phase 3: dual-core plumbing
//
// Core assignment is deliberately the REVERSE of the original project spec.
// Arduino-ESP32 pins the WiFi/TCP-IP stack to Core 0 and runs loopTask on
// Core 1 (confirmed empirically in Phase 1: the banner reports "core 1").
// Following the spec would put 200Hz IMU sampling and the 60us-critical HX711
// read on the same core as the WiFi driver, turning every beacon and TCP
// retransmit into sample jitter. See AUDIT.md section C.
//
// Depth 4: enough to absorb a slow publish without blocking the sampler, small
// enough that a stalled consumer surfaces as a visible drop rather than
// silently buffering minutes of stale readings.
static QueueHandle_t gMeasurementQueue = nullptr;
static TaskHandle_t gSamplingTask = nullptr;
static TaskHandle_t gNetworkTask = nullptr;

// Guards ALL sensor hardware. Both the automatic sampling task and a manual
// 'm' typed at the console call acquireMeasurement(), and without this they can
// interleave on the shared I2C bus and mid-HX711-conversion -- producing
// corrupt readings that look like real ones. The console path is rare, so
// contention is negligible; correctness is not.
static SemaphoreHandle_t gSensorMutex = nullptr;

static volatile bool gAutoMeasure = false;
static volatile uint32_t gDroppedRecords = 0;

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
  Serial.printf("  %s | %s | %s\n", STUDENT_NAME, STUDENT_SECTION,
                STUDENT_ROLL);
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
// ------------------------------------------------- Phase 3: acquisition
//
// Runs on CORE_SAMPLING. Reads every channel and extracts vibration features
// into a self-contained record. Does NOT classify, print, or touch the network
// -- keeping this function pure sampling is what lets it stay on a core with
// no WiFi driver competing for time.
static MeasurementRecord acquireMeasurement() {
  static uint32_t seq = 0;
  MeasurementRecord rec;

  // Exclusive access to every sensor for the whole cycle. Held across the IMU
  // burst too: a competing I2C transaction mid-burst would drop samples and
  // silently skew the FFT.
  if (gSensorMutex) xSemaphoreTake(gSensorMutex, portMAX_DELAY);

  rec.seq = ++seq;
  rec.uptimeMs = millis();

  // The OLED and IMU share one I2C bus and a full 128x64 frame push is ~20ms --
  // enough to drop four samples from a 200Hz window and skew the FFT bin
  // spacing. Nothing writes to the display while a burst is in flight.
  if (imu.status() == SensorStatus::OK) {
    float achievedHz = NAN;
    const uint16_t got = imu.captureBurst(gBurst, IMU_BURST_SAMPLES, achievedHz);
    if (got == IMU_BURST_SAMPLES) {
      // Uses the ACHIEVED rate, not the nominal 200Hz: bus contention makes
      // them differ, and the nominal rate would bias every reported frequency.
      rec.vib = extractVibrationFeatures(gBurst, got, achievedHz);
    }
  }

  Reading mv = moisture.readMilliVolts();
  Reading tc = temp.readCelsius();
  Reading load = loadcell.read();

  rec.moistureValid = mv.valid();
  rec.tempValid = tc.valid();
  rec.loadValid = load.valid();
  rec.moistureMv = mv.valid() ? mv.value : NAN;
  rec.tempC = tc.valid() ? tc.value : NAN;
  rec.loadCounts = load.valid() ? load.value : NAN;

  rec.derived = deriveAll(rec.moistureMv, rec.tempC, rec.loadCounts,
                          moistureAnchors, loadAnchors);
  rec.gps = gps.fix();

  if (gSensorMutex) xSemaphoreGive(gSensorMutex);
  return rec;
}

// ------------------------------------------------- Phase 3: reporting
static void reportMeasurement(const MeasurementRecord& rec,
                              const Inference& inf, QualityClass ruleClass,
                              bool csv) {
  const DerivedProperties& d = rec.derived;

  if (csv) {
    // Matches FEATURE_COLUMNS in tinyml_model/dataset_generator.py so real
    // readings merge cleanly with --merge-real. Blank fields stay blank: a
    // zero would be indistinguishable from a real reading and poison training.
    Serial.print(F("CSV,"));
    if (rec.moistureValid) Serial.print(rec.moistureMv, 1); Serial.print(',');
    if (rec.tempValid) Serial.print(rec.tempC, 4); Serial.print(',');
    if (!isnan(d.forceN)) Serial.print(d.forceN, 4); Serial.print(',');
    if (rec.vib.valid) Serial.print(rec.vib.rms, 5); Serial.print(',');
    if (rec.vib.valid) Serial.print(rec.vib.dominantFreqHz, 3); Serial.print(',');
    if (rec.vib.valid) Serial.print(rec.vib.spectralEntropy, 5); Serial.print(',');
    if (rec.vib.valid) Serial.print(rec.vib.dampingRatio, 5);
    Serial.println();
    return;
  }

  Serial.printf("\n--------- measurement #%lu (core %d) ---------\n",
                (unsigned long)rec.seq, xPortGetCoreID());

  Serial.print(F("  moisture   : "));
  if (rec.moistureValid) Serial.printf("%.0f mV\n", rec.moistureMv);
  else Serial.println(statusName(moisture.status()));

  Serial.print(F("  temperature: "));
  if (rec.tempValid) Serial.printf("%.2f C\n", rec.tempC);
  else Serial.println(statusName(temp.status()));

  Serial.print(F("  load       : "));
  if (rec.loadValid) Serial.printf("%.2f counts\n", rec.loadCounts);
  else Serial.println(statusName(loadcell.status()));

  Serial.print(F("  vibration  : "));
  if (rec.vib.valid) {
    Serial.printf("rms=%.4f g  dom=%.2f Hz  entropy=%.3f  damping=%.4f\n",
                  rec.vib.rms, rec.vib.dominantFreqHz, rec.vib.spectralEntropy,
                  rec.vib.dampingRatio);
    if (rec.vib.dominantFreqHz > 40.0f) {
      Serial.println(F("               (near the 44Hz DLPF corner - this peak"
                       " is filter-shaped)"));
    }
  } else {
    Serial.println(imu.status() == SensorStatus::OK ? F("burst incomplete")
                                                    : F("ABSENT"));
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
    Serial.printf("  F=%.3f N  tau_0=%.0f Pa  slump=%.1f mm\n", d.forceN,
                  d.yieldStressPa, d.slumpMm);
  } else {
    Serial.print(F("  slump      : unavailable ("));
    Serial.println(loadAnchors.calibrated ? F("no load reading)")
                                          : F("NOT CALIBRATED - run 'cl')"));
  }

  // --- both classifiers, side by side.
  //
  // The model does NOT replace the rule engine. Printing both is the only way
  // to notice if the MLP has simply re-learned the IS 456 thresholds it was
  // trained on -- in which case it is adding nothing, however good its
  // accuracy number looks.
  Serial.println(F("  --- classification ---"));
  Serial.printf("  IS 456 rules : %s\n", qualityName(ruleClass));
  if (inf.valid) {
    Serial.printf("  TinyML model : %s  (conf %.1f%%)\n",
                  inferenceClassName(inf.classIndex), inf.confidence * 100.0f);
    Serial.printf("                 GOOD %.2f  MARGINAL %.2f  REJECT %.2f\n",
                  inf.probabilities[0], inf.probabilities[1],
                  inf.probabilities[2]);
    const bool agree =
        strcmp(inferenceClassName(inf.classIndex), qualityName(ruleClass)) == 0;
    if (!agree && ruleClass != QualityClass::UNKNOWN) {
      Serial.println(F("  ** MODEL AND RULES DISAGREE - rules take precedence"));
      Serial.println(F("     for compliance; the disagreement is published."));
    }
    // Out-of-distribution check. A softmax score says nothing about whether
    // the input resembles the training data -- a classifier will report 95%
    // confidence on a feature vector it has never seen anything like. Flagging
    // |z| > 3 makes that failure visible instead of silent.
    float feats[MODEL_N_FEATURES];
    if (rec.toFeatureVector(feats)) {
      float worstZ = 0.0f;
      int worstIdx = -1;
      for (int i = 0; i < MODEL_N_FEATURES; i++) {
        if (MODEL_FEATURE_SCALE[i] <= 1e-12f) continue;
        const float z =
            fabsf((feats[i] - MODEL_FEATURE_MEAN[i]) / MODEL_FEATURE_SCALE[i]);
        if (z > worstZ) { worstZ = z; worstIdx = i; }
      }
      if (worstZ > 3.0f) {
        Serial.printf("  ** OUT OF DISTRIBUTION: feature %d is %.1f sigma from"
                      " the training mean.\n", worstIdx, worstZ);
        Serial.println(F("     The model's confidence above is not meaningful"
                         " for this input."));
      }
    }
  } else {
    Serial.println(F("  TinyML model : UNKNOWN (incomplete feature vector)"));
    Serial.println(F("  (refusing to classify beats guessing from zeros)"));
  }
  Serial.println(F("------------------------------------------\n"));

  if (oled.status() == SensorStatus::OK) {
    char l0[24], l1[24], l2[24], l3[24];
    snprintf(l0, sizeof(l0), "IS456: %s", qualityName(ruleClass));
    snprintf(l1, sizeof(l1), "ML: %s",
             inf.valid ? inferenceClassName(inf.classIndex) : "--");
    if (d.wcValid) snprintf(l2, sizeof(l2), "w/c  %.2f", d.wcRatio);
    else snprintf(l2, sizeof(l2), "w/c  --");
    if (d.slumpValid) snprintf(l3, sizeof(l3), "slump %.0fmm", d.slumpMm);
    else snprintf(l3, sizeof(l3), "slump --");
    const char* lines[] = {l0, l1, l2, l3};
    oled.showStatusGrid(lines, 4);
  }
}

// Classify + report + publish. Runs on CORE_INFERENCE_NET.
static void processMeasurement(const MeasurementRecord& rec, bool csv) {
  QualityClass ruleClass = QualityClass::UNKNOWN;
  if (rec.derived.wcValid && rec.derived.slumpValid) {
    ruleClass = classifyIS456(rec.derived.wcRatio, rec.derived.slumpMm,
                              rec.tempValid ? rec.tempC : NAN);
  }

  Inference inf;
  float features[MODEL_N_FEATURES];
  if (rec.toFeatureVector(features)) {
    inf = runInference(features, MODEL_N_FEATURES);
  }

  updateVerdictLeds(ruleClass);
  reportMeasurement(rec, inf, ruleClass, csv);

  if (net::state() == NetState::BROKER_UP) {
    net::publish(rec, inf, qualityName(ruleClass));
  }
}

// Synchronous single-shot, for the 'm' / 'c' serial commands. Runs inline on
// whichever core the console is on rather than going through the queue, so the
// output appears immediately after the command.
static void runMeasurementCycle(bool csv) {
  processMeasurement(acquireMeasurement(), csv);
}

// ------------------------------------------------- Phase 3: FreeRTOS tasks

// CORE_SAMPLING (Core 1). Timing-critical work only.
//
// Ticks at 100ms and measures on a longer interval, rather than simply sleeping
// for MEASURE_INTERVAL_MS. The NEO-6M emits an NMEA burst every second at 9600
// baud and the UART FIFO is only 128 bytes, so sleeping 5s between polls would
// overflow it and corrupt sentences. All GPS access lives on this core so the
// non-thread-safe TinyGPS++ parser is never touched from two cores at once.
static void samplingTask(void*) {
  uint32_t lastMeasureMs = 0;
  for (;;) {
    gps.poll();

    if (gAutoMeasure && (millis() - lastMeasureMs >= MEASURE_INTERVAL_MS)) {
      lastMeasureMs = millis();
      MeasurementRecord rec = acquireMeasurement();
      // Non-blocking send: if the consumer is stalled, refuse the new record
      // and count it rather than blocking here. Blocking the sampler would
      // leave the HX711 sitting mid-conversion and let the IMU burst cadence
      // drift -- corrupting the very data being queued.
      if (xQueueSend(gMeasurementQueue, &rec, 0) != pdTRUE) {
        gDroppedRecords++;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// CORE_INFERENCE_NET (Core 0). Inference, display, and networking -- all
// jitter-tolerant, and this is the core the WiFi driver already lives on.
static void networkTask(void*) {
  MeasurementRecord rec;
  for (;;) {
    net::poll();
    // Short timeout rather than portMAX_DELAY: net::poll() must keep running
    // to service MQTT keepalive and WiFi reconnects even when no measurements
    // are arriving.
    if (xQueueReceive(gMeasurementQueue, &rec, pdMS_TO_TICKS(200)) == pdTRUE) {
      processMeasurement(rec, false);
    }
  }
}

static void startTasks() {
  gSensorMutex = xSemaphoreCreateMutex();
  gMeasurementQueue = xQueueCreate(4, sizeof(MeasurementRecord));
  if (gMeasurementQueue == nullptr || gSensorMutex == nullptr) {
    Serial.println(F("FATAL: could not allocate queue/mutex"));
    return;
  }

  // Stack sizes are generous because both tasks hold a MeasurementRecord (with
  // its embedded feature structs) on the stack, and the network task also runs
  // ArduinoJson serialisation.
  xTaskCreatePinnedToCore(samplingTask, "sampling", 8192, nullptr, 3,
                          &gSamplingTask, CORE_SAMPLING);
  xTaskCreatePinnedToCore(networkTask, "network", 12288, nullptr, 2,
                          &gNetworkTask, CORE_INFERENCE_NET);

  Serial.printf("[rtos] sampling task -> core %d (priority 3)\n", CORE_SAMPLING);
  Serial.printf("[rtos] network  task -> core %d (priority 2)\n",
                CORE_INFERENCE_NET);
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

// ------------------------------------------------- Phase 5: simulation mode
//
// Synthesises a complete measurement so the ENTIRE downstream pipeline --
// classification, OLED, MQTT publish, dashboard, audit PDF -- can be verified
// on real silicon before any sensor exists. Without this, the device-to-broker
// link would stay untested until the parts arrive.
//
// The values are NOT hardcoded sensor readings. A target physical state is
// chosen, then the sensor reading that produces it is found by bisection
// through the SAME deriveAll() the real path uses. That means the simulator
// cannot drift from the physics, and a bug in the calibration chain shows up
// here too instead of being papered over.
//
// Every simulated record is flagged and published as
// data_source=simulated_onboard.

// Anchors used only by the simulator. Never written to NVS, never touching the
// real calibration -- a simulated run must not leave the device looking
// calibrated when it is not.
static MoistureAnchors simMoistureAnchors() {
  MoistureAnchors a;   // defaults are the nominal v1.2 response
  a.calibrated = true;
  return a;
}
static LoadCellAnchors simLoadAnchors() {
  LoadCellAnchors a;
  a.countsPerNewton = 20000.0f;  // representative of a 5kg cell at gain 128
  a.calibrated = true;
  return a;
}

// Generic bisection: find the input in [lo,hi] whose derived output matches
// `target`. `eval` must be monotonic over the interval.
static float solveFor(float lo, float hi, float target,
                      float (*eval)(float, float), float tempC) {
  for (int i = 0; i < 40; i++) {
    const float mid = 0.5f * (lo + hi);
    const float v = eval(mid, tempC);
    if (isnan(v)) return NAN;
    // eval() is monotonically DECREASING in both uses here (higher mV -> drier;
    // higher counts -> stiffer), so the branch is inverted versus the usual form.
    if (v > target) lo = mid; else hi = mid;
  }
  return 0.5f * (lo + hi);
}

static float evalWc(float mv, float tempC) {
  DerivedProperties d = deriveAll(mv, tempC, NAN, simMoistureAnchors(),
                                  simLoadAnchors());
  return d.wcRatio;
}
static float evalSlump(float counts, float tempC) {
  DerivedProperties d = deriveAll(NAN, tempC, counts, simMoistureAnchors(),
                                  simLoadAnchors());
  return d.slumpMm;
}

static MeasurementRecord makeSimulatedMeasurement(uint8_t scenario) {
  // Target physical states spanning the three IS 456 classes.
  struct Target { const char* name; float wc, slump, tempC; };
  static const Target targets[] = {
      {"well-proportioned", 0.45f,  90.0f, 30.0f},
      {"slightly wet",      0.52f, 135.0f, 33.0f},
      {"excess water",      0.62f, 185.0f, 31.0f},
  };
  const Target& t = targets[scenario % 3];

  static uint32_t simSeq = 0;
  MeasurementRecord rec;
  rec.simulated = true;
  rec.seq = ++simSeq;
  rec.uptimeMs = millis();

  // Invert the physics to get the sensor readings that produce this state.
  rec.moistureMv = solveFor(1300.0f, 2900.0f, t.wc, evalWc, t.tempC);
  rec.loadCounts = solveFor(1.0f, 200000.0f, t.slump, evalSlump, t.tempC);
  rec.tempC = t.tempC;
  rec.moistureValid = !isnan(rec.moistureMv);
  rec.tempValid = true;
  rec.loadValid = !isnan(rec.loadCounts);

  rec.derived = deriveAll(rec.moistureMv, rec.tempC, rec.loadCounts,
                          simMoistureAnchors(), simLoadAnchors());

  // Vibration: synthesise a damped waveform whose features land where the
  // TRAINING distribution puts them, then run the REAL FFT over it so the
  // features come from the same DSP the sensor path uses.
  //
  // Targets below mirror vibration_signature() in
  // tinyml_model/concresense_physics.py. Matching that distribution is not
  // cosmetic: an earlier version used an arbitrary fast decay and produced a
  // damping ratio +4.6 SIGMA above the training mean. The model then predicted
  // REJECT on every scenario with high confidence -- not a real disagreement,
  // just a classifier being shown input unlike anything it was trained on.
  const float wetness = constrain((t.wc - 0.35f) / 0.25f, 0.0f, 1.5f);
  const float stiffness = constrain((150.0f - t.slump) / 150.0f, 0.0f, 1.2f);

  const float targetRms = constrain(0.28f + 0.30f * stiffness - 0.10f * wetness,
                                    0.02f, 1.5f);
  const float domHz = constrain(34.0f + 14.0f * stiffness - 6.0f * wetness,
                                5.0f, 44.0f);
  const float targetEntropy = constrain(
      0.42f + 0.26f * wetness - 0.10f * stiffness, 0.05f, 0.99f);
  const float targetDamping = constrain(
      0.055f + 0.070f * wetness - 0.020f * stiffness, 0.005f, 0.45f);

  // Derive the decay constant from the target damping instead of guessing.
  //   zeta = delta / sqrt(4pi^2 + delta^2)   =>  delta = 2*pi*zeta/sqrt(1-zeta^2)
  // extractVibrationFeatures() computes delta = 0.5*ln(E_first/E_last) over
  // quarter-windows whose energy centroids are ~T/8 and ~7T/8 apart, so for an
  // exp(-t/tau) envelope:  delta = (3T/4)/tau  =>  tau = 0.75*T/delta.
  const float windowT = (float)IMU_BURST_SAMPLES / IMU_SAMPLE_RATE_HZ;
  const float zeta = targetDamping;
  const float delta = 2.0f * PI * zeta / sqrtf(1.0f - zeta * zeta);
  const float tau = (delta > 1e-6f) ? (0.75f * windowT / delta) : 1e6f;

  for (uint16_t i = 0; i < IMU_BURST_SAMPLES; i++) {
    const float tt = (float)i / IMU_SAMPLE_RATE_HZ;
    const float decay = expf(-tt / tau);
    // Noise fraction sets spectral entropy: a pure tone is ~0, broadband ~1.
    const float noise = ((float)random(-1000, 1000) / 1000.0f) * targetEntropy;
    // The 1g gravity pedestal is included on purpose -- real accel magnitude
    // carries it, so the FFT's DC-removal path must be exercised here too.
    gBurst[i] = 1.0f + (sinf(2.0f * PI * domHz * tt) * decay + noise);
  }

  // Rescale the AC component to hit the target RMS exactly. Done by measuring
  // with the real extractor rather than predicting analytically, because the
  // decay envelope and the noise term both contribute in ways that are fiddly
  // to solve in closed form and easy to get subtly wrong.
  VibrationFeatures probe = extractVibrationFeatures(gBurst, IMU_BURST_SAMPLES,
                                                     IMU_SAMPLE_RATE_HZ);
  if (probe.valid && probe.rms > 1e-6f) {
    const float k = targetRms / probe.rms;
    float mean = 0.0f;
    for (uint16_t i = 0; i < IMU_BURST_SAMPLES; i++) mean += gBurst[i];
    mean /= IMU_BURST_SAMPLES;
    for (uint16_t i = 0; i < IMU_BURST_SAMPLES; i++) {
      gBurst[i] = mean + (gBurst[i] - mean) * k;
    }
  }
  rec.vib = extractVibrationFeatures(gBurst, IMU_BURST_SAMPLES,
                                     IMU_SAMPLE_RATE_HZ);

  rec.gps = gps.fix();  // real GPS if present; no fix indoors, published as such

  Serial.printf("  [sim] target: %s  w/c %.2f  slump %.0fmm  %.1fC\n",
                t.name, t.wc, t.slump, t.tempC);
  Serial.printf("  [sim] solved: moisture %.0f mV, load %.0f counts\n",
                rec.moistureMv, rec.loadCounts);
  return rec;
}

static void runSimulation(uint8_t count) {
  Serial.println(F("\n=== SIMULATED measurements (no sensors involved) ==="));
  Serial.println(F("Published as data_source=simulated_onboard so they can"));
  Serial.println(F("never be mistaken for real readings downstream."));
  for (uint8_t i = 0; i < count; i++) {
    MeasurementRecord rec = makeSimulatedMeasurement(i);
    processMeasurement(rec, false);
    delay(400);   // let the publish drain before the next one
  }
  Serial.println(F("=== end simulation ===\n"));
}

// ------------------------------------------------- Phase 3: model self-test
//
// Runs the classifier on canned feature vectors and prints the result. This
// exists because the sensors are not wired yet: without it there is no way to
// know the model actually executes correctly on real silicon rather than just
// on the host. The vectors are real rows from the held-out split, and the
// expected classes are what sklearn predicted for them at training time.
static void modelSelfTest() {
  Serial.println(F("\n--- on-device model self-test ---"));
  Serial.printf("  arch: %d -> %d (ReLU) -> %d (softmax), float32\n",
                MODEL_N_FEATURES, MODEL_N_HIDDEN, MODEL_N_CLASSES);
  Serial.println(F("  vectors + expected classes are generated with the"));
  Serial.println(F("  weights, so they cannot go stale after a retrain."));

  uint8_t pass = 0;
  for (int v = 0; v < SELFTEST_N; v++) {
    const uint32_t t0 = micros();
    Inference inf = runInference(SELFTEST_INPUTS[v], MODEL_N_FEATURES);
    const uint32_t dt = micros() - t0;

    if (!inf.valid) {
      Serial.printf("  vector %d -> INVALID\n", v);
      continue;
    }
    const char* got = inferenceClassName(inf.classIndex);
    const bool ok = strcmp(got, SELFTEST_EXPECTED[v]) == 0;
    if (ok) pass++;

    Serial.printf("  [%s] expect %-8s got %-8s conf %.1f%%  (%lu us)\n",
                  ok ? "ok" : "FAIL", SELFTEST_EXPECTED[v], got,
                  inf.confidence * 100.0f, (unsigned long)dt);
    Serial.printf("       GOOD %.3f  MARGINAL %.3f  REJECT %.3f\n",
                  inf.probabilities[0], inf.probabilities[1],
                  inf.probabilities[2]);
  }

  Serial.printf("  %u/%d vectors match the trained model.\n", pass, SELFTEST_N);
  Serial.println(F("  (times above are the real on-device inference cost)"));
  Serial.println(F("---------------------------------\n"));
}

// ------------------------------------------------- Phase 4: network config
//
// Credentials are typed here at runtime and stored in NVS. They are
// deliberately NOT in any source file, so this repository can be committed,
// shared, or submitted without leaking WiFi access.
static void configureWifi(const String& args) {
  const int sp = args.indexOf(' ');
  if (sp <= 0) {
    Serial.println(F("usage: wifi <ssid> <password>"));
    Serial.println(F("  (an SSID containing spaces is not supported here;"));
    Serial.println(F("   set it once from the Arduino IDE monitor instead)"));
    return;
  }
  String ssid = args.substring(0, sp);
  String pass = args.substring(sp + 1);
  ssid.trim();
  pass.trim();

  strncpy(netConfig.ssid, ssid.c_str(), sizeof(netConfig.ssid) - 1);
  strncpy(netConfig.pass, pass.c_str(), sizeof(netConfig.pass) - 1);
  netConfig.configured = strlen(netConfig.ssid) > 0 &&
                         strlen(netConfig.mqttHost) > 0;
  net::saveConfig(netConfig);

  // The password is never echoed back, here or in 'net'.
  Serial.printf("saved WiFi SSID '%s' (password %u chars, not shown)\n",
                netConfig.ssid, (unsigned)strlen(netConfig.pass));
  if (!netConfig.configured) {
    Serial.println(F("MQTT broker still unset - run: mqtt <host> [port]"));
  } else {
    net::begin(netConfig);
  }
}

static void configureMqtt(const String& args) {
  if (args.length() == 0) {
    Serial.println(F("usage: mqtt <host-or-ip> [port]   (default port 1883)"));
    return;
  }
  const int sp = args.indexOf(' ');
  String host = sp > 0 ? args.substring(0, sp) : args;
  host.trim();
  uint16_t port = 1883;
  if (sp > 0) {
    const long p = args.substring(sp + 1).toInt();
    if (p > 0 && p < 65536) port = (uint16_t)p;
  }

  strncpy(netConfig.mqttHost, host.c_str(), sizeof(netConfig.mqttHost) - 1);
  netConfig.mqttPort = port;
  netConfig.configured = strlen(netConfig.ssid) > 0 &&
                         strlen(netConfig.mqttHost) > 0;
  net::saveConfig(netConfig);

  Serial.printf("saved MQTT broker %s:%u\n", netConfig.mqttHost,
                netConfig.mqttPort);
  if (!netConfig.configured) {
    Serial.println(F("WiFi still unset - run: wifi <ssid> <password>"));
  } else {
    net::begin(netConfig);
  }
}

static void printNetState() {
  Serial.println(F("\nnetwork state:"));
  Serial.printf("  state    : %s\n", netStateName(net::state()));
  Serial.printf("  ssid     : %s\n",
                strlen(netConfig.ssid) ? netConfig.ssid : "(unset)");
  Serial.printf("  password : %s\n",
                strlen(netConfig.pass) ? "(set, not shown)" : "(unset)");
  Serial.printf("  broker   : %s:%u\n",
                strlen(netConfig.mqttHost) ? netConfig.mqttHost : "(unset)",
                netConfig.mqttPort);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("  ip       : %s   rssi %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }
  Serial.printf("  ntp time : %s\n", net::timeSynced() ? "synced" : "NOT synced");
  Serial.printf("  topic    : concresense/site/%s/test\n", DEVICE_ID);
  Serial.printf("  auto     : %s (every %d ms)\n", gAutoMeasure ? "ON" : "OFF",
                MEASURE_INTERVAL_MS);
  Serial.printf("  dropped  : %lu record(s) (queue full)\n",
                (unsigned long)gDroppedRecords);
  Serial.println();
}

static void printHelp() {
  Serial.println(F("\ncommands:"));
  Serial.println(F("  m  - run one measurement cycle"));
  Serial.println(F("  c  - run measurement, print as CSV (for --merge-real)"));
  Serial.println(F("  a  - toggle automatic measurement loop"));
  Serial.println(F("  cm - calibrate moisture range anchors"));
  Serial.println(F("  cl - calibrate load cell"));
  Serial.println(F("  s  - show calibration state"));
  Serial.println(F("  x  - erase stored calibration"));
  Serial.println(F("  t  - on-device model self-test"));
  Serial.println(F("  sim [n] - publish n SIMULATED measurements (default 3)"));
  Serial.println(F("  --- network (Phase 4) ---"));
  Serial.println(F("  wifi <ssid> <password>  - set + save WiFi credentials"));
  Serial.println(F("  mqtt <host> [port]      - set + save MQTT broker"));
  Serial.println(F("  net                     - show network status"));
  Serial.println(F("  netclear                - erase stored credentials"));
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

  if (c.startsWith("wifi ")) { configureWifi(c.substring(5)); return; }
  if (c.startsWith("mqtt")) { configureMqtt(c.substring(4)); return; }
  if (c == "net") { printNetState(); return; }
  if (c.startsWith("sim")) {
    const long n = c.length() > 3 ? c.substring(3).toInt() : 3;
    runSimulation((uint8_t)constrain(n, 1, 20));
    return;
  }
  if (c == "netclear") {
    net::clearConfig();
    netConfig = NetConfig();
    Serial.println(F("network credentials erased (reboot to apply)"));
    return;
  }
  if (c == "a") {
    gAutoMeasure = !gAutoMeasure;
    Serial.printf("automatic measurement %s\n", gAutoMeasure ? "ON" : "OFF");
    return;
  }

  if (c == "m") runMeasurementCycle(false);
  else if (c == "c") runMeasurementCycle(true);
  else if (c == "cm") calibrateMoisture();
  else if (c == "cl") calibrateLoadCell();
  else if (c == "s") printCalState();
  else if (c == "x") { anchors::clear(); Serial.println(F("calibration erased (reboot to apply)")); }
  else if (c == "t") modelSelfTest();
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
  if (s == SensorStatus::OK) {
    oled.splash(FW_VERSION);
    delay(800);
    // Coursework add-on: brief student-identity screen, shown once at boot
    // before the bring-up report. Uses the existing showStatusGrid() API
    // only -- no changes to the display driver itself.
    const char* idLines[] = {STUDENT_NAME, STUDENT_SECTION, STUDENT_ROLL};
    oled.showStatusGrid(idLines, 3);
    delay(1500);
  }

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

  // --- Phase 4: restore saved network credentials and start connecting.
  net::loadConfig(netConfig);
  if (netConfig.configured) {
    Serial.printf("[net] stored config: ssid '%s', broker %s:%u\n",
                  netConfig.ssid, netConfig.mqttHost, netConfig.mqttPort);
    net::begin(netConfig);
  } else {
    Serial.println(F("[net] not configured - run 'wifi' and 'mqtt' to set up"));
  }

  // --- Phase 3: hand the real work to the pinned tasks.
  startTasks();

  printCalState();
  printHelp();
  Serial.println(F("Ready. 'm' for one measurement, 'a' for the auto loop.\n"));
}

// ---------------------------------------------------------------- loop
//
// Deliberately almost empty. All real work now lives in the two pinned tasks;
// loopTask exists only to service the console.
//
// It must NOT read sensors directly any more. loopTask runs on Core 1 -- the
// same core as samplingTask -- so direct reads here would interleave with an
// in-flight IMU burst or HX711 conversion. Manual measurements go through
// runMeasurementCycle(), which takes gSensorMutex like everything else.
void loop() {
  handleCommand();

  // Low-rate heartbeat so an idle board still visibly proves both cores are
  // alive, without flooding the console during the auto loop.
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    Serial.printf("[hb] uptime %lus  heap %u B  net %s  auto %s  dropped %lu\n",
                  (unsigned long)(millis() / 1000), ESP.getFreeHeap(),
                  netStateName(net::state()), gAutoMeasure ? "ON" : "OFF",
                  (unsigned long)gDroppedRecords);
  }

  delay(50);
}
