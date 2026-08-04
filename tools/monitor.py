#!/usr/bin/env python3
"""
Serial monitor for ConcreSense bring-up.

Pulses DTR/RTS to reset the ESP32 so the boot banner and bring-up report are
captured from the start, then reads for a fixed window. Optionally sends a
command first (e.g. 'm' for a measurement cycle).

Usage:
    python3 tools/monitor.py --seconds 12
    python3 tools/monitor.py --seconds 8 --send m
"""
import argparse
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--send", default=None, help="command to send after reset")
    ap.add_argument("--send-delay", type=float, default=9.0,
                    help="wait this long after reset before sending")
    ap.add_argument("--no-reset", action="store_true")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as e:
        print(f"cannot open {args.port}: {e}", file=sys.stderr)
        return 1

    if not args.no_reset:
        # esptool's run-mode reset. Order matters: IO0 must be released HIGH
        # (dtr False) and settled BEFORE EN is released, or the ROM samples IO0
        # low and drops into UART download mode. When that happens the board
        # sits echoing boot-message fragments for ~15s before timing out into
        # the app -- which looks exactly like a firmware boot loop but is not.
        ser.dtr = False   # IO0 high
        ser.rts = True    # EN low  -> held in reset
        time.sleep(0.15)
        ser.dtr = False   # re-assert IO0 high while still in reset
        ser.rts = False   # EN high -> boot, sampling IO0 high = run mode
        time.sleep(0.05)
        ser.reset_input_buffer()

    start = time.time()
    sent = False
    while time.time() - start < args.seconds:
        if args.send and not sent and (time.time() - start) >= args.send_delay:
            ser.write((args.send + "\n").encode())
            ser.flush()
            sent = True
        data = ser.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()

    ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
