#!/usr/bin/env python3
"""ADAT output on the SMI: DSPi's ADAT stream on SD8 (GPIO 16), one bit per SMI word at 256 fs.

Usage: python3.14 firmware/PiZero/tools/adat_check.py [--rates 48000,44100,96000] [--watch SECONDS]
Puts slot 0 on I2S, enables ADAT with DSPi's REQ_SET_ADAT_ENABLE and plays a 1 kHz tone straight to the Pi
(the Mac's default output is not changed). From composed SMI frames it decodes every ADAT frame (sync, the
forced 1 bits, eight 24-bit channels) and slot 0's I2S left word. DSPi holds ADAT a fixed 96 frames behind
slot 0, which at 48 kHz is exactly two periods of the tone, so ADAT channel 1 must equal the I2S word frame
for frame; at 44.1 kHz the lag is read from the phase. At 96 kHz DSPi stops ADAT and the pin must be idle.
Slot types and the ADAT setting are put back afterwards.
"""
import argparse
import math
import struct
import sys
import time

from uac2_stream_check import Tone, find_board
from smi_check import VENDOR_IFACE, get_type, set_type, smi_stats, snapshot
import pll_check as p

REQ_SET_ADAT_ENABLE, REQ_GET_ADAT_ENABLE, REQ_SET_ADAT_PIN, REQ_GET_ADAT_STATUS = 0xCA, 0xCB, 0xCC, 0xCE
ADAT_BIT = 8
LEAD = 96


def adat_status(dev):
    raw = bytes(dev.ctrl_transfer(0xC1, REQ_GET_ADAT_STATUS, 0, VENDOR_IFACE, 8))
    en, active, pin, rate_ok, resyncs, slips = struct.unpack("<4B2H", raw)
    return dict(enabled=en, active=active, pin=pin, rate_ok=rate_ok, resyncs=resyncs, slips=slips)


def gpio16(dev):
    """GPIO 16's function: 5 (ALT1) while the SMI drives it, 0 as an input."""
    return (p.status(dev)["gpfsel1"] >> 18) & 7


def decode_adat(words, frames):
    """Per SMI frame (from the second): (sync ok, user bits, forced 1 bits ok, 8 signed samples) or None."""
    out = []
    for f in range(1, frames):
        base = f * 256
        bits = [(words[base + j] >> ADAT_BIT & 1) ^ (words[base + j - 1] >> ADAT_BIT & 1) for j in range(256)]
        sync = bits[0] == 1 and not any(bits[1:11]) and bits[11] == 1
        user = bits[12:16]
        forced, samples, i = True, [], 16
        for ch in range(8):
            v = 0
            for g in range(6):
                forced &= bits[i] == 1
                for b in bits[i + 1:i + 5]:
                    v = v << 1 | b
                i += 5
            samples.append((v ^ 0x800000) - 0x800000)
        out.append((sync, user, forced, samples))
    return out


def i2s_left(words, frames, bit=0):
    """Slot `bit`'s left word per SMI frame: bit clocks 1 to 32, sampled while BCK is high (word 4k + 2)."""
    out = []
    for f in range(1, frames):
        v = 0
        for k in range(1, 33):
            v = v << 1 | (words[f * 256 + 4 * k + 2] >> bit & 1)
        out.append(((v ^ 0x80000000) - 0x80000000) >> 8)
    return out


def phase(xs, w):
    """Least-squares fit of a sin(wn) + b cos(wn): returns the phase and amplitude."""
    s = [math.sin(w * n) for n in range(len(xs))]
    c = [math.cos(w * n) for n in range(len(xs))]
    ss, cc, sc = sum(v * v for v in s), sum(v * v for v in c), sum(u * v for u, v in zip(s, c))
    xs_, xc = sum(x * v for x, v in zip(xs, s)), sum(x * v for x, v in zip(xs, c))
    det = ss * cc - sc * sc
    a, b = (xs_ * cc - xc * sc) / det, (xc * ss - xs_ * sc) / det
    return math.atan2(b, a), math.hypot(a, b)


