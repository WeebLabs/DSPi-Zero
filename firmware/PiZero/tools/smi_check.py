#!/usr/bin/env python3
"""Multi-slot output check on the SMI: every DSPi slot on one parallel stream (slot n on GPIO 8+n, I2S BCK on
GPIO 12, LRCLK on GPIO 13).

Usage: python3.14 firmware/PiZero/tools/smi_check.py [--types i2s,spdif,i2s,spdif] [--rate HZ] [--keep]
Sets each slot's output type with DSPi's REQ_SET_OUTPUT_TYPE, plays a quiet tone straight to the Pi (the
Mac's default output is not changed), measures the SMI frame rate, then snapshots composed frames and
decodes every slot from the pin bits. Slot types are put back afterwards unless --keep is given.
"""
import argparse
import struct
import sys
import time

from uac2_stream_check import Tone, find_board, read_stats

REQ_SET_OUTPUT_TYPE, REQ_GET_OUTPUT_TYPE = 0xC0, 0xC1
VENDOR_IFACE = 2
BCK, LRCLK = 4, 5
SMI_FIELDS = ("running", "dma_channel", "freq", "div_q8", "frames", "composed", "late", "irqs", "cs", "dma_debug",
              "compose_cycles", "clean_cycles")
PREAMBLES = {"X": 0b11001001, "Y": 0b01101001, "Z": 0b00111001}


def smi_stats(dev):
    raw = bytes(dev.ctrl_transfer(0xC0, 0x4F, 0, 0, 388, timeout=500))
    s = read_stats(dev, raw)    # the same reply, so frames and uptime are sampled together
    if s["version"] < 7:
        raise SystemExit(f"FAIL: stats version {s['version']}, need 7 (load the SMI pizero_z4.img)")
    d = dict(zip(SMI_FIELDS, struct.unpack_from("<12I", raw, 300)))
    fb = struct.unpack_from("<3I2iI", raw, 348) if len(raw) >= 372 else None
    if fb:
        d.update(fb_sent=fb[0], fb_rate_iir=fb[1], fb_rate_long=fb[2], fb_servo=fb[3], fb_fill_err=fb[4])
    if len(raw) >= 388:     # version 9: SMI clock source and the PLLA watchdog
        src, ev, rs, fbk = struct.unpack_from("<4I", raw, 372)
        d.update(smi_source=src, pll_wd_events=ev, pll_wd_restores=rs, pll_wd_fallbacks=fbk)
    d["s"] = s
    return d


def snapshot(dev, n=16):
    raw = bytes(dev.ctrl_transfer(0xC0, 0x49, n, 0, 4 + n * 512, timeout=1000))
    first = struct.unpack_from("<I", raw)[0]
    return first, struct.unpack_from("<%dH" % (n * 256), raw, 4)


def decode_i2s(words, bit):
    """Samples data and LRCLK on BCK rising edges; a word starts one bit clock after LRCLK changes."""
    bits, lr = [], []
    for i in range(1, len(words)):
        if not words[i - 1] >> BCK & 1 and words[i] >> BCK & 1:
            bits.append(words[i] >> bit & 1)
            lr.append(words[i] >> LRCLK & 1)
    out = []
    for k in range(1, len(lr)):
        if lr[k] != lr[k - 1] and k + 33 <= len(bits):
            v = 0
            for b in bits[k + 1:k + 33]:
                v = v << 1 | b
            v = (v ^ 0x80000000) - 0x80000000
            out.append(("R" if lr[k] else "L", v >> 8))
    return out


