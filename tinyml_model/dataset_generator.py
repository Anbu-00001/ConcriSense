#!/usr/bin/env python3
"""
ConcreSense synthetic dataset generator (Phase 2).

WHAT THIS IS
------------
A physics-based *simulator*. It samples plausible fresh-mix states, forward-
models what each sensor on the ConcreSense board would read in that state
(including that sensor's real noise and distortion), and labels the result
against IS 456:2000 thresholds.

WHAT THIS IS NOT
----------------
It is not measured data. No concrete was tested. Every row is stamped
`data_source=synthetic_physics` and the provenance header names the governing
equations. Any accuracy figure computed against this file measures how well a
model recovers the simulator, not how well it screens real concrete.

Why that distinction is enforced in code rather than left to the report: a
classifier trained on rule-labelled data will score ~99% against those same
rules. Reporting that as "classification accuracy" would be meaningless. The
generator therefore (a) labels from *true* physical values but (b) exports only
*sensor-level* features, so the model has to invert a noisy forward model
rather than re-read the labels off its own inputs. The residual difficulty is
real, and the reported metric is called separability, not accuracy.

GOVERNING RELATIONS (each cited where used)
-------------------------------------------
  Lichtenecker logarithmic mixture rule      -> permittivity of the mix
  Malmberg & Maryott (1956) J.Res.NBS 56(1)  -> eps_water(T)
  Prandtl flat-punch bearing capacity        -> penetration force -> tau_0
  Hu & de Larrard FEA, in Ferraris & de
    Larrard (1998) Cem.Concr.Aggr. 20(2)     -> tau_0 <-> slump
  IS 456:2000                                -> class thresholds

Usage:
    python3 dataset_generator.py --n 900 --out dataset.csv
    python3 dataset_generator.py --n 900 --merge-real real_readings.csv
"""

from __future__ import annotations

import argparse
import csv
import datetime as _dt
import json
import math
import pathlib
import sys

import numpy as np

# ----------------------------------------------------------------- constants
EPS_CEMENT = 4.5
EPS_AGGREGATE = 5.5
EPS_AIR = 1.0

RHO_WATER = 1000.0
RHO_CEMENT = 3150.0
RHO_CONCRETE = 2400.0

V_CEMENT = 0.13
V_AGGREGATE = 0.65

PRANDTL_NC = 2.0 + math.pi  # ~5.14

# Board anchors. These are the *simulated* board's response; a real deployment
# overwrites them from tools/calibrate.py. Kept explicit so the synthetic ADC
# span matches the hardware the model will actually run on.
MV_DRY = 2900.0
MV_SAT = 1300.0

PLUNGER_AREA_M2 = math.pi * (0.005 ** 2)  # 10mm diameter flat punch

CLASSES = ["GOOD", "MARGINAL", "REJECT"]


# ----------------------------------------------------------------- physics
def water_permittivity(temp_c: np.ndarray) -> np.ndarray:
    """Malmberg & Maryott (1956), J. Res. NBS 56(1):1-8. Valid 0-100 degC."""
    t = np.clip(temp_c, 0.0, 100.0)
    return 87.740 - 0.40008 * t + 9.398e-4 * t**2 - 1.410e-6 * t**3


def wc_to_water_volume_fraction(wc: np.ndarray) -> np.ndarray:
    """w/c by mass -> water volume fraction, at fixed cement content."""
    return (wc * V_CEMENT * RHO_CEMENT) / RHO_WATER


def lichtenecker_eps_mix(v_water: np.ndarray, temp_c: np.ndarray) -> np.ndarray:
    """ln(eps_mix) = sum_i v_i ln(eps_i).  ln(eps_air)=0 so air drops out."""
    v_air = np.clip(1.0 - v_water - V_CEMENT - V_AGGREGATE, 0.0, 0.10)
    ln_eps = (
        v_water * np.log(water_permittivity(temp_c))
        + V_CEMENT * math.log(EPS_CEMENT)
        + V_AGGREGATE * math.log(EPS_AGGREGATE)
        + v_air * math.log(EPS_AIR)
    )
    return np.exp(ln_eps)


