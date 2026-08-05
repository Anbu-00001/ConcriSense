"""
ConcreSense shared physics — single source of truth.

Imported by BOTH `dataset_generator.py` and `simulation/concresense_sim.py` so the
animated simulation and the training data can never drift apart. If the simulation
showed one w/c and the dataset encoded another, every number in the report would be
unverifiable; keeping one module removes that possibility structurally.

These functions mirror the firmware in `firmware/concresense/src/physics/calibration.cpp`
line for line. The C++ side is covered by host tests in `tools/hosttest/`, which check
eps_w(25C) against the published 78.4 and close the slump<->tau0 round trip to <0.5mm.

GOVERNING RELATIONS
-------------------
  Lichtenecker logarithmic mixture rule       -> permittivity of the mix
  Malmberg & Maryott (1956) J.Res.NBS 56(1)   -> eps_water(T)
  Prandtl flat-punch bearing capacity (2+pi)  -> penetration force -> tau_0
  Hu & de Larrard FEA, reported in Ferraris &
    de Larrard (1998) Cem.Concr.Aggr. 20(2)   -> tau_0 <-> slump
  IS 456:2000                                 -> class thresholds

None of this is fitted against laboratory-tested concrete. The relations are
published; the mapping from a specific board's sensors into them is anchored on
proxy materials only.
"""

from __future__ import annotations

import math

import numpy as np

# ----------------------------------------------------------------- constants
EPS_CEMENT = 4.5
EPS_AGGREGATE = 5.5
EPS_AIR = 1.0

RHO_WATER = 1000.0
RHO_CEMENT = 3150.0  # Portland, specific gravity 3.15
RHO_CONCRETE = 2400.0

# Nominal volumetric proportions of a typical structural mix, held fixed so a
# single dielectric measurement can be inverted for water content. This is the
# standard simplification and also the largest error source in the w/c estimate:
# a mix with unusual aggregate content is mis-estimated, and permittivity alone
# cannot detect that.
V_CEMENT = 0.13
V_AGGREGATE = 0.65

PRANDTL_NC = 2.0 + math.pi  # ~5.14

# Board anchors: the simulated board's response. A real deployment overwrites
# these with the on-device 'cm' command (see anchors_store.h).
MV_DRY = 2900.0
MV_SAT = 1300.0

PLUNGER_AREA_M2 = math.pi * (0.005 ** 2)  # 10mm diameter flat punch

CLASSES = ["GOOD", "MARGINAL", "REJECT"]

# The MPU-6050 DLPF is configured at 44Hz, so content above that corner is
# attenuated in hardware before it is ever sampled.
IMU_SAMPLE_RATE_HZ = 200.0
IMU_BURST_SAMPLES = 256
DLPF_CORNER_HZ = 44.0


# ------------------------------------------------------------------ physics
def water_permittivity(temp_c):
    """Malmberg & Maryott (1956), J. Res. NBS 56(1):1-8. Valid 0-100 degC."""
    t = np.clip(temp_c, 0.0, 100.0)
    return 87.740 - 0.40008 * t + 9.398e-4 * t**2 - 1.410e-6 * t**3


def wc_to_water_volume_fraction(wc):
    """w/c by mass -> water volume fraction, at fixed cement content."""
    return (np.asarray(wc) * V_CEMENT * RHO_CEMENT) / RHO_WATER


def lichtenecker_eps_mix(v_water, temp_c):
    """ln(eps_mix) = sum_i v_i ln(eps_i).  ln(eps_air)=0 so air drops out."""
    v_air = np.clip(1.0 - v_water - V_CEMENT - V_AGGREGATE, 0.0, 0.10)
    ln_eps = (
        v_water * np.log(water_permittivity(temp_c))
        + V_CEMENT * math.log(EPS_CEMENT)
        + V_AGGREGATE * math.log(EPS_AGGREGATE)
        + v_air * math.log(EPS_AIR)
    )
    return np.exp(ln_eps)


