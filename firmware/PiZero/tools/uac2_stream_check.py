#!/usr/bin/env python3
"""UAC2 High Speed check: the Pi enumerates at 480 Mbit/s, audio arrives at the sample rate (within 0.2%),
the paced consumer never underruns, feedback flows at about 1000 packets/s, and all of it survives
stopping and restarting the stream.

Usage: python3.14 firmware/PiZero/tools/uac2_stream_check.py [cycles]
Needs pyusb with libusb, and the sounddevice package. It plays a quiet 1 kHz tone straight to the Pi's
output by device name, so the Mac's default output is never changed. A switch cycle stops the stream
(the host drops to alt 0) and starts it again.
"""
import math
import struct
import sys
import time

import usb.core

VID, PID = 0x2E8B, 0xFEAA
REQ_Z_GET_STATS = 0x4F
FIELDS = ("class_requests", "set_alt_count", "rate_sets", "reserved0", "fb_sent", "reserved1",
          "out_packets", "out_bytes", "consumed_bytes", "underruns", "fifo_level", "suspends", "resumes",
          "mounts", "irqs", "irq_spurious", "fb_diepctl", "fb_dieptsiz", "fb_diepint", "data_doepctl", "gintmsk",
          "dsts", "fb_busy", "fb_parity", "data_parity")
# Data goes once per millisecond (about 1000 packets/s), but with feedback working the host varies the
# frames per packet, so the data is judged by bytes/s against the sample rate rather than packet count.
SPEEDS = {0: "Full", 1: "Low", 2: "High"}


def find_board():
    for dev in usb.core.find(find_all=True, idVendor=VID, idProduct=PID):
        try:
            if "Zero" in (dev.product or "") or read_stats(dev):
                return dev
        except usb.core.USBError:
            continue
    return None


