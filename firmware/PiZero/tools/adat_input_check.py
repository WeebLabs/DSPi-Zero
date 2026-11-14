#!/usr/bin/env python3
"""DSPi's ADAT input on the Pi (z4/adat_input_pi.c, bench request 0x52), on its own ADAT output looped back.

Needs a jumper from GPIO 16 (header pin 36, the ADAT output) to GPIO 20 (header pin 38, the PCM block's
input). DSPi's ADAT output carries its signal generator at 997 Hz on all eight channels, and the input is
switched to ADAT with DSPi's own vendor requests. At each rate it runs:

  master  ADAT clock mode master: DSPi's rate is the device rate, as for I2S master mode.
  slave   then a live switch to slave mode: DSPi finds the wire's rate itself and servos to it.

The bench's frame reader checks every frame's header and every sample of all eight channels against
the sine. A run passes only with no header error, no lock loss, no wrong sample, no publishing overflow
and no output underrun, after the prefill handshake has unmuted the outputs.

Input source, ADAT output and input settings, the input rate and output enables are put back.

Usage: python3.14 firmware/PiZero/tools/adat_input_check.py [--rates 48000,44100] [--modes master,slave]
           [--seconds 20]
"""
import argparse
import math
import struct
import time

from uac2_stream_check import Tone, find_board
from smi_check import smi_stats
from cpu_check import meter
from i2s_input_check import (VENDOR_IFACE, SRC_USB, set_source, get_source, set_rate, get_rate, peaks,
                             set_output_enable, get_output_enable, siggen, wait_for)

SRC_ADAT = 3
REQ_SET_ADAT_INPUT_ENABLE, REQ_GET_ADAT_INPUT_ENABLE = 0x68, 0x69
REQ_SET_ADAT_INPUT_PIN, REQ_GET_ADAT_INPUT_PIN = 0x6A, 0x6B
REQ_SET_ADAT_INPUT_CLOCK_MODE, REQ_GET_ADAT_INPUT_CLOCK_MODE = 0x6C, 0x6D
REQ_GET_ADAT_INPUT_STATUS = 0x6E
REQ_SET_ADAT_ENABLE, REQ_GET_ADAT_ENABLE, REQ_GET_ADAT_PIN = 0xCA, 0xCB, 0xCD
STATES = {0: "inactive", 1: "acquiring", 2: "syncing", 3: "locked", 4: "relocking"}

FIELDS = ("magic running rate chunks samples_lo samples_hi words published overflows flips dec_lo dec_hi "
          "chunk_cycles_max headroom_min frames header_errors syncs sine_checked sine_errors sine_worst "
          "p0 p1 p2 p3 p4 p5 p6 p7 inst l1d l2d mispred").split()


def req(dev, code, value=0, n=1):
    return bytes(dev.ctrl_transfer(0xC1, code, value, VENDOR_IFACE, n, timeout=1000))


def status(dev):
    raw = bytes(dev.ctrl_transfer(0xC0, 0x52, 0, 0, 4 * len(FIELDS), timeout=1000))
    v = struct.unpack_from(f"<{len(FIELDS)}I", raw)
    s = dict(zip(FIELDS, v))
    if s["magic"] != 0x50494441:
        raise SystemExit("FAIL: no ADAT receiver in this image (load the current pizero_z4.img)")
    s["headroom_min"] -= (1 << 32) if s["headroom_min"] >= 1 << 31 else 0
    s["samples"] = s["samples_lo"] | s["samples_hi"] << 32
    s["dec"] = s["dec_lo"] | s["dec_hi"] << 32
    return s


def bench(dev, rate=48000, hz=997.0, tol=16, on=True):
    if not on:
        dev.ctrl_transfer(0x40, 0x52, 0, 0, None, timeout=1000)
        return
    c = int(round(2 * math.cos(2 * math.pi * hz / rate) * (1 << 30)))
    dev.ctrl_transfer(0x40, 0x52, 0, 0, struct.pack("<B3xiI", 0xFF, c, tol), timeout=1000)


