#!/usr/bin/env python3
"""S/PDIF input on the PCM block (spdif_rx_pi.c): DSPi's spdif_input.c, compiled unchanged, on a receiver
that samples GPIO 20 at 125 MHz and decodes in software.

Usage: python3.14 firmware/PiZero/tools/spdif_in_check.py [--monitor | --source [--watch S]] [--rates ...]

With no wiring: switches DSPi's input to S/PDIF with REQ_SET_INPUT_SOURCE and checks that the receiver
captures at 125 MHz, finds no signal, and costs little; then switches back to USB and checks a tone plays
cleanly. --monitor needs a jumper from GPIO 8 (header pin 24, slot 1's S/PDIF out) to GPIO 20 (header pin
38): it runs the receiver as a bench monitor on the board's own output while a tone plays straight to the
Pi (the Mac's default output is not changed), and checks lock, rate, parity and the decoded samples.
--source needs a real S/PDIF source on GPIO 20 (through a 3.3 V optical receiver): it switches DSPi's
input to S/PDIF and checks lock, rate, parity, the decoded audio, DSPi's clock tracking (the PLLA trim)
and the output stream, then switches back to USB.
"""
import argparse
import struct
import sys
import time

from uac2_stream_check import Tone, find_board
from smi_check import VENDOR_IFACE, get_type, set_type, smi_stats
import pll_check as p

REQ_SET_INPUT_SOURCE, REQ_GET_INPUT_SOURCE, REQ_GET_SPDIF_RX_STATUS = 0xE0, 0xE1, 0xE2
FIELDS = ("magic running state dma_channel samples_lo samples_hi subframes decode_errors cell_q8 blocks samp_freq "
          "samp_freq_actual_mhz parity_errors fifo_used irqs decode_us pcm_cs dma_debug monitor").split()
LIB_STATES = ("no signal", "waiting stable", "STABLE")
DSPI_STATES = ("inactive", "acquiring", "locked", "relocking")


def rx(dev):
    raw = bytes(dev.ctrl_transfer(0xC0, 0x44, 0, 0, 4 * (len(FIELDS) + 8), timeout=1000))
    v = struct.unpack_from("<%dI" % len(FIELDS), raw)
    s = dict(zip(FIELDS, v))
    if s["magic"] != 0x58524453:
        raise SystemExit("FAIL: no S/PDIF receiver status (load the current pizero_z4.img)")
    s["samples"] = s["samples_lo"] | s["samples_hi"] << 32
    s["last"] = struct.unpack_from("<8I", raw, 4 * len(FIELDS))
    return s


def dspi_rx(dev):
    raw = bytes(dev.ctrl_transfer(0xC1, REQ_GET_SPDIF_RX_STATUS, 0, VENDOR_IFACE, 16))
    return dict(state=raw[0], input_source=raw[1], lock_count=raw[2], loss_count=raw[3])


def set_source(dev, src):
    dev.ctrl_transfer(0x41, REQ_SET_INPUT_SOURCE, 0, VENDOR_IFACE, bytes([src]))


def get_source(dev):
    return dev.ctrl_transfer(0xC1, REQ_GET_INPUT_SOURCE, 0, VENDOR_IFACE, 1)[0]


def rates(a, b):
    dt = (b["decode_us"] - a["decode_us"]) & 0xFFFFFFFF
    span = (b["samples"] - a["samples"]) / 125e6
    return (b["samples"] - a["samples"]), ((b["irqs"] - a["irqs"]) & 0xFFFFFFFF), dt, span