def decode_spdif(words, bit):
    """Two words per cell. Finds preambles in the cell transitions, then reads each subframe's 28 bits."""
    levels = [words[i] >> bit & 1 for i in range(0, len(words), 2)]
    t = [levels[i] ^ levels[i - 1] for i in range(1, len(levels))]
    pre = {tuple((p >> j) & 1 for j in range(8)): name for name, p in PREAMBLES.items()}
    out = []
    i = 0
    while i + 64 <= len(t):
        name = pre.get(tuple(t[i:i + 8]))
        if not name:
            i += 1
            continue
        data, clock_ok = [], True
        for k in range(4, 32):
            a, b = t[i + 2 * k], t[i + 2 * k + 1]
            clock_ok &= a == 1
            data.append(b)
        sample = sum(b << j for j, b in enumerate(data[:24]))
        sample = (sample ^ 0x800000) - 0x800000
        out.append((name, sample, clock_ok and sum(data) % 2 == 0, data[26]))
        i += 64
    return out


def set_type(dev, slot, t):
    dev.ctrl_transfer(0xC1, REQ_SET_OUTPUT_TYPE, (t << 8) | slot, VENDOR_IFACE, 1)


def get_type(dev, slot):
    return dev.ctrl_transfer(0xC1, REQ_GET_OUTPUT_TYPE, slot, VENDOR_IFACE, 1)[0]


def classify(words, bit):
    """What a slot's pin carries: S/PDIF if subframes decode, I2S if data words frame on BCK, else idle."""
    if len({w >> bit & 1 for w in words}) == 1:
        return "idle"
    sp = decode_spdif(words, bit)
    if len(sp) >= 28 and all(g[2] for g in sp):
        return "S/PDIF"
    if any(w >> BCK & 1 for w in words) and len(decode_i2s(words, bit)) >= 28:
        return "I2S"
    return "unrecognised"


