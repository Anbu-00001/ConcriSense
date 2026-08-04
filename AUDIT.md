# ConcreSense — Plausibility & Legitimacy Audit of `PROJECT_ANALYSIS.md`

> Auditor: Claude Opus 5 · Date: 2026-08-04
> Subject: `PROJECT_ANALYSIS.md` + `CLAUDE.md` (authored by Gemini 3.6 Flash, 2026-08-04)
> Method: line-by-line read, hardware probe of the actual ESP32 over USB, and targeted
> literature/datasheet verification of every load-bearing physical claim.

**Verdict: the project is real and buildable. The document is ~75% sound, but it contains
three factual errors that would break the build, one architectural inversion, one
unsupported statistic, and one section whose framing I recommend rewriting.**

Nothing here is a reason to abandon the project. Every defect below has a fix, and the
fixes are already reflected in the Phase 1 code.

---

## A. Verified TRUE against the physical board

Probed via `esptool` on `/dev/ttyUSB0`:

```
Chip type:  ESP32-D0WD-V3 (revision v3.1)
Features:   Wi-Fi, BT, Dual Core + LP Core, 240MHz, Vref calibration in eFuse
Crystal:    40MHz     Flash: 4MB     MAC: c0:cd:d6:ce:4a:50
```

| Doc claim | Status | Evidence |
| :--- | :--- | :--- |
| Dual-core Xtensa LX6 @ 240 MHz | **TRUE** | Chip reports `Dual Core, 240MHz` |
| GPIO 34 is input-only, ADC1, no internal pull-ups | **TRUE** | ESP32 TRM; correct pin choice for an analog source |
| GPIO 16/17 free for UART2 | **TRUE — but only by luck** | Only true because this is a WROOM (no PSRAM). On a WROVER, GPIO16/17 are consumed by PSRAM and the GPS would silently fail. Doc never states this dependency. |
| MPU-6050 `0x68` + SSD1306 `0x3C` can share one I²C bus | **TRUE** | Distinct addresses, no conflict |
| ESP32 has eFuse-burned ADC Vref | **TRUE** | Chip reports `Vref calibration in eFuse`. Espressif burns this for D0WD produced after week 1 of 2018. This is *good news* the doc didn't know it had. |

---

## B. Errors that would have broken the build

### B1. **"FC-28" is a resistive sensor, not capacitive** — the doc's central sensing claim is self-contradictory

The document calls the moisture sensor **"Capacitive Moisture Sensor (FC-28)"** in the pin
table (line 31), in `CLAUDE.md`, and throughout §8.1 — where it builds an entire dielectric
physics justification (Lichtenecker mixture rule) on that word "capacitive".

**FC-28 is the classic *resistive* module**: two exposed gold-plated prongs driven by an
LM393 comparator. The capacitive one is a *different board* — "Capacitive Soil Moisture
Sensor v1.2", built around a 555 timer. The doc's own shopping list (line 118) correctly
names the v1.2 board, so it is contradicting itself between §2.1 and §4.1.

Why this is not cosmetic:

- A resistive probe in fresh cement paste measures **ionic conductivity**, not permittivity.
  §8.1's Lichtenecker model does not describe it at all.
- Fresh concrete pore solution is saturated with Ca²⁺/OH⁻. A DC-biased resistive probe in
  that environment **electrolyses and corrodes visibly within minutes**, and its reading
  drifts monotonically the whole time. It is not merely inaccurate, it is unstable.
- §8.1's own "Practical Fix" (insulate the pins with epoxy/PTFE) is *incoherent for a
  resistive probe* — insulating a resistive probe stops it working entirely. That fix only
  makes sense for a capacitive one, which confirms the author conflated the two.

**Fix applied:** all code, pin tables and naming use the **capacitive v1.2** part. The
`FC28` name is retired from the codebase. Buy the v1.2 board, not the FC-28.

### B2. ADC attenuation is never specified — default config clips ~⅔ of the signal

The capacitive v1.2 sensor swings roughly **1.0 V → 3.3 V**. The ESP32 ADC defaults to
**0 dB attenuation, which saturates at ~1.1 V**. With the doc as written, essentially the
entire useful moisture range reads as a flat railed `4095`.

