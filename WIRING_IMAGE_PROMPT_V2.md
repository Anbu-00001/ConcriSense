# Follow-up prompt — corrections to the ConcreSense wiring diagram

Paste below the line into the **same** ChatGPT/Gemini chat that produced the first image, so
it edits that image rather than starting over.

---

The diagram is close — the layout, the component choices, and the solid/dashed staging are
all correct. Keep the overall composition, style, colours and arrangement exactly as they
are. Fix these four specific things and regenerate.

## 1. CRITICAL — the GPS TX/RX wires are crossed

UART lines must cross over: one device's transmit pin goes to the other device's receive pin.
The current diagram connects transmit-to-transmit and receive-to-receive, which would stop
the GPS from ever working.

Redraw so that:

- The wire from **GPIO16** on the ESP32 goes to the pin labelled **TX** on the NEO-6M module.
  Label this wire: **"GPIO16 (ESP32 RX) → GPS TX"**
- The wire from **GPIO17** on the ESP32 goes to the pin labelled **RX** on the NEO-6M module.
  Label this wire: **"GPIO17 (ESP32 TX) → GPS RX"**

Do not label these simply "GPS RX" and "GPS TX" — that phrasing is ambiguous about which end
of the link is meant, and is what caused the error. Always show both ends, in the
"ESP32 pin → module pin" form above.

## 2. Fix two misspelled pin labels

- On the **MPU-6050** board, one pin is currently labelled **"SOL"**. The correct label is
  **"SDA"**.
- On the **HX711** board, one pin is currently labelled **"SDR"**. The correct label is
  **"VCC"**.

## 3. Correct the MPU-6050 pin order

The MPU-6050 (GY-521) pins are currently drawn in the order `VCC, GND, SDA, SCL`. The real
board's silkscreen order is:

**`VCC, GND, SCL, SDA`** — SCL comes **before** SDA.

Redraw the pin labels in that order and move the wires so the green SDA wire and the yellow
SCL wire each connect to the correctly-named pin.

## 4. Anchor every GPIO label to a real ESP32 pin

At the moment the GPIO callout boxes float loosely over the breadboard and do not point at
anything specific, so the diagram cannot be used to wire from. Instead:

- Print the pin names **directly along both edges of the ESP32 board itself**, as they appear
  on the real silkscreen — a vertical list of small labels running down each side of the
  module (for example: `3V3, GND, GPIO15, GPIO2, GPIO4, GPIO16, GPIO17, GPIO5, GPIO18,
  GPIO19, GPIO21, GPIO22, GPIO23, VIN` and so on).
- Draw each coloured jumper wire so it visibly **starts at that specific labelled pin** on
  the ESP32 and ends at the specific labelled pin on the peripheral module.
- Remove the floating rectangular callout boxes. The wire itself, running from a named pin to
  a named pin, should be the label.

Accuracy of these pin-to-pin connections matters more than visual polish. Every wire must
begin and end at a clearly readable pin name.

## Keep unchanged

- Title, the "Solid = in hand · Dashed = on order" legend, the wire-colour legend, the
  resistor legend, and the notes box at the bottom left.
- Dashed/faded rendering for the **capacitive moisture sensor** and the **aluminium bar load
  cell** only. Everything else stays solid.
- All other connections, which are correct:
  GPIO21 → SDA on both OLED and MPU-6050 · GPIO22 → SCL on both ·
  GPIO4 → DS18B20 data with the 4.7 kΩ pull-up to 3.3 V · GPIO34 → moisture sensor AOUT ·
  GPIO18 → HX711 DT · GPIO19 → HX711 SCK ·
  GPIO25/26/27 → green/yellow/red LEDs through 220 Ω resistors ·
  3.3 V on the red rail, ground on the blue rail.