def read_stats(dev, raw=None):
    """Parses a stats reply; reads one if raw is not given."""
    if raw is None:
        raw = bytes(dev.ctrl_transfer(0xC0, REQ_Z_GET_STATS, 0, 0, 256, timeout=500))
    magic, version, uptime_ms, rate, alt, mounted, speed, _ = struct.unpack_from("<HHIIBBBB", raw)
    if magic != 0x5532:
        return None
    n = min(len(FIELDS), (len(raw) - 16) // 4)
    s = dict(zip(FIELDS, struct.unpack_from("<%dI" % n, raw, 16)))
    for k in ("fb_parity", "data_parity"):
        if k in s and s[k] >= 0x80000000:
            s[k] -= 1 << 32
    if version >= 2 and len(raw) >= 204:   # Z4: DSPi pipeline block at byte 116
        cpu0, cpu1, core1_mode, nch = struct.unpack_from("<BBBB", raw, 116)
        blocks, frames, us_last, us_max, us_avg100, clip = struct.unpack_from("<6I", raw, 120)
        peaks = struct.unpack_from("<18H", raw, 144)
        fill, target, drops, under_f, drop_f, prate = struct.unpack_from("<iIIIII", raw, 180)
        s.update(cpu0=cpu0, cpu1=cpu1, core1_mode=core1_mode, n_channels=nch, blocks=blocks, frames=frames,
                 block_us_last=us_last, block_us_max=us_max, block_us_avg=us_avg100 / 100, clip_flags=clip,
                 peaks=peaks, slot0_fill=fill, ring_target=target, ring_drops=drops,
                 slot0_underrun_frames=under_f, slot0_drop_frames=drop_f, pipeline_rate=prate)
    if version >= 3 and len(raw) >= 220:   # Z4b: vendor requests and notifications
        vreq, vstall, npkt, nidle = struct.unpack_from("<4I", raw, 204)
        s.update(vendor_requests=vreq, vendor_stalls=vstall, notify_packets=npkt, notify_keepalives=nidle)
    s.update(version=version, uptime_ms=uptime_ms, rate=rate, alt=alt, mounted=mounted, speed=speed)
    return s


class Tone:
    """-20 dBFS stereo tone (1 kHz unless hz says otherwise) on a PortAudio stream opened on one named device."""

    def __init__(self, device_substring, rate=48000, set_device_rate=False, hz=1000):
        import sounddevice as sd
        self.sd = sd
        self.rate = rate
        # Asks Core Audio to switch this device's own rate, instead of resampling to it.
        self.extra = sd.CoreAudioSettings(change_device_parameters=True) if set_device_rate else None
        self.amp = int(32767 * 0.1)
        matches = [i for i, d in enumerate(sd.query_devices())
                   if device_substring in d["name"] and d["max_output_channels"] >= 2]
        if not matches:
            raise RuntimeError(f"no output device named like {device_substring!r}")
        self.device = matches[0]
        self.name = sd.query_devices(self.device)["name"]
        self.stream = None
        self.pos = 0
        one = bytearray()
        for i in range(rate):
            v = int(self.amp * math.sin(2 * math.pi * hz * i / rate))
            one += struct.pack("<hh", v, v)
        self.buf = bytes(one)

    def _callback(self, outdata, frames, time_info, status):
        # One second of tone is precomputed, so the callback only copies bytes.
        n = frames * 4
        start = self.pos
        end = start + n
        if end <= len(self.buf):
            outdata[:] = self.buf[start:end]
        else:
            outdata[:] = self.buf[start:] + self.buf[:end - len(self.buf)]
        self.pos = end % len(self.buf)

    def start(self):
        self.stream = self.sd.RawOutputStream(samplerate=self.rate, channels=2, dtype="int16", device=self.device,
                                              callback=self._callback, blocksize=480,
                                              extra_settings=self.extra)
        self.stream.start()

    def stop(self):
        # abort() rather than stop(): stop() waits for queued buffers and has hung inside CoreAudio.
        if self.stream:
            self.stream.abort()
            self.stream.close()
            self.stream = None


def measure(dev, seconds):
    a = read_stats(dev)
    time.sleep(seconds)
    b = read_stats(dev)
    dt = (b["uptime_ms"] - a["uptime_ms"]) / 1000
    return b, (b["out_packets"] - a["out_packets"]) / dt, (b["out_bytes"] - a["out_bytes"]) / dt


def main():
    cycles = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    dev = find_board()
    if dev is None:
        print(f"FAIL: no DSPi Zero ({VID:04x}:{PID:04x}) answering request 0x{REQ_Z_GET_STATS:02x}")
        return 1
    try:
        tone = Tone("DSPi Zero")
    except RuntimeError as e:
        print(f"FAIL: {e}")
        return 1

    start = read_stats(dev)
    print(f"board: {dev.product!r}, {SPEEDS.get(start['speed'], start['speed'])} Speed, uptime "
          f"{start['uptime_ms'] / 1000:.1f} s, alt {start['alt']}, rate {start['rate']}, suspends {start['suspends']}, "
          f"mounts {start['mounts']}")
    if start["speed"] != 2:
        print("FAIL: not at High Speed")
        return 1

    failures = []
    steady_underruns = 0
    try:
        tone.start()
        time.sleep(2.0)
        s0 = read_stats(dev)
        s, pps, bps = measure(dev, 3.0)
        under = s["underruns"] - s0["underruns"]
        print(f"steady on {tone.name!r}: {pps:.1f} packets/s, {bps:.0f} bytes/s, alt {s['alt']}, rate {s['rate']}, "
              f"feedback sent {s['fb_sent']}, FIFO {s['fifo_level']} bytes, underruns +{under}, "
              f"IRQs {s['irqs']}", flush=True)
        steady_underruns = s["underruns"]
        if "cpu0" in s:
            print(f"    DSPi: cpu0 {s['cpu0']}%, block {s['block_us_avg']:.1f} us avg / {s['block_us_max']} us max for "
                  f"{s['frames'] / max(s['blocks'], 1):.1f} frames, blocks {s['blocks']}, slot0 fill {s['slot0_fill']} "
                  f"(target {s['ring_target']}), ring drops {s['ring_drops']}, underrun frames "
                  f"{s['slot0_underrun_frames']}, peaks in {s['peaks'][:2]} out {s['peaks'][8:10]}", flush=True)
        if "vendor_requests" in s:
            print(f"    vendor requests {s['vendor_requests']} ({s['vendor_stalls']} stalled), notifications "
                  f"{s['notify_packets']} + {s['notify_keepalives']} keep-alive", flush=True)
        print(f"    EP2 IN DIEPCTL {s['fb_diepctl']:08x} DIEPTSIZ {s['fb_dieptsiz']:08x} busy {s['fb_busy']} "
              f"parity {s['fb_parity']}; EP1 OUT DOEPCTL {s['data_doepctl']:08x} parity {s['data_parity']}; "
              f"GINTMSK {s['gintmsk']:08x}", flush=True)
        want_bps = s["rate"] * (4 if s["alt"] == 1 else 6)
        if abs(bps - want_bps) > want_bps * 0.002 or under:
            failures.append(f"steady: {bps:.0f} bytes/s (want {want_bps}), {under} underruns")

        for c in range(1, cycles + 1):
            tone.stop()
            time.sleep(1.5)
            away, away_pps, _ = measure(dev, 1.0)
            tone.start()
            time.sleep(1.5)
            mid = read_stats(dev)
            back, back_pps, back_bps = measure(dev, 2.0)
            fb_delta = back["fb_sent"] - mid["fb_sent"]
            under = back["underruns"] - mid["underruns"]
            want_bps = back["rate"] * (4 if back["alt"] == 1 else 6)
            ok = abs(back_bps - want_bps) <= want_bps * 0.002 and back["mounted"] \
                and back["suspends"] == start["suspends"] and fb_delta / 2 > 900 and under == 0
            print(f"cycle {c:2d}: stopped {away_pps:6.1f} pkt/s alt {away['alt']}, playing {back_pps:6.1f} pkt/s "
                  f"alt {back['alt']} {back_bps:.0f} B/s, feedback {fb_delta / 2:.0f}/s, FIFO {back['fifo_level']}, "
                  f"underruns +{under}, suspends {back['suspends']} {'ok' if ok else 'FAIL'}", flush=True)
            if not ok:
                failures.append(f"cycle {c}")
                print(f"    EP2 IN DIEPCTL {back['fb_diepctl']:08x} DIEPTSIZ {back['fb_dieptsiz']:08x} DIEPINT "
                      f"{back['fb_diepint']:08x} busy {back['fb_busy']} parity {back['fb_parity']}; EP1 OUT DOEPCTL "
                      f"{back['data_doepctl']:08x} parity {back['data_parity']}; GINTMSK {back['gintmsk']:08x} "
                      f"DSTS {back['dsts']:08x}", flush=True)
    finally:
        tone.stop()

    end = read_stats(dev)
    print(f"end: packets {end['out_packets']}, bytes {end['out_bytes']}, set_alt {end['set_alt_count']}, "
          f"suspends {end['suspends']}, resumes {end['resumes']}, mounts {end['mounts']}, "
          f"feedback sent {end['fb_sent']}, consumed {end['consumed_bytes']}, underruns {end['underruns']} "
          f"(after the steady window {end['underruns'] - steady_underruns}), IRQs {end['irqs']} "
          f"(spurious {end['irq_spurious']})")
    if failures:
        print("FAIL: " + ", ".join(failures))
        return 1
    print(f"PASS: High Speed, data at the sample rate steady and after {cycles} stop/start cycles, feedback "
          f"flowing, no underruns or suspends")
    return 0


if __name__ == "__main__":
    sys.exit(main())
