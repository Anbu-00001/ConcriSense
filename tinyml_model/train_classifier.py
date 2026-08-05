#!/usr/bin/env python3
"""
ConcreSense classifier training — Phase 3.

WHAT THIS TRAINS
-----------------
A small MLP (StandardScaler -> Dense -> ReLU -> Dense -> softmax) on the
SENSOR-LEVEL features in dataset.csv (moisture_mv, temperature_c,
penetration_force_n, vib_rms_g, vib_dominant_hz, vib_spectral_entropy,
vib_damping_ratio) against the IS 456-derived label.

WHY NOT EDGE IMPULSE / TFLITE MICRO
-------------------------------------
The original project docs specify an "Edge Impulse INT8 model" via the
TFLite Micro C++ SDK. Two things changed that plan, checked rather than
assumed:

  1. As of this build, Arduino-ESP32 bundles the OFFICIAL esp-tflite-micro
     library, but it ships with "almost no usage examples," and the
     previously-common third-party ports (e.g. Chirale_TensorFlowLite) are
     explicitly flagged outdated/not recommended by their own maintainers.
     For a 7-input, 3-class classifier this is a large, poorly-documented
     framework dependency for very little benefit.
  2. The ESP32's Xtensa LX6 has a real hardware single-precision FPU
     (confirmed: accelerates add/multiply; division is partially software).
     A model this small is dominated by multiply-accumulates, which the FPU
     handles natively in a normal FreeRTOS task (no special setup needed
     outside ISR context, which this code never touches).

So: train here with plain scikit-learn (no framework install on the MCU
side), export the weights as a flat C header, and hand-write a ~50-line
float32 forward pass in firmware/concresense/src/tinyml/. This also means
the model is NOT quantized to INT8 -- quantizing a model this small buys
nothing on hardware with no int8 SIMD path, while a scale/zero-point bug
would silently corrupt every prediction. That tradeoff is not worth it here,
so it is declined outright rather than implemented sloppily.

HONESTY CONSTRAINT
-------------------
Reported accuracy is against a HELD-OUT split of the SAME synthetic
dataset. It measures whether the MLP recovers signal the physics simulator
put there under sensor noise -- not real-world concrete-screening accuracy.
See AUDIT.md and dataset_generator.py's docstring for the full reasoning.

The IS 456 rule engine (calibration.cpp / concresense_physics.py) is NOT
replaced by this model. The firmware runs BOTH and reports a disagreement
rather than picking one silently -- if the MLP ever just re-derives the
rules, running both is what would expose that.

Usage:
    python3 train_classifier.py --dataset dataset.csv --hidden 12
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import pathlib

import numpy as np
from sklearn.model_selection import train_test_split
from sklearn.neural_network import MLPClassifier
from sklearn.preprocessing import StandardScaler

from concresense_physics import CLASSES

FEATURE_COLUMNS = [
    "moisture_mv",
    "temperature_c",
    "penetration_force_n",
    "vib_rms_g",
    "vib_dominant_hz",
    "vib_spectral_entropy",
    "vib_damping_ratio",
]


def load_dataset(path: pathlib.Path):
    import csv

    rows = []
    with path.open() as fh:
        for line in fh:
            if line.startswith("#"):
                continue
            rows.append(line)
    reader = csv.DictReader(rows)
    data = list(reader)

    # Only synthetic_physics rows are guaranteed complete: real_hardware rows
    # (from --merge-real) may have blank sensor fields for channels that
    # weren't wired at capture time, which would poison training silently.
    data = [r for r in data if r.get("data_source") == "synthetic_physics"]

    X = np.array([[float(r[c]) for c in FEATURE_COLUMNS] for r in data])
    y = np.array([r["label"] for r in data])
    return X, y


def cf(v) -> str:
    """
    Format one float as a valid C float literal.

    %g alone is not enough: a whole number renders as "2362", and "2362f" is not
    a legal C++ literal (g++ reads it as a user-defined literal operator and
    fails to compile). A decimal point has to be forced in.
    """
    s = f"{float(v):.8g}"
    if not any(ch in s for ch in ".eEnia"):  # no point/exponent/inf/nan
        s += ".0"
    return s + "f"


def c_array(name: str, arr: np.ndarray) -> str:
    """Render a numpy array as a flat, row-major C initializer."""
    arr = np.asarray(arr, dtype=np.float32)
    flat = ", ".join(cf(v) for v in arr.flatten())
    dims = "".join(f"[{d}]" for d in arr.shape) if arr.ndim > 0 else ""
    return f"static const float {name}{dims} = {{{flat}}};"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dataset", default="dataset.csv")
    ap.add_argument("--hidden", type=int, default=12,
                    help="hidden layer width -- kept small deliberately: this "
                         "is a 7-feature, 3-class problem, not a vision task")
    ap.add_argument("--seed", type=int, default=13)
    ap.add_argument("--out-header",
                    default="../firmware/concresense/src/tinyml/model_weights.h")
    ap.add_argument("--out-test-vectors",
                    default="../tools/hosttest/model_test_vectors.h")
    args = ap.parse_args()

    X, y_str = load_dataset(pathlib.Path(args.dataset))
    n = len(X)

    # MLPClassifier's early_stopping validation path chokes on object-dtype
    # (string) labels in this sklearn version (np.isnan on a string array).
    # Encode to integers ourselves rather than relying on sklearn to handle
    # strings; class_order is fixed to sorted(CLASSES) so it's known statically
    # rather than read back from clf.classes_ after the fact.
    class_order = sorted(CLASSES)
    label_to_idx = {c: i for i, c in enumerate(class_order)}
    y = np.array([label_to_idx[v] for v in y_str])

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=args.seed, stratify=y)

    scaler = StandardScaler().fit(X_train)
    Xs_train = scaler.transform(X_train)
    Xs_test = scaler.transform(X_test)

    def make_mlp(hidden: int) -> MLPClassifier:
        return MLPClassifier(
            hidden_layer_sizes=(hidden,),
            activation="relu",
            solver="adam",
            alpha=1e-3,       # L2 regularisation -- 900 rows is not much data
            max_iter=3000,
            random_state=args.seed,
            early_stopping=True,
            n_iter_no_change=25,
        )

    # Pick the hidden width by cross-validation on the TRAINING SET ONLY.
    # Selecting it by held-out accuracy would leak the test split into a
    # hyperparameter choice and inflate the number reported below -- the exact
    # kind of quiet self-deception AUDIT.md flags elsewhere in this project.
    if args.hidden <= 0:
        from sklearn.model_selection import cross_val_score
        candidates = [8, 12, 16, 24, 32]
        cv_means = {}
        for h in candidates:
            cv_means[h] = float(cross_val_score(
                make_mlp(h), Xs_train, y_train, cv=5).mean())
        hidden = max(cv_means, key=lambda k: cv_means[k])
        print("hidden-width CV sweep (on train split only):")
        for h in candidates:
            mark = "  <- chosen" if h == hidden else ""
            print(f"    hidden={h:3d}   cv={cv_means[h]*100:.1f}%{mark}")
    else:
        hidden = args.hidden

    clf = make_mlp(hidden)
    clf.fit(Xs_train, y_train)

    held_out_acc = clf.score(Xs_test, y_test)
    train_acc = clf.score(Xs_train, y_train)
    print(f"train accuracy:     {train_acc*100:.1f}%  (n={len(X_train)})")
    print(f"held-out accuracy:  {held_out_acc*100:.1f}%  (n={len(X_test)})")
    print("^ synthetic-set held-out accuracy. NOT real-concrete accuracy.")
    print("  See AUDIT.md / dataset_generator.py docstring.")

    if clf.classes_.tolist() != list(range(len(class_order))):
        raise SystemExit(f"class index mismatch: sklearn gave {clf.classes_}")

    # Refit on ALL available data for the deployed model: the held-out split
    # above is what earns the honesty of the reported accuracy number, but
    # once that number is recorded, training the shipped model on every row
    # available is strictly better and is standard practice.
    scaler_final = StandardScaler().fit(X)
    Xs_final = scaler_final.transform(X)
    clf_final = MLPClassifier(
        hidden_layer_sizes=(hidden,), activation="relu", solver="adam",
        alpha=1e-3, max_iter=3000, random_state=args.seed,
        early_stopping=True, n_iter_no_change=25,
    )
    clf_final.fit(Xs_final, y)

    W1, b1 = clf_final.coefs_[0], clf_final.intercepts_[0]     # (7,H) (H,)
    W2, b2 = clf_final.coefs_[1], clf_final.intercepts_[1]     # (H,3) (3,)

    out_header = pathlib.Path(args.out_header)
    out_header.parent.mkdir(parents=True, exist_ok=True)
    header = f"""// AUTO-GENERATED by tinyml_model/train_classifier.py -- DO NOT EDIT BY HAND.
