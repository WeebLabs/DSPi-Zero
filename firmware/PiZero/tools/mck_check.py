#!/usr/bin/env python3
"""MCK on GPIO 4 (GPCLK0), from the SMI's own clock. Turns MCK on with DSPi's REQ_SET_MCK_ENABLE and
plays a tone straight to the Pi (the Mac's default output is not changed). At 44.1, 48 and 96 kHz and
both multipliers, the clock manager's counter must read MCK at exactly mult x fs and at mult / 768 of
the SMI clock, with an integer divider from PLLA. Then checks that MCK follows a PLLA trim and a move to
PLLD, and that DSPi's policy stops it when no slot is I2S. Slot types and MCK are put back afterwards.

Usage: python3.14 firmware/PiZero/tools/mck_check.py [--rates 48000,44100,96000]
"""
import argparse
import sys
import time

from uac2_stream_check import Tone, find_board
from smi_check import VENDOR_IFACE, get_type, set_type
import pll_check as p
import smi_pll_check as q

REQ_SET_MCK_ENABLE, REQ_GET_MCK_ENABLE = 0xC4, 0xC5
REQ_SET_MCK_MULTIPLIER, REQ_GET_MCK_MULTIPLIER = 0xC8, 0xC9
MUX_GP0 = 20


def vendor_set(dev, req, value):
    return dev.ctrl_transfer(0xC1, req, value, VENDOR_IFACE, 1)[0]


def vendor_get(dev, req):
    return dev.ctrl_transfer(0xC1, req, 0, VENDOR_IFACE, 1)[0]


def gates_for(hz):
    return max(1, min(40, int(2**24 / (hz * 0.01)) - 1))


def gp0(dev):
    s = p.status(dev)
    ctl, div = s["gp0_ctl"], s["gp0_div"]
    return dict(src=ctl & 15, enab=bool(ctl & 0x10), busy=bool(ctl & 0x80), mash=(ctl >> 9) & 3,
                div=((div >> 12) & 0xFFF) + (div & 0xFFF) / 4096, ctl=ctl, gpio4=(s["gpfsel0"] >> 12) & 7)


def read_clocks(dev, rate, mult):
    """MCK and SMI in ppm from nominal, with the GPCLK0 settings."""
    mck = p.measure(dev, MUX_GP0, gates_for(rate * mult))[0]
    smi = p.measure(dev, p.MUX_SMI, gates_for(rate * 768))[0]
    return (mck - rate * mult) / (rate * mult) * 1e6, (smi - rate * 768) / (rate * 768) * 1e6, gp0(dev), mck


def check_rate(dev, rate, failures):
    tone = Tone("DSPi Zero", rate=rate, set_device_rate=True)
    try:
        tone.start()
        time.sleep(3.0)
        for mult in (256, 128):
            vendor_set(dev, REQ_SET_MCK_MULTIPLIER, 1 if mult == 256 else 0)
            time.sleep(0.05)
            m, s, g, hz = read_clocks(dev, rate, mult)
            want_div = q.smi_clock(dev)[1] * 768 / mult
            ok = (g["enab"] and g["src"] == 4 and g["mash"] == 0 and abs(g["div"] - want_div) < 1e-6 and abs(m) < 0.1
                  and g["gpio4"] == 4)
            print(f"  {rate} Hz x {mult}: MCK {hz / 1e6:.6f} MHz ({m:+.3f} ppm), SMI {s:+.3f} ppm, GPCLK0 src "
                  f"{g['src']} div {g['div']:.4f} MASH {g['mash']} {'ok' if ok else 'BAD'}")
            if not ok:
                failures.append(f"{rate} x {mult}: {g}, MCK {m:+.3f} ppm")
    finally:
        tone.stop()


