# ConcreSense — End-to-End Simulation

An animated, interactive simulation of the full ConcreSense measurement pipeline:

```
SENSE  ->  FEATURES  ->  PHYSICS  ->  CLASSIFY  ->  PUBLISH
```

Built for demonstrating the workflow without hardware in hand. Every number on
screen comes from `tinyml_model/concresense_physics.py` — the **same** module
`tinyml_model/dataset_generator.py` uses to build the training set, which in turn
mirrors `firmware/concresense/src/physics/calibration.cpp` line for line (that C++
is covered by 34 passing host tests in `tools/hosttest/`). Nothing on screen is
hard-coded for the demo.

## Running it

**Interactive (recommended for a live demo in front of your teacher):**

```bash
python3 simulation/concresense_sim.py --mode interactive
```

Opens a window with three sliders — **w/c ratio**, **slump (mm)**, **temperature
(°C)** — under the plots. Drag any slider and the *entire* pipeline re-derives
live: the simulated sensor readings, the FFT spectrum, the derived w/c and
slump, the IS 456:2000 verdict, and the MQTT JSON payload all update together.
This is the honest version of the device: what you're dragging is the **true**
mix state, and everything the panel shows on the right (`measured` values) is
what the instrument would actually compute from noisy sensors — including when
it disagrees with the truth.

**Scripted walkthrough (auto-plays through 7 built-in scenarios):**

```bash
python3 simulation/concresense_sim.py
```

**Export a video/GIF to embed in slides:**

```bash
python3 simulation/concresense_sim.py --export simulation/out/concresense_demo.mp4
python3 simulation/concresense_sim.py --export simulation/out/demo.gif --scenarios 3
```

## What the panel shows

| Region | What it is |
| :--- | :--- |
| Top strip | The 5-stage pipeline, current stage highlighted |
| MPU-6050 burst (left) | Simulated 256-sample accel-magnitude waveform. Dashed line is the DC/gravity pedestal — removed before the FFT, and the demo shows that step explicitly because skipping it is the single most common mistake in this kind of pipeline |
| Spectrum (middle) | The same 256-pt FFT the firmware runs. Dotted red line marks the MPU-6050's 44 Hz hardware filter corner — any peak near it is flagged as filter-shaped, not a materials effect |
| Derived properties (bottom-left) | w/c, slump, temp against their IS 456 GOOD windows (shaded green band) |
| Vibration features (bottom-middle) | The 6 numbers that would feed a Phase-3 classifier |
| Verdict (right) | GOOD/MARGINAL/REJECT, **why** (which rule fired), and true-vs-measured — the device is judged on what it could actually see |
| Bottom strip | The actual MQTT JSON the firmware would publish |

## Two scenarios worth pointing out to your teacher

The scripted demo deliberately includes both threshold gaps the original
project brief left open (see `AUDIT.md` in the project root):

- **w/c 0.35–0.40** — the brief's rules don't cover this range; it would fall
  through to `GOOD` as originally written. The rule engine here closes it to
  `MARGINAL`, and the firmware does the same.
- **slump < 50 mm** — same gap, same fix.

Dragging the sliders into either of those windows live is a stronger
demonstration than a slide bullet: it shows the rule engine catching a case the
spec missed, in real time.

## What is simulated vs. real

This is a **physics simulation**, not a replay of sensor logs. The relations are
published (Lichtenecker mixture rule, Malmberg & Maryott 1956 for ε_w(T),
Prandtl flat-punch bearing capacity, Hu & de Larrard for slump↔τ₀) — see
`AUDIT.md` for citations. The sensor noise models (ionic-conduction bias,
stone-strike outliers, ADC quantisation) are plausible, not fitted to a real
board. No concrete has been tested. The on-screen banner says so on purpose,
and every payload carries `"data_source": "simulated_physics"`.

For a hardware-in-the-loop simulation instead of a physics model, see
`wokwi/` — the same firmware running in Espressif's cycle-accurate ESP32
simulator via the Wokwi extension.
