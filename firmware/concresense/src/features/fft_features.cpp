#include "fft_features.h"

#include "../config.h"

namespace {

// Scratch buffers sized for the fixed burst length. Static rather than heap:
// this runs inside the sampling task where a mid-burst malloc would be both a
// latency spike and a fragmentation risk over long deployments.
float gRe[IMU_BURST_SAMPLES];
float gIm[IMU_BURST_SAMPLES];

bool isPowerOfTwo(uint16_t n) { return n && ((n & (n - 1)) == 0); }

uint16_t log2u(uint16_t n) {
  uint16_t k = 0;
  while (n > 1) {
    n >>= 1;
    k++;
  }
  return k;
}

// In-place iterative radix-2 Cooley-Tukey.
void fft(float* re, float* im, uint16_t n) {
  const uint16_t levels = log2u(n);

  // Bit-reversal permutation.
  for (uint16_t i = 0; i < n; i++) {
    uint16_t j = 0;
    uint16_t x = i;
    for (uint16_t k = 0; k < levels; k++) {
      j = (j << 1) | (x & 1);
      x >>= 1;
    }
    if (j > i) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }

  // Butterflies.
  for (uint16_t size = 2; size <= n; size <<= 1) {
    const uint16_t half = size / 2;
    const float ang = -2.0f * PI / size;
    for (uint16_t i = 0; i < n; i += size) {
      for (uint16_t k = 0; k < half; k++) {
        const float wr = cosf(ang * k);
        const float wi = sinf(ang * k);
        const uint16_t a = i + k;
        const uint16_t b = a + half;
        const float tr = re[b] * wr - im[b] * wi;
        const float ti = re[b] * wi + im[b] * wr;
        re[b] = re[a] - tr;
        im[b] = im[a] - ti;
        re[a] += tr;
        im[a] += ti;
      }
    }
  }
}

}  // namespace

VibrationFeatures extractVibrationFeatures(float* buf, uint16_t n,
                                           float sampleRateHz) {
  VibrationFeatures f;

  if (buf == nullptr || n < 16 || !isPowerOfTwo(n) || n > IMU_BURST_SAMPLES) {
    return f;
  }
  if (isnan(sampleRateHz) || sampleRateHz <= 0.0f) return f;

  // --- 1. Remove the DC component.
  //
  // This is not optional. buf holds acceleration MAGNITUDE, which carries a
  // constant ~1g gravity term. Left in, bin 0 would hold almost all the energy,
  // the "dominant frequency" would always be 0Hz, and the spectral entropy
  // would be pinned near zero for every sample regardless of the mix.
  float mean = 0.0f;
  for (uint16_t i = 0; i < n; i++) mean += buf[i];
  mean /= n;

  float sumSq = 0.0f, vmin = buf[0], vmax = buf[0];
  for (uint16_t i = 0; i < n; i++) {
    if (buf[i] < vmin) vmin = buf[i];
    if (buf[i] > vmax) vmax = buf[i];
    const float ac = buf[i] - mean;
    sumSq += ac * ac;
  }
  f.rms = sqrtf(sumSq / n);
  f.peakToPeak = vmax - vmin;

  // --- 2. Hann window, to stop a non-integer number of cycles in the window
  // from smearing energy across every bin.
  for (uint16_t i = 0; i < n; i++) {
    const float w = 0.5f * (1.0f - cosf(2.0f * PI * i / (n - 1)));
    gRe[i] = (buf[i] - mean) * w;
    gIm[i] = 0.0f;
  }

  fft(gRe, gIm, n);

  // --- 3. One-sided power spectrum. Bin 0 is dropped: the mean was already
  // removed, so whatever remains there is numerical residue, not signal.
  const uint16_t half = n / 2;
  const float binHz = sampleRateHz / n;

  float total = 0.0f, peak = 0.0f, centroidNum = 0.0f;
  uint16_t peakBin = 0;

  for (uint16_t k = 1; k < half; k++) {
    const float p = gRe[k] * gRe[k] + gIm[k] * gIm[k];
    gRe[k] = p;  // stash the PSD back into gRe for the entropy pass
    total += p;
    centroidNum += p * (k * binHz);
    if (p > peak) {
      peak = p;
      peakBin = k;
    }
  }

  if (total <= 0.0f) {
    // A perfectly flat window — sensor stationary and noiseless, or stuck.
    f.dominantFreqHz = 0.0f;
    f.spectralEntropy = 0.0f;
    f.spectralCentroidHz = 0.0f;
    f.dampingRatio = 0.0f;
    f.valid = true;
    return f;
  }

  f.dominantFreqHz = peakBin * binHz;
  f.spectralCentroidHz = centroidNum / total;

  // --- 4. Shannon entropy of the normalised PSD, scaled to [0,1] by log(bins)
  // so the value stays comparable if the window length ever changes.
  float h = 0.0f;
  for (uint16_t k = 1; k < half; k++) {
    const float p = gRe[k] / total;
    if (p > 1e-12f) h -= p * logf(p);
  }
  const float hMax = logf((float)(half - 1));
  f.spectralEntropy = (hMax > 0.0f) ? (h / hMax) : 0.0f;

  // --- 5. Damping, as a log decrement between the first and last quarter of
  // the window. A well-proportioned mix absorbs the excitation and decays
  // smoothly; a segregated one rings on or spikes erratically.
  const uint16_t q = n / 4;
  float e1 = 0.0f, e2 = 0.0f;
  for (uint16_t i = 0; i < q; i++) {
    const float a = buf[i] - mean;
    e1 += a * a;
    const float b = buf[n - q + i] - mean;
    e2 += b * b;
  }
  if (e1 > 1e-12f && e2 > 1e-12f) {
    // delta = ln(A1/A2) over the separation, then the standard small-damping
    // approximation zeta ~= delta / sqrt(4pi^2 + delta^2).
    const float delta = 0.5f * logf(e1 / e2);
    f.dampingRatio = delta / sqrtf(4.0f * PI * PI + delta * delta);
  } else {
    f.dampingRatio = 0.0f;
  }

  f.valid = true;
  return f;
}