def no_signal(dev, failures):
    print("== DSPi input on S/PDIF, nothing connected")
    set_source(dev, 1)
    time.sleep(1.5)
    if get_source(dev) != 1:
        failures.append("the input did not switch to S/PDIF")
        return
    a = rx(dev)
    t0 = time.monotonic()
    time.sleep(2.0)
    b = rx(dev)
    wall = time.monotonic() - t0
    n, irqs, dus, span = rates(a, b)
    d = dspi_rx(dev)
    hz = n / wall
    print(f"  capture {hz / 1e6:.2f} MHz of samples (wall clock), {irqs / wall:.0f} chunk IRQs/s, decode "
          f"{dus / wall / 1e4:.2f}% of the CPU, receiver {LIB_STATES[b['state']]}, DSPi "
          f"{DSPI_STATES[d['state']]}, subframes {b['subframes']}, PCM CS {b['pcm_cs']:#x}, DMA debug "
          f"{b['dma_debug']}")
    if not b["running"] or abs(hz - 125e6) > 125e6 * 0.01 or b["dma_debug"]:
        failures.append(f"capture {hz:.0f} samples/s, running {b['running']}, DMA debug {b['dma_debug']}")
    if b["state"] != 0 or d["state"] != 1:
        failures.append(f"with no signal: receiver {b['state']}, DSPi {d['state']}")
    set_source(dev, 0)
    time.sleep(1.5)
    b = rx(dev)
    print(f"  back on USB: receiver {'stopped' if not b['running'] else 'STILL RUNNING'}, input {get_source(dev)}")
    if b["running"] or get_source(dev) != 0:
        failures.append("the receiver did not stop on the switch back to USB")


def usb_tone(dev, failures):
    tone = Tone("DSPi Zero", rate=48000, set_device_rate=True)
    try:
        tone.start()
        time.sleep(2.0)
        a = smi_stats(dev)
        time.sleep(5.0)
        b = smi_stats(dev)
    finally:
        tone.stop()
    d = dict(late=b["late"] - a["late"], underrun_frames=b["s"]["slot0_underrun_frames"] - a["s"]["slot0_underrun_frames"],
             usb_underruns=b["s"]["underruns"] - a["s"]["underruns"], peak=b["s"]["peaks"][8])
    print(f"  USB tone afterwards: {d}")
    if d["late"] or d["underrun_frames"] or d["usb_underruns"] or d["peak"] == 0:
        failures.append(f"USB playback after S/PDIF: {d}")


def monitor(dev, rate_list, failures):
    old0 = get_type(dev, 0)
    set_type(dev, 0, 0)
    time.sleep(0.5)
    try:
        for rate in rate_list:
            print(f"== monitor at {rate} Hz (jumper GPIO 8 to GPIO 20)")
            tone = Tone("DSPi Zero", rate=rate, set_device_rate=True)
            try:
                tone.start()
                time.sleep(2.0)
                dev.ctrl_transfer(0x40, 0x44, 1, 0, None, timeout=1000)
                time.sleep(1.5)
                a = rx(dev)
                t0 = time.monotonic()
                time.sleep(3.0)
                b = rx(dev)
                wall = time.monotonic() - t0
            finally:
                tone.stop()
                dev.ctrl_transfer(0x40, 0x44, 0, 0, None, timeout=1000)
            n, irqs, dus, span = rates(a, b)
            subs = (b["subframes"] - a["subframes"]) / span
            errs = b["decode_errors"] - a["decode_errors"]
            par = b["parity_errors"] - a["parity_errors"]
            words = b["last"]
            syncs = "".join({0xF: "B", 0xB: "M", 0x7: "W"}.get(w & 0xF, "?") for w in words)
            samples = [((w >> 4) & 0xFFFFFF) for w in words]
            samples = [(s ^ 0x800000) - 0x800000 for s in samples]
            print(f"  receiver {LIB_STATES[b['state']]}, rate {b['samp_freq']} ({b['samp_freq_actual_mhz'] / 1000:.2f} "
                  f"measured), cell {b['cell_q8'] / 256:.2f} samples, {subs:.0f} subframes/s, decode errors +{errs}, "
                  f"parity errors +{par}, decode {dus / wall / 1e4:.2f}% of the CPU")
            print(f"  newest subframes {syncs}: {samples}")
            ok = (b["state"] == 2 and b["samp_freq"] == rate and errs == 0 and par == 0
                  and abs(subs - 2 * rate) < 2 * rate * 0.001 and any(samples))
            print("  ok" if ok else "  BAD")
            if not ok:
                failures.append(f"monitor at {rate}")
    finally:
        set_type(dev, 0, old0)