def check_rate(dev, rate, watch, failures):
    print(f"== {rate} Hz")
    tone = Tone("DSPi Zero", rate=rate, set_device_rate=True)
    try:
        tone.start()
        time.sleep(3.0)
        st = adat_status(dev)
        a = smi_stats(dev)
        first, words = snapshot(dev, 16)
        if rate > 48000:
            idle = not any(w >> ADAT_BIT & 1 for w in words)
            fn = gpio16(dev)
            ok = not st["active"] and not st["rate_ok"] and idle and fn == 5
            print(f"  DSPi ADAT status {st}, pin {'held low' if idle and fn == 5 else 'NOT HELD LOW'} {'ok' if ok else 'BAD'}")
            if not ok:
                failures.append(f"{rate}: ADAT should be stopped, {st}")
            return
        frames = decode_adat(words, 16)
        left = i2s_left(words, 16)
        sync_ok = all(fr[0] for fr in frames)
        forced_ok = all(fr[2] for fr in frames)
        ch1 = [fr[3][0] for fr in frames]
        ch2 = [fr[3][1] for fr in frames]
        others = {i: max(abs(fr[3][i]) for fr in frames) for i in range(2, 8)}
        print(f"  DSPi ADAT status {st}")
        print(f"  {len(frames)} ADAT frames from SMI frame {first + 1}: sync {'ok' if sync_ok else 'BAD'}, forced "
              f"bits {'ok' if forced_ok else 'BAD'}, user bits {frames[0][1]}")
        print(f"  channel 1 {ch1[:6]}, channel 2 {ch2[:6]}, channels 3-8 peak {list(others.values())}")
        print(f"  slot 0 I2S left {left[:6]}")
        if gpio16(dev) != 5:
            failures.append(f"{rate}: GPIO 16 is not on the SMI")
        if not (st["active"] and sync_ok and forced_ok):
            failures.append(f"{rate}: ADAT frames do not decode, {st}")
        w = 2 * math.pi * 1000 / rate
        pa, amp_a = phase(ch1, w)
        pi_, amp_i = phase(left, w)
        lag = ((pi_ - pa) / w) % (rate / 1000)
        want = LEAD % (rate / 1000)
        print(f"  amplitude ADAT {amp_a:.0f}, I2S {amp_i:.0f}; ADAT lags slot 0 by {lag:.2f} frames modulo one "
              f"period (expected {want:.2f})")
        if amp_a < 1000 or abs(amp_a - amp_i) > 0.01 * amp_i:
            failures.append(f"{rate}: ADAT amplitude {amp_a:.0f} against I2S {amp_i:.0f}")
        if min(abs(lag - want), rate / 1000 - abs(lag - want)) > 0.2:
            failures.append(f"{rate}: lag {lag:.2f} frames, expected {want:.2f}")
        if rate == 48000:
            same = ch1 == left and ch2 == ch1
            print(f"  channel 1 equals the I2S word frame for frame: {'yes' if same else 'NO'}")
            if not same:
                failures.append("48000: ADAT channel 1 differs from slot 0")
        if watch:
            time.sleep(watch)
            b = smi_stats(dev)
            st2 = adat_status(dev)
            d = dict(late=b["late"] - a["late"],
                     underrun_frames=b["s"]["slot0_underrun_frames"] - a["s"]["slot0_underrun_frames"],
                     ring_drops=b["s"]["ring_drops"] - a["s"]["ring_drops"],
                     usb_underruns=b["s"]["underruns"] - a["s"]["underruns"],
                     adat_resyncs=st2["resyncs"] - st["resyncs"], adat_slips=st2["slips"] - st["slips"])
            nf = max((b["composed"] - a["composed"]) & 0xFFFFFFFF, 1)
            cyc = ((b["compose_cycles"] - a["compose_cycles"]) & 0xFFFFFFFF) / nf
            print(f"  over {watch:.0f} s: {d}, composer {cyc:.0f} cycles a frame")
            if any(d.values()):
                failures.append(f"{rate}: {d}")
    finally:
        tone.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rates", default="48000,44100,96000")
    ap.add_argument("--watch", type=float, default=20)
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    failures = []
    old_types = [get_type(dev, i) for i in range(4)]
    old_enable = dev.ctrl_transfer(0xC1, REQ_GET_ADAT_ENABLE, 0, VENDOR_IFACE, 1)[0]
    print(f"found: slot types {old_types}, ADAT {adat_status(dev)}, GPIO 16 function {gpio16(dev)}")
    try:
        set_type(dev, 0, 1)
        time.sleep(0.5)
        st = dev.ctrl_transfer(0xC1, REQ_SET_ADAT_ENABLE, 1, VENDOR_IFACE, 1)[0]
        if st != 0:
            print(f"FAIL: REQ_SET_ADAT_ENABLE returned {st}")
            return 1
        time.sleep(0.5)
        for r in args.rates.split(","):
            check_rate(dev, int(r), args.watch, failures)
        dev.ctrl_transfer(0xC1, REQ_SET_ADAT_ENABLE, 0, VENDOR_IFACE, 1)
        time.sleep(0.5)
        fn = gpio16(dev)
        print(f"ADAT disabled: GPIO 16 function {fn} {'(released) ok' if fn == 0 else 'STILL CLAIMED'}")
        if fn != 0:
            failures.append("GPIO 16 still on the SMI with ADAT disabled")
    finally:
        dev.ctrl_transfer(0xC1, REQ_SET_ADAT_ENABLE, old_enable, VENDOR_IFACE, 1)
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
