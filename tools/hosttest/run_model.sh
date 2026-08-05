#!/usr/bin/env bash
# Verify the on-device classifier reproduces sklearn's trained model.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter \
    -I"$HERE" \
    "$HERE/test_model.cpp" \
    "$ROOT/firmware/concresense/src/tinyml/model_infer.cpp" \
    -lm -o "$HERE/hosttest_model"
"$HERE/hosttest_model"