**Fix applied:** `ADC_ATTEN_DB_12` (11 dB on older core headers) + `esp_adc_cal`
characterisation against the eFuse Vref this specific chip has burned in.

### B3. HX711 will be corrupted by FreeRTOS preemption

Per the HX711 datasheet: **if SCK is held high longer than 60 µs, the chip enters power-down
mode.** The doc puts a bit-banged HX711 read inside a FreeRTOS task on a preemptively
scheduled core and never mentions this. Any task switch or ISR landing mid-read silently
resets the converter or yields a garbage 24-bit word.

Also unmentioned: most red HX711 breakout boards ship with the RATE pin strapped low =
**10 SPS**. The doc's "penetrometer insertion force curve" is not meaningfully sampled at
10 Hz.

**Fix applied:** reads are wrapped in a `portMUX` critical section, with the shift loop
timing-guarded; driver reports the observed sample rate at bring-up so the 10-vs-80 SPS
question is answered empirically rather than assumed.

---

## C. Architectural inversion

**§2.2 assigns sensor sampling to Core 0 and Wi-Fi/MQTT to Core 1. This is backwards.**

On ESP-IDF/Arduino-ESP32 the **Wi-Fi and TCP/IP stacks are pinned to Core 0 (PRO_CPU) by
default**, and Arduino's `loopTask` runs on Core 1. The doc therefore schedules the two most
jitter-sensitive jobs in the system — 200 Hz IMU sampling and a bit-banged HX711 whose
timing budget is 60 µs — onto the *same core as the Wi-Fi driver*, while leaving the
jitter-tolerant TinyML inference alone on Core 1.

Every Wi-Fi beacon, association event, and TCP retransmit becomes IMU sample jitter and
HX711 corruption.

**Recommended (deferred to Phase 3, where the task split is actually created):** put
timing-critical sampling on **Core 1**, and inference + networking + OLED on **Core 0**
alongside the Wi-Fi stack that already lives there. Phase 1 is single-threaded so this is
not yet load-bearing, but it must be corrected before the task split is written.

---

## D. Unsupported / overstated claims

### D1. §8.2's "R² > 0.92" statistic is not supported

The doc asserts *"Research by NIST and ASCE demonstrates a strong logarithmic relationship
(R² > 0.92) between insertion resistance force and shear yield stress"*, giving the formula
τ₀ ≈ K₁·ln(F_pen) + K₂. No author, year, or title is given.

What is actually true:

- **The underlying physics is real.** Fresh concrete as a Bingham/Herschel–Bulkley material,
  and penetration resistance as a probe of its yield behaviour, are both well-established
  (NIST NISTIR 6094 rheology work; *Evolution of penetration resistance in fresh concrete*,
  Cement & Concrete Research).
- **The specific R² > 0.92 logarithmic fit is unverifiable as cited** and should be treated
  as fabricated until a real source is produced.
- Worth noting **NIST's own conclusion cuts slightly against the doc**: they find
  Herschel–Bulkley describes concrete better than the linear Bingham model the doc adopts.
- Separately, standardised penetration resistance (**ASTM C403**) measures **time of
  setting**, not slump. Using it as a slump proxy is defensible as a yield-stress
  correlation but is *not* the standard's purpose, and the doc implies otherwise.

**Action:** cite Bingham/Herschel–Bulkley honestly as the model family, present the
force→slump mapping as **our own fitted heuristic**, and drop the borrowed R².

### D2. §8.1's citations are unverifiable as given

"Rui He et al." and "K. Mubarak et al." are named with no title or year. Dielectric moisture
sensing of cementitious materials is a genuine research area, so these are plausible rather
than invented — but they cannot be cited in a report in this form. Get real references or
remove them.

### D3. "ASTM C1074" (line 5) is cited and then never used

C1074 is the **maturity method** — strength estimation from temperature history. Nothing in
the design implements it. It is decorative citation padding.

### D4. The "≥90% accuracy" target is, as specified, unfalsifiable

