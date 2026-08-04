#!/usr/bin/env bash
# Compile and run the host-side tests for the firmware's pure-math modules.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter \
    -I"$HERE" \
    "$HERE/test_features.cpp" \
    "$ROOT/firmware/concresense/src/features/fft_features.cpp" \
    "$ROOT/firmware/concresense/src/physics/calibration.cpp" \
    -lm -o "$HERE/hosttest"
"$HERE/hosttest"