def eps_mix_to_millivolts(eps_mix, temp_c, rng) -> np.ndarray:
    """
    Forward model of the capacitive v1.2 sensor.

    Inverts the firmware's log-space interpolation, then adds the distortions a
    real board shows. These are what make the task non-trivial: without them the
    mapping is bijective and any model recovers w/c exactly.
    """
    ln_dry = math.log(EPS_AIR + 2.0)
    ln_sat = np.log(water_permittivity(temp_c))
    theta = (np.log(eps_mix) - ln_dry) / (ln_sat - ln_dry)
    theta = np.clip(theta, 0.0, 1.0)

    mv = MV_DRY - theta * (MV_DRY - MV_SAT)

    # 1. Ionic conduction. Fresh pore solution is loaded with Ca2+/OH-, and at
    #    the v1.2 board's ~1.5MHz excitation that conduction still leaks into
    #    the reading, biasing it wet. This is the single largest physical error
    #    source and the reason the docs' insulated-probe note matters.
    ionic_bias = rng.normal(0.0, 45.0, size=mv.shape)

    # 2. Residual temperature drift of the 555 oscillator, beyond what the
    #    eps_w(T) compensation already removes.
    temp_drift = -2.2 * (temp_c - 25.0)

    # 3. ESP32 ADC noise after 64x averaging, plus quantisation at 12 bits.
    adc_noise = rng.normal(0.0, 6.0, size=mv.shape)

    mv = mv + ionic_bias + temp_drift + adc_noise
    mv = np.round(np.clip(mv, 0.0, 3300.0) / 3300.0 * 4095.0) / 4095.0 * 3300.0
    return mv


def slump_to_yield_stress(slump_mm: np.ndarray) -> np.ndarray:
    """Hu & de Larrard, via Ferraris & de Larrard (1998): tau0 = rho/270*(300-s)."""
    return np.maximum(RHO_CONCRETE / 270.0 * (300.0 - slump_mm), 1.0)


def yield_stress_to_force_n(tau0: np.ndarray, rng) -> np.ndarray:
    """
    Prandtl flat-punch: q_ult = (2+pi)*tau_0, so F = tau_0 * Nc * A.

    Coarse aggregate is the dominant nuisance: a 20mm stone under a 10mm punch
    reads the stone, not the mix. That shows up as rare large positive outliers,
    modelled here as a heavy right tail rather than symmetric noise -- getting
    this shape wrong would make the classifier over-trust the load channel.
    """
    f = tau0 * PRANDTL_NC * PLUNGER_AREA_M2
    f = f * rng.normal(1.0, 0.08, size=f.shape)                    # mix scatter
    strike = rng.random(f.shape) < 0.07                            # stone strike
    f = np.where(strike, f * rng.uniform(1.4, 2.6, size=f.shape), f)
    f = f + rng.normal(0.0, 0.05, size=f.shape)                    # HX711 noise
    return np.maximum(f, 0.01)


def vibration_signature(wc, slump_mm, rng, n):
    """
    Vibration features vs mix state.

    Physical reasoning: a well-proportioned mix fluidises uniformly and damps
    the excitation smoothly. A wet, segregating mix damps more but its spectrum
    broadens as bleed water and settling aggregate decouple. A stiff mix
    transmits more energy and rings at higher frequency.

    Stated plainly: this is the weakest-grounded channel in the project. The
    trend directions are defensible; the coefficients are chosen to be
    plausible, not fitted. It is included because the docs specify it, and it is
    flagged here so nobody reports it as validated.
    """
    wetness = np.clip((wc - 0.35) / 0.25, 0.0, 1.5)
    stiffness = np.clip((150.0 - slump_mm) / 150.0, 0.0, 1.2)

    rms = 0.28 + 0.30 * stiffness - 0.10 * wetness + rng.normal(0, 0.035, n)
    rms = np.clip(rms, 0.02, 1.5)

    dom = 34.0 + 14.0 * stiffness - 6.0 * wetness + rng.normal(0, 2.4, n)
    dom = np.clip(dom, 5.0, 44.0)  # DLPF corner is 44Hz; nothing survives above

    entropy = 0.42 + 0.26 * wetness - 0.10 * stiffness + rng.normal(0, 0.05, n)
    entropy = np.clip(entropy, 0.05, 0.99)

    damping = 0.055 + 0.070 * wetness - 0.020 * stiffness + rng.normal(0, 0.012, n)
    damping = np.clip(damping, 0.005, 0.45)

    return rms, dom, entropy, damping