def eps_mix_to_millivolts(eps_mix, temp_c, rng, noisy=True):
    """
    Forward model of the capacitive v1.2 sensor: permittivity -> ADC millivolts.

    The distortions below are what make the classification task non-trivial.
    Without them the mapping is bijective and any model recovers w/c exactly.
    """
    ln_dry = math.log(EPS_AIR + 2.0)
    ln_sat = np.log(water_permittivity(temp_c))
    theta = np.clip((np.log(eps_mix) - ln_dry) / (ln_sat - ln_dry), 0.0, 1.0)

    mv = MV_DRY - theta * (MV_DRY - MV_SAT)

    if noisy:
        shape = np.shape(mv)
        # 1. Ionic conduction: fresh pore solution is loaded with Ca2+/OH-, and
        #    at the v1.2 board's ~1.5MHz excitation that conduction still leaks
        #    into the reading, biasing it wet. Largest physical error source.
        mv = mv + rng.normal(0.0, 45.0, size=shape)
        # 2. Residual 555-oscillator drift beyond the eps_w(T) compensation.
        mv = mv - 2.2 * (np.asarray(temp_c) - 25.0)
        # 3. ESP32 ADC noise after 64x averaging.
        mv = mv + rng.normal(0.0, 6.0, size=shape)

    # 12-bit quantisation over the 0-3.3V span.
    mv = np.round(np.clip(mv, 0.0, 3300.0) / 3300.0 * 4095.0) / 4095.0 * 3300.0
    return mv


def millivolts_to_eps_mix(mv, temp_c):
    """Inverse of the forward model — what the firmware actually computes."""
    ln_dry = math.log(EPS_AIR + 2.0)
    ln_sat = np.log(water_permittivity(temp_c))
    theta = np.clip((MV_DRY - mv) / (MV_DRY - MV_SAT), 0.0, 1.0)
    return np.exp(ln_dry + theta * (ln_sat - ln_dry))


def lichtenecker_water_fraction(eps_mix, temp_c):
    """Invert the mixture rule for water volume fraction."""
    known = V_CEMENT * math.log(EPS_CEMENT) + V_AGGREGATE * math.log(EPS_AGGREGATE)
    return (np.log(eps_mix) - known) / np.log(water_permittivity(temp_c))


def water_fraction_to_wc(v_water):
    """w/c by mass = (v_w * rho_w) / (v_c * rho_c)."""
    return (v_water * RHO_WATER) / (V_CEMENT * RHO_CEMENT)


def slump_to_yield_stress(slump_mm):
    """Hu & de Larrard, via Ferraris & de Larrard (1998): tau0 = rho/270*(300-s)."""
    return np.maximum(RHO_CONCRETE / 270.0 * (300.0 - np.asarray(slump_mm)), 1.0)


def yield_stress_to_slump(tau0_pa, density=RHO_CONCRETE):
    """Inverse: s = 300 - 270*tau0/rho."""
    return np.clip(300.0 - 270.0 * np.asarray(tau0_pa) / density, 0.0, 300.0)


# The firmware's LoadCellHX711::read() takes a MEDIAN of this many raw reads.
# The forward model must apply the same filter, otherwise the simulated sensor
# is noisier than the real one and every downstream number is pessimistic.
LOADCELL_MEDIAN_READS = 5


def _raw_force_draw(f, rng):
    """One unfiltered HX711 reading, with all three real error sources."""
    shape = np.shape(f)
    f = f * rng.normal(1.0, 0.08, size=shape)          # mix-to-mix scatter
    # Coarse aggregate is the dominant nuisance: a 20mm stone under a 10mm punch
    # reads the stone, not the mix. Modelled as a heavy RIGHT tail rather than
    # symmetric noise -- getting that shape wrong would make a classifier
    # over-trust the load channel.
    strike = rng.random(shape) < 0.07
    f = np.where(strike, f * rng.uniform(1.4, 2.6, size=shape), f)
    return f + rng.normal(0.0, 0.05, size=shape)       # HX711 electrical noise


