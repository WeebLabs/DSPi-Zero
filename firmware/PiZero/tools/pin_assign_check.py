#!/usr/bin/env python3
"""Pin assignment on the Pi (PiZero_GPIO_plan.md, phase 8). Moves each output function to other pins through
DSPi's own pin requests, as DSPi Console sends them, and reads the pins with the toggle probe (bench request
0x4B, which samples GPLEV0), so no wiring is needed: the signal must appear on the new pin and leave the old
one. Pins the Pi cannot drive must be refused. A tone plays straight to the Pi throughout (the Mac's default
output is not changed), and every setting is put back at the end.

Usage: python3.14 firmware/PiZero/tools/pin_assign_check.py [--spare 17 18 19]
The spare pins must be SD lines (GPIO 8-23) that nothing else drives. The second must be the first plus one,
since BCK takes it for LRCLK. The defaults leave GPIO 14 and 15 (the UART) alone.
"""
import argparse
import struct
import sys
import time

from uac2_stream_check import Tone, find_board
from smi_check import VENDOR_IFACE

REQ_SET_ADAT_INPUT_PIN = 0x6A
REQ_SET_OUTPUT_PIN, REQ_GET_OUTPUT_PIN = 0x7C, 0x7D
REQ_SET_OUTPUT_TYPE, REQ_GET_OUTPUT_TYPE = 0xC0, 0xC1
REQ_SET_I2S_BCK_PIN, REQ_GET_I2S_BCK_PIN = 0xC2, 0xC3
REQ_SET_MCK_ENABLE, REQ_GET_MCK_ENABLE, REQ_SET_MCK_PIN, REQ_GET_MCK_PIN = 0xC4, 0xC5, 0xC6, 0xC7
REQ_SET_ADAT_ENABLE, REQ_GET_ADAT_ENABLE, REQ_SET_ADAT_PIN, REQ_GET_ADAT_PIN = 0xCA, 0xCB, 0xCC, 0xCD
REQ_SET_I2S_RX_PIN, REQ_GET_I2S_RX_PIN = 0xF1, 0xF2
OK, INVALID_PIN = 0, 1
OUTPUT_TYPE_I2S = 1
SETTLE = 0.4    # a slot pin change goes through a muted pipeline reset in the main loop


def req(dev, code, value):
    return dev.ctrl_transfer(0xC1, code, value, VENDOR_IFACE, 1)[0]


def toggles(dev, gpio, ms=20):
    return struct.unpack("<2I", bytes(dev.ctrl_transfer(0xC0, 0x4B, gpio, ms, 8)))[0]