def check_follow(dev, failures):
    tone = Tone("DSPi Zero", rate=48000, set_device_rate=True)
    try:
        tone.start()
        time.sleep(3.0)
        vendor_set(dev, REQ_SET_MCK_MULTIPLIER, 1)
        base = read_clocks(dev, 48000, 256)[0]
        dev.ctrl_transfer(0x40, 0x46, 6, 1000, None, timeout=1000)     # +10 ppm
        time.sleep(0.2)
        got = read_clocks(dev, 48000, 256)[0] - base
        dev.ctrl_transfer(0x40, 0x46, 6, 0, None, timeout=1000)
        ok = abs(got - 10) < 0.15
        print(f"  PLLA trim +10 ppm: MCK moved {got:+.3f} ppm {'ok' if ok else 'BAD'}")
        if not ok:
            failures.append(f"trim: MCK moved {got:+.3f} ppm")
        q.set_source(dev, "plld")
        m, s, g, hz = read_clocks(dev, 48000, 256)
        ok = g["enab"] and g["src"] == 6 and abs(m - s) < 0.2
        print(f"  SMI on PLLD: MCK {m:+.3f} ppm, SMI {s:+.3f} ppm, GPCLK0 src {g['src']} div {g['div']:.4f} MASH "
              f"{g['mash']} {'ok' if ok else 'BAD'}")
        if not ok:
            failures.append(f"PLLD: MCK {m:+.3f} SMI {s:+.3f} {g}")
        q.set_source(dev, "plla")
        m, s, g, hz = read_clocks(dev, 48000, 256)
        ok = g["src"] == 4 and abs(m) < 0.1
        print(f"  back on PLLA: MCK {m:+.3f} ppm, GPCLK0 src {g['src']} {'ok' if ok else 'BAD'}")
        if not ok:
            failures.append(f"back on PLLA: MCK {m:+.3f} {g}")
    finally:
        tone.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rates", default="48000,44100,96000")
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    failures = []
    old_types = [get_type(dev, i) for i in range(4)]
    old_enable, old_mult = vendor_get(dev, REQ_GET_MCK_ENABLE), vendor_get(dev, REQ_GET_MCK_MULTIPLIER)
    print(f"found: slot types {old_types}, MCK {'on' if old_enable else 'off'}, multiplier "
          f"{256 if old_mult else 128}, GPCLK0 {gp0(dev)}")
    try:
        set_type(dev, 0, 1)
        time.sleep(0.5)
        st = vendor_set(dev, REQ_SET_MCK_ENABLE, 1)
        if st != 0:
            print(f"FAIL: REQ_SET_MCK_ENABLE returned {st}")
            return 1
        for r in args.rates.split(","):
            check_rate(dev, int(r), failures)
        check_follow(dev, failures)
        # DSPi's policy: no I2S slot, no MCK.
        for i in range(4):
            set_type(dev, i, 0)
        time.sleep(0.5)
        g = gp0(dev)
        ok = not g["enab"] and not g["busy"] and g["gpio4"] == 0
        print(f"  no I2S slot: GPCLK0 {'stopped' if ok else 'STILL RUNNING'} (ctl {g['ctl']:#x}), GPIO 4 function "
              f"{g['gpio4']} (0 = input)")
        if not ok:
            failures.append("MCK still running with no I2S slot")
        set_type(dev, 0, 1)
        time.sleep(0.5)
        g = gp0(dev)
        print(f"  slot 0 back to I2S: GPCLK0 {'running' if g['enab'] else 'STOPPED'}")
        if not g["enab"]:
            failures.append("MCK did not restart with an I2S slot")
        vendor_set(dev, REQ_SET_MCK_ENABLE, 0)
        time.sleep(0.05)
        g = gp0(dev)
        print(f"  MCK disabled: GPCLK0 {'stopped' if not g['enab'] and not g['busy'] else 'STILL RUNNING'}")
        if g["enab"] or g["busy"]:
            failures.append("MCK still running after REQ_SET_MCK_ENABLE 0")
    finally:
        vendor_set(dev, REQ_SET_MCK_MULTIPLIER, old_mult)
        vendor_set(dev, REQ_SET_MCK_ENABLE, old_enable)
        for i, t in enumerate(old_types):
            if get_type(dev, i) != t:
                set_type(dev, i, t)
                time.sleep(0.3)
    if failures:
        print("FAIL:", "; ".join(failures))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
