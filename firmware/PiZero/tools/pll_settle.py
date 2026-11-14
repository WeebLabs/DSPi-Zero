#!/usr/bin/env python3
"""PLLA phase 3 (PiZero_PLLA_plan.md): how long the SMI clock takes to settle after a live family move
between rows b48 and b44. The firmware counts the SMI clock in back-to-back 1 ms windows and moves PLLA at
the start of the third (bench request 0x46 op 5), so the main loop stalls for the capture.

Usage: python3.14 firmware/PiZero/tools/pll_settle.py [--windows N] [--repeat N] [--rows 4,3]
Puts the SMI on PLLA first and leaves PLLA on b48 at the end. With --rows 5 it captures the move from b48
to c48, the 48 kHz row used while PLLA is trimmed; the firmware goes back to b48 after each capture.
"""
import argparse
import struct
import sys
import time

from uac2_stream_check import find_board
import pll_check as p
import smi_pll_check as q

XOSC = 19.2e6
ROW_VCO = {3: 1843.2e6, 4: 1693.44e6, 5: 2211.84e6}     # b48, b44, c48


def capture(dev, row, n):
    seq = struct.unpack("<I", bytes(dev.ctrl_transfer(0xC0, 0x46, 1, 0, 4, timeout=1000)))[0]
    dev.ctrl_transfer(0x40, 0x46, 5, row | n << 8, None, timeout=1000)
    for _ in range(50):
        time.sleep(0.05)
        raw = bytes(dev.ctrl_transfer(0xC0, 0x46, 1, 0, 24 + 64 * 8, timeout=1000))
        h = struct.unpack_from("<6I", raw)
        if h[0] != seq:
            counts = struct.unpack_from("<64I", raw, 24)[:h[5]]
            starts = struct.unpack_from("<64i", raw, 24 + 256)[:h[5]]
            return h, counts, starts
    raise SystemExit("FAIL: no capture")


def show(dev, row, n):
    s = p.status(dev)
    smi_div = (s["smi_div"] >> 12) & 15
    h, counts, starts = capture(dev, row, n)
    gate_s = h[4] / XOSC
    want = ROW_VCO[row] / 5 / smi_div * gate_s
    old = ROW_VCO[h[2]] / 5 / smi_div * gate_s if h[2] in ROW_VCO else 0
    settled = None
    for i in range(len(counts) - 1, -1, -1):
        if abs(counts[i] - want) > 1.5:
            break
        settled = i
    print(f"== row {h[2]} -> {row}: target {want:.1f} counts a window (old {old:.1f}, 1 count = "
          f"{1e6 / want:.0f} ppm)")
    line = []
    for i, (c, t) in enumerate(zip(counts, starts)):
        line.append(f"{t / 1000:+5.1f} ms {c:6d} ({(c - want) / want * 1e6:+7.0f})")
    for i in range(0, len(line), 4):
        print("  " + "   ".join(line[i:i + 4]))
    if settled is None or settled >= len(counts) - 1:
        print("  NOT settled within the capture")
        return None
    t = starts[settled]
    print(f"  within one count of the target from the window starting {t / 1000:+.1f} ms after the write")
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", type=int, default=24)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--rows", default="4,3")
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    q.set_source(dev, "plla")
    if q.smi_clock(dev)[0] != "plla":
        print("FAIL: the SMI is not on PLLA at this rate")
        return 1
    worst = 0
    for _ in range(args.repeat):
        for row in [int(r) for r in args.rows.split(",")]:
            t = show(dev, row, args.windows)
            time.sleep(0.1)
            worst = max(worst, 10**9 if t is None else t)
    print(f"worst settling: {worst / 1000:.1f} ms" if worst < 10**9 else "FAIL: did not settle")
    return 0 if worst < 10**9 else 1


if __name__ == "__main__":
    sys.exit(main())
