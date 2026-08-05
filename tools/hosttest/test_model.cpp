// Host-side verification of the on-device classifier.
//
// This compiles the SAME model_infer.cpp the firmware uses and checks it
// against sklearn's own predict_proba() output on held-out rows, captured at
// training time into model_test_vectors.h.
//
// This is the test that matters most in the whole project. A transposed weight
// matrix, a wrong activation, or a scaler applied in the wrong direction all
// compile cleanly and all produce confident, plausible-looking probabilities.
// Only a numerical comparison against the trained model catches them.

#include <cmath>
#include <cstdio>

#include "../../firmware/concresense/src/tinyml/model_infer.h"
#include "model_test_vectors.h"

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

int main() {
  printf("=== ConcreSense model inference vs sklearn ===\n\n");

  // float32 accumulation in a different order than numpy's float64 path means
  // exact equality is not expected. 1e-4 is far tighter than any decision
  // boundary while still tolerating that.
  const float TOL = 1e-4f;

  float worst = 0.0f;
  int argmaxMismatches = 0;

  for (int v = 0; v < TEST_N_VECTORS; v++) {
    Inference inf = runInference(TEST_INPUTS[v], 7);
    if (!inf.valid) {
      printf("  FAIL  vector %d returned invalid\n", v);
      failures++;
      checks++;
      continue;
    }

    int skBest = 0;
    for (int c = 1; c < 3; c++) {
      if (TEST_EXPECTED_PROBA[v][c] > TEST_EXPECTED_PROBA[v][skBest]) skBest = c;
    }
    if (skBest != inf.classIndex) argmaxMismatches++;

    for (int c = 0; c < 3; c++) {
      const float d = std::fabs(inf.probabilities[c] - TEST_EXPECTED_PROBA[v][c]);
      if (d > worst) worst = d;
    }
  }

  printf("  worst per-class probability delta vs sklearn: %.3e\n", worst);
  checks++;
  if (worst > TOL) {
    failures++;
    printf("  FAIL  probabilities diverge from sklearn beyond %.1e\n", TOL);
  } else {
    printf("  ok    all %d vectors match sklearn within %.1e\n",
           TEST_N_VECTORS, TOL);
  }

  check(argmaxMismatches == 0, "predicted class matches sklearn on every vector");

  // --- guards
  printf("\n[guards]\n");
  float nanInput[7] = {2400.f, 30.f, 0.7f, 0.3f, 36.f, 0.5f, NAN};
  check(!runInference(nanInput, 7).valid, "NaN feature -> invalid, not a guess");

  float infInput[7] = {2400.f, 30.f, 0.7f, 0.3f, 36.f, 0.5f, INFINITY};
  check(!runInference(infInput, 7).valid, "inf feature -> invalid");

  float ok[7] = {2400.f, 30.f, 0.7f, 0.3f, 36.f, 0.5f, 0.08f};
  check(!runInference(ok, 6).valid, "wrong feature count -> invalid");
  check(!runInference(nullptr, 7).valid, "null pointer -> invalid");

  Inference good = runInference(ok, 7);
  check(good.valid, "well-formed input -> valid");
  float sum = good.probabilities[0] + good.probabilities[1] + good.probabilities[2];
  check(std::fabs(sum - 1.0f) < 1e-5f, "probabilities sum to 1");
  check(good.confidence >= 1.0f / 3.0f - 1e-6f,
        "confidence is the max probability (>= 1/3 for 3 classes)");

  // Extreme inputs must not overflow softmax into NaN.
  float extreme[7] = {1e6f, 1e6f, 1e6f, 1e6f, 1e6f, 1e6f, 1e6f};
  Inference ex = runInference(extreme, 7);
  check(ex.valid && !std::isnan(ex.confidence),
        "extreme inputs do not overflow softmax to NaN");

  printf("\n=== %d/%d checks passed ===\n", checks - failures, checks);
  return failures == 0 ? 0 : 1;
}
