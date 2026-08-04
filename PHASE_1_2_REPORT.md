# ConcreSense — Phase 1 & 2 Completion Report

Date: 2026-08-04 · Board: ESP32-D0WD-V3 rev 3.1, MAC `c0:cd:d6:ce:4a:50`, `/dev/ttyUSB0`

Both phases are complete, compiled, flashed, and verified on the physical board.
See [`AUDIT.md`](./AUDIT.md) for the plausibility review of the source documents that
shaped these decisions.

---

## Build & verification status

| Check | Result |
| :--- | :--- |
| Firmware compiles (`esp32:esp32`, core 3.3.11) | **Pass** — 354,864 B flash (27%), 27,164 B RAM (8%) |
| Flashed to hardware, hash verified | **Pass** |
| Boots cleanly, no panic, no reset loop | **Pass** |
| Heap stable over sustained run | **Pass** — flat at 317,928 B |
| Host unit tests (DSP + physics) | **Pass** — 34/34 |
| Synthetic dataset generated | **Pass** — 900 rows, balanced 300/300/300 |

Reproduce:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/concresense
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 firmware/concresense
python3 tools/monitor.py --seconds 13 --send m      # bring-up + one measurement
./tools/hosttest/run.sh                             # 34 host tests
python3 tinyml_model/dataset_generator.py --n 900 --out tinyml_model/dataset.csv
```

---

## Phase 1 — drivers and bring-up

Six drivers, each reporting presence rather than assuming it:
`moisture_cap`, `temp_ds18b20`, `loadcell_hx711`, `imu_mpu6050`, `gps_neo6m`, `oled_ssd1306`.

The central design decision: **every sensor is optional**. `begin()` returns
`OK / ABSENT / TIMEOUT / OUT_OF_RANGE / ERROR`, nothing blocks on missing hardware, and
the bring-up report prints a per-subsystem table. That was driven by the fact that the
sensors are not purchased yet — this way the board is testable today and each module
starts working the moment it is plugged in, with no code change.

Live output from the board with nothing wired:

```
  ConcreSense - Phase 1 Hardware Bring-up
  chip ESP32-D0WD-V3 rev 301   240 MHz   2 core(s)
  flash 4 MB   free heap 323212 B
  sketch running on core 1

--- I2C bus scan (SDA=21 SCL=22 @400kHz) ---
  no devices.
--- 0 device(s) ---

=============== BRING-UP REPORT ===============
  SSD1306 OLED     ABSENT        not on bus
  Moisture(cap)    ABSENT        GPIO34 floating
  DS18B20          ABSENT        no OneWire device (4.7k pull-up?)
  HX711            OUT_OF_RANGE  railed reading, check bridge
  MPU-6050         ABSENT        not at 0x68/0x69
  NEO-6M GPS       ABSENT        silent on UART2
  0/6 subsystems present.
