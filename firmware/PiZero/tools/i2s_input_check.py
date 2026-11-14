#!/usr/bin/env python3
"""DSPi's I2S input on the Pi (z4/i2s_input_pi.c, bench request 0x51), with no wiring. Switches DSPi's input
to I2S with its own vendor requests and checks each of its three clock roles:

  loop    every slot on I2S (the input is DSPi's slave) and DSPi's signal generator playing 997 Hz on all
          eight outputs. The receiver decodes the Pi's own outputs (GPIO 8-11) as the input, every decoded
          word must follow the sine, and DSPi must see it on all eight input channels.
  master  every slot on S/PDIF, so the input is the clock master: the SMI keeps BCK and LRCLK running
          and the receiver must frame every LRCLK half cleanly.
  ext     DSPi in I2S slave mode, with the PCM block as an external master (BCK 18, LRCLK 19, a sine on
          21) at --ext-rate, --ext-ppm off nominal: DSPi must lock and follow its rate, the SMI's clocks
          must lock to it, and the sine must decode cleanly.

Slot types, input source, clock mode, channel count, I2S input rate and the receiver's bench settings are put back.

A run passes only with no concealed frame, bad LRCLK half, wrong word, overrun, restart or output underrun.

Usage: python3.14 firmware/PiZero/tools/i2s_input_check.py [--roles loop,master,ext] [--rate 48000]
           [--ext-rate 44100] [--ext-ppm 40] [--seconds 20] [--hist]
"""
import argparse
import math
import struct
import time

from uac2_stream_check import find_board
from smi_check import set_type, get_type, smi_stats
from cpu_check import meter

VENDOR_IFACE = 2
I2S, SPDIF = 1, 0
SRC_USB, SRC_I2S = 0, 2
REQ_SET_INPUT_SOURCE, REQ_GET_INPUT_SOURCE = 0xE0, 0xE1
REQ_SET_INPUT_RATE, REQ_GET_INPUT_RATE = 0xED, 0xEE
REQ_SET_I2S_INPUT_CHANNELS, REQ_GET_I2S_INPUT_CHANNELS = 0xF3, 0xF4
REQ_SET_I2S_CLOCK_MODE, REQ_GET_I2S_CLOCK_MODE, REQ_GET_I2S_SLAVE_STATUS = 0x88, 0x89, 0x8A
REQ_GET_STATUS = 0x50
REQ_SET_OUTPUT_ENABLE, REQ_GET_OUTPUT_ENABLE = 0x72, 0x73
REQ_SIGGEN_SET_CONFIG, REQ_SIGGEN_CONTROL = 0xA4, 0xA6
PCM_MASTER, HIST, KEEP_LINES = 1, 2, 4
EXTRA_FLAGS = 0     # --hist: every bench request also asks for the rise-distance histogram

FIELDS = ("magic running role waits dma_channel warm_channel pairs ext_bck_hz sample_ps ticks skipped overruns "
          "wraps warms frames bad_halves concealed repaired gaps rescued wr samples_lo samples_hi dec_lo dec_hi "
          "tick_cycles_max elapsed_us slips starts reframes lock_state lock_updates locked_us unlocks "
          "rate_mismatch trim_ppb trim_min trim_max phase phase_min phase_max sine_checked sine_errors "
          "sine_worst pcm_master dma_cs dma_conblk dma_dest dma_debug bck_ratio rf0 rf1 rf2 rf3 rf4 bad_short "
          "bad_one_short bad_one_long bad_long bad_at_gap dumps h1 h2 h3 h4 h5 h6 h7 h8 h9 h10 " + " ".join(f"w{i}" for i in range(16)) + " " + " ".join(f"r{i}" for i in range(16)) + " by_step rd0 rd1 rd2 rd3 prefill_drains prefill_starts dac_muted").split()
SIGNED = {"bck_ratio", "trim_ppb", "trim_min", "trim_max", "phase", "phase_min", "phase_max"}
SLAVE_STATES = {0: "inactive", 1: "acquiring", 2: "relocking", 3: "locked"}