class Check:
    def __init__(self, dev):
        self.dev, self.failures = dev, []

    def expect(self, what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'} {what}{': ' + detail if detail else ''}")
        if not ok:
            self.failures.append(what)

    def status(self, what, code, value, want):
        got = req(self.dev, code, value)
        self.expect(f"{what} returns {want}", got == want, f"got {got}")
        return got

    def signal(self, what, on, off):
        """The function's signal on GPIO `on` and none on GPIO `off`."""
        time.sleep(SETTLE)
        a, b = toggles(self.dev, on), toggles(self.dev, off)
        self.expect(f"{what}: GPIO {on} carries it, GPIO {off} does not", a > 0 and b == 0,
                    f"{a} and {b} level changes in 20 ms")


def refusals(c):
    print("== pins the Pi cannot drive")
    for pin in (4, 24, 26):
        c.status(f"output slot 1 on GPIO {pin}", REQ_SET_OUTPUT_PIN, pin << 8 | 0, INVALID_PIN)
    for pin in (6, 13):
        c.status(f"MCK on GPIO {pin}", REQ_SET_MCK_PIN, pin, INVALID_PIN)
    c.status("ADAT out on GPIO 24", REQ_SET_ADAT_PIN, 24, INVALID_PIN)
    c.status("ADAT in on GPIO 21", REQ_SET_ADAT_INPUT_PIN, 21, INVALID_PIN)
    c.status("I2S input pair 1 on GPIO 1", REQ_SET_I2S_RX_PIN, 0 << 8 | 1, INVALID_PIN)
    c.status("I2S BCK on GPIO 23 (LRCLK 24)", REQ_SET_I2S_BCK_PIN, 0 << 8 | 23, INVALID_PIN)


def outputs(c, spare):
    print("== output slot 1")
    old = req(c.dev, REQ_GET_OUTPUT_PIN, 0)
    try:
        if c.status(f"output slot 1 to GPIO {spare[0]}", REQ_SET_OUTPUT_PIN, spare[0] << 8 | 0, OK) == OK:
            c.signal("slot 1 moved", spare[0], old)
    finally:
        req(c.dev, REQ_SET_OUTPUT_PIN, old << 8 | 0)
    c.signal("slot 1 back", old, spare[0])


def clocks(c, spare):
    """DSPi refuses a BCK move while an I2S output uses the pair, so the pin moves first and an I2S slot
    then starts the clocks."""
    print("== I2S BCK and LRCLK")
    types = [req(c.dev, REQ_GET_OUTPUT_TYPE, s) for s in range(4)]
    if OUTPUT_TYPE_I2S in types:
        print("  skipped: an output is already I2S, and DSPi will not move its clocks")
        return
    slot, old_bck = 3, req(c.dev, REQ_GET_I2S_BCK_PIN, 0)
    try:
        if c.status(f"BCK to GPIO {spare[0]}", REQ_SET_I2S_BCK_PIN, 0 << 8 | spare[0], OK) == OK:
            req(c.dev, REQ_SET_OUTPUT_TYPE, OUTPUT_TYPE_I2S << 8 | slot)
            c.signal("BCK moved", spare[0], old_bck)
            c.signal("LRCLK moved", spare[1], old_bck + 1)
            c.status("BCK move refused while I2S runs", REQ_SET_I2S_BCK_PIN, 0 << 8 | old_bck, 4)
    finally:
        req(c.dev, REQ_SET_OUTPUT_TYPE, types[slot] << 8 | slot)
        time.sleep(SETTLE)
        req(c.dev, REQ_SET_I2S_BCK_PIN, 0 << 8 | old_bck)


def adat(c, spare):
    print("== ADAT out")
    old_en, old_pin = req(c.dev, REQ_GET_ADAT_ENABLE, 0), req(c.dev, REQ_GET_ADAT_PIN, 0)
    try:
        c.status("ADAT on", REQ_SET_ADAT_ENABLE, 1, OK)
        c.signal("ADAT", old_pin, spare[2])
        if c.status(f"ADAT to GPIO {spare[2]}", REQ_SET_ADAT_PIN, spare[2], OK) == OK:
            c.signal("ADAT moved", spare[2], old_pin)
    finally:
        req(c.dev, REQ_SET_ADAT_PIN, old_pin)
        req(c.dev, REQ_SET_ADAT_ENABLE, old_en)


def mck(c):
    """DSPi refuses an MCK move while MCK is on, so each move is made with it off."""
    print("== MCK")
    old_en, old_pin = req(c.dev, REQ_GET_MCK_ENABLE, 0), req(c.dev, REQ_GET_MCK_PIN, 0)
    try:
        for pin, other, name in ((5, 4, "GPCLK1"), (4, 5, "GPCLK0")):
            req(c.dev, REQ_SET_MCK_ENABLE, 0)
            if c.status(f"MCK to GPIO {pin}", REQ_SET_MCK_PIN, pin, OK) == OK:
                c.status("MCK on", REQ_SET_MCK_ENABLE, 1, OK)
                c.signal(f"MCK on {name}", pin, other)
    finally:
        req(c.dev, REQ_SET_MCK_ENABLE, 0)
        req(c.dev, REQ_SET_MCK_PIN, old_pin)
        req(c.dev, REQ_SET_MCK_ENABLE, old_en)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spare", type=int, nargs=3, default=[17, 18, 19])
    args = ap.parse_args()
    s = args.spare
    if not all(8 <= p <= 23 for p in s) or s[1] != s[0] + 1 or len(set(s)) != 3:
        print("FAIL: --spare takes three different SD lines (8-23), the second the first plus one")
        return 1
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    c = Check(dev)
    tone = Tone("DSPi Zero", rate=48000, set_device_rate=True)
    try:
        tone.start()
        time.sleep(2.0)
        refusals(c)
        outputs(c, s)
        clocks(c, s)
        adat(c, s)
        mck(c)
    finally:
        tone.stop()
    if c.failures:
        print("FAIL: " + ", ".join(c.failures))
        return 1
    print("PASS: every function moved to its new pin and left the old one, and invalid pins were refused")
    return 0


if __name__ == "__main__":
    sys.exit(main())
