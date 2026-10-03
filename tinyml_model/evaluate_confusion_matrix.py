#!/usr/bin/env python3
"""
Train the ConcreSense MLP on trainingdata.csv and report a held-out confusion
matrix with the usual companions (per-class precision/recall/F1, accuracy,
balanced accuracy, macro-F1, Cohen's kappa).

Outputs, written to --out-dir (default eval_output/):
    confusion_matrix.png        counts + row-normalised recall, side by side
    confusion_matrix.csv        raw counts, labelled rows/cols
    classification_report.txt   precision / recall / F1 / support
    metrics.json                every scalar above, plus provenance

WHAT THIS IS
------------
Same model as train_classifier.py (StandardScaler -> Dense(hidden) -> ReLU ->
Dense(3) -> softmax, adam, L2 1e-3, early stopping) on a stratified 80/20
split, scaler fitted on the training split only. Unlike train_classifier.py it
does NOT write firmware headers, so running it cannot change what is flashed.

WHAT THE NUMBERS MEAN
---------------------
trainingdata.csv is simulated (data_source=synthetic_physics). The matrix shows
how well the MLP recovers the simulator's IS 456 labels from noisy sensor-level
features: *synthetic-set separability*. It is not real-concrete accuracy, no
board captured these rows, and the figure says so in its footer so the image
stays honest when pasted into a report or slide. See AUDIT.md section E.

Usage:
    python3 generate_trainingdata.py
    python3 evaluate_confusion_matrix.py --dataset trainingdata.csv
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import pathlib

import matplotlib

matplotlib.use("Agg")  # headless: write files, never open a window
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.colors import LinearSegmentedColormap  # noqa: E402
from sklearn.metrics import (  # noqa: E402
    balanced_accuracy_score,
    classification_report,
    cohen_kappa_score,
    confusion_matrix,
    f1_score,
)
from sklearn.model_selection import train_test_split  # noqa: E402
from sklearn.neural_network import MLPClassifier  # noqa: E402
from sklearn.preprocessing import StandardScaler  # noqa: E402

from concresense_physics import CLASSES  # noqa: E402
from train_classifier import FEATURE_COLUMNS, load_dataset  # noqa: E402

# Single-hue sequential ramp, light -> dark (dataviz palette: blue 100 -> 700).
# Lightest step means "none of this row landed here"; it recedes into the surface.
_RAMP = ["#cde2fb", "#86b6ef", "#3987e5", "#1c5cab", "#0d366b"]
_SURFACE = "#fcfcfb"
_INK = "#1a1a19"
_INK_MUTED = "#5f5f5b"
_INK_ON_DARK = "#fcfcfb"

FOOTER = ("data_source=synthetic_physics  |  simulated, not measured  |  "
          "synthetic-set separability, NOT real-concrete accuracy")


def plot_confusion(cm: np.ndarray, class_names: list[str], n_test: int,
                   acc: float, out_path: pathlib.Path, dpi: int = 200) -> None:
    """Counts (left) and row-normalised recall % (right), one shared colour scale."""
    cmap = LinearSegmentedColormap.from_list("seq_blue", _RAMP)

    row_sums = cm.sum(axis=1, keepdims=True)
    frac = np.divide(cm, row_sums, out=np.zeros(cm.shape, dtype=float),
                     where=row_sums > 0)

    fig, axes = plt.subplots(1, 2, figsize=(11.5, 5.6), facecolor=_SURFACE)
    panels = [
        ("Counts", lambda i, j: f"{cm[i, j]:d}"),
        ("Row-normalised (recall per true class)", lambda i, j: f"{frac[i, j] * 100:.1f}%"),
    ]

    for ax, (title, fmt) in zip(axes, panels):
        ax.set_facecolor(_SURFACE)
        ax.imshow(frac, cmap=cmap, vmin=0.0, vmax=1.0, aspect="equal")

        # 2px surface gap between cells, per the dataviz mark spec.
        k = len(class_names)
        for edge in np.arange(-0.5, k, 1.0):
            ax.axhline(edge, color=_SURFACE, linewidth=2)
            ax.axvline(edge, color=_SURFACE, linewidth=2)

        for i in range(k):
            for j in range(k):
                # Ink flips on dark cells so contrast holds across the ramp.
                ax.text(j, i, fmt(i, j), ha="center", va="center", fontsize=14,
                        fontweight="bold" if i == j else "normal",
                        color=_INK_ON_DARK if frac[i, j] > 0.45 else _INK)

        ax.set_xticks(range(k), class_names, fontsize=11, color=_INK)
        ax.set_yticks(range(k), class_names, fontsize=11, color=_INK)
        ax.set_xlabel("Predicted class (MLP)", fontsize=11, color=_INK_MUTED, labelpad=8)
        ax.set_ylabel("True class (IS 456 rule label)", fontsize=11,
                      color=_INK_MUTED, labelpad=8)
        ax.set_title(title, fontsize=12, color=_INK, pad=10)
        ax.tick_params(length=0)
        for spine in ax.spines.values():
            spine.set_visible(False)

    fig.suptitle(
        f"ConcreSense MLP - held-out confusion matrix  "
        f"(n={n_test}, accuracy {acc * 100:.1f}% on synthetic set)",
        fontsize=14, color=_INK, y=0.99)
    fig.text(0.5, 0.012, FOOTER, ha="center", fontsize=9, color=_INK_MUTED)
    fig.tight_layout(rect=(0, 0.04, 1, 0.95))
    fig.savefig(out_path, dpi=dpi, facecolor=_SURFACE)
    plt.close(fig)


def make_mlp(hidden: int, seed: int) -> MLPClassifier:
    # Keep in sync with make_mlp() in train_classifier.py.
    return MLPClassifier(
        hidden_layer_sizes=(hidden,), activation="relu", solver="adam",
        alpha=1e-3, max_iter=3000, random_state=seed,
        early_stopping=True, n_iter_no_change=25,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dataset", default="trainingdata.csv")
    ap.add_argument("--hidden", type=int, default=12)
    ap.add_argument("--seed", type=int, default=13)
    ap.add_argument("--test-size", type=float, default=0.2)
    ap.add_argument("--out-dir", default="eval_output")
    args = ap.parse_args()

    X, y_str = load_dataset(pathlib.Path(args.dataset))

    # Integer-encode labels ourselves: sklearn's early_stopping path breaks on
    # string labels in this version (see train_classifier.py).
    class_order = sorted(CLASSES)
    y = np.array([class_order.index(v) for v in y_str])

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=args.test_size, random_state=args.seed, stratify=y)

    scaler = StandardScaler().fit(X_train)  # train split only: no test leakage
    clf = make_mlp(args.hidden, args.seed)
    clf.fit(scaler.transform(X_train), y_train)

    y_pred = clf.predict(scaler.transform(X_test))
    labels = list(range(len(class_order)))

    cm = confusion_matrix(y_test, y_pred, labels=labels)
    acc = float(np.mean(y_pred == y_test))
    bal_acc = float(balanced_accuracy_score(y_test, y_pred))
    macro_f1 = float(f1_score(y_test, y_pred, average="macro"))
    kappa = float(cohen_kappa_score(y_test, y_pred))
    report = classification_report(y_test, y_pred, labels=labels,
                                   target_names=class_order, digits=3)
    train_acc = float(clf.score(scaler.transform(X_train), y_train))

    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    (out_dir / "confusion_matrix.csv").write_text(
        "true\\predicted," + ",".join(class_order) + "\n"
        + "\n".join(f"{class_order[i]}," + ",".join(str(int(v)) for v in cm[i])
                    for i in range(len(class_order))) + "\n")
    (out_dir / "classification_report.txt").write_text(
        f"{FOOTER}\n\nheld-out n={len(y_test)}  (train n={len(y_train)})\n\n{report}\n")
    (out_dir / "metrics.json").write_text(json.dumps({
        "generated": _dt.datetime.now().isoformat(),
        "dataset": str(args.dataset),
        "n_train": int(len(y_train)),
        "n_test": int(len(y_test)),
        "hidden_units": args.hidden,
        "feature_order": FEATURE_COLUMNS,
        "class_order": class_order,
        "train_accuracy": round(train_acc, 4),
        "held_out_accuracy": round(acc, 4),
        "balanced_accuracy": round(bal_acc, 4),
        "macro_f1": round(macro_f1, 4),
        "cohen_kappa": round(kappa, 4),
        "confusion_matrix": cm.tolist(),
        "data_source": "synthetic_physics",
        "validated_against_real_concrete": False,
        "interpretation": "synthetic-set separability, not real-concrete accuracy",
    }, indent=2))
    plot_confusion(cm, class_order, len(y_test), acc, out_dir / "confusion_matrix.png")

    print(FOOTER, "\n")
    print(f"train n={len(y_train)}  held-out n={len(y_test)}  hidden={args.hidden}\n")
    print("confusion matrix (rows = true, cols = predicted):")
    print(f"{'':>10}" + "".join(f"{c:>10}" for c in class_order))
    for i, c in enumerate(class_order):
        print(f"{c:>10}" + "".join(f"{int(v):>10}" for v in cm[i]))
    print(f"\n{report}")
    print(f"train accuracy      {train_acc * 100:5.1f}%")
    print(f"held-out accuracy   {acc * 100:5.1f}%")
    print(f"balanced accuracy   {bal_acc * 100:5.1f}%")
    print(f"macro F1            {macro_f1:5.3f}")
    print(f"Cohen's kappa       {kappa:5.3f}")
    print(f"\nwrote {out_dir}/  (confusion_matrix.png/.csv, classification_report.txt, metrics.json)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