Phase 2 generates a synthetic dataset from IS 456:2000 threshold rules; Phase 3 trains a
classifier on it. A model trained on rule-generated data will score ~99% against those same
rules — that number measures **nothing about real concrete**. It is a self-fulfilling metric.

**Action:** report it as *"separability of the synthetic feature space"*, never as
"classification accuracy" unqualified. See §E.

---

## E. §10 "Hybrid Dataset Strategy" — the framing needs to change

The *technique* in §10 is legitimate and I have implemented it: generate a physics-based
synthetic dataset, then anchor it to the real sensor's measured range using cheap physical
proxies. Synthetic-plus-domain-calibration is standard practice when real labelled data is
expensive.

**The framing is the problem.** The section sells itself on *"Zero Suspicion"*,
*"100% Guaranteed Live Demo Success"*, and *"the final dataset combines genuine hardware
noise signatures with IS 456:2000 standard-compliant physics data"* — i.e. it is explicitly
optimising for the evaluator **not noticing** that no real concrete was ever tested.

That is the one thing in this document I won't build toward as written. It also fails on its
own terms: a viva examiner who asks *"what did you validate against?"* gets a much better
answer from "synthetic physics baseline, honestly labelled" than from a claim that collapses
under one follow-up question.

**What I implemented instead — same work, honest label:**

- The dataset generator stamps every row `data_source=synthetic_physics` and writes a
  provenance header naming the governing equations.
- Wet-sand/water cups are called what they are: **range-anchoring proxies for ADC span and
  sensor offset**, not concrete samples.
- Reported metrics are labelled *synthetic-set separability*, with the real-concrete
  validation gap stated explicitly as future work.

This costs nothing in demo reliability — the device behaves identically — and it turns the
project's weakest point into a defensible engineering-methods statement.

---

## F. Physical limits the doc does not acknowledge

1. **A 1–5 kg bar load cell is the wrong transducer for a penetrometer.** Bar cells measure
   *bending*; a plunger loads them *axially*. Off-axis loading gives nonlinear readings and
   can permanently deform the cell. 5 kg ≈ 49 N full scale, which is low for penetration
   work.
2. **Coarse aggregate breaks the measurement.** Structural concrete uses 20 mm aggregate.
   A small plunger hitting one stone reads the stone, not the mix. This method is only
   meaningful on **cement paste or mortar**, and the report should scope that claim
   explicitly.
3. **NEO-6M gets no fix indoors**, and cold start is 27 s+. A lab demo needs a
   last-known-good/stub fallback or the pipeline stalls waiting for a fix.
4. **I²C bus contention:** the OLED and MPU-6050 share one bus. A full 128×64 frame push is
   slow and will stall 200 Hz IMU sampling if they interleave naively. Needs either a
   dedicated sampling window with the display quiesced, or 400 kHz + partial updates.
5. **Cost:** the ₹5,000 target is comfortable — the listed BOM totals roughly ₹1,200–1,650
   plus the ESP32. This claim is safe.

---

## G. Bottom line

| Area | Assessment |
| :--- | :--- |
| Core concept (multi-sensor + TinyML concrete screening) | **Legitimate**, publishable as a prototype |
| Hardware selection & pinout | **Sound**, after the FC-28 → capacitive v1.2 correction |
| Physics models (Lichtenecker, Bingham) | **Real models**, correctly stated; misapplied to the wrong sensor in §8.1 |
| Citations | **Weak** — one likely-fabricated statistic, two unverifiable, one irrelevant |
| Core assignment | **Inverted** — fix before Phase 3 |
| §10 demo strategy | **Technique fine, framing must change** |
| Feasibility of the stated timeline | Optimistic but not unreasonable *if* sensors are in hand |

The honest one-line description of what this project can actually claim:

> A low-cost multi-sensor edge device that screens **fresh cement paste/mortar** against
> IS 456:2000-derived thresholds, using an on-device INT8 classifier trained on a
> physics-simulated dataset, with GPS-tagged MQTT logging. Not yet validated against
> laboratory-tested concrete cylinders.

That is a genuinely good undergraduate embedded-systems project, and it survives scrutiny.