def yield_stress_to_force_n(tau0, rng, noisy=True):
    """
    Prandtl flat-punch: q_ult = (2+pi)*tau_0, so F = tau_0 * Nc * A.

    Returns the MEDIAN of LOADCELL_MEDIAN_READS draws, mirroring
    LoadCellHX711::read(). The median is not cosmetic here: stone-strike errors
    are large and one-sided, so a mean drags with them while a median rejects
    them outright. Simulating a single raw draw instead made ~30% of otherwise
    valid mixes misclassify, because one strike is enough to push the derived
    slump to zero.
    """
    f = np.asarray(tau0, dtype=float) * PRANDTL_NC * PLUNGER_AREA_M2
    if not noisy:
        return np.maximum(f, 0.01)

    draws = np.stack([_raw_force_draw(f, rng) for _ in range(LOADCELL_MEDIAN_READS)])
    return np.maximum(np.median(draws, axis=0), 0.01)


def force_n_to_yield_stress(force_n):
    """tau_0 = (F/A)/(2+pi) — what the firmware computes from the load cell."""
    return (np.asarray(force_n) / PLUNGER_AREA_M2) / PRANDTL_NC


def vibration_signature(wc, slump_mm, rng, n):
    """
    Vibration features vs mix state.

    Physical reasoning: a well-proportioned mix fluidises uniformly and damps the
    excitation smoothly. A wet, segregating mix damps more but its spectrum
    broadens as bleed water and settling aggregate decouple. A stiff mix
    transmits more energy and rings at higher frequency.

    Stated plainly: this is the weakest-grounded channel in the project. The
    trend directions are defensible; the coefficients are plausible, not fitted.
    Flagged here so it is never reported as validated.
    """
    wetness = np.clip((np.asarray(wc) - 0.35) / 0.25, 0.0, 1.5)
    stiffness = np.clip((150.0 - np.asarray(slump_mm)) / 150.0, 0.0, 1.2)

    rms = np.clip(0.28 + 0.30 * stiffness - 0.10 * wetness + rng.normal(0, 0.035, n),
                  0.02, 1.5)
    dom = np.clip(34.0 + 14.0 * stiffness - 6.0 * wetness + rng.normal(0, 2.4, n),
                  5.0, DLPF_CORNER_HZ)
    entropy = np.clip(0.42 + 0.26 * wetness - 0.10 * stiffness + rng.normal(0, 0.05, n),
                      0.05, 0.99)
    damping = np.clip(0.055 + 0.070 * wetness - 0.020 * stiffness + rng.normal(0, 0.012, n),
                      0.005, 0.45)
    return rms, dom, entropy, damping


# ---------------------------------------------------------------- labelling
def classify_is456(wc, slump_mm, temp_c):
    """
    IS 456:2000 rule engine. Mirrors classifyIS456() in the firmware exactly.

    Two gaps in the thresholds as written in the project docs are closed here,
    conservatively, and the firmware closes them identically:
      - w/c in [0.35, 0.40) belonged to no class
      - slump < 50mm belonged to no class
    Both are genuine workability defects and must not fall through to GOOD.
    """
    wc = np.asarray(wc)
    slump_mm = np.asarray(slump_mm)
    temp_c = np.asarray(temp_c)

    out = np.full(wc.shape, "GOOD", dtype=object)

    marginal = (
        ((wc >= 0.50) & (wc <= 0.55))
        | ((wc >= 0.35) & (wc < 0.40))
        | ((slump_mm > 125.0) & (slump_mm <= 150.0))
        | (slump_mm < 50.0)
        | ((temp_c >= 35.0) & (temp_c <= 40.0))
    )
    out[marginal] = "MARGINAL"

    reject = (wc > 0.55) | (wc < 0.35) | (slump_mm > 150.0) | (temp_c > 40.0)
    out[reject] = "REJECT"
    return out


def classify_scalar(wc, slump_mm, temp_c) -> str:
    """Scalar convenience wrapper for the simulation's per-frame call."""
    return str(classify_is456(np.array([wc]), np.array([slump_mm]),
                              np.array([temp_c]))[0])


