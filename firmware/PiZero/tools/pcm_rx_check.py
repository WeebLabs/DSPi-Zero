#!/usr/bin/env python3
"""Port plan section 6: the fastest PCM bit clock the PCM block can capture in master receive mode. For each
clock, sets the PCM clock (bench request 0x46 op 2), reads it with the clock manager's counter, then runs a
DMA capture (bench request 0x47) and compares the bits captured per second with the clock. Every bit clock
is one captured bit, so the two must agree and the RX FIFO must never overflow.

Usage: python3.14 firmware/PiZero/tools/pcm_rx_check.py [--ms 200]
Each capture stalls the main loop, so do not stream audio while it runs. GPIO 18 and 19 are driven while
it runs; GPIO 20 (PCM_DIN) is read, and its words are shown as captured.
"""
import argparse
import struct
import sys
import time

from uac2_stream_check import find_board
import pll_check as p

SRC_PLLA, SRC_PLLD = 4, 6
CLOCKS = [(SRC_PLLA, d) for d in (30, 15, 10, 8, 6, 5, 4, 3, 2)] + [(SRC_PLLD, d) for d in (4, 3, 2)]


def capture(dev, ms):
    raw = bytes(dev.ctrl_transfer(0xC0, 0x47, 0, 0, 32 + 256, timeout=1000))
    seq = struct.unpack_from("<I", raw)[0]
    dev.ctrl_transfer(0x40, 0x47, ms, 0, None, timeout=1000)
    for _ in range(100):
        time.sleep(ms / 1000 + 0.05)
        raw = bytes(dev.ctrl_transfer(0xC0, 0x47, 0, 0, 32 + 256, timeout=2000))
        v = struct.unpack_from("<8I", raw)
        if v[0] != seq:
            return dict(ms=v[1], bytes=v[2], elapsed_us=v[3], rx_errors=v[4], dma_debug=v[5], dma_channel=v[6],
                        cs=v[7], words=struct.unpack_from("<64I", raw, 32))
    raise SystemExit("FAIL: no capture result")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ms", type=int, default=200)
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    fastest = 0
    try:
        for src, div in CLOCKS:
            dev.ctrl_transfer(0x40, 0x46, 2, src << 12 | div, None, timeout=1000)
            time.sleep(0.02)
            gates = max(1, min(40, int(2**24 / (500e6 / div * 0.01)) - 1))
            hz = p.measure(dev, p.MUX_PCM, gates)[0]
            r = capture(dev, args.ms)
            bits = r["bytes"] * 8 / (r["elapsed_us"] / 1e6)
            err = (bits - hz) / hz * 1e6 if hz else float("nan")
            ok = hz > 0 and abs(err) < 100 and not r["rx_errors"] and not r["dma_debug"]
            if ok:
                fastest = max(fastest, hz)
            ones = sum(bin(w).count("1") for w in r["words"]) / (64 * 32)
            print(f"{'PLLA' if src == SRC_PLLA else 'PLLD'} /{div:2d}: PCM clock {hz / 1e6:8.3f} MHz, captured "
                  f"{bits / 1e6:8.3f} Mbit/s ({err:+8.1f} ppm), RX overflows {r['rx_errors']}, DMA errors "
                  f"{r['dma_debug']}, ones {ones:.2f} {'ok' if ok else 'FAILS'}", flush=True)
    finally:
        dev.ctrl_transfer(0x40, 0x46, 2, 0, None, timeout=1000)
    print(f"fastest clean capture: {fastest / 1e6:.3f} MHz")
    return 0


if __name__ == "__main__":
    sys.exit(main())
