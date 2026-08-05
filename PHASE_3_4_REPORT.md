# ConcreSense — Phase 3 & 4 Completion Report

Date: 2026-08-05 · Board: ESP32-D0WD-V3 rev 3.1, `/dev/ttyUSB0`
Student: Anbuchelvan · CSE A · 24CS0059

Both phases are complete, compiled, flashed, and verified on the physical board.
See [`AUDIT.md`](./AUDIT.md) for the source-document review and
[`PHASE_1_2_REPORT.md`](./PHASE_1_2_REPORT.md) for the earlier phases.

---

## Verification status

| Check | Result |
| :--- | :--- |
| Firmware compiles (esp32 core 3.3.11) | **Pass** — 1,019,521 B flash (77%), 53,500 B RAM (16%) |
| Flashed, boots, no panic | **Pass** |
| Physics/DSP host tests | **Pass** — 34/34 |
| Model inference vs sklearn | **Pass** — 10/10, worst delta **1.788e-07** |
| On-device model self-test | **Pass** — 3/3 classes, 39–101 µs per inference |
| FreeRTOS core pinning | **Pass** — sampling→core 1, network→core 0 |
| Cross-core queue under load | **Pass** — 0 dropped, heap flat at 241,740 B |
| Backend end-to-end (MQTT→WS→REST) | **Pass** — 14/14, no hardware needed |

Reproduce:

```bash
python3 tinyml_model/train_classifier.py --dataset tinyml_model/dataset.csv
./tools/hosttest/run.sh && ./tools/hosttest/run_model.sh
arduino-cli compile --fqbn esp32:esp32:esp32 --export-binaries firmware/concresense
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 firmware/concresense
python3 tools/monitor.py --seconds 22 --send t      # on-device model self-test
cd dashboard/server && npm install && npm start     # then: npm run selftest
```

---

## Phase 3 — classifier + dual-core

### Deviation: no Edge Impulse / TFLite Micro, and no INT8

The source docs specify an "Edge Impulse INT8 model" via TFLite Micro. Both parts
were dropped deliberately, after checking rather than assuming:

- Arduino-ESP32 now bundles the official `esp-tflite-micro`, but it ships with
  **almost no usage examples**, and the previously-common third-party ports are
  flagged outdated by their own maintainers. That is a large, poorly-documented
  dependency for a 7-input, 3-class problem.
- The Xtensa LX6 has a **real hardware single-precision FPU** (add/multiply
  accelerated; division partly in software). A model this small is pure
  multiply-accumulate, which the FPU handles natively in a normal FreeRTOS task.
- **INT8 quantization was declined outright.** It buys nothing on hardware with
  no int8 SIMD path, while a scale/zero-point bug would silently corrupt every
  prediction. Measured cost of float32 on-device: **39–101 µs**. There was no
  problem to solve.

Instead: train with scikit-learn, export weights to a C header, hand-write a
~50-line float32 forward pass. **Architecture: 7 → 12 (ReLU) → 3 (softmax).**

The hidden width was chosen by cross-validation **on the training split only** —
selecting it by held-out accuracy would have leaked the test set into a
hyperparameter choice and inflated the reported number.

### Honest accuracy

**Held-out accuracy: 70.6%** on a 20% stratified split (chance = 33.3%).

This is *synthetic-set separability*, not concrete-screening accuracy — the model
recovers signal the physics simulator put in, under simulated sensor noise. It has
never seen real concrete. Reported the same way everywhere per [`AUDIT.md`](./AUDIT.md) §E.

### The model does not replace the rule engine

Both run on every measurement and **both are published**. If the MLP ever merely
re-derives the IS 456 thresholds it was trained on, `agreement: true` on every
single record is what makes that visible instead of impressive. The dashboard
surfaces disagreements; IS 456 rules take precedence for compliance.

### Core assignment — the audit fix, now load-bearing

```
[rtos] sampling task -> core 1 (priority 3)
[rtos] network  task -> core 0 (priority 2)
```

This is the **reverse** of the source spec, for the reason established in
[`AUDIT.md`](./AUDIT.md) §C and confirmed empirically in Phase 1: Arduino-ESP32
pins the WiFi/TCP-IP stack to Core 0. Following the spec would have put 200 Hz IMU
sampling and the 60 µs-critical HX711 read on the same core as the WiFi driver.

Two concurrency hazards were found and fixed while building this:

1. **Sensor race.** A manual `m` from the console and the automatic sampling task
   both call `acquireMeasurement()`, on different cores. Without a mutex they
   interleave on the shared I2C bus and mid-HX711-conversion, producing corrupt
   readings indistinguishable from real ones. Now guarded by `gSensorMutex`, held
   across the whole cycle including the IMU burst.