def real_source(dev, watch, failures):
    print("== DSPi input on S/PDIF from the source on GPIO 20")
    set_source(dev, 1)
    t0 = time.monotonic()
    b = rx(dev)
    while time.monotonic() - t0 < 5.0 and dspi_rx(dev)["state"] != 2:
        time.sleep(0.1)
    d = dspi_rx(dev)
    b = rx(dev)
    print(f"  after {time.monotonic() - t0:.1f} s: receiver {LIB_STATES[b['state']]}, DSPi {DSPI_STATES[d['state']]}, "
          f"rate {b['samp_freq']} ({b['samp_freq_actual_mhz'] / 1000:.2f} measured), cell {b['cell_q8'] / 256:.2f} "
          f"samples, subframes {b['subframes']}, decode errors {b['decode_errors']}")
    if d["state"] != 2:
        failures.append(f"no lock: receiver {b['state']}, DSPi {d['state']}, subframes {b['subframes']}")
        set_source(dev, 0)
        return
    time.sleep(2.0)
    a, sa, t1 = rx(dev), smi_stats(dev), time.monotonic()
    end = t1 + watch
    while True:
        time.sleep(min(5.0, max(0.0, end - time.monotonic())))
        c, sc, now = rx(dev), smi_stats(dev), time.monotonic()
        ps = p.status(dev)
        n, irqs, dus, span = rates(a, c)
        s0, s1 = sa["s"], sc["s"]
        words = c["last"]
        samples = [(((w >> 4) & 0xFFFFFF) ^ 0x800000) - 0x800000 for w in words]
        print(f"  {now - t1:5.1f} s: {DSPI_STATES[dspi_rx(dev)['state']]}, {(c['subframes'] - a['subframes']) / span:.0f} "
              f"subframes/s, decode errors +{c['decode_errors'] - a['decode_errors']}, parity +"
              f"{c['parity_errors'] - a['parity_errors']}, decode {dus / (now - t1) / 1e4:.2f}% CPU, PLLA trim "
              f"{(ps['trim_ppb'] ^ 0x80000000) - 0x80000000:+d} ppb, slot 0 fill {s1['slot0_fill']} (target {s1['ring_target']}), underrun "
              f"frames +{s1['slot0_underrun_frames'] - s0['slot0_underrun_frames']}, composer late +{sc['late'] - sa['late']}, "
              f"peaks in {s1['peaks'][:2]} out {s1['peaks'][8:10]}", flush=True)
        if now >= end:
            break
    print(f"  newest samples {samples}")
    if c["decode_errors"] != a["decode_errors"] or c["parity_errors"] != a["parity_errors"]:
        failures.append("decode or parity errors while locked")
    if s1["slot0_underrun_frames"] != s0["slot0_underrun_frames"] or sc["late"] != sa["late"]:
        failures.append("output underruns while locked")
    if dspi_rx(dev)["state"] != 2:
        failures.append("lost lock")
    set_source(dev, 0)
    time.sleep(1.5)
    print(f"  back on USB: receiver {'stopped' if not rx(dev)['running'] else 'STILL RUNNING'}, PLLA trim "
          f"{(p.status(dev)['trim_ppb'] ^ 0x80000000) - 0x80000000:+d} ppb")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--monitor", action="store_true", help="decode slot 1's output through a GPIO 8 to 20 jumper")
    ap.add_argument("--rates", default="48000,44100,96000")
    ap.add_argument("--source", action="store_true", help="a real S/PDIF source is on GPIO 20")
    ap.add_argument("--watch", type=float, default=30, help="seconds to watch a locked source")
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    failures = []
    if get_source(dev) != 0:
        set_source(dev, 0)
        time.sleep(1.0)
    if args.source:
        real_source(dev, args.watch, failures)
    elif args.monitor:
        monitor(dev, [int(r) for r in args.rates.split(",")], failures)
    else:
        no_signal(dev, failures)
        usb_tone(dev, failures)
    if failures:
        print("FAIL:", "; ".join(failures))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
