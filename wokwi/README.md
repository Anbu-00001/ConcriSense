# ConcreSense — Wokwi Simulation

## Student

- **Name:** Anbuchelvan
- **Section:** CSE A
- **Roll No:** 24CS0059

This also appears on the ESP32's serial banner at boot and briefly on the
OLED splash screen (see `concresense.ino`), so it is visible both in this
README and inside the running simulation / on real hardware.

---

## What this is

A [Wokwi](https://wokwi.com) circuit simulation of the ConcreSense hardware,
for use with the Wokwi extension for VS Code / Antigravity
(`wokwi.wokwi-vscode`). It wires a virtual ESP32 DevKit V1 to virtual
stand-ins for every sensor in `firmware/concresense/src/config.h`, and loads
the exact same firmware image that flashes onto the real board.

## Files

| File | Purpose |
|---|---|
| `diagram.json` | The virtual circuit: parts + wiring. |
| `wokwi.toml` | Tells the Wokwi extension which compiled firmware/elf to load. Paths inside it are relative to this `wokwi/` directory. |
| `README.md` | This file. |

## What's wired (matches `config.h` pin-for-pin)

| Signal | ESP32 pin | Wokwi board pin name | Part |
|---|---|---|---|
| I2C SDA | GPIO21 | `D21` | shared bus |
| I2C SCL | GPIO22 | `D22` | shared bus |
| SSD1306 OLED | I2C, addr `0x3C` | `D21`/`D22` | `wokwi-ssd1306` |
| MPU-6050 IMU | I2C, addr `0x68` | `D21`/`D22` | `wokwi-mpu6050` |
| HX711 DOUT/DT | GPIO18 | `D18` | `wokwi-hx711` (`type: "5kg"`) |
| HX711 SCK | GPIO19 | `D19` | `wokwi-hx711` |
| DS18B20 DQ | GPIO4 | `D4` | `wokwi-ds18b20` + 4.7k pull-up to 3V3 |
| Moisture AOUT | GPIO34 | `D34` | see substitution below |
| Verdict LEDs | GPIO25/26/27 | `D25`/`D26`/`D27` | `wokwi-led` x3, added for this sim (see below) |

All parts get power from `3V3` except the HX711, which is wired to `VIN`
(5V) to match its documented supply voltage — its `DT`/`SCK` logic lines
are still 3.3V-compatible.

## What's substituted, and why

**Moisture sensor -> `wokwi-slide-potentiometer` on GPIO34.**
Wokwi has no capacitive soil-moisture part. The slide pot's `SIG` pin is
wired directly to GPIO34 (VCC->3V3, GND->GND), and this **is** what the
firmware actually reads via `analogReadMilliVolts()` — dragging the pot in
the running sim changes the moisture reading the physics chain sees, just
like turning the real probe in air vs. water changes its output voltage.

**Load cell -> `wokwi-hx711` + a *second*, functionally separate
`wokwi-slide-potentiometer` on a spare ADC pin (GPIO35).**
This one needed more care. The task asked us to wire the HX711's bridge
pins (`E+`/`E-`/`A+`/`A-`) to a potentiometer so a demo user could drag a
"load" value. Checking
[docs.wokwi.com/parts/wokwi-hx711](https://docs.wokwi.com/parts/wokwi-hx711)
first: those four pins (plus `B+`/`B-`) are documented as **non-interactive,
rendered from attributes** — they cannot be wired to anything in the
simulator. The HX711's simulated weight is instead set through Wokwi's
**Automation Scenarios** (`load` control, float, in kg), not through the
bridge pins.

So: the HX711 is wired for real on `VCC`/`DT`/`SCK`/`GND` — that's the path
the firmware's `HX711_ADC`-based driver actually talks to, and it behaves
like real hardware. Its `E+`/`E-`/`A+`/`A-` pins are deliberately left
unconnected in `diagram.json` (wiring them would be cosmetic and misleading).
The second slide pot on GPIO35 is a **manual demo dial only** — it is *not*
read by any firmware code path. If you want the simulated load cell to
report a specific weight, use Wokwi's Automation Scenarios feature on the
`hx711_1` part; if you just want *something* draggable on screen for a
demo, that's what the GPIO35 pot is for. This is called out again inline
next to those connections at the bottom of `diagram.json`'s wiring (see the
`pot_load` part).

**Verdict LEDs (optional, added).** Three `wokwi-led` parts (green/yellow/
red) with 220-ohm series resistors on GPIO25/26/27, driven by a small,
clearly-commented addition to `concresense.ino` (`updateVerdictLeds()`) that
lights up according to `QualityClass` (GOOD/MARGINAL/REJECT) after each
measurement cycle. No sensor/physics/classification source under `src/` was
touched — only the top-level `.ino`.

## What's missing

**NEO-6M GPS is not in the diagram.** Wokwi has no built-in GPS/NMEA part,
so `gps_neo6m` has nothing to attach to on UART2 (GPIO16/17). The firmware
still runs fine — `gps.begin()` will report `ABSENT`/no-NMEA in the bring-up
log and loop, exactly as it does on a real board with the GPS unplugged.

## Building the firmware

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 --export-binaries firmware/concresense
```

Output lands in `firmware/concresense/build/esp32.esp32.esp32/`. `wokwi.toml`
points at:
- `concresense.ino.merged.bin` — the bootloader + partition table + app
  image pre-merged at their flash offsets by `--export-binaries`, so Wokwi
  doesn't have to guess a load address.
- `concresense.ino.elf` — optional, improves GDB symbols / crash traces.

Re-run the compile command after any firmware change; a running simulation
picks up the new binary automatically (Wokwi watches the firmware file).

Verified in this environment: compiles cleanly against
`esp32:esp32 3.3.11` with the project's existing libraries in
`~/Arduino/libraries` (Adafruit_SSD1306, Adafruit_GFX_Library, Adafruit_BusIO,
Adafruit_MPU6050, Adafruit_Unified_Sensor, OneWire, DallasTemperature,
HX711_ADC, TinyGPSPlus, PubSubClient, ArduinoJson) — 27% flash, 8% RAM.

## Running it in Antigravity

1. Open this project folder in Antigravity (the Wokwi extension
   `wokwi.wokwi-vscode` must be installed — it already is).
2. Make sure the firmware is built (see above) so
   `build/esp32.esp32.esp32/concresense.ino.merged.bin` exists.
3. Open `wokwi/diagram.json` — it has an inline **"Start Simulation"**
   button in the custom editor view. Click it, **or**:
4. `Ctrl+Shift+P` -> **"Wokwi: Start Simulator"**.
5. If this is the first time the extension has looked for a config in this
   workspace, it may ask you to pick a config file since `wokwi.toml` lives
   in `wokwi/` rather than the workspace root — use
   **"Wokwi: Select Config File"** and choose `wokwi/wokwi.toml` once; it is
   remembered after that.

You'll need a Wokwi for VS Code license (free trial or paid) to actually run
the simulator — that's a one-time interactive step only you can do; it was
not attempted here.

## What was NOT done by this setup (manual steps left for you)

- **Clicking "Start Simulation" and watching it run.** This was built and
  compiled headlessly; nobody has visually confirmed the animated sim yet.
- **`wokwi-cli` headless run.** No `wokwi-cli` binary was found on `PATH` in
  this environment (`npx wokwi-cli` also isn't a published npm package under
  that name), so a headless CLI run was not attempted. If you install it
  (see Wokwi's official install script) you will additionally need a
  `WOKWI_CLI_TOKEN` license token, which was intentionally not set up here.
- **Trying the load-cell Automation Scenario** on the `hx711_1` part to
  drive a specific simulated weight, since that's an interactive step inside
  the running simulator.
