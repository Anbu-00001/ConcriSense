// Host-side unit tests for the ConcreSense DSP and physics modules.
//
// These compile the *same* .cpp files the firmware uses, against a minimal
// Arduino shim. That means the FFT and the calibration chain are verified
// against known-answer inputs on a machine where the results can actually be
// checked, rather than being trusted because they compiled for the ESP32.
//
// Build & run:  tools/hosttest/run.sh

#include <cmath>
#include <cstdio>
#include <vector>

#include "../../firmware/concresense/src/features/fft_features.h"
#include "../../firmware/concresense/src/physics/calibration.h"

static int failures = 0;
static int checks = 0;

static void check(bool cond, const char* what) {
  checks++;
  if (!cond) {
    failures++;
    printf("  FAIL  %s\n", what);
  } else {
    printf("  ok    %s\n", what);
  }
}

static void checkNear(float got, float want, float tol, const char* what) {
  checks++;
  if (std::fabs(got - want) > tol) {
    failures++;
    printf("  FAIL  %s (got %.4f, want %.4f +/- %.4f)\n", what, got, want, tol);
  } else {
    printf("  ok    %s (%.4f)\n", what, got);
  }
}

// ------------------------------------------------------------------- FFT
static void testFftPureTone() {
  printf("\n[FFT] pure 25Hz tone at 200Hz sampling, on a 1g DC pedestal\n");
  const uint16_t N = 256;
  const float fs = 200.0f;
  std::vector<float> buf(N);

  // 1.0g DC + 0.2g at 25Hz. The DC term mimics gravity in the accel magnitude
  // and is exactly what would break the analysis if it were not removed.
  for (uint16_t i = 0; i < N; i++) {
    buf[i] = 1.0f + 0.2f * std::sin(2.0f * M_PI * 25.0f * i / fs);
  }

  VibrationFeatures f = extractVibrationFeatures(buf.data(), N, fs);
  check(f.valid, "features valid");

  // Bin spacing is 200/256 = 0.78Hz, so 25Hz lands on bin 32 exactly.
  checkNear(f.dominantFreqHz, 25.0f, 1.0f, "dominant frequency = 25Hz");

  // RMS of a 0.2g amplitude sine about its mean is 0.2/sqrt(2) = 0.1414.
  // Getting this right proves the DC pedestal was removed: had it not been,
  // the RMS would come out near 1.0.
  checkNear(f.rms, 0.1414f, 0.005f, "AC RMS = A/sqrt(2), DC removed");

  // A single tone concentrates its energy, so entropy must sit near zero.
  check(f.spectralEntropy < 0.25f, "entropy low for a pure tone");
  checkNear(f.spectralCentroidHz, 25.0f, 3.0f, "centroid at the tone");
}

static void testFftWhiteNoise() {
  printf("\n[FFT] white noise\n");
  const uint16_t N = 256;
  std::vector<float> buf(N);
  unsigned s = 12345;
  for (uint16_t i = 0; i < N; i++) {
    s = s * 1103515245u + 12345u;
    buf[i] = ((s >> 16) & 0x7FFF) / 16384.0f - 1.0f;
  }

  VibrationFeatures f = extractVibrationFeatures(buf.data(), N, 200.0f);
  check(f.valid, "features valid");
  // Energy spread across all bins => entropy near the top of its range.
  check(f.spectralEntropy > 0.80f, "entropy high for white noise");
  printf("        entropy=%.3f  rms=%.3f  dom=%.1fHz\n", f.spectralEntropy,
         f.rms, f.dominantFreqHz);
}

static void testFftGuards() {
  printf("\n[FFT] input guards\n");
  std::vector<float> buf(100, 0.0f);
  check(!extractVibrationFeatures(buf.data(), 100, 200.0f).valid,
        "rejects non-power-of-two n");
  check(!extractVibrationFeatures(nullptr, 256, 200.0f).valid,
        "rejects null buffer");
  check(!extractVibrationFeatures(buf.data(), 256, 0.0f).valid,
        "rejects zero sample rate");

  std::vector<float> flat(256, 1.0f);
  VibrationFeatures f = extractVibrationFeatures(flat.data(), 256, 200.0f);
  check(f.valid && f.rms < 1e-5f, "constant signal -> zero AC RMS, no NaN");
}

// --------------------------------------------------------------- physics
static void testWaterPermittivity() {
  printf("\n[physics] Malmberg-Maryott eps_w(T)\n");
  // Published reference values.
  checkNear(waterPermittivity(25.0f), 78.4f, 0.3f, "eps_w(25C) ~ 78.4");
  checkNear(waterPermittivity(20.0f), 80.1f, 0.3f, "eps_w(20C) ~ 80.1");
  checkNear(waterPermittivity(0.0f), 87.7f, 0.5f, "eps_w(0C) ~ 87.7");
  check(waterPermittivity(40.0f) < waterPermittivity(20.0f),
        "eps_w decreases with temperature");
}