2. **GPS UART overflow.** The sampling task originally slept for the full 5 s
   measurement interval. The NEO-6M emits an NMEA burst every second at 9600 baud
   into a **128-byte FIFO** — sleeping 5 s would overflow it and corrupt sentences.
   The task now ticks at 100 ms and measures on the longer interval. All GPS access
   stays on one core, since TinyGPS++ is not thread-safe.

### Self-test vectors are generated, never hand-copied

An earlier iteration had test vectors transcribed into the `.ino` by hand. After a
retrain reshuffled the held-out split, those stale copies made a **correct** device
look like it disagreed with sklearn — a 20-minute false alarm. `train_classifier.py`
now emits `model_selftest.h` alongside the weights, so the two cannot drift.

---

## Phase 4 — MQTT + backend

### No hardcoded credentials, anywhere

WiFi and broker settings are entered at runtime over serial (`wifi <ssid> <pass>`,
`mqtt <host> [port]`) and stored in NVS. **No SSID or password appears in any source
file**, so this repository can be committed, shared, or submitted without leaking
network access. The password is never echoed back, including in `net` status.

### Two real bugs prevented by checking the libraries

1. **PubSubClient's default buffer is 256 bytes.** The payload (GPS + 7 features +
   class probabilities + both verdicts) exceeds that. Verified behaviour:
   `publish()` returns false and sends **nothing** — it does not truncate. Without
   `setBufferSize(768)` the dashboard would simply never receive a message, with no
   visible cause. Every `publish()` return value is now checked and logged.
2. **`MQTT_CONNECTED` is a `#define` in PubSubClient.h**, which silently mangled the
   `NetState` enum through the preprocessor. Enum values renamed to `LINK_UP` /
   `BROKER_UP`.

### NTP, because GPS has no fix indoors

Confirmed during Phase 1 bring-up. Without NTP every `timestamp_utc` would be a 1970
epoch value. When NTP has not synced, the timestamp is explicitly `UNSYNCED+<ms>`
rather than a plausible-looking fake date a dashboard would silently plot. Likewise,
no GPS fix publishes `fix: false` — **not** `0,0`, which is a real location off the
coast of Africa.

### Backend: embedded broker, zero system dependencies

`dashboard/server/` runs an **embedded MQTT broker (aedes)** by default, so the whole
stack starts with `npm install && npm start` — no `sudo`, no mosquitto, no cloud
account. `--no-broker` uses an external broker instead.

The end-to-end self-test (`npm run selftest`) publishes a payload structurally
identical to what `net_client.cpp` emits and asserts it traverses
MQTT → server → REST → WebSocket, **including that a malformed publish is counted
rather than crashing the server**. 14/14 pass, with no hardware involved.

> **Dependency note:** `npm audit` reports 3 moderate advisories via
> `aedes → hyperid → uuid`. The advisory (GHSA-w5hq-g745-h8pq) affects `uuid` v3/v5/v6
> **when a `buf` argument is supplied**. Verified in `node_modules/hyperid/uuid-node.js`
> that hyperid calls only `uuid.v4` and passes no buffer, so the vulnerable path is
> unreachable here. Not force-upgraded, because `aedes@1.x` is a breaking API change
> for a localhost development broker.

---

## What still needs you

1. **Buy the six sensors** — the board still reports `0/6 subsystems present`. The
   full list is in the earlier shopping list; the capacitive **v1.2** moisture
   sensor, *not* the FC-28.
2. **Calibrate** once wired: `cm` then `cl`. Until then w/c and slump stay
   suppressed and both classifiers correctly report `UNKNOWN`.
3. **Configure the network** on the device:
   ```
   wifi <your-ssid> <your-password>
   mqtt <your-laptop-ip> 1883
   net
   ```
   Find your laptop IP with `ip addr show | grep 'inet '`. Both the ESP32 and the
   laptop must be on the same network.
4. **Optional:** `sudo apt install mosquitto mosquitto-clients` if you prefer a
   system broker over the embedded one. Not required.
5. **Retrain after collecting real readings:** use `c` to emit CSV rows, then
   `dataset_generator.py --merge-real` and re-run `train_classifier.py`.

---

## Not done

- **No real-concrete validation.** Nothing in this codebase claims otherwise.
- **The dashboard is a single live page**, not the full Next.js/Leaflet/PDF-export
  app in the source docs' Phase 5. GPS map pins and PDF audit export are unbuilt.
- **The MQTT publish path is unverified against a live broker from the device**,
  because that needs WiFi credentials I don't have. The firmware side compiles and
  the receiving side is proven; the join between them is the one untested link.