// Regenerate with: python3 tinyml_model/train_classifier.py
//
// Trained {_dt.datetime.now().isoformat()} on {n} rows of
// tinyml_model/dataset.csv (data_source=synthetic_physics only).
//
// Held-out accuracy on a 20% stratified split: {held_out_acc*100:.1f}%
// This is SYNTHETIC-SET separability, not real-concrete accuracy -- see
// AUDIT.md and dataset_generator.py. The deployed model below is refit on
// 100% of the data after that honest number was recorded.
//
// Architecture: {len(FEATURE_COLUMNS)} -> {hidden} (ReLU) -> {len(class_order)} (softmax)
// Float32, NOT quantized -- see train_classifier.py module docstring for why.

#pragma once

static const int MODEL_N_FEATURES = {len(FEATURE_COLUMNS)};
static const int MODEL_N_HIDDEN = {hidden};
static const int MODEL_N_CLASSES = {len(class_order)};

// Feature order the model expects -- MUST match FEATURE_COLUMNS in
// tinyml_model/dataset_generator.py and how the firmware assembles the vector
// in runMeasurementCycle(). Getting this order wrong compiles fine and
// produces confident, silently-wrong classifications.
// {FEATURE_COLUMNS}

// Class order is alphabetical (sklearn's default): {class_order}
static const char* const MODEL_CLASS_NAMES[{len(class_order)}] = {{
  {", ".join(f'"{c}"' for c in class_order)}
}};