def status(dev):
    raw = bytes(dev.ctrl_transfer(0xC0, 0x51, 0, 0, 4 * len(FIELDS), timeout=1000))
    v = struct.unpack_from(f"<{len(FIELDS)}I", raw)
    s = {k: (x - (1 << 32) if k in SIGNED and x >= 1 << 31 else x) for k, x in zip(FIELDS, v)}
    if s["magic"] != 0x50493249:
        raise SystemExit("FAIL: no I2S receiver in this image (load the current pizero_z4.img)")
    s["samples"] = s["samples_lo"] | s["samples_hi"] << 32
    s["dec"] = s["dec_lo"] | s["dec_hi"] << 32
    return s


def bench(dev, shift=0, data=0, ext_bck=0, flags=0, check_lines=0, pcm_hz=0, hz=997, rate=48000, tol=3000):
    if not (shift or data or ext_bck or flags or check_lines):
        dev.ctrl_transfer(0x40, 0x51, 0, 0, None, timeout=1000)
        return
    c = int(round(2 * math.cos(2 * math.pi * hz / rate) * (1 << 30)))
    dev.ctrl_transfer(0x40, 0x51, 0, 0, struct.pack("<5B3xIiI", shift, data, ext_bck, flags | EXTRA_FLAGS, check_lines,
                                                     pcm_hz, c, tol), timeout=1000)


def set_source(dev, src):
    dev.ctrl_transfer(0x41, REQ_SET_INPUT_SOURCE, 0, VENDOR_IFACE, bytes([src]))


def get_source(dev):
    return dev.ctrl_transfer(0xC1, REQ_GET_INPUT_SOURCE, 0, VENDOR_IFACE, 1)[0]


def set_clock_mode(dev, mode):
    dev.ctrl_transfer(0x41, REQ_SET_I2S_CLOCK_MODE, 0, VENDOR_IFACE, bytes([mode]))


def get_clock_mode(dev):
    return dev.ctrl_transfer(0xC1, REQ_GET_I2S_CLOCK_MODE, 0, VENDOR_IFACE, 1)[0]


def set_channels(dev, n):
    dev.ctrl_transfer(0xC1, REQ_SET_I2S_INPUT_CHANNELS, n, VENDOR_IFACE, 1)


def get_channels(dev):
    return dev.ctrl_transfer(0xC1, REQ_GET_I2S_INPUT_CHANNELS, 0, VENDOR_IFACE, 1)[0]


def set_rate(dev, hz):
    dev.ctrl_transfer(0x41, REQ_SET_INPUT_RATE, 0, VENDOR_IFACE, struct.pack("<I", hz))


def get_rate(dev):
    """The I2S input rate DSPi has selected (the second word; the first is the pipeline's rate)."""
    return struct.unpack("<II", bytes(dev.ctrl_transfer(0xC1, REQ_GET_INPUT_RATE, 0, VENDOR_IFACE, 8)))[1]


def prefilled(dev, before):
    """The main loop's prefill handshake ran since `before`: outputs drained, then started together, and
    the DAC hardware mute (when one is configured) released."""
    s = status(dev)
    drains, starts = s["prefill_drains"] - before["prefill_drains"], s["prefill_starts"] - before["prefill_starts"]
    print(f"   prefill handshake: outputs drained {drains} times and started together {starts} times, DAC "
          f"hardware mute {'asserted' if s['dac_muted'] else 'not asserted'}")
    if not (drains >= 1 and starts >= 1 and not s["dac_muted"]):
        print("   FAIL: the prefill handshake did not finish")
        return False
    return True


def slave_status(dev):
    raw = bytes(dev.ctrl_transfer(0xC1, REQ_GET_I2S_SLAVE_STATUS, 0, VENDOR_IFACE, 16))
    state, mode, locks, losses, rate, measured, slips = struct.unpack_from("<4BIIB", raw)
    return dict(state=SLAVE_STATES.get(state, state), locks=locks, losses=losses, rate=rate, measured=measured,
                slips=slips)