```

### Bugs found and fixed by running on real hardware

1. **Moisture channel falsely reported `OK` on an unconnected pin.**
   At 11 dB attenuation the ESP32's calibrated conversion has a **~140 mV floor** — a
   floating GPIO34 reading a hard `0` raw counts still reports `142 mV`. A
   millivolt-based absence threshold therefore *cannot ever fire*. Presence detection now
   uses raw counts. Without this the classifier would have been fed a constant from a
   disconnected sensor with no indication anything was wrong.

2. **HX711 `OUT_OF_RANGE` was reported with the `ABSENT` diagnostic text**, pointing users
   at the wrong fault. These are genuinely different failures (chip missing vs. bridge
   disconnected) and now print different guidance.

3. **`LoadCellHX711::read()` gated only on `ABSENT`**, so a chip in `OUT_OF_RANGE` returned
   a confident `0.00` — indistinguishable from a real zero load. Now gated on `OK`.

4. **`tools/monitor.py` reset sequence dropped the board into UART download mode**, which
   produced ~19,000 lines of boot-fragment echo and looked exactly like a firmware boot
   loop. It was the harness, not the firmware. IO0 must be released high and settled
   before EN is released.

### Audit finding confirmed empirically

The banner reports `sketch running on core 1`. This confirms **audit finding C**: Arduino's
`loopTask` runs on Core 1 while the Wi-Fi stack is pinned to Core 0. The source document's
"sensors on Core 0, networking on Core 1" split is inverted, and would put 200 Hz IMU
sampling and the 60 µs-critical HX711 read on the same core as the Wi-Fi driver.
`config.h` now defines `CORE_SAMPLING 1` / `CORE_INFERENCE_NET 0`; this becomes
load-bearing in Phase 3 when the tasks are actually created.

---

## Phase 2 — features, physics, dataset

### Vibration feature engine (`src/features/`)

256-point radix-2 FFT at 200 Hz (0.78 Hz bins). Extracts RMS, peak-to-peak, dominant
frequency, spectral entropy, spectral centroid, and a log-decrement damping ratio.

Two details that matter:

- **DC removal is mandatory, not cosmetic.** The buffer holds acceleration *magnitude*,
  which carries a constant ~1 g gravity term. Left in, bin 0 swamps the spectrum, dominant
  frequency is always 0 Hz, and entropy pins near zero for every sample. The host test
  asserts RMS = 0.1414 for a 0.2 g tone (= A/√2), which only holds if the pedestal is gone.
- The FFT uses the **achieved** sample rate from `captureBurst()`, not the nominal 200 Hz,
  because I²C contention with the OLED makes them differ and the nominal rate would bias
  every reported frequency.

Known limit, stated rather than hidden: the MPU-6050 DLPF is set to 44 Hz, so content above
that corner is attenuated in hardware before sampling. The firmware flags any dominant peak
above 40 Hz as filter-shaped.

### Physics chain (`src/physics/`)

Replaced the source document's unsourced `τ₀ = K₁·ln(F) + K₂, R² > 0.92` claim with a
properly attributable chain:

| Step | Relation | Source |
| :--- | :--- | :--- |
| ε_w(T) | `87.740 − 0.40008T + 9.398e-4T² − 1.410e-6T³` | Malmberg & Maryott (1956), J. Res. NBS 56(1) |
| mix permittivity | Lichtenecker logarithmic mixture rule | standard |
| F → τ₀ | `τ₀ = (F/A)/(2+π)` | Prandtl flat-punch bearing capacity |
| τ₀ → slump | `s = 300 − 270·τ₀/ρ` | Hu & de Larrard FEA, in Ferraris & de Larrard (1998), *Cem. Concr. Aggr.* 20(2) |

Host tests verify ε_w(25 °C) = 78.30 against the published 78.4, and that the slump↔τ₀
round trip closes to <0.5 mm.

**Two threshold gaps in the source document were found and closed.** As written, w/c in
[0.35, 0.40) and slump below 50 mm belonged to *no class* — they fell through to GOOD. Both
are real workability defects. They now resolve to MARGINAL, identically in the firmware and
the Python labeller, and both are covered by tests.

**Calibration is persisted and gates output.** Anchors live in NVS (`anchors_store`), and
`wcValid`/`slumpValid` stay false until the board is actually calibrated. An uncalibrated
board reports `IS 456 : UNKNOWN` rather than a confident-looking number — verified on
hardware. On-device commands: `cm` (moisture range anchoring), `cl` (load cell).

### Synthetic dataset (`tinyml_model/dataset_generator.py`)

900 rows, balanced 300/300/300, with full provenance in the CSV header and a
`.meta.json` recording `validated_against_real_concrete: false`.

**Stratified sampling was necessary, and the reason is worth recording.** Uniform sampling
of the physical space gave **GOOD 53 / MARGINAL 299 / REJECT 548** — 5.9% GOOD. REJECT fires
on any *one* of four independent violations while GOOD requires all three parameters inside
narrow windows simultaneously. At that imbalance a classifier maximises accuracy by never
predicting GOOD, which is the one class the device exists to identify.

The generator labels from **true** physical values but exports only **sensor-level**
features, forcing a model to invert a noisy forward model rather than read the labels off
its own inputs. Reported separability: **75.4% ± 2.0%** (5-fold CV) against a 33.3% chance
baseline — non-trivial, which confirms the noise model makes this a real inference task.

Per the audit's §E, this is deliberately reported as *synthetic-set separability*, never as
classification accuracy. The `--merge-real` flag appends real board readings tagged
`data_source=real_hardware` so the two never blur together.

---

## What needs you (manual steps)

1. **Buy the sensors.** Nothing except the ESP32 is connected — `0/6 subsystems present`.
   Per the audit, buy the **capacitive soil moisture sensor v1.2**, *not* the FC-28 the
   documents name: FC-28 is resistive, corrodes within minutes in cement pore solution, and
   the Lichtenecker physics in the docs does not describe it.
   Also needed: HX711 + load cell, DS18B20 + 4.7 kΩ resistor, MPU-6050, NEO-6M, SSD1306.
   Budget ≈ ₹1,200–1,650. A breadboard is effectively required — the DevKit has one 3V3 pin
   for six modules.

2. **Wire and re-run bring-up.** No code change needed; each module is detected on the next
   boot. `python3 tools/monitor.py --seconds 13`.

3. **Calibrate**, once the moisture sensor and load cell are wired: send `cm`, then `cl`.
   Until then w/c and slump stay suppressed by design.

4. **Decide on the load cell mounting.** The audit flags a real physical problem: a bar load
   cell measures *bending*, but a penetrometer plunger loads it *axially* — off-axis loading
   is nonlinear and can permanently deform the cell. Related: this method only works on
   **cement paste or mortar**; 20 mm coarse aggregate will jam a 10 mm plunger. The report
   should scope that claim explicitly.

5. **Confirm the HX711 sample rate.** The bring-up prints the measured SPS. If it shows
   ~10 SPS and you want a force-vs-depth curve, the RATE pad needs bridging to VCC for
   80 SPS.

---

## Not done (out of scope for Phases 1–2)

- Phase 3: FreeRTOS task split and the TinyML INT8 classifier. The corrected core
  assignment is already in `config.h` but no tasks are created yet — Phase 1/2 is
  single-threaded.
- No model is trained. `dataset.csv` is ready for it.
- No real-concrete validation exists, and nothing in this codebase claims otherwise.