def dspi_status(dev):
    state, mode, en, pin, rate_ok, locks, losses, slips, herr, _, rate, measured = struct.unpack(
        "<8BHHII", req(dev, REQ_GET_ADAT_INPUT_STATUS, 0, 20))
    return dict(state=STATES.get(state, state), mode=mode, enabled=en, pin=pin, rate_ok=rate_ok, locks=locks,
                losses=losses, slips=slips, header_err=herr, rate=rate, measured=measured)


def set_mode(dev, mode):
    return req(dev, REQ_SET_ADAT_INPUT_CLOCK_MODE, mode)[0]


def watch(dev, seconds, rate):
    """Counters over the run. Any header error, lock loss, wrong sample, overflow or underrun fails it."""
    a, da, ta, ma, oa = status(dev), dspi_status(dev), time.monotonic(), meter(dev), smi_stats(dev)
    time.sleep(2)
    m = status(dev)     # the PMU's event counts are 32 bits, so they are taken over these 2 s
    time.sleep(seconds - 2)
    b, db, tb, mb, ob = status(dev), dspi_status(dev), time.monotonic(), meter(dev), smi_stats(dev)
    d = {k: b[k] - a[k] for k in ("frames", "header_errors", "syncs", "sine_checked", "sine_errors", "overflows",
                                  "flips", "chunks", "dec", "samples", "inst", "l1d", "l2d", "mispred")}
    d.update({k: (m[k] - a[k]) % (1 << 32) for k in ("inst", "l1d", "l2d", "mispred")})
    fs = d["frames"] / (tb - ta)
    late = ob["late"] - oa["late"]
    underrun = ob["s"]["slot0_underrun_frames"] - oa["s"]["slot0_underrun_frames"]
    print(f"   {fs:.1f} frames/s, {d['header_errors']} header errors, {d['syncs']} resyncs, "
          f"{d['sine_checked']} samples checked, {d['sine_errors']} wrong (worst residual {b['sine_worst']}), "
          f"{d['flips']} grid flips, {d['overflows']} overflows, headroom at least "
          f"{b['headroom_min'] / 125:.0f} us; since the bench request {b['syncs']} syncs, {b['header_errors']} header "
          f"errors, {b['overflows']} overflows, worst residual {b['sine_worst']}")
    print(f"   DSPi: {db['state']}, rate {db['rate']}, measured {db['measured']} Hz, locks +{db['locks'] - da['locks']}, "
          f"losses +{db['losses'] - da['losses']}, header errors +{(db['header_err'] - da['header_err']) & 0xFFFF}")
    print(f"   outputs: composer late +{late}, slot 0 underrun frames +{underrun}")
    kc = d["dec"] / max(d["chunks"], 1) / 1000.0
    print(f"   decoding {d['dec'] / (tb - ta) / 1e7:.2f}% of the 1 GHz core ({kc:.0f} k cycles a chunk, worst "
          f"{b['chunk_cycles_max'] / 1000:.0f} k); whole core {mb['load']:.2f}%")
    ch = max(m["chunks"] - a["chunks"], 1)
    print(f"   a chunk: {d['inst'] / ch / 1000:.0f} k instructions, {d['l1d'] / ch:.0f} L1 and {d['l2d'] / ch:.0f} L2 "
          f"refills, {d['mispred'] / ch:.0f} branch mispredictions")
    ok = (abs(fs - rate) < rate * 0.002 and d["header_errors"] == 0 and d["syncs"] == 0 and d["sine_errors"] == 0
          and d["overflows"] == 0 and late == 0 and underrun == 0 and db["state"] == "locked"
          and db["losses"] == da["losses"] and db["header_err"] == da["header_err"] and b["headroom_min"] > 0)
    ok &= d["sine_checked"] > 0.98 * 8 * rate * seconds
    return ok


def locked_and_live(dev, rate, what):
    """DSPi locked at `rate` and the prefill handshake has unmuted the outputs."""
    ok = wait_for(lambda: dspi_status(dev)["state"] == "locked" and dspi_status(dev)["rate"] == rate, 10,
                  f"DSPi's ADAT input locked at {rate} Hz ({what})")
    ok &= wait_for(lambda: peaks(dev)[1][0] > 0, 5, "the outputs unmuted by the prefill handshake")
    pin, pout = peaks(dev)
    print(f"   {what}: DSPi {dspi_status(dev)}; peaks, inputs {pin}, outputs {pout}")
    return ok


