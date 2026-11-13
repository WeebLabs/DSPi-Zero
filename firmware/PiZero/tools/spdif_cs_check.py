#!/usr/bin/env python3
"""S/PDIF channel status on the SMI stream: reads one whole 192-frame block of every S/PDIF slot from
snapshots of composed frames and compares its 24 bytes, in both subframes, with what DSPi's encoder
sends at the rate (consumer, 24-bit, byte 3 the sample rate). Also checks V, U, parity, the preambles and
that the tone is unbroken from frame to frame, which covers the frames at the slot ring's wrap.

Usage: python3.14 firmware/PiZero/tools/spdif_cs_check.py [--rate HZ] [--silent]
Plays a quiet tone straight to the Pi (the Mac's default output is not changed) unless --silent is given.
The slot types are left as they are; slots that are not S/PDIF are skipped.
"""
import argparse
import sys
import time

from smi_check import PREAMBLES, get_type, snapshot
from uac2_stream_check import Tone, find_board

BLOCK = 192
RATE_BYTE = {44100: 0x00, 48000: 0x02, 96000: 0x0A}


def expected(rate):
    return bytes([0x04, 0x00, 0x00, RATE_BYTE.get(rate, 0x01), 0x0B]) + bytes(19)


def frame_subframes(words, bit):
    """One frame's two subframes as (preamble name, 28 data bits), or None if the cells are not biphase."""
    levels = [0] + [words[i] >> bit & 1 for i in range(0, 256, 2)]
    if any(words[i] >> bit & 1 != words[i + 1] >> bit & 1 for i in range(0, 256, 2)):
        return None
    t = [levels[i] ^ levels[i - 1] for i in range(1, 129)]
    pre = {tuple((p >> j) & 1 for j in range(8)): name for name, p in PREAMBLES.items()}
    out = []
    for sf in range(2):
        c = t[64 * sf:64 * sf + 64]
        name = pre.get(tuple(c[:8]))
        if name is None or any(c[2 * k] != 1 for k in range(4, 32)):
            return None
        out.append((name, [c[2 * k + 1] for k in range(4, 32)]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--silent", action="store_true")
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    slots = [s for s in range(4) if get_type(dev, s) == 0]
    if not slots:
        print("FAIL: no S/PDIF slot")
        return 1
    tone = Tone("DSPi Zero", rate=args.rate, set_device_rate=True, hz=997)
    frames = {}
    try:
        tone.start()
        time.sleep(2.0)
        if args.silent:
            tone.stop()
            time.sleep(1.0)
        t_end = time.monotonic() + 30
        # A frame's block phase is its number plus a constant, modulo 192, so any 192 residues make a block.
        # 2,500 frames also make it near certain that frames at the 512-frame slot ring's wrap are seen.
        while (len({f % BLOCK for f in frames}) < BLOCK or len(frames) < 2500) and time.monotonic() < t_end:
            first, words = snapshot(dev)
            for k in range(16):
                frames[first + k] = words[256 * k:256 * k + 256]
    finally:
        tone.stop()
    if len({f % BLOCK for f in frames}) < BLOCK:
        print(f"FAIL: only {len({f % BLOCK for f in frames})} of 192 block phases seen")
        return 1
    want = expected(args.rate)
    ok = True
    for slot in slots:
        errs, z_at, cs, nonzero = [], set(), {}, 0
        for f, words in sorted(frames.items()):
            sub = frame_subframes(words, slot)
            if sub is None:
                errs.append(f"frame {f} does not decode")
                continue
            (pl, dl), (pr, dr) = sub
            if pl == "Z":
                z_at.add(f % BLOCK)
            if pl not in "XZ" or pr != "Y":
                errs.append(f"frame {f} preambles {pl}{pr}")
            for d in (dl, dr):
                if d[24] or d[25]:
                    errs.append(f"frame {f} V or U set")
                if sum(d) % 2:
                    errs.append(f"frame {f} parity")
                nonzero += any(d[:24])
            if dl[26] != dr[26] or cs.setdefault(f % BLOCK, dl[26]) != dl[26]:
                errs.append(f"frame {f} C differs between subframes or blocks")
        # The tone is the same in both channels and moves little from one frame to the next.
        val = {f: [sum(b << j for j, b in enumerate(d[:24])) for _, d in sub]
               for f, sub in ((f, frame_subframes(w, slot)) for f, w in frames.items()) if sub}
        val = {f: [(v ^ 0x800000) - 0x800000 for v in lr] for f, lr in val.items()}
        peak = max(abs(lr[0]) for lr in val.values())
        if any(lr[0] != lr[1] for lr in val.values()):
            errs.append("left and right differ")
        if any(f + 1 in val and abs(val[f + 1][0] - lr[0]) > 0.16 * peak for f, lr in val.items()):
            errs.append("a step in the tone")
        if len(z_at) != 1:
            errs.append(f"Z preamble at {len(z_at)} block phases")
            z = 0
        else:
            z = next(iter(z_at))
            x_only = all(frame_subframes(w, slot) and (frame_subframes(w, slot)[0][0] == "Z") == (f % BLOCK == z)
                         for f, w in frames.items())
            if not x_only:
                errs.append("Z preamble not once a block")
        got = bytes(sum(cs.get((z + 8 * i + j) % BLOCK, 0) << j for j in range(8)) for i in range(24))
        if got != want:
            errs.append(f"channel status {got.hex()} is not {want.hex()}")
        ok &= not errs
        print(f"slot {slot}: {len(frames)} frames, channel status {got[:6].hex()}..., {nonzero} subframes with audio: "
              f"{'ok' if not errs else '; '.join(errs[:4])}")
    print(f"{'PASS' if ok else 'FAIL'}: channel status at {args.rate} Hz{' (silent)' if args.silent else ''}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