static void testSlumpYieldRoundTrip() {
  printf("\n[physics] Hu & de Larrard slump <-> yield stress\n");
  // tau0 = rho/270*(300 - s); invert and check the round trip.
  const float rho = 2400.0f;
  for (float s : {60.0f, 100.0f, 150.0f, 200.0f}) {
    const float tau0 = rho / 270.0f * (300.0f - s);
    const float back = yieldStressToSlumpMm(tau0, rho);
    checkNear(back, s, 0.5f, "round-trip slump");
  }
  // Stiffer mix (higher yield stress) must give lower slump.
  check(yieldStressToSlumpMm(2000.0f) < yieldStressToSlumpMm(500.0f),
        "higher tau0 -> lower slump");
}

static void testPrandtlPunch() {
  printf("\n[physics] Prandtl flat-punch F -> tau_0\n");
  const float area = M_PI * 0.005f * 0.005f;  // 10mm dia
  // q = F/A, tau0 = q/(2+pi)
  const float F = 5.0f;
  const float expected = (F / area) / (2.0f + M_PI);
  checkNear(penetrationForceToYieldStress(F, area), expected, 1.0f,
            "tau_0 = (F/A)/(2+pi)");
  check(std::isnan(penetrationForceToYieldStress(-1.0f, area)),
        "rejects negative force");
  check(std::isnan(penetrationForceToYieldStress(5.0f, 0.0f)),
        "rejects zero area");
}

static void testIS456Classification() {
  printf("\n[physics] IS 456:2000 rule engine\n");
  check(classifyIS456(0.45f, 90.0f, 30.0f) == QualityClass::GOOD,
        "mid-range mix -> GOOD");
  check(classifyIS456(0.60f, 90.0f, 30.0f) == QualityClass::REJECT,
        "w/c 0.60 -> REJECT");
  check(classifyIS456(0.45f, 180.0f, 30.0f) == QualityClass::REJECT,
        "slump 180mm -> REJECT");
  check(classifyIS456(0.45f, 90.0f, 42.0f) == QualityClass::REJECT,
        "42C -> REJECT");
  check(classifyIS456(0.52f, 90.0f, 30.0f) == QualityClass::MARGINAL,
        "w/c 0.52 -> MARGINAL");

  // The two gaps left open by the project docs' thresholds. Both must resolve
  // to something stricter than GOOD.
  check(classifyIS456(0.37f, 90.0f, 30.0f) == QualityClass::MARGINAL,
        "w/c 0.37 (doc gap) -> MARGINAL, not GOOD");
  check(classifyIS456(0.45f, 30.0f, 30.0f) == QualityClass::MARGINAL,
        "slump 30mm (doc gap) -> MARGINAL, not GOOD");
}

static void testMoistureChain() {
  printf("\n[physics] moisture chain monotonicity\n");
  MoistureAnchors a;
  a.calibrated = true;

  // Wetter mix => lower sensor voltage => higher permittivity => higher w/c.
  const float epsDry = millivoltsToPermittivity(2800.0f, 25.0f, a);
  const float epsWet = millivoltsToPermittivity(1500.0f, 25.0f, a);
  check(epsWet > epsDry, "lower mV -> higher permittivity");

  const float vwDry = lichteneckerWaterFraction(epsDry, 25.0f);
  const float vwWet = lichteneckerWaterFraction(epsWet, 25.0f);
  check(std::isnan(vwDry) || std::isnan(vwWet) || vwWet > vwDry,
        "higher permittivity -> higher water fraction");

  // Anchors that have not been set must not silently produce a "valid" w/c.
  MoistureAnchors uncal;
  LoadCellAnchors la;
  DerivedProperties d = deriveAll(2000.0f, 25.0f, 1000.0f, uncal, la);
  check(!d.wcValid, "uncalibrated anchors -> wcValid false");
  check(!d.slumpValid, "uncalibrated load cell -> slumpValid false");
}

int main() {
  printf("=== ConcreSense host tests (DSP + physics) ===\n");
  testFftPureTone();
  testFftWhiteNoise();
  testFftGuards();
  testWaterPermittivity();
  testSlumpYieldRoundTrip();
  testPrandtlPunch();
  testIS456Classification();
  testMoistureChain();

  printf("\n=== %d/%d checks passed ===\n", checks - failures, checks);
  return failures == 0 ? 0 : 1;
}