def peaks(dev):
    """DSPi's peaks (32767 full scale): the eight inputs, then outputs 1-8."""
    raw = bytes(dev.ctrl_transfer(0xC1, REQ_GET_STATUS, 9, VENDOR_IFACE, 64))
    v = struct.unpack_from("<16H", raw)
    return v[:8], v[8:]


def set_output_enable(dev, out, on):
    dev.ctrl_transfer(0x41, REQ_SET_OUTPUT_ENABLE, out, VENDOR_IFACE, bytes([on]))


def get_output_enable(dev, out):
    return dev.ctrl_transfer(0xC1, REQ_GET_OUTPUT_ENABLE, out, VENDOR_IFACE, 1)[0]


def siggen(dev, on, hz=997.0):
    if on:
        cfg = struct.pack("<BBHHBBfIHH4f", 1, 0, 0xFF, 0, 1, 0, -20.0, 0, 0, 0, hz, 0, 0, 0)
        dev.ctrl_transfer(0x41, REQ_SIGGEN_SET_CONFIG, 0, VENDOR_IFACE, cfg, timeout=1000)
    dev.ctrl_transfer(0xC1, REQ_SIGGEN_CONTROL, 1 if on else 0, VENDOR_IFACE, 1, timeout=1000)


def wait_for(cond, seconds, what):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.1)
    print(f"   FAIL: {what} within {seconds} s")
    return False


def dump(dev, k, s):
    """The BCK and LRCLK levels in the snapshots around bad half k, and the distances between BCK rises."""
    raw = bytes(dev.ctrl_transfer(0xC0, 0x51, k + 1, 0, 4096, timeout=1000))
    w = struct.unpack(f"<{len(raw) // 4}I", raw)
    n, e, bits = w[0], w[1], w[2]
    v = w[4:4 + n]
    bck = 18 if s["role"] == 3 else 12      # the bench's external master, or the SMI's own BCK
    rises = [i for i in range(1, n) if (v[i] >> bck) & 1 and not (v[i - 1] >> bck) & 1]
    print(f"   bad half {k}: {bits} bit clocks, ended at snapshot {e} of {n}")
    print("     BCK   ", "".join("#" if (x >> bck) & 1 else "." for x in v[max(0, e - 200):e + 10]))
    print("     LRCLK ", "".join("#" if (x >> (bck + 1)) & 1 else "." for x in v[max(0, e - 200):e + 10]))
    print("     rises ", "".join(str(min(b - a, 9)) for a, b in zip(rises, rises[1:])))


