# Image-generation prompt — ConcreSense breadboard wiring diagram

Paste everything below the line into ChatGPT (GPT Image) or Gemini (Nano Banana / Imagen).

**Read the caveat at the bottom first** — image models cannot render exact breadboard hole
numbers reliably, and this prompt is written to work around that limitation rather than
pretend it doesn't exist.

---

Create a clean, flat-vector **technical wiring diagram** (Fritzing-style breadboard
illustration), top-down orthographic view, on a white background. Not photorealistic, not
3D, no perspective, no shadows. Crisp vector lines, high contrast, readable labels.

**Subject:** an ESP32 DevKit V1 microcontroller on a full-size 830-point solderless
breadboard, wired to six peripherals.

## Layout

- A **full-size 830-point breadboard**, horizontal, with the red (+) and blue (−) power
  rails along the top and bottom edges, and the center channel running horizontally.
- The **ESP32 DevKit V1** (30-pin, black PCB, silver ESP-WROOM-32 shield can, micro-USB
  connector at one end) sits **straddling the center channel**, centered left-to-right,
  with its USB port facing left. Its two pin headers plug into the rows on either side of
  the channel, leaving one free column of holes on each side for jumper wires.
- Peripherals arranged around it with clear space, each connected by colored jumper wires:
  - **Top-left:** SSD1306 OLED display, 0.96 inch, small blue PCB, 4 pins (VCC, GND, SDA, SCL)
  - **Top-right:** MPU-6050 accelerometer, small blue GY-521 breakout board
  - **Left:** DS18B20 waterproof temperature probe — stainless steel cylindrical tip on a
    black cable with 3 wires (red, black, yellow)
  - **Bottom-left:** capacitive soil moisture sensor — a single flat black paddle-shaped
    PCB with a 3-pin connector
  - **Right:** HX711 load cell amplifier, small green PCB, connected to a silver aluminium
    bar load cell
  - **Bottom-right:** NEO-6M GPS module, blue PCB, with a separate square ceramic patch
    antenna on a thin cable
  - **Far right:** three LEDs in a vertical column — green, yellow, red — each with a
    220 ohm resistor

## Two build stages — show these differently

Some parts are already in hand and some are still on order. Draw them distinctly so the
diagram doubles as a build checklist:

- **Draw solid and fully opaque** (available now — wire these first):
  SSD1306 OLED, MPU-6050, DS18B20 probe, NEO-6M GPS, HX711 board, the LEDs and resistors.
- **Draw with a dashed outline and slightly faded/greyed fill** (on order, not yet arrived):
  the **capacitive moisture sensor**, and the **aluminium bar load cell** that connects to
  the HX711. Note that the HX711 amplifier board itself IS available — only the load cell
  that plugs into its screw terminals is pending.
- Add a small legend entry: *"Solid = in hand · Dashed = on order"*

## Wire colors — follow these exactly

- **Red** = 3.3 V power
- **Black** = ground
- **Green** = I2C data (SDA)
- **Yellow** = I2C clock (SCL)
- **Blue** = analog and digital signal lines
- **Orange** = clock lines for the HX711

## Connections to draw and label

Label every wire endpoint with its GPIO number in a small clear callout box:

| From (ESP32) | To |
| :-- | :-- |
| GPIO21 | OLED SDA **and** MPU-6050 SDA (shared I2C bus) |
| GPIO22 | OLED SCL **and** MPU-6050 SCL (shared I2C bus) |
| GPIO4 | DS18B20 data pin, with a 4.7 kΩ resistor pulling it up to 3.3 V |
| GPIO34 | Capacitive moisture sensor analog output |
| GPIO18 | HX711 DT (data) |
| GPIO19 | HX711 SCK (clock) |
| GPIO16 | NEO-6M GPS **TX** pin (ESP32 receives here) |
| GPIO17 | NEO-6M GPS **RX** pin (ESP32 transmits here) |
| GPIO25 | Green LED, through a 220 Ω resistor, to ground |
| GPIO26 | Yellow LED, through a 220 Ω resistor, to ground |
| GPIO27 | Red LED, through a 220 Ω resistor, to ground |
| 3V3 | Red rail — powers OLED, MPU-6050, DS18B20, moisture sensor, GPS, HX711 |
| GND | Blue rail — common ground for all modules |

## Labels required

- A title at the top: **"ConcreSense — ESP32 Breadboard Wiring"**
- Each module labeled with its name in a clean sans-serif font
- Each GPIO number shown in a small box where the wire meets the ESP32
- A small legend in a corner showing the wire color meanings
- Draw the 4.7 kΩ pull-up resistor and the three 220 Ω resistors explicitly, with values printed

Style reference: an official Arduino or Adafruit "Fritzing" wiring guide — clean, flat,
instructional, textbook quality. Prioritise **legibility and correctness of the labeled
connections** over visual flourish.

---

## Caveat — read this before trusting the output

Your request was for **exact, accurate placement relative to the numbered breadboard holes.**
That specific requirement is the one thing current image-generation models genuinely cannot
do. They will produce something that *looks* like a professional wiring diagram, but:

- They cannot count to 63 or render row numbers accurately — the numbers will be garbled,
  repeated, or skipped.
- Wires will terminate at arbitrary holes that are not electrically correct.
- Pin labels on the ESP32 will be invented; the real pin *order* varies between DevKit
  variants anyway.

This is a limitation of how these models work, not of the prompt. For a diagram you'll
actually wire from — or submit to a teacher who may check it — the output should be treated
as an **illustration of the concept, not a wiring reference.**

**Reliable alternatives, in order of accuracy:**

1. **Fritzing** (fritzing.org, ~€8) — purpose-built for exactly this. Has real ESP32 DevKit
   V1, OLED, MPU-6050, HX711 and DS18B20 parts with correct pin geometry, and it renders
   true breadboard hole positions.
2. **A generated SVG/HTML diagram** — precise because it is drawn from coordinates rather
   than sampled from a model. Accurate hole positions and labels, no hallucination.
3. **Wokwi** — the project already has `wokwi/diagram.json` with a verified-correct
   connection list; its schematic view can be screenshotted.
