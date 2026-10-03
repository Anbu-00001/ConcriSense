#!/usr/bin/env python3
"""
Bulk training-set generator -> trainingdata.csv.

Scales up tinyml_model/dataset_generator.py (same physics, same sensor forward
models, same IS 456 labeller) from the 900-row dev set to a few thousand rows,
so the classifier has enough data for a stable held-out split.

THIS DOES NOT TRAIN ANYTHING. It samples physical states, forward-models the
sensors, labels, and writes a CSV -- numpy only, a second or two of CPU. The
sklearn separability check that dataset_generator.main() runs is deliberately
not called here.

PROVENANCE -- READ THIS BEFORE QUOTING ANY NUMBER FROM THIS FILE
----------------------------------------------------------------
Every row is `data_source=synthetic_physics`: simulated, not measured by the
board. No concrete was tested. The label is a deterministic IS 456 rule applied
to the *true* simulated state, and the features are noisy sensor-level
readings of that state, so a model trained here measures how well it inverts
the simulator -- synthetic-set separability -- not how well it screens real
concrete. The header written into the CSV says the same, so the file still
tells the truth after it leaves this repo. See AUDIT.md section E.

Usage:
    python3 generate_trainingdata.py                  # 6000 rows -> trainingdata.csv
    python3 generate_trainingdata.py --n 12000 --seed 7
"""

from __future__ import annotations

import argparse
import csv
import datetime as _dt
import pathlib

from concresense_physics import CLASSES
from dataset_generator import generate

# Different default seed from dataset.csv (42) so the two files are independent
# draws rather than one being a prefix-ish subset of the other.
DEFAULT_SEED = 2026


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--n", type=int, default=6000,
                    help="total rows; split evenly across GOOD/MARGINAL/REJECT")
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED)
    ap.add_argument("--out", default="trainingdata.csv")
    args = ap.parse_args()

    rows = generate(args.n, args.seed)

    out = pathlib.Path(args.out)
    with out.open("w", newline="") as fh:
        fh.write(f"# ConcreSense training set generated {_dt.datetime.now().isoformat()}"
                 f"  (n={len(rows)}, seed={args.seed})\n")
        fh.write("# data_source=synthetic_physics rows are SIMULATED, not measured.\n")
        fh.write("# Models: Lichtenecker mixture rule; Malmberg-Maryott (1956) eps_w(T);\n")
        fh.write("#         Prandtl flat-punch (2+pi); Hu & de Larrard slump<->tau0;\n")
        fh.write("#         IS 456:2000 thresholds.\n")
        fh.write("# NOT validated against laboratory-tested concrete.\n")
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    counts = {c: sum(1 for r in rows if r["label"] == c) for c in CLASSES}
    print(f"wrote {out}  ({len(rows)} rows, all data_source=synthetic_physics)")
    print(f"  class balance: {counts}")
    print("  next: python3 evaluate_confusion_matrix.py --dataset", out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
