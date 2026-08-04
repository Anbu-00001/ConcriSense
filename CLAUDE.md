# Claude Code Project Guidelines: ConcreSense

## Project Overview
**ConcreSense** is an ESP32-based embedded IoT project performing real-time, on-site concrete quality verification using sensor fusion, TinyML (Edge Impulse INT8 model), NEO-6M GPS geo-tagging, SSD1306 OLED display, and MQTT cloud logging.

Detailed architectural analysis, hardware specs, pinouts, academic research literature, and feasibility evaluation can be found in [`PROJECT_ANALYSIS.md`](./PROJECT_ANALYSIS.md).

---

## Technical Specifications
- **Microcontroller:** ESP32 DevKit V1 (Xtensa LX6 240MHz, 520KB SRAM)
- **Primary Framework:** Arduino / C++ (PlatformIO) with FreeRTOS dual-core task division
- **Core 1:** Sensor polling (HX711 load cell, capacitive moisture v1.2, DS18B20 temp, MPU6050 vibration) & feature extraction
- **Core 0:** Edge Impulse TinyML INT8 C++ inference engine, OLED display driving, Wi-Fi / MQTT data publishing

> **Core assignment is deliberately the reverse of the original spec.** Arduino-ESP32 pins
> the Wi-Fi/TCP-IP stack to Core 0 and runs `loopTask` on Core 1 (confirmed on this board).
> Putting 200 Hz IMU sampling and the 60 µs-critical HX711 read on Core 0 would collide with
> the Wi-Fi driver. See [`AUDIT.md`](./AUDIT.md) §C.
- **GPS Module:** NEO-6M on Hardware Serial UART2 (GPIO 16 RX / GPIO 17 TX)
- **OLED Address:** SSD1306 0.96" on shared I2C (GPIO 21 SDA / GPIO 22 SCL, Address `0x3C`)
- **Moisture Pin:** Capacitive soil moisture v1.2 AOUT on GPIO 34 (ADC1 input only), **11 dB attenuation**
  - **Not the FC-28.** FC-28 is the *resistive* two-prong module: it measures ionic conductivity, corrodes within minutes in cement pore solution, and is not described by the Lichtenecker dielectric model below. See [`AUDIT.md`](./AUDIT.md) §B1.
  - Presence detection must use **raw ADC counts**, not millivolts: at 11 dB the calibrated conversion has a ~140 mV floor, so a floating pin reads 142 mV and a millivolt threshold can never detect it.
- **Load Cell Pin:** HX711 DT -> GPIO 18, SCK -> GPIO 19
- **Temp Pin:** DS18B20 DQ -> GPIO 4 (4.7kΩ pull-up)

---

## Academic Physics & Rheology Models (Section 8 Ground Truth)
- **Moisture / $w/c$ Ratio Dielectric Model (Lichtenecker Rule):**  
  $\ln \varepsilon_{\text{mix}} = v_w \ln \varepsilon_w + v_c \ln \varepsilon_c + v_a \ln \varepsilon_a + v_{\text{air}} \ln \varepsilon_{\text{air}}$  
  *Note:* High ionic conductivity in fresh pore solution ($\text{Ca}^{2+}, \text{OH}^-$) requires insulated capacitive probes + polynomial ADC/Temp mapping.
- **Slump / Shear Yield Stress Model (Bingham Plastic):**  
  $\tau = \tau_0 + \mu_p \dot{\gamma}$  
  Plunger insertion force $F_{\text{pen}}$ measured by HX711 correlates logarithmically to yield stress $\tau_0 \propto \text{slump}^{-1}$.
- **Vibration Damping (MPU-6050):**  
  200 Hz sampling -> FFT spectral entropy & acceleration RMS damping.

---

## Quality Classification Benchmarks (IS 456:2000)
- **`GOOD`**: Water-cement ratio $0.40 - 0.50$, slump $50 - 125\text{ mm}$, temp $< 35^\circ\text{C}$, low vibration damping anomaly.
- **`MARGINAL`**: Water-cement ratio $0.50 - 0.55$, slump $125 - 150\text{ mm}$, temp $35 - 40^\circ\text{C}$.
- **`REJECT`**: Water-cement ratio $> 0.55$ (or $< 0.35$), slump $> 150\text{ mm}$ (segregated/excess water), temp $> 40^\circ\text{C}$.

> **Two gaps in the above are closed in code** (identically in `classifyIS456()` and the
> Python labeller): w/c in $[0.35, 0.40)$ and slump $< 50\text{ mm}$ match no rule as written
> and would fall through to `GOOD`. Both are real workability defects and resolve to
> `MARGINAL`.

## Honesty constraints on results
The TinyML model is trained on a **physics-simulated** dataset; no real concrete has been
tested. Report metrics as *synthetic-set separability*, never as unqualified "classification
accuracy" — a model trained on rule-labelled data trivially reproduces those rules. Every
generated row carries `data_source=synthetic_physics`. See [`AUDIT.md`](./AUDIT.md) §E.

---

## Code Base Organization
- `firmware/`: ESP32 firmware source code
- `tinyml_model/`: Dataset generators, training scripts, and Edge Impulse C++ SDK export
- `dashboard/`: Cloud dashboard (React / Next.js + Node.js MQTT subscriber)