def watch(dev, seconds, rate, lines_checked):
    """Counters over the run: frames against time, framing, the sine check, the outputs and the CPU. Any
    concealed frame, bad half, wrong word, overrun, restart or output underrun fails the run."""
    a, ta, ma, oa = status(dev), time.monotonic(), meter(dev), smi_stats(dev)
    time.sleep(seconds)
    b, tb, mb, ob = status(dev), time.monotonic(), meter(dev), smi_stats(dev)
    d = {k: b[k] - a[k] for k in ("frames", "bad_halves", "concealed", "repaired", "gaps", "rescued", "overruns",
                                  "wraps", "sine_checked", "sine_errors", "slips", "starts", "samples", "dec")}
    fs = d["frames"] / (tb - ta)
    cycles = d["dec"] / max(d["frames"], 1)
    late = ob["late"] - oa["late"]
    underrun = ob["s"]["slot0_underrun_frames"] - oa["s"]["slot0_underrun_frames"]
    print(f"   role {b['role']}, {b['pairs']} pairs, waits {b['waits']}, {1e6 / b['sample_ps']:.1f} M snapshots/s: "
          f"{fs:.0f} frames/s, {d['bad_halves']} bad halves, {d['concealed']} concealed, {d['repaired']} of "
          f"{d['gaps']} reload gaps repaired, {d['rescued']} stalls put right, {d['overruns']} overruns, "
          f"{d['slips']} slips, {d['starts']} restarts")
    print(f"   outputs: composer late +{late}, slot 0 underrun frames +{underrun}")
    print(f"   decoding {cycles:.0f} cycles a frame; whole core {mb['load']:.2f}%")
    if d["rescued"]:
        print("   stalls put right, by sixteenths of the tick's snapshots:", [b[f"r{i}"] - a[f"r{i}"] for i in range(16)])
        print("   found by the two rises being 0, 1, 2 or more snapshots further apart than the least that shows a "
              f"lost rise: {[b[f'rd{i}'] - a[f'rd{i}'] for i in range(4)]}; by the step in the spacing: "
              f"{b['by_step'] - a['by_step']}")
    if d["bad_halves"]:
        print("   bad halves: " + ", ".join(f"{k} {b[k] - a[k]}" for k in (
            "bad_short", "bad_one_short", "bad_one_long", "bad_long", "bad_at_gap")) + "; not put right for want "
            "of: snapshots in reach %d, a long distance %d, a single one %d, sure data %d, a long BCK run %d" % tuple(
                b[f"rf{i}"] - a[f"rf{i}"] for i in range(5)))
        for k in range(b["dumps"]):
            dump(dev, k, b)
    if EXTRA_FLAGS & HIST:
        print("   BCK rises 1 to 10 or more snapshots apart:", [b[f"h{i}"] for i in range(1, 11)])
        print("   stalls by sixteenths of the tick's snapshots:", [b[f"w{i}"] for i in range(16)])
    ok = (abs(fs - rate) < rate * 0.002 and d["concealed"] == 0 and d["bad_halves"] == 0 and d["overruns"] == 0
          and d["starts"] == 0 and d["slips"] == 0 and late == 0 and underrun == 0)
    if lines_checked:
        print(f"   sine check: {d['sine_checked']} samples, {d['sine_errors']} wrong, worst residual {b['sine_worst']}")
        ok &= d["sine_errors"] == 0
        ok &= d["sine_checked"] > 0.9 * 2 * lines_checked * rate * seconds
    return ok


def outputs_live(dev):
    """Outputs 1 and 2 carry inputs 1 and 2 (the default routing) once the prefill has unmuted them."""
    pin, pout = peaks(dev)
    print(f"   DSPi's peaks, inputs 1-2 {pin[:2]}, outputs 1-2 {pout[:2]}")
    if not (pout[0] > 0 and pout[1] > 0):
        print("   FAIL: the outputs are still muted")
        return False
    return True


def role_loop(dev, args):
    print(f"loop: slots on I2S, input I2S as DSPi's slave, generator at 997 Hz, {args.rate} Hz")
    for s in range(4):
        set_type(dev, s, I2S)
    enabled = [get_output_enable(dev, o) for o in range(8)]
    for o in range(8):
        set_output_enable(dev, o, 1)
    set_rate(dev, args.rate)
    bench(dev, shift=8, data=8, check_lines=0xF, rate=args.rate, tol=16)
    s0 = status(dev)
    set_source(dev, SRC_I2S)
    siggen(dev, True)
    ok = wait_for(lambda: status(dev)["running"] and status(dev)["frames"] > args.rate, 10, "the input running")
    time.sleep(2)
    ok &= prefilled(dev, s0)
    bench(dev, shift=8, data=8, check_lines=0xF, rate=args.rate, tol=16)     # restarts the sine check
    ok &= watch(dev, args.seconds, args.rate, 4)
    pin, pout = peaks(dev)
    print(f"   DSPi's peaks, inputs {pin}, outputs {pout}")
    ok &= all(o > 300 and abs(i - o) <= o * 0.02 + 2 for i, o in zip(pin, pout))
    siggen(dev, False)
    for o in range(8):
        set_output_enable(dev, o, enabled[o])
    return ok


def role_master(dev, args):
    print(f"master: slots on S/PDIF, input I2S as the clock master, {args.rate} Hz")
    for s in range(4):
        set_type(dev, s, SPDIF)
    set_rate(dev, args.rate)
    bench(dev, shift=8, data=8)
    s0 = status(dev)
    set_source(dev, SRC_I2S)
    ok = wait_for(lambda: status(dev)["running"] and status(dev)["role"] == 1, 10, "the input running as master")
    time.sleep(2)
    ok &= prefilled(dev, s0)
    ok &= watch(dev, args.seconds, args.rate, 0)
    ok &= outputs_live(dev)
    return ok


