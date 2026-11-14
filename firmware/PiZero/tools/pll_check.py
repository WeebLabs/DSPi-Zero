#!/usr/bin/env python3
"""PLLA phase 1 (PiZero_PLLA_plan.md): retune PLLA in place through bench request 0x46, drive the unused PCM
clock from plla_per with an integer divider, and read it back with the clock manager's frequency counter.
Nothing audible uses PLLA in this phase; the SMI stays on PLLD.

Usage: python3.14 firmware/PiZero/tools/pll_check.py [--rows a48,a44,b48,b44,c48] [--leave a48] [--soak SECONDS]
--leave picks the row left running at the end (default: the firmware's values). --soak then checks the
registers, lock bit and counter once a second for that long.
"""
import argparse
import struct
import sys
import time

from uac2_stream_check import find_board

XOSC = 19.2e6
MUX_PCM, MUX_SMI = 23, 27
SRC_PLLD, SRC_PLLA = 6, 4
FIELDS = ("magic row state retunes lock_us unlock_first_us unlock_reads lock_reads lock_lost count count_mux "
          "count_osc count_seq count_state count_us ctrl frac ana1 per core cm_plla cm_lock v3d_ctl v3d_div "
          "pcm_ctl pcm_div fw_ctrl fw_frac fw_per fw_cm_plla smi_ctl smi_div in_use refused trim_ppb trims wd_events wd_restores wd_fallbacks wd_ctrl wd_frac wd_per "
          "wd_cm gp0_ctl gp0_div gpfsel0 gpfsel1").split()
# name: (row index, NDIV, FRAC, PER divider, PCM divider giving 768 fs at 48 or 44.1 kHz)
ROWS = {"fw": (0, None, None, None, None), "a48": (1, 61, 0x70A3D, 8, 8), "a44": (2, 56, 0x72B02, 8, 8),
        "b48": (3, 48, 0, 5, 10), "b44": (4, 44, 0x1999A, 5, 10), "c48": (5, 57, 0x9999A, 5, 12)}
STATES = ["idle", "waiting", "locked", "TIMEOUT"]


def status(dev):
    raw = bytes(dev.ctrl_transfer(0xC0, 0x46, 0, 0, 4 * len(FIELDS), timeout=1000))
    s = dict(zip(FIELDS, struct.unpack("<%dI" % len(FIELDS), raw)))
    if s["magic"] != 0x4F4C4C50:
        raise SystemExit("FAIL: no PLL status (load the phase 1 pizero_z4.img)")
    return s


def vco(s):
    ndiv, frac = s["ctrl"] & 0x3FF, s["frac"] & 0xFFFFF
    return 2 * XOSC * (ndiv + frac / 2**20)


def retune(dev, row):
    dev.ctrl_transfer(0x40, 0x46, 1, row, None, timeout=1000)
    t0 = time.time()
    while True:
        s = status(dev)
        if s["retunes"] and s["row"] == row and s["state"] in (2, 3):
            return s
        if time.time() - t0 > 2:
            return s
        time.sleep(0.02)


def pcm_clock(dev, src, div):
    dev.ctrl_transfer(0x40, 0x46, 2, (src << 12) | div, None, timeout=1000)


def measure(dev, mux, gates=10):
    """Returns (Hz, raw count, oscillator cycles). The gate is gates x 10 ms of the 19.2 MHz crystal."""
    seq = status(dev)["count_seq"]
    dev.ctrl_transfer(0x40, 0x46, 3, (gates << 8) | mux, None, timeout=1000)
    t0 = time.time()
    while time.time() - t0 < gates * 0.01 + 1:
        time.sleep(gates * 0.01 + 0.005)
        s = status(dev)
        if s["count_seq"] != seq:
            return s["count"] * XOSC / s["count_osc"], s["count"], s["count_osc"]
        if s["count_state"] == 3:
            break
    return 0.0, 0, 0