# ----------------------------------------------------------------- labelling
def classify_is456(wc, slump_mm, temp_c) -> np.ndarray:
    """
    IS 456:2000 rule engine. Mirrors classifyIS456() in the firmware exactly.

    Two gaps in the thresholds as written in the project docs are closed here,
    conservatively, and the firmware closes them identically:
      - w/c in [0.35, 0.40) belonged to no class
      - slump < 50mm belonged to no class
    Both are genuine workability defects and must not fall through to GOOD.
    """
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


# ----------------------------------------------------------------- generation
def _sample_pool(size: int, rng):
    """Candidate physical states, with deliberate mass near class boundaries."""
    n_bulk = int(size * 0.67)
    n_edge = size - n_bulk

    wc = np.concatenate([
        rng.uniform(0.30, 0.65, n_bulk),
        rng.choice([0.35, 0.40, 0.50, 0.55], n_edge) + rng.normal(0, 0.018, n_edge),
    ])
    slump = np.clip(np.concatenate([
        rng.uniform(10.0, 220.0, n_bulk),
        rng.choice([50.0, 125.0, 150.0], n_edge) + rng.normal(0, 7.0, n_edge),
    ]), 0.0, 260.0)
    temp = np.clip(np.concatenate([
        rng.normal(30.0, 5.5, n_bulk),
        rng.choice([35.0, 40.0], n_edge) + rng.normal(0, 1.6, n_edge),
    ]), 12.0, 48.0)

    return wc, slump, temp


def generate(n: int, seed: int = 42):
    rng = np.random.default_rng(seed)

    # Stratified sampling, because uniform sampling of the physical space does
    # NOT give usable classes. REJECT fires on any one of four independent
    # violations (w/c high, w/c low, slump high, temp high) while GOOD requires
    # all three parameters inside narrow windows simultaneously. Sampled
    # uniformly that yields ~6% GOOD against ~61% REJECT -- an imbalance severe
    # enough that a classifier maximises accuracy by never predicting GOOD,
    # which is precisely the class the device exists to identify.
    #
    # So: oversample a large pool, label it, then draw equal counts per class.
    # Class balance is a sampling decision, not a property of the physics.
    target = n // len(CLASSES)
    quota = {c: target for c in CLASSES}
    for c in CLASSES[: n - target * len(CLASSES)]:
        quota[c] += 1

    buckets = {c: {"wc": [], "slump": [], "temp": []} for c in CLASSES}
    attempts = 0
    while any(len(buckets[c]["wc"]) < quota[c] for c in CLASSES) and attempts < 200:
        attempts += 1
        pw, ps, pt = _sample_pool(max(n * 8, 4000), rng)
        plabels = classify_is456(pw, ps, pt)
        for c in CLASSES:
            need = quota[c] - len(buckets[c]["wc"])
            if need <= 0:
                continue
            idx = np.flatnonzero(plabels == c)[:need]
            buckets[c]["wc"].extend(pw[idx])
            buckets[c]["slump"].extend(ps[idx])
            buckets[c]["temp"].extend(pt[idx])

    wc = np.array([v for c in CLASSES for v in buckets[c]["wc"]])
    slump = np.array([v for c in CLASSES for v in buckets[c]["slump"]])
    temp = np.array([v for c in CLASSES for v in buckets[c]["temp"]])

    order = rng.permutation(len(wc))
    wc, slump, temp = wc[order], slump[order], temp[order]
    n = len(wc)

    # --- labels come from TRUE physical values
    labels = classify_is456(wc, slump, temp)

    # --- features come from SIMULATED SENSORS
    v_water = wc_to_water_volume_fraction(wc)
    eps_mix = lichtenecker_eps_mix(v_water, temp)
    moisture_mv = eps_mix_to_millivolts(eps_mix, temp, rng)

    tau0 = slump_to_yield_stress(slump)
    force_n = yield_stress_to_force_n(tau0, rng)

    # DS18B20 at 12-bit resolution: 0.0625 degC steps, +/-0.5 degC accuracy.
    temp_meas = np.round((temp + rng.normal(0, 0.22, n)) / 0.0625) * 0.0625

    rms, dom, entropy, damping = vibration_signature(wc, slump, rng, n)

    rows = []
    for i in range(n):
        rows.append({
            # --- sensor-level features: exactly what the firmware can measure
            "moisture_mv": round(float(moisture_mv[i]), 1),
            "temperature_c": round(float(temp_meas[i]), 4),
            "penetration_force_n": round(float(force_n[i]), 4),
            "vib_rms_g": round(float(rms[i]), 5),
            "vib_dominant_hz": round(float(dom[i]), 3),
            "vib_spectral_entropy": round(float(entropy[i]), 5),
            "vib_damping_ratio": round(float(damping[i]), 5),
            # --- ground truth, for auditing only. NOT model inputs.
            "true_wc_ratio": round(float(wc[i]), 4),
            "true_slump_mm": round(float(slump[i]), 2),
            "true_temp_c": round(float(temp[i]), 3),
            "true_yield_stress_pa": round(float(tau0[i]), 2),
            "label": labels[i],
            "data_source": "synthetic_physics",
        })
    return rows