def reason_for_class(wc, slump_mm, temp_c):
    """
    Which rule actually fired. The device is a screening tool, so 'why' matters
    more than the label: 'REJECT' alone tells an engineer nothing actionable.
    """
    reasons = []
    if wc > 0.55:
        reasons.append(f"w/c {wc:.2f} > 0.55 (excess water)")
    elif wc < 0.35:
        reasons.append(f"w/c {wc:.2f} < 0.35 (too dry, unworkable)")
    elif 0.50 <= wc <= 0.55:
        reasons.append(f"w/c {wc:.2f} in 0.50-0.55")
    elif 0.35 <= wc < 0.40:
        reasons.append(f"w/c {wc:.2f} in 0.35-0.40 (doc gap -> MARGINAL)")

    if slump_mm > 150.0:
        reasons.append(f"slump {slump_mm:.0f}mm > 150 (segregating)")
    elif slump_mm < 50.0:
        reasons.append(f"slump {slump_mm:.0f}mm < 50 (doc gap -> MARGINAL)")
    elif 125.0 < slump_mm <= 150.0:
        reasons.append(f"slump {slump_mm:.0f}mm in 125-150")

    if temp_c > 40.0:
        reasons.append(f"temp {temp_c:.1f}C > 40")
    elif 35.0 <= temp_c <= 40.0:
        reasons.append(f"temp {temp_c:.1f}C in 35-40")

    return reasons or ["all parameters within IS 456 GOOD limits"]


# ------------------------------------------------- DSP (mirrors fft_features.cpp)
def synth_vibration_waveform(dom_hz, rms, entropy, rng, n=IMU_BURST_SAMPLES,
                             fs=IMU_SAMPLE_RATE_HZ):
    """
    Build an acceleration-magnitude waveform with the requested character.

    Includes the ~1g gravity pedestal on purpose: accel MAGNITUDE always carries
    it, and removing it is the single step that makes the spectrum meaningful.
    The simulation shows that step explicitly.
    """
    t = np.arange(n) / fs
    tone = rms * math.sqrt(2.0) * np.sin(2 * math.pi * dom_hz * t)
    noise = rng.normal(0.0, rms * entropy * 1.4, n)
    decay = np.exp(-t / (t[-1] / 2.2))  # damped ring-down
    return 1.0 + tone * decay + noise


def extract_features(buf, fs=IMU_SAMPLE_RATE_HZ):
    """
    Mirrors extractVibrationFeatures() in fft_features.cpp.

    Returns (features_dict, freqs, psd) so the animation can draw the same
    spectrum the firmware computes.
    """
    n = len(buf)
    mean = float(np.mean(buf))
    ac = buf - mean  # DC/gravity removal — without this bin 0 swamps everything

    rms = float(np.sqrt(np.mean(ac**2)))
    p2p = float(np.max(buf) - np.min(buf))

    window = 0.5 * (1.0 - np.cos(2 * math.pi * np.arange(n) / (n - 1)))  # Hann
    spec = np.fft.rfft(ac * window)
    psd = (np.abs(spec) ** 2)[1:]  # drop bin 0; mean already removed
    freqs = np.fft.rfftfreq(n, 1.0 / fs)[1:]

    total = float(psd.sum())
    if total <= 0:
        feats = dict(rms=rms, p2p=p2p, dominant_hz=0.0, entropy=0.0,
                     centroid_hz=0.0, damping=0.0)
        return feats, freqs, psd

    dominant_hz = float(freqs[int(np.argmax(psd))])
    centroid_hz = float((psd * freqs).sum() / total)

    p = psd / total
    p = p[p > 1e-12]
    entropy = float(-(p * np.log(p)).sum() / math.log(len(psd)))

    q = n // 4
    e1 = float(np.sum(ac[:q] ** 2))
    e2 = float(np.sum(ac[-q:] ** 2))
    if e1 > 1e-12 and e2 > 1e-12:
        delta = 0.5 * math.log(e1 / e2)
        damping = delta / math.sqrt(4 * math.pi**2 + delta**2)
    else:
        damping = 0.0

    feats = dict(rms=rms, p2p=p2p, dominant_hz=dominant_hz, entropy=entropy,
                 centroid_hz=centroid_hz, damping=float(damping))
    return feats, freqs, psd