// StandardScaler: (x - mean) / scale, applied per-feature before layer 1.
{c_array("MODEL_FEATURE_MEAN", scaler_final.mean_)}
{c_array("MODEL_FEATURE_SCALE", scaler_final.scale_)}

// Layer 1: hidden = relu(x_scaled @ W1 + b1)
{c_array("MODEL_W1", W1)}
{c_array("MODEL_B1", b1)}

// Layer 2: logits = hidden @ W2 + b2  (softmax applied in the inference code)
{c_array("MODEL_W2", W2)}
{c_array("MODEL_B2", b2)}
"""
    out_header.write_text(header)
    print(f"wrote {out_header}")

    # Cross-check vectors: real held-out rows plus sklearn's own predict_proba
    # on them, so the hand-written C++ forward pass can be verified to
    # reproduce sklearn's actual output -- not just "looks architecturally
    # right". This is the check that catches a transposed weight matrix or a
    # wrong activation before it ever reaches the ESP32.
    rng = np.random.default_rng(args.seed)
    idx = rng.choice(len(X_test), size=min(12, len(X_test)), replace=False)
    proba = clf_final.predict_proba(scaler_final.transform(X_test[idx]))

    vec_path = pathlib.Path(args.out_test_vectors)
    vec_path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "// AUTO-GENERATED by tinyml_model/train_classifier.py -- DO NOT EDIT.",
        "// Held-out rows + sklearn's predict_proba(), for cross-checking the",
        "// hand-written C++ forward pass bit-for-bit against the trained model.",
        "#pragma once",
        f"static const int TEST_N_VECTORS = {len(idx)};",
        f"static const float TEST_INPUTS[{len(idx)}][{len(FEATURE_COLUMNS)}] = {{",
    ]
    for i in idx:
        lines.append("  {" + ", ".join(cf(v) for v in X_test[i]) + "},")
    lines.append("};")
    lines.append(f"static const float TEST_EXPECTED_PROBA[{len(idx)}][{len(class_order)}] = {{")
    for row in proba:
        lines.append("  {" + ", ".join(cf(v) for v in row) + "},")
    lines.append("};")
    vec_path.write_text("\n".join(lines) + "\n")
    print(f"wrote {vec_path}  ({len(idx)} cross-check vectors)")

    # --- on-device self-test vectors.
    #
    # Generated, never hand-copied. An earlier version of this project had these
    # transcribed into the .ino by hand; a later retrain reshuffled the held-out
    # split and the hardcoded copies silently went stale, making a CORRECT
    # device look like it disagreed with sklearn. Emitting them here means the
    # firmware's self-test can never drift from the deployed weights.
    #
    # One representative vector per predicted class where available, so the
    # self-test exercises all three softmax outputs rather than one.
    pred_final = clf_final.predict(scaler_final.transform(X_test))
    chosen = []
    for ci, cname in enumerate(class_order):
        where = np.flatnonzero(pred_final == ci)
        if len(where):
            chosen.append((int(where[0]), cname))

    st_path = out_header.parent / "model_selftest.h"
    st = [
        "// AUTO-GENERATED by tinyml_model/train_classifier.py -- DO NOT EDIT.",
        "// Held-out feature vectors + the class the trained model predicts for",
        "// each, so the on-device self-test always matches the deployed weights.",
        "#pragma once",
        f"static const int SELFTEST_N = {len(chosen)};",
        f"static const float SELFTEST_INPUTS[{len(chosen)}][{len(FEATURE_COLUMNS)}] = {{",
    ]
    for idx, _ in chosen:
        st.append("  {" + ", ".join(cf(v) for v in X_test[idx]) + "},")
    st.append("};")
    st.append(f"static const char* const SELFTEST_EXPECTED[{len(chosen)}] = {{")
    st.append("  " + ", ".join(f'"{c}"' for _, c in chosen))
    st.append("};")
    st_path.write_text("\n".join(st) + "\n")
    print(f"wrote {st_path}  ({len(chosen)} self-test vectors: "
          f"{', '.join(c for _, c in chosen)})")

    meta = out_header.with_suffix(".meta.json")
    meta.write_text(json.dumps({
        "generated": _dt.datetime.now().isoformat(),
        "n_rows": n,
        "hidden_units": hidden,
        "held_out_accuracy": round(float(held_out_acc), 4),
        "train_accuracy": round(float(train_acc), 4),
        "class_order": class_order,
        "feature_order": FEATURE_COLUMNS,
        "quantization": "none (float32) -- see train_classifier.py docstring",
        "validated_against_real_concrete": False,
    }, indent=2))
    print(f"wrote {meta}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