FEATURE_COLUMNS = [
    "moisture_mv",
    "temperature_c",
    "penetration_force_n",
    "vib_rms_g",
    "vib_dominant_hz",
    "vib_spectral_entropy",
    "vib_damping_ratio",
]


def separability_report(rows):
    """
    Honest metric. Trains a small model on sensor features only and reports
    cross-validated performance, explicitly labelled as separability of the
    SYNTHETIC feature space -- never as real-world accuracy.
    """
    try:
        from sklearn.ensemble import RandomForestClassifier
        from sklearn.model_selection import cross_val_score
    except ImportError:
        return None

    X = np.array([[r[c] for c in FEATURE_COLUMNS] for r in rows])
    y = np.array([r["label"] for r in rows])
    clf = RandomForestClassifier(n_estimators=120, random_state=0)
    scores = cross_val_score(clf, X, y, cv=5)
    clf.fit(X, y)
    return {
        "cv_mean": float(scores.mean()),
        "cv_std": float(scores.std()),
        "importances": {
            c: round(float(v), 4)
            for c, v in sorted(
                zip(FEATURE_COLUMNS, clf.feature_importances_),
                key=lambda kv: -kv[1],
            )
        },
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--n", type=int, default=900)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--out", default="dataset.csv")
    ap.add_argument("--merge-real", default=None,
                    help="CSV of real board readings to append (rows are kept "
                         "distinguishable via data_source=real_hardware)")
    args = ap.parse_args()

    rows = generate(args.n, args.seed)

    n_real = 0
    if args.merge_real:
        p = pathlib.Path(args.merge_real)
        if not p.exists():
            print(f"error: {p} not found", file=sys.stderr)
            return 1
        with p.open() as fh:
            for r in csv.DictReader(fh):
                r["data_source"] = "real_hardware"
                rows.append(r)
                n_real += 1

    out = pathlib.Path(args.out)
    fieldnames = list(rows[0].keys())
    with out.open("w", newline="") as fh:
        # Provenance travels with the data, so a CSV that escapes this repo
        # still says what it is.
        fh.write(f"# ConcreSense dataset generated {_dt.datetime.now().isoformat()}\n")
        fh.write("# data_source=synthetic_physics rows are SIMULATED, not measured.\n")
        fh.write("# Models: Lichtenecker mixture rule; Malmberg-Maryott (1956) eps_w(T);\n")
        fh.write("#         Prandtl flat-punch (2+pi); Hu & de Larrard slump<->tau0;\n")
        fh.write("#         IS 456:2000 thresholds.\n")
        fh.write("# NOT validated against laboratory-tested concrete.\n")
        w = csv.DictWriter(fh, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)

    counts = {c: sum(1 for r in rows if r["label"] == c) for c in CLASSES}
    print(f"wrote {out}  ({len(rows)} rows: {args.n} synthetic + {n_real} real)")
    print(f"  class balance: {counts}")

    rep = separability_report([r for r in rows if r["data_source"] == "synthetic_physics"])
    if rep:
        print(f"\n  synthetic-set separability (5-fold CV): "
              f"{rep['cv_mean']*100:.1f}% +/- {rep['cv_std']*100:.1f}%")
        print("  ^ this is how well a model recovers the simulator.")
        print("    It is NOT concrete-screening accuracy and must not be reported as such.")
        print("\n  feature importance:")
        for k, v in rep["importances"].items():
            print(f"    {k:24s} {v:.3f}")

        meta = out.with_suffix(".meta.json")
        meta.write_text(json.dumps({
            "generated": _dt.datetime.now().isoformat(),
            "n_synthetic": args.n,
            "n_real": n_real,
            "seed": args.seed,
            "class_balance": counts,
            "synthetic_separability_cv": rep,
            "validated_against_real_concrete": False,
            "feature_columns": FEATURE_COLUMNS,
        }, indent=2))
        print(f"\n  metadata -> {meta}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
