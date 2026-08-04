#pragma once
#include <Arduino.h>

// Physics calibration: raw sensor units -> engineering quantities.
//
// Every constant here is either (a) a published physical relation, cited at its
// definition, or (b) a device-specific anchor that MUST be measured on the
// actual hardware. The two are kept visibly separate so nobody mistakes a
// placeholder for a validated value.
//
// Honest scope statement: none of this has been fitted against laboratory-
// tested concrete. The published relations are sound; the mapping from THIS
// board's sensors into them is anchored on proxy materials only. Treat outputs
// as a screening indication, not a lab measurement.

// ---------------------------------------------------------------- anchors
//
// Measured per-board with the on-device 'cm' and 'cl' serial commands, and
// persisted to NVS (see anchors_store.h). The defaults below are typical values
// for a capacitive v1.2 sensor at 3.3V and exist only so the firmware runs
// before calibration -- they are NOT valid for real measurement, which is why
// `calibrated` gates every derived reading.
struct MoistureAnchors {
  float mvDry = 2900.0f;   // probe in air / oven-dry sand
  float mvSat = 1300.0f;   // probe fully immersed in water
  float refTempC = 25.0f;  // temperature at which the anchors were taken
  bool calibrated = false; // set true only by a real calibration run
};

struct LoadCellAnchors {
  float countsPerNewton = 1.0f;  // from a known-mass calibration
  float plungerAreaM2 = 7.85e-5f; // 10mm dia flat punch: pi*r^2
  bool calibrated = false;
};

// ---------------------------------------------------------------- results
struct DerivedProperties {
  float epsilonMix = NAN;    // apparent relative permittivity
  float waterVolFrac = NAN;  // v_w
  float wcRatio = NAN;       // water/cement by mass
  float yieldStressPa = NAN; // tau_0
  float slumpMm = NAN;
  bool wcValid = false;
  bool slumpValid = false;
};

enum class QualityClass : uint8_t { GOOD = 0, MARGINAL = 1, REJECT = 2, UNKNOWN = 3 };
const char* qualityName(QualityClass q);

// --- Relative permittivity of free water vs temperature.
// Malmberg & Maryott (1956), J. Res. NBS 56(1):1-8. Valid 0-100 degC.
// Matters because eps_w falls ~0.36 per degC: a 20degC swing shifts the
// reading enough to look like a w/c change if left uncompensated.
float waterPermittivity(float tempC);

// --- Lichtenecker logarithmic mixture rule, inverted for water fraction.
//   ln(eps_mix) = sum_i v_i ln(eps_i)
float lichteneckerWaterFraction(float epsMix, float tempC);

// --- Capacitive sensor mV -> apparent permittivity, via two-point anchoring.
float millivoltsToPermittivity(float mv, float tempC, const MoistureAnchors& a);

// --- Water volume fraction -> w/c by mass.
float waterFractionToWcRatio(float vWater);

// --- Penetration force -> shear yield stress.
//
// Prandtl's flat-punch bearing-capacity solution for a cohesive (Tresca)
// medium: q_ult = (2 + pi) * tau_0 ~= 5.14 * tau_0.
//
// This replaces the "tau_0 = K1*ln(F) + K2, R^2 > 0.92" claim in the project
// docs, which cites no source and could not be verified. The punch solution is
// standard plasticity theory and is at least honestly attributable.
float penetrationForceToYieldStress(float forceN, float plungerAreaM2);

// --- Yield stress -> slump.
//
// Inverted from the finite-element result of Hu & de Larrard, as reported in
// Ferraris & de Larrard, "Modified Slump Test to Measure Rheological Parameters
// of Fresh Concrete", Cement, Concrete and Aggregates 20(2), 1998:
//     tau_0 = rho/270 * (300 - s)      [tau_0 in Pa, rho in kg/m^3, s in mm]
// so  s = 300 - 270 * tau_0 / rho.
// The relation is calibrated for slumps of roughly 50-260mm; outside that it
// is extrapolation and the caller is told so.
float yieldStressToSlumpMm(float tau0Pa, float densityKgM3 = 2400.0f);

// --- IS 456:2000 rule-based classification.
//
// Used to LABEL the synthetic dataset, and as a sanity check against the
// TinyML output. Note this is a deterministic rule engine, not a model: if the
// classifier ever merely reproduces these rules, the ML adds nothing, which is
// exactly the trap the project docs' ">=90% accuracy" target falls into.
QualityClass classifyIS456(float wcRatio, float slumpMm, float tempC);

DerivedProperties deriveAll(float moistureMv, float tempC, float loadCounts,
                            const MoistureAnchors& ma, const LoadCellAnchors& la);
