#!/usr/bin/env python3
"""Whole-core CPU load (z4/cpu_meter.c), the figure DSPi Console shows as CPU. Reads bench request 0x43 and
splits the last 500 ms window into interrupts, the producer (USB and input delivery, DSPi's blocks), and
DSPi's main-loop services; calls that only polled are shown apart and do not count.

Usage: python3.14 firmware/PiZero/tools/cpu_check.py [--seconds N]
"""
import argparse
import struct
import sys
import time

from uac2_stream_check import find_board

REGIONS = ("producer", "services")


def meter(dev):
    raw = bytes(dev.ctrl_transfer(0xC0, 0x43, 0, 0, 4 * 12, timeout=1000))
    v = struct.unpack("<12I", raw)
    if v[0] != 0x4D555043:
        raise SystemExit("FAIL: no CPU meter (load the current pizero_z4.img)")
    return dict(load=v[1] / 100, window=v[2], irq=v[3], work=v[4:6], idle=v[6:8], calls=v[8:10], longest=v[10:12])


def show(m):
    w = m["window"]
    parts = ", ".join(f"{n} {m['work'][i] / w * 100:.2f}% (polling {m['idle'][i] / w * 100:.2f}%, {m['calls'][i]} calls, "
                      f"longest {m['longest'][i] / 1000:.0f} us)" for i, n in enumerate(REGIONS))
    print(f"load {m['load']:.2f}%: interrupts {m['irq'] / w * 100:.2f}%, {parts}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=3)
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    end = time.monotonic() + args.seconds
    while True:
        show(meter(dev))
        if time.monotonic() >= end:
            return 0
        time.sleep(0.5)


if __name__ == "__main__":
    sys.exit(main())
