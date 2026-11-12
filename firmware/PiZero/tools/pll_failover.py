#!/usr/bin/env python3
"""PLLA phase 6 (PiZero_PLLA_plan.md): PLLA is the boot default, and a once-a-second watchdog restores it
if the firmware rewrites it, or falls back to PLLD if the rewrite comes back. Bench request 0x46 op 7
stands in for the firmware by rewriting PLLA to its boot multiplier. A tone plays straight to the Pi (the
Mac's default output is not changed).

Usage: python3.14 firmware/PiZero/tools/pll_failover.py [--boot-only]
--boot-only just checks that the board came up on PLLA, locked and exact. Otherwise it also forces one
restore and then a fallback, checks that the slots still decode on PLLD, and puts the SMI back on PLLA.
"""
import argparse
import sys
import time

from uac2_stream_check import Tone, find_board
from smi_check import smi_stats, snapshot, decode_i2s, decode_spdif
import pll_check as p
import smi_pll_check as q


def decode_slots(first, words):
    """Problems found decoding each slot as S/PDIF or else I2S; an empty list when all four decode."""
    bad = []
    for slot in range(4):
        sp = decode_spdif(words, slot)
        if len(sp) >= 20:
            errors = sum(1 for x in sp if not x[2])
            if errors:
                bad.append(f"slot {slot} S/PDIF {errors} errors")
        elif len(decode_i2s(words, slot)) < 28:
            bad.append(f"slot {slot} does not decode")
    return bad


def wd(dev):
    st = smi_stats(dev)
    return dict(source=st.get("smi_source"), events=st.get("pll_wd_events"), restores=st.get("pll_wd_restores"),
                fallbacks=st.get("pll_wd_fallbacks"), freq=st["freq"])


def boot_check(dev, failures):
    w = wd(dev)
    src, err = q.report_clock(dev, "boot")
    if w["source"] != 1 or src != "plla" or abs(err) > 0.1:
        failures.append(f"boot: source {w['source']} {src} {err:+.3f} ppm")
    return w


def sabotage(dev, ms):
    dev.ctrl_transfer(0x40, 0x46, 7, ms, None, timeout=1000)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boot-only", action="store_true")
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    failures = []
    w0 = boot_check(dev, failures)
    print(f"watchdog at boot: {w0}")
    if not args.boot_only:
        tone = Tone("DSPi Zero", rate=48000, set_device_rate=True)
        try:
            tone.start()
            time.sleep(2.0)
            # One rewrite: the watchdog should restore PLLA within about a second and stay on it.
            sabotage(dev, 0)
            time.sleep(2.5)
            w1 = wd(dev)
            src, err = q.report_clock(dev, "after one rewrite")
            print(f"  watchdog: {w1}")
            if (w1["restores"] - w0["restores"], w1["fallbacks"] - w0["fallbacks"]) != (1, 0) or src != "plla" \
                    or abs(err) > 0.1:
                failures.append(f"one rewrite: {w1}, {src} {err:+.3f} ppm")
            # Rewrites that keep coming back: restore once, then fall back to PLLD.
            time.sleep(1.0)
            sabotage(dev, 5000)
            time.sleep(6.5)
            w2 = wd(dev)
            src, err = q.report_clock(dev, "after repeated rewrites")
            print(f"  watchdog: {w2}")
            if w2["fallbacks"] - w1["fallbacks"] != 1 or src != "plld" or w2["source"] != 0:
                failures.append(f"repeated rewrites: {w2}, {src}")
            time.sleep(1.0)
            first, words = snapshot(dev)
            bad = decode_slots(first, words)
            print(f"  on PLLD after the fallback: {'slots decode' if not bad else bad}")
            if bad:
                failures.append(f"decode on PLLD: {bad}")
        finally:
            tone.stop()
        q.set_source(dev, "plla")
        src, err = q.report_clock(dev, "back on PLLA")
        if src != "plla" or abs(err) > 0.1:
            failures.append(f"back on PLLA: {src} {err:+.3f} ppm")
    if failures:
        print("FAIL:", "; ".join(failures))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
