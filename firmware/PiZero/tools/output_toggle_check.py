#!/usr/bin/env python3
"""Output enable toggling, as the matrix mixer does it (REQ_SET_OUTPUT_ENABLE), while a tone plays straight to
the Pi (the Mac's default output is not changed). Each toggle can ask for DSPi's Core 1 modes, which the Pi
does not have; the board must keep answering and keep streaming. Output states are put back afterwards.

Usage: python3.14 firmware/PiZero/tools/output_toggle_check.py [--toggles 400] [--rate 50]
"""
import argparse
import random
import sys
import time

from uac2_stream_check import Tone, find_board
from smi_check import VENDOR_IFACE, smi_stats

REQ_SET_OUTPUT_ENABLE, REQ_GET_OUTPUT_ENABLE = 0x72, 0x73
OUTPUTS = 9     # 8 outputs and PDM


def get_enable(dev, out):
    return dev.ctrl_transfer(0xC1, REQ_GET_OUTPUT_ENABLE, out, VENDOR_IFACE, 1)[0]


def set_enable(dev, out, on):
    dev.ctrl_transfer(0x41, REQ_SET_OUTPUT_ENABLE, out, VENDOR_IFACE, bytes([1 if on else 0]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--toggles", type=int, default=400)
    ap.add_argument("--rate", type=float, default=50, help="toggles a second")
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    before = [get_enable(dev, o) for o in range(OUTPUTS)]
    print(f"output enables found: {before}")
    random.seed(1)
    tone = Tone("DSPi Zero", rate=48000, set_device_rate=True)
    failed = None
    try:
        tone.start()
        time.sleep(2.0)
        a = smi_stats(dev)
        for i in range(args.toggles):
            out = random.randrange(OUTPUTS)
            try:
                set_enable(dev, out, not get_enable(dev, out))
                if i % 50 == 49:
                    s = smi_stats(dev)
                    print(f"  {i + 1} toggles: frames {s['frames']}, core1_mode {s['s'].get('core1_mode')}, "
                          f"enables {[get_enable(dev, o) for o in range(OUTPUTS)]}", flush=True)
            except Exception as e:
                failed = f"no answer after {i} toggles ({e})"
                break
            time.sleep(1 / args.rate)
        if not failed:
            time.sleep(1.0)
            b = smi_stats(dev)
            moved = (b["frames"] - a["frames"]) & 0xFFFFFFFF
            print(f"  stream: {moved} frames over the run, composer late +{b['late'] - a['late']}")
            if moved == 0:
                failed = "the output stream stopped"
    finally:
        tone.stop()
        if not failed:
            for o, v in enumerate(before):
                set_enable(dev, o, v)
            print(f"output enables put back: {[get_enable(dev, o) for o in range(OUTPUTS)]}")
    print(f"FAIL: {failed}" if failed else "PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
