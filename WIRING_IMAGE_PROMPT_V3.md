# Follow-up prompt — one remaining fix to the ConcreSense wiring diagram

Paste below the line into the same chat. This is a small, targeted fix — everything else in
the last version was correct, keep it all exactly as it is.

---

This version is almost right. One specific problem: **GPIO4 is missing from the pin labels
printed on the ESP32 board.**

The left-edge pin list currently reads:
`3V3, EN, VP, VN, GPIO34, GPIO35, GPIO32, GPIO33, GPIO25, GPIO26, GPIO27, GPIO14, GND`

**GPIO4 must be added to this list**, in its correct position on the real ESP32 DevKit V1
silkscreen — it sits between GPIO33 and GPIO25 on the left edge. The corrected left-edge
list should read:

`3V3, EN, VP, VN, GPIO34, GPIO35, GPIO32, GPIO33, GPIO25, GPIO26, GPIO27, GPIO14, GPIO4, GND`

Then draw the DS18B20 data wire (blue) so it visibly starts at this newly-labeled **GPIO4**
pin and ends at the DS18B20's data line, passing through the 4.7 kΩ pull-up to the 3.3 V
rail exactly as before.

Also double-check: the yellow (SCL) and green (SDA) wires from the MPU-6050 should run
directly down to their labeled pins without crossing each other. If they currently cross,
redraw them so the yellow wire runs cleanly to the SCL breadboard column and the green wire
runs cleanly to the SDA column, with no overlap.

Do not change anything else — the GPS wiring, the MPU-6050 pin order, the HX711 labels, the
LED section, the legends, and the solid/dashed staging are all correct as they are.
