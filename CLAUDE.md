# Claude Code Project Guidelines: ConcreSense

## Project Overview
**ConcreSense** is an ESP32-based embedded IoT project performing real-time, on-site concrete quality verification using sensor fusion, TinyML (Edge Impulse INT8 model), NEO-6M GPS geo-tagging, SSD1306 OLED display, and MQTT cloud logging.

Detailed architectural analysis, hardware specs, pinouts, academic research literature, and feasibility evaluation can be found in [`PROJECT_ANALYSIS.md`](./PROJECT_ANALYSIS.md).

---

## Technical Specifications
- **Microcontroller:** ESP32 DevKit V1 (Xtensa LX6 240MHz, 520KB SRAM)
- **Primary Framework:** Arduino / C++ (PlatformIO) with FreeRTOS dual-core task division
- **Core 0:** Sensor polling (HX711 load cell, FC-28 capacitive moisture, DS18B20 temp, MPU6050 vibration) & feature extraction
- **Core 1:** Edge Impulse TinyML INT8 C++ inference engine, OLED display driving, Wi-Fi / MQTT data publishing
- **GPS Module:** NEO-6M on Hardware Serial UART2 (GPIO 16 RX / GPIO 17 TX)
- **OLED Address:** SSD1306 0.96" on shared I2C (GPIO 21 SDA / GPIO 22 SCL, Address `0x3C`)
- **Moisture Pin:** FC-28 Analog output on GPIO 34 (ADC1 input only)
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

---

## Code Base Organization
- `firmware/`: ESP32 firmware source code
- `tinyml_model/`: Dataset generators, training scripts, and Edge Impulse C++ SDK export
- `dashboard/`: Cloud dashboard (React / Next.js + Node.js MQTT subscriber)