def set_device_rate(rate):
    """The Pi's USB rate, set by playing to it directly for a moment (the Mac's default output is not
    changed). The pipeline follows it while the input is USB. Starting the loopback at its own rate
    matters: a rate change on the switch to ADAT holds the outputs, the ADAT output among them, until the
    input locks, and in a loopback the input can only lock to that output."""
    tone = Tone("DSPi Zero", rate=rate, set_device_rate=True)
    tone.start()
    time.sleep(1.5)
    tone.stop()


def run_rate(dev, rate, modes, seconds):
    ok = True
    print(f"== {rate} Hz")
    set_mode(dev, 0)
    set_device_rate(rate)
    set_rate(dev, rate)
    set_source(dev, SRC_ADAT)
    t0 = time.monotonic()
    ok &= locked_and_live(dev, rate, f"master mode, {time.monotonic() - t0:.1f} s after the switch")
    if "master" in modes:
        bench(dev, rate)
        time.sleep(0.2)
        r = watch(dev, seconds, rate)
        print(f"   master at {rate} Hz: {'ok' if r else 'FAIL'}")
        ok &= r
    if "slave" in modes:
        set_mode(dev, 1)
        time.sleep(0.5)
        ok &= locked_and_live(dev, rate, "slave mode")
        time.sleep(1)
        bench(dev, rate)
        time.sleep(0.2)
        r = watch(dev, seconds, rate)
        st = dspi_status(dev)
        r &= abs(st["measured"] - rate) <= 10     # DSPi's 32 ms window, a word in 12288
        print(f"   slave at {rate} Hz: {'ok' if r else 'FAIL'}")
        ok &= r
        set_mode(dev, 0)
    set_source(dev, SRC_USB)
    time.sleep(1)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rates", default="48000,44100")
    ap.add_argument("--modes", default="master,slave")
    ap.add_argument("--seconds", type=int, default=20)
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        raise SystemExit("FAIL: no DSPi Zero")
    status(dev)
    old = dict(source=get_source(dev), rate=get_rate(dev), adat_out=req(dev, REQ_GET_ADAT_ENABLE)[0],
               adat_in=req(dev, REQ_GET_ADAT_INPUT_ENABLE)[0], pin=req(dev, REQ_GET_ADAT_INPUT_PIN)[0],
               mode=req(dev, REQ_GET_ADAT_INPUT_CLOCK_MODE)[0], enables=[get_output_enable(dev, o) for o in range(8)])
    results = {}
    try:
        set_source(dev, SRC_USB)
        time.sleep(0.5)
        st = req(dev, REQ_SET_ADAT_ENABLE, 1)[0]
        # The Pi reads ADAT on GPIO 20, the PCM block's input, which it shares with S/PDIF input 1.
        in_pin = 20
        st_pin = req(dev, REQ_SET_ADAT_INPUT_PIN, in_pin)[0]
        st_en = req(dev, REQ_SET_ADAT_INPUT_ENABLE, 1)[0]
        print(f"ADAT output enable {st}, input pin {in_pin} {st_pin}, input enable {st_en} (0 is success)")
        for o in range(8):
            set_output_enable(dev, o, 1)
        siggen(dev, True)
        for rate in (int(r) for r in args.rates.split(",")):
            results[rate] = run_rate(dev, rate, args.modes.split(","), args.seconds)
    finally:
        siggen(dev, False)
        bench(dev, on=False)
        set_source(dev, SRC_USB)
        time.sleep(0.5)
        set_mode(dev, old["mode"])
        req(dev, REQ_SET_ADAT_INPUT_ENABLE, old["adat_in"])
        req(dev, REQ_SET_ADAT_INPUT_PIN, old["pin"])     # 0xFF clears it, as it was
        req(dev, REQ_SET_ADAT_ENABLE, old["adat_out"])
        set_rate(dev, old["rate"])
        for o in range(8):
            set_output_enable(dev, o, old["enables"][o])
        set_source(dev, old["source"])
    for rate, ok in results.items():
        print(f"{rate} Hz: {'PASS' if ok else 'FAIL'}")
    return 0 if results and all(results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