def role_ext(dev, args):
    print(f"ext: I2S slave mode, the PCM block as master at {args.ext_rate} Hz {args.ext_ppm:+.0f} ppm, slots on I2S")
    for s in range(4):
        set_type(dev, s, I2S)
    pcm_hz = round(args.ext_rate * (1 + args.ext_ppm * 1e-6))
    bench(dev, shift=12, data=21, ext_bck=18, flags=PCM_MASTER, check_lines=1, pcm_hz=pcm_hz,
          hz=85 * args.ext_rate / 4096, rate=args.ext_rate, tol=200)
    s0 = status(dev)
    set_clock_mode(dev, 1)
    set_source(dev, SRC_I2S)
    t0 = time.monotonic()
    ok = wait_for(lambda: slave_status(dev)["state"] == "locked" and slave_status(dev)["rate"] == args.ext_rate
                  and status(dev)["lock_state"] == 2, 15, "DSPi and the clock follower locked")
    s, st = status(dev), slave_status(dev)
    print(f"   locked after {time.monotonic() - t0:.1f} s: DSPi {st}, follower locked {s['locked_us'] / 1e6:.2f} s "
          f"after its start, trim {s['trim_ppb'] / 1000:+.3f} ppm, external BCK {s['ext_bck_hz']} Hz")
    time.sleep(1)
    ok &= prefilled(dev, s0)
    bench(dev, shift=12, data=21, ext_bck=18, flags=PCM_MASTER, check_lines=1, pcm_hz=pcm_hz,
          hz=85 * args.ext_rate / 4096, rate=args.ext_rate, tol=200)
    ok &= watch(dev, args.seconds, pcm_hz, 1)
    s, st = status(dev), slave_status(dev)
    print(f"   follower: lag {s['phase_min'] / 100:+.2f} to {s['phase_max'] / 100:+.2f} ns, trim "
          f"{s['trim_min'] / 1000:+.3f} to {s['trim_max'] / 1000:+.3f} ppm, {s['unlocks']} unlocks, "
          f"{s['lock_updates']} updates, {s['rate_mismatch']} at another rate (BCK period "
          f"{s['bck_ratio']:+d} ppm from the SMI's); DSPi {st}")
    ok &= s["unlocks"] == 0 and st["state"] == "locked"
    ok &= outputs_live(dev)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roles", default="loop,master,ext")
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--ext-rate", type=int, default=44100)
    ap.add_argument("--ext-ppm", type=float, default=40.0)
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--keep-lines", action="store_true",
                    help="leave decoded snapshots in the cache, which brings the 96 kHz capture stalls back")
    ap.add_argument("--hist", action="store_true", help="count the snapshots between BCK rises (costs CPU)")
    args = ap.parse_args()
    global EXTRA_FLAGS
    EXTRA_FLAGS = (HIST if args.hist else 0) | (KEEP_LINES if args.keep_lines else 0)
    dev = find_board()
    if dev is None:
        raise SystemExit("FAIL: no DSPi Zero")
    old = dict(types=[get_type(dev, s) for s in range(4)], source=get_source(dev), mode=get_clock_mode(dev),
               channels=get_channels(dev), rate=get_rate(dev))
    status(dev)
    results = {}
    try:
        set_channels(dev, 8)
        for role in args.roles.split(","):
            results[role] = {"loop": role_loop, "master": role_master, "ext": role_ext}[role](dev, args)
            set_source(dev, SRC_USB)
            set_clock_mode(dev, 0)
            bench(dev)
            time.sleep(1)
    finally:
        siggen(dev, False)
        set_source(dev, old["source"])
        set_clock_mode(dev, old["mode"])
        bench(dev)
        set_channels(dev, old["channels"])
        set_rate(dev, old["rate"])
        for s in range(4):
            set_type(dev, s, old["types"][s])
    for role, ok in results.items():
        print(f"{role}: {'PASS' if ok else 'FAIL'}")
    return 0 if results and all(results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
