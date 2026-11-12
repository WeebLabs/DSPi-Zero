#!/usr/bin/env python3
"""PLLA phase 6 soak (PiZero_PLLA_plan.md): stream for an hour with a rate switch every minute, cycling
44.1, 48 and 96 kHz, playing straight to the Pi (the Mac's default output is not changed). Each minute is
checked over a 50 s window that leaves out the stream restart: slot underruns, ring drops, USB underruns
and composer lateness must not move, the SMI must stay on PLLA with an exact clock, and the PLLA watchdog
must see nothing.

Usage: python3.14 firmware/PiZero/tools/pll_soak.py [--minutes 60]
"""
import argparse
import sys
import time

from uac2_stream_check import Tone, find_board
from smi_check import smi_stats
import smi_pll_check as q

RATES = (44100, 48000, 96000)


def counters(dev):
    st = smi_stats(dev)
    s = st["s"]
    return dict(late=st["late"], underrun_frames=s["slot0_underrun_frames"], ring_drops=s["ring_drops"],
                usb_underruns=s["underruns"], wd_events=st.get("pll_wd_events"),
                fallbacks=st.get("pll_wd_fallbacks"), source=st.get("smi_source"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=int, default=60)
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    bad = 0
    start = counters(dev)
    for m in range(args.minutes):
        rate = RATES[m % 3]
        tone = Tone("DSPi Zero", rate=rate, set_device_rate=True)
        t0 = time.monotonic()
        try:
            tone.start()
            time.sleep(5.0)
            a = counters(dev)
            time.sleep(48.0)
            b = counters(dev)
        finally:
            tone.stop()
        d = {k: b[k] - a[k] for k in ("late", "underrun_frames", "ring_drops", "usb_underruns", "wd_events")}
        hz = q.p.measure(dev, q.p.MUX_SMI, min(40, int(2**24 / (rate * 768 * 0.01))))[0]
        err = (hz - rate * 768) / (rate * 768) * 1e6
        ok = not any(d.values()) and b["source"] == 1 and abs(err) < 0.1
        bad += not ok
        print(f"{m + 1:3d} {rate:6d} Hz: {d}, source {'PLLA' if b['source'] == 1 else 'PLLD'}, SMI clock "
              f"{err:+.3f} ppm {'ok' if ok else 'BAD'}", flush=True)
        time.sleep(max(0.0, 60.0 - (time.monotonic() - t0)))
    end = counters(dev)
    print(f"over the soak: watchdog events {end['wd_events'] - start['wd_events']}, fallbacks "
          f"{end['fallbacks'] - start['fallbacks']}, {bad} bad minutes of {args.minutes}")
    print("PASS" if bad == 0 and end["wd_events"] == start["wd_events"] else "FAIL")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