def check_counter(dev):
    """PLLD is exactly 2000 MHz, so PCM from plld_per /20 must read 25 MHz to the count."""
    pcm_clock(dev, SRC_PLLD, 20)
    ok = True
    for gates in (1, 10, 40):
        hz, n, osc = measure(dev, MUX_PCM, gates)
        want = 25e6 * osc / XOSC
        good = n and abs(n - want) <= 1
        ok &= bool(good)
        print(f"  counter check, PCM from PLLD /20, gate {osc / XOSC * 1e3:5.0f} ms: count {n} (want {want:.0f}) "
              f"{'ok' if good else 'MISMATCH'}")
    hz, n, osc = measure(dev, MUX_SMI, 10)
    print(f"  SMI clock now (PLLD, fractional with MASH): {hz / 1e6:.6f} MHz")
    return ok


def test_row(dev, name):
    idx, ndiv, frac, per, pcm_div = ROWS[name]
    before = status(dev)
    s = retune(dev, idx)
    first = "never" if s["unlock_first_us"] == 0xFFFFFFFF else f"{s['unlock_first_us']} us"
    print(f"== {name}: {STATES[s['state']]}, lock at {s['lock_us']} us, FLOCKA first clear {first}, "
          f"clear in {s['unlock_reads']} of {s['lock_reads']} reads")
    print(f"  VCO {vco(s) / 1e6:.4f} MHz (NDIV {s['ctrl'] & 0x3FF}, FRAC {s['frac']:#07x}), PER {s['per']:#05x}, "
          f"CM_PLLA {s['cm_plla']:#05x}, CORE {s['core']:#05x}, V3D CTL/DIV {s['v3d_ctl']:#x}/{s['v3d_div']:#x}")
    ok = s["state"] == 2
    if (s["core"], s["v3d_ctl"], s["v3d_div"]) != (before["core"], before["v3d_ctl"], before["v3d_div"]):
        print("  CHANGED: PLLA CORE or V3D registers")
        ok = False
    if ndiv is None:
        return ok
    pcm_clock(dev, SRC_PLLA, pcm_div)
    want_pcm = 2 * XOSC * (ndiv + frac / 2**20) / per / pcm_div
    for gates in (10, 40):
        hz, n, osc = measure(dev, MUX_PCM, gates)
        want = want_pcm * osc / XOSC
        good = n and abs(n - want) <= 1.5
        ok &= bool(good)
        print(f"  PCM from plla_per /{pcm_div}: {hz / 1e6:.6f} MHz, count {n} (want {want:.1f}, "
              f"{(n - want) / want * 1e6 if n else 0:+.3f} ppm) {'ok' if good else 'MISMATCH'}")
    return ok


def soak(dev, name, seconds):
    idx, ndiv, frac, per, pcm_div = ROWS[name]
    ref = status(dev)
    keys = ("ctrl", "frac", "per", "core", "cm_plla", "v3d_ctl", "v3d_div")
    want_pcm = 2 * XOSC * (ndiv + frac / 2**20) / per / pcm_div if ndiv else 0
    t0, bad, worst = time.time(), 0, 0.0
    while time.time() - t0 < seconds:
        s = status(dev)
        changed = [k for k in keys if s[k] != ref[k]]
        hz = measure(dev, MUX_PCM, 10)[0] if ndiv else 0
        err = (hz - want_pcm) / want_pcm * 1e6 if ndiv else 0.0
        worst = max(worst, abs(err))
        if changed or s["lock_lost"] != ref["lock_lost"] or not s["cm_lock"] & 0x100:
            bad += 1
            print(f"  {time.time() - t0:6.0f} s: changed {changed}, lock_lost {s['lock_lost']}, "
                  f"CM_LOCK {s['cm_lock']:#x}", flush=True)
        if int(time.time() - t0) % 60 == 0:
            print(f"  {time.time() - t0:6.0f} s: PCM {hz / 1e6:.6f} MHz ({err:+.3f} ppm), {bad} bad", flush=True)
        time.sleep(max(0.0, 1 - 0.12))
    print(f"== soak {seconds:.0f} s on {name}: {bad} bad samples, worst counter error {worst:.3f} ppm")
    return bad == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", default="a48,a44,b48,b44,c48")
    ap.add_argument("--leave", default="fw")
    ap.add_argument("--soak", type=float, default=0)
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    ok = check_counter(dev)
    for name in args.rows.split(","):
        ok &= test_row(dev, name)
    ok &= test_row(dev, args.leave)
    if args.soak:
        ok &= soak(dev, args.leave, args.soak)
    if args.leave == "fw":
        pcm_clock(dev, 0, 0)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