def observe():
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    want = ["I2S" if get_type(dev, s) else "S/PDIF" for s in range(4)]
    _, words = snapshot(dev)
    bck = any(w >> BCK & 1 for w in words)
    ok = True
    for slot in range(4):
        got = classify(words, slot)
        match = got == want[slot]
        ok &= match
        print(f"slot {slot + 1} (outputs {2 * slot + 1}-{2 * slot + 2}, GPIO {8 + slot}): DSPi type {want[slot]:6s}, "
              f"pin carries {got:12s} {'ok' if match else 'MISMATCH'}")
    print(f"I2S clocks (GPIO 12, 13): {'running' if bck else 'stopped'}, "
          f"{'as expected' if bck == ('I2S' in want) else 'NOT as expected'}")
    ok &= bck == ("I2S" in want)
    print("PASS: every slot's pin matches its type in DSPi" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--types", default="i2s,spdif,i2s,spdif")
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--keep", action="store_true", help="leave the slot types as tested")
    ap.add_argument("--watch", type=float, default=0, help="then watch fill and underruns for this many seconds")
    ap.add_argument("--observe", action="store_true",
                    help="change nothing: compare each slot's type in DSPi with what its pin carries")
    args = ap.parse_args()
    if args.observe:
        return observe()
    types = [1 if t.strip() == "i2s" else 0 for t in args.types.split(",")]
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    before = [get_type(dev, s) for s in range(4)]
    for s, t in enumerate(types):
        set_type(dev, s, t)
    time.sleep(1.0)
    print("slot types:", ", ".join(("I2S" if get_type(dev, s) else "S/PDIF") for s in range(4)))

    failures = []
    tone = Tone("DSPi Zero", rate=args.rate, set_device_rate=True)
    try:
        tone.start()
        time.sleep(2.0)
        a = smi_stats(dev)
        time.sleep(3.0)
        b = smi_stats(dev)
        dt = (b["s"]["uptime_ms"] - a["s"]["uptime_ms"]) / 1000
        rate = ((b["frames"] - a["frames"]) & 0xFFFFFFFF) / dt
        s = b["s"]
        print(f"SMI: DMA ch {b['dma_channel']}, divider {b['div_q8'] / 256:.4f}, {rate:.1f} frames/s at "
              f"{b['freq']} Hz, {((b['irqs'] - a['irqs']) & 0xFFFFFFFF) / dt:.0f} IRQs/s, composer late "
              f"+{b['late'] - a['late']}, lead {b['composed'] - b['frames']} frames, CS {b['cs']:#x}")
        nf = max((b["composed"] - a["composed"]) & 0xFFFFFFFF, 1)
        cyc = ((b["compose_cycles"] - a["compose_cycles"]) & 0xFFFFFFFF) / nf
        cln = ((b["clean_cycles"] - a["clean_cycles"]) & 0xFFFFFFFF) / nf
        print(f"composer: {cyc:.0f} cycles a frame composing and {cln:.0f} cleaning, "
              f"{(cyc + cln) * b['freq'] / 1e9 * 100:.2f}% of the 1 GHz CPU")
        print(f"DSPi: slot 0 fill {s['slot0_fill']} (target {s['ring_target']}), USB underruns "
              f"{s['underruns']}, peaks out {s['peaks'][8:16]}")
        if abs(rate - b["freq"]) > b["freq"] * 0.002:
            failures.append(f"SMI rate {rate:.1f}")
        if b["late"] != a["late"]:
            failures.append("composer late")
        first, words = snapshot(dev)
        prev = b
        t_end = time.monotonic() + args.watch
        while time.monotonic() < t_end:
            time.sleep(5.0)
            c = smi_stats(dev)
            p, q = prev["s"], c["s"]
            print(f"  {(q['uptime_ms'] - b['s']['uptime_ms']) / 1000:6.1f} s: slot 0 fill {q['slot0_fill']}, underrun "
                  f"frames +{q['slot0_underrun_frames'] - p['slot0_underrun_frames']}, ring drops "
                  f"+{q['ring_drops'] - p['ring_drops']}, USB underruns +{q['underruns'] - p['underruns']}, FIFO "
                  f"{q['fifo_level']}, composer late +{c['late'] - prev['late']}", flush=True)
            if "fb_sent" in c:
                nom = c["freq"] / 1000 * 65536
                ppm = lambda v: (v / nom - 1) * 1e6 if v else float("nan")
                print(f"           feedback {ppm(c['fb_sent']):+7.1f} ppm (rate short {ppm(c['fb_rate_iir']):+7.1f}, long "
                      f"{ppm(c['fb_rate_long']):+7.1f}, servo {c['fb_servo'] / nom * 1e6:+7.1f}), fill error "
                      f"{c['fb_fill_err'] / 65536 * 48:+6.1f} frames", flush=True)
            if q["slot0_underrun_frames"] != p["slot0_underrun_frames"] or q["underruns"] != p["underruns"]:
                failures.append(f"underruns at {(q['uptime_ms'] - b['s']['uptime_ms']) / 1000:.0f} s")
            prev = c
    finally:
        tone.stop()
        if not args.keep:
            for slot, t in enumerate(before):
                set_type(dev, slot, t)

    print(f"snapshot: frames {first} to {first + 15}")
    for slot, t in enumerate(types):
        if t:
            got = decode_i2s(words, slot)
            ls = [v for c, v in got if c == "L"]
            rs = [v for c, v in got if c == "R"]
            print(f"  slot {slot} I2S: {len(ls)} left and {len(rs)} right words, L {ls[:6]}, R {rs[:6]}")
            if len(ls) < 14 or len(rs) < 14:
                failures.append(f"slot {slot} I2S framing")
        else:
            got = decode_spdif(words, slot)
            bad = sum(1 for g in got if not g[2])
            names = "".join(g[0] for g in got)
            print(f"  slot {slot} S/PDIF: {len(got)} subframes ({names[:12]}...), {bad} with parity or clock "
                  f"errors, samples {[g[1] for g in got[:6]]}")
            if len(got) < 28 or bad:
                failures.append(f"slot {slot} S/PDIF decode")
    for gpio, name, want in ((13, "LRCLK", 2 * args.rate * 0.02),):
        n = struct.unpack("<2I", bytes(dev.ctrl_transfer(0xC0, 0x4B, gpio, 20, 8)))[0]
        print(f"  GPIO {gpio} ({name}) on the pin: {n} level changes in 20 ms (about {want:.0f} expected)")
    if failures:
        print("FAIL: " + ", ".join(failures))
        return 1
    print("PASS: every slot decodes from the SMI stream at the sample rate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
