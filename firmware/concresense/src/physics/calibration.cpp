#include "calibration.h"

namespace {
// Relative permittivities at ~25degC, radio frequency.
constexpr float EPS_CEMENT = 4.5f;     // hydrated/unhydrated Portland, 2-6
constexpr float EPS_AGGREGATE = 5.5f;  // siliceous aggregate, 4-7
constexpr float EPS_AIR = 1.0f;

// Densities, kg/m^3.
constexpr float RHO_WATER = 1000.0f;
constexpr float RHO_CEMENT = 3150.0f;  // Portland, specific gravity 3.15

// Nominal volumetric proportions of a typical structural mix. These are held
// fixed so a single dielectric measurement can be inverted for water content;
// that is the standard simplification, and it is also the largest source of
// error in the w/c estimate. A mix with unusual aggregate content will be
// mis-estimated and there is no way to detect that from permittivity alone.
constexpr float V_CEMENT = 0.13f;
constexpr float V_AGGREGATE = 0.65f;
constexpr float V_AIR = 0.02f;

constexpr float PRANDTL_NC = 2.0f + PI;  // ~5.14
}  // namespace

const char* qualityName(QualityClass q) {
  switch (q) {
    case QualityClass::GOOD: return "GOOD";
    case QualityClass::MARGINAL: return "MARGINAL";
    case QualityClass::REJECT: return "REJECT";
    default: return "UNKNOWN";
  }
}

float waterPermittivity(float tempC) {
  // Malmberg & Maryott (1956), J. Res. NBS 56(1):1-8.
  if (isnan(tempC)) tempC = 25.0f;
  if (tempC < 0.0f) tempC = 0.0f;
  if (tempC > 100.0f) tempC = 100.0f;
  const float t = tempC;
  return 87.740f - 0.40008f * t + 9.398e-4f * t * t - 1.410e-6f * t * t * t;
}

float lichteneckerWaterFraction(float epsMix, float tempC) {
  if (isnan(epsMix) || epsMix <= 1.0f) return NAN;

  const float epsW = waterPermittivity(tempC);

  // ln(eps_mix) = v_w ln(eps_w) + v_c ln(eps_c) + v_a ln(eps_a) + v_air ln(eps_air)
  // ln(eps_air) = 0, so that term vanishes.
  const float known = V_CEMENT * logf(EPS_CEMENT) + V_AGGREGATE * logf(EPS_AGGREGATE);
  const float vw = (logf(epsMix) - known) / logf(epsW);

  if (vw < 0.0f || vw > 0.45f) return NAN;  // outside anything physical
  return vw;
}

float millivoltsToPermittivity(float mv, float tempC, const MoistureAnchors& a) {
  if (isnan(mv)) return NAN;
  if (a.mvDry <= a.mvSat) return NAN;  // anchors inverted or unset

  // The v1.2 board's output voltage falls as permittivity rises, so the index
  // is built to increase with wetness.
  float theta = (a.mvDry - mv) / (a.mvDry - a.mvSat);
  if (theta < 0.0f) theta = 0.0f;
  if (theta > 1.0f) theta = 1.0f;

  // Interpolate in log-permittivity space, which is the space Lichtenecker is
  // linear in -- interpolating eps directly would bias every intermediate
  // reading low.
  const float lnDry = logf(EPS_AIR + 2.0f);  // dry solids baseline, ~3
  const float lnSat = logf(waterPermittivity(tempC));
  return expf(lnDry + theta * (lnSat - lnDry));
}

float waterFractionToWcRatio(float vWater) {
  if (isnan(vWater) || vWater <= 0.0f) return NAN;
  // w/c by mass = (v_w * rho_w) / (v_c * rho_c)
  return (vWater * RHO_WATER) / (V_CEMENT * RHO_CEMENT);
}

float penetrationForceToYieldStress(float forceN, float plungerAreaM2) {
  if (isnan(forceN) || forceN <= 0.0f) return NAN;
  if (plungerAreaM2 <= 0.0f) return NAN;
  // q = F/A,  tau_0 = q / (2 + pi)
  return (forceN / plungerAreaM2) / PRANDTL_NC;
}

float yieldStressToSlumpMm(float tau0Pa, float densityKgM3) {
  if (isnan(tau0Pa) || tau0Pa < 0.0f || densityKgM3 <= 0.0f) return NAN;
  const float s = 300.0f - 270.0f * tau0Pa / densityKgM3;
  if (s < 0.0f) return 0.0f;
  if (s > 300.0f) return 300.0f;
  return s;
}

QualityClass classifyIS456(float wcRatio, float slumpMm, float tempC) {
  if (isnan(wcRatio) || isnan(slumpMm)) return QualityClass::UNKNOWN;

  // Two gaps exist in the thresholds as written in the project docs:
  //   - w/c between 0.35 and 0.40 is in neither GOOD nor REJECT
  //   - slump below 50mm is in neither GOOD nor REJECT
  // Both are left unclassified there. They are closed here deliberately, and
  // in the conservative direction, because a very low w/c or a near-zero slump
  // is a real workability defect and must not silently pass as GOOD.

  // --- REJECT: any single hard violation is disqualifying.
  if (wcRatio > 0.55f || wcRatio < 0.35f) return QualityClass::REJECT;
  if (slumpMm > 150.0f) return QualityClass::REJECT;
  if (!isnan(tempC) && tempC > 40.0f) return QualityClass::REJECT;

  // --- MARGINAL
  bool marginal = false;
  if (wcRatio >= 0.50f && wcRatio <= 0.55f) marginal = true;
  if (wcRatio >= 0.35f && wcRatio < 0.40f) marginal = true;  // gap closure
  if (slumpMm > 125.0f && slumpMm <= 150.0f) marginal = true;
  if (slumpMm < 50.0f) marginal = true;                      // gap closure
  if (!isnan(tempC) && tempC >= 35.0f && tempC <= 40.0f) marginal = true;
  if (marginal) return QualityClass::MARGINAL;

  // --- GOOD: w/c 0.40-0.50, slump 50-125mm, temp < 35degC
  return QualityClass::GOOD;
}

DerivedProperties deriveAll(float moistureMv, float tempC, float loadCounts,
                            const MoistureAnchors& ma, const LoadCellAnchors& la) {
  DerivedProperties d;

  // --- moisture chain
  if (!isnan(moistureMv)) {
    d.epsilonMix = millivoltsToPermittivity(moistureMv, tempC, ma);
    d.waterVolFrac = lichteneckerWaterFraction(d.epsilonMix, tempC);
    d.wcRatio = waterFractionToWcRatio(d.waterVolFrac);
    // Only trustworthy once the board has actually been anchored.
    d.wcValid = ma.calibrated && !isnan(d.wcRatio);
  }

  // --- force chain
  if (!isnan(loadCounts) && la.countsPerNewton > 0.0f) {
    const float forceN = loadCounts / la.countsPerNewton;
    d.forceN = forceN;
    d.yieldStressPa = penetrationForceToYieldStress(forceN, la.plungerAreaM2);
    d.slumpMm = yieldStressToSlumpMm(d.yieldStressPa);
    d.slumpValid = la.calibrated && !isnan(d.slumpMm);
  }

  return d;
}
