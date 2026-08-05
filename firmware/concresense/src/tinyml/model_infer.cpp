#include "model_infer.h"

#include "model_weights.h"

// If a retrain changes the class count, this fails the build instead of
// silently truncating probabilities[] everywhere downstream.
static_assert(INFERENCE_N_CLASSES == MODEL_N_CLASSES,
              "INFERENCE_N_CLASSES in model_infer.h is out of sync with the "
              "trained model in model_weights.h -- regenerate and update it.");

const char* inferenceClassName(int classIndex) {
  if (classIndex < 0 || classIndex >= MODEL_N_CLASSES) return "UNKNOWN";
  return MODEL_CLASS_NAMES[classIndex];
}

Inference runInference(const float* features, int nFeatures) {
  Inference out;

  if (features == nullptr || nFeatures != MODEL_N_FEATURES) return out;

  // Refuse to classify on a missing channel. Propagating NaN would sail through
  // the matmul, survive softmax as NaN, and then lose every comparison in the
  // argmax below -- leaving classIndex at whatever index happened to be first.
  // That failure is silent and looks exactly like a real prediction.
  for (int i = 0; i < nFeatures; i++) {
    if (isnan(features[i]) || isinf(features[i])) return out;
  }

  // --- StandardScaler: (x - mean) / scale
  float scaled[MODEL_N_FEATURES];
  for (int i = 0; i < MODEL_N_FEATURES; i++) {
    const float s = MODEL_FEATURE_SCALE[i];
    // A zero scale means that feature had zero variance in training. Dividing
    // would yield inf; the scaled value is definitionally 0 in that case.
    scaled[i] = (s > 1e-12f) ? (features[i] - MODEL_FEATURE_MEAN[i]) / s : 0.0f;
  }

  // --- Layer 1: hidden = relu(scaled @ W1 + b1)
  // W1 is stored row-major as [n_features][n_hidden], matching sklearn's
  // coefs_[0] shape. The index order here is the single easiest thing to get
  // wrong in this file, which is why the host test compares against sklearn.
  float hidden[MODEL_N_HIDDEN];
  for (int h = 0; h < MODEL_N_HIDDEN; h++) {
    float acc = MODEL_B1[h];
    for (int i = 0; i < MODEL_N_FEATURES; i++) {
      acc += scaled[i] * MODEL_W1[i][h];
    }
    hidden[h] = acc > 0.0f ? acc : 0.0f;  // ReLU
  }

  // --- Layer 2: logits = hidden @ W2 + b2
  float logits[MODEL_N_CLASSES];
  for (int c = 0; c < MODEL_N_CLASSES; c++) {
    float acc = MODEL_B2[c];
    for (int h = 0; h < MODEL_N_HIDDEN; h++) {
      acc += hidden[h] * MODEL_W2[h][c];
    }
    logits[c] = acc;
  }

  // --- Softmax, shifted by the max logit.
  // The shift is not optional: expf() of a moderately large logit overflows to
  // inf in float32, and inf/inf is NaN. Subtracting the max is mathematically
  // identity and keeps every exponent <= 0.
  float maxLogit = logits[0];
  for (int c = 1; c < MODEL_N_CLASSES; c++) {
    if (logits[c] > maxLogit) maxLogit = logits[c];
  }

  float sum = 0.0f;
  for (int c = 0; c < MODEL_N_CLASSES; c++) {
    out.probabilities[c] = expf(logits[c] - maxLogit);
    sum += out.probabilities[c];
  }
  if (sum <= 0.0f || isnan(sum)) return out;

  for (int c = 0; c < MODEL_N_CLASSES; c++) {
    out.probabilities[c] /= sum;
  }

  int best = 0;
  for (int c = 1; c < MODEL_N_CLASSES; c++) {
    if (out.probabilities[c] > out.probabilities[best]) best = c;
  }

  out.classIndex = best;
  out.confidence = out.probabilities[best];
  out.valid = true;
  return out;
}
