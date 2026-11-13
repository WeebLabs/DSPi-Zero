# Pi Zero Z0 Bring-up: Boot, LED and UART

*Created: 2026-11-03*

This covers milestone Z0 from `PiZero_port_plan.md` on the first-generation Pi Zero (BCM2835). It explains how to build the image, prepare the SD card, wire the UART, read the result, and try USB boot with `rpiboot`.

## 1. What the Z0 image does

The image is `kernel.img`, built from `firmware/PiZero/`. The GPU firmware loads it at `0x8000` and starts it with the MMU and caches off. Our startup turns on the instruction cache and branch prediction, but leaves the data cache and MMU off. It then does four things.

1. It flashes the ACT LED three times quickly (5 Hz), then leaves it on. This happens before anything that could stall, so it proves our code runs and controls the LED.
2. It sets up the PL011 UART (UART0) on GPIO 14/15 at 115200 baud, 8N1. It prints a load-check report built only from computed characters (lines starting `LC`, then hex dumps), and then a banner.
3. It asks the VideoCore mailbox for the board revision, the firmware revision, the ARM memory split, and the ARM, core and UART clock rates, and prints them.
4. It blinks the ACT LED at 1 Hz (500 ms on, 500 ms off) from the 1 MHz system timer. Once per second it prints a heartbeat line with the ARM and core clock rates the firmware measures.

Any CPU exception prints a `FAULT:` line and makes the LED flash fast (10 Hz).

## 2. Build

The build needs `arm-none-eabi-gcc`, CMake and Ninja. DSPi itself, with its pico-sdk and TinyUSB, is the `DSPi` submodule. The build also needs Adafruit's `broadcom-peripherals` inside TinyUSB, which lives there as untracked files. On a fresh checkout, fetch the submodules and then that dependency.

```bash
git submodule update --init --recursive
```

```bash
cd DSPi/firmware/pico-sdk/lib/tinyusb && python3 tools/get_deps.py broadcom_32bit
```

Then build from the repo root.

```bash
cmake -S firmware/PiZero -B firmware/PiZero/build -G Ninja
```

```bash
cmake --build firmware/PiZero/build
```

The build writes one raw image per milestone, `firmware/PiZero/build/pizero_z0.img` and `pizero_z1.img`. The latest milestone, set by `SDCARD_TARGET` in `CMakeLists.txt`, is copied to `firmware/PiZero/build/sdcard/kernel.img` along with `firmware/PiZero/boot/config.txt`. To boot Z0 again, copy `pizero_z0.img` to the card as `kernel.img`.

## 3. GPU boot files

The BCM2835 boot ROM needs three closed-source files from Raspberry Pi: `bootcode.bin`, `start.elf` and `fixup.dat`. This script downloads them from `github.com/raspberrypi/firmware` at tag `1.20260915` and copies them into `build/sdcard/`.

```bash
firmware/PiZero/tools/fetch_boot_firmware.sh
```

After that, `firmware/PiZero/build/sdcard/` holds the whole boot partition. It contains `bootcode.bin`, `start.elf`, `fixup.dat`, `config.txt` and `kernel.img`.

## 4. Prepare the SD card (macOS)

Formatting erases the whole card, so check the disk number twice. The card needs an MBR partition table with one FAT32 partition. Any size from 1 GB up works.

1. Insert the card and find its disk number. It is the entry marked `(external, physical)` whose size matches the card.

   ```bash
   diskutil list external
   ```

2. Format it, replacing `diskN` with that number.

   ```bash
   diskutil eraseDisk FAT32 PIZERO MBRFormat /dev/diskN
   ```

3. Copy the boot files to the card.

   ```bash
   cp firmware/PiZero/build/sdcard/* /Volumes/PIZERO/
   ```

4. Eject it.

   ```bash
   diskutil eject /dev/diskN
   ```

For later builds only `kernel.img` changes, so step 3 can copy just that file.

## 5. Wire the UART

Use a 3.3 V USB-to-UART adapter. A 5 V adapter can damage the BCM2835. Do not connect the adapter's VCC or 5 V pin. The Zero has its own power.

| Adapter pin | Pi Zero header pin | Signal |
|---|---|---|
| GND | Pin 6 | Ground |
| RX | Pin 8 (GPIO 14) | Pi TXD0 |
| TX | Pin 10 (GPIO 15) | Pi RXD0 |

Pin 1 is the square pad at the end of the header nearest the SD card slot. Pins 6, 8 and 10 are on the outer row, counting from that end. A bare Zero needs a soldered header or test clips.

Open the adapter in a terminal. Its device name varies by chip, so list the candidates first.

```bash
ls /dev/tty.usb*
```

```bash
screen /dev/tty.usbserial-XXXX 115200
```

To leave `screen`, press Ctrl-A, then K, then Y.

### Using a Raspberry Pi Pico as the adapter

Any RP2040 Pico can act as the adapter, because both boards use 3.3 V logic. The bridge firmware is in `firmware/PiZero/tools/rp2040_uart_bridge/`. It links the Pico's UART0 at 115200 baud to a USB serial port, and the Pico's LED stays on while it runs.

```bash
cmake -S firmware/PiZero/tools/rp2040_uart_bridge -B firmware/PiZero/build/uart_bridge -G Ninja
```

```bash
cmake --build firmware/PiZero/build/uart_bridge
```

Flash `firmware/PiZero/build/uart_bridge/rp2040_uart_bridge.uf2` with `picotool load -x`, or copy it to the `RPI-RP2` drive in BOOTSEL mode. It then appears as `/dev/cu.usbmodem*`.

| Pico pin | Pi Zero header pin | Signal |
|---|---|---|
| Pin 3 (GND) | Pin 6 | Ground |
| Pin 2 (GP1, UART0 RX) | Pin 8 (GPIO 14) | Zero TXD0 to Pico RX |
| Pin 1 (GP0, UART0 TX) | Pin 10 (GPIO 15) | Pico TX to Zero RXD0 |

Do not connect the boards' 3V3, VSYS or VBUS pins together. Each board keeps its own USB power.

## 6. Power on and read the result

Start `screen` first. Then put the card in the Zero and power it from the **PWR** micro USB port.

The firmware prints its own boot log first, because `config.txt` sets `uart_2ndstage=1`. Then our banner appears, followed by one heartbeat line per second. The lines look like this, although the numbers will differ.

```
DSPi PiZero Z0 bring-up (Nov  3 2026 23:34:00)
board revision 0x00900093, firmware revision 0x...
ARM memory base 0x00000000 size 448 MB
set rates: arm 1000000000 Hz, core 400000000 Hz, uart 48000000 Hz
hb 1 s: arm 1000000000 Hz, core 400000000 Hz (measured)
hb 2 s: arm 1000000000 Hz, core 400000000 Hz (measured)
```

**Z0 passes** when the LED blinks at 1 Hz and the heartbeat lines show non-zero ARM and core rates.

If it does not pass, the LED narrows down where it stopped.

| What you see | What it means |
|---|---|
| No three quick flashes, LED dark or solid | Our code never ran, or GPIO 47 is not this board's LED. The UART boot log from `uart_2ndstage` tells these apart. |
| Three quick flashes, then solid | Our code ran but stopped before the blink loop. Every wait is time-bounded, so this should not happen. Report it with the UART output. |
| Three quick flashes, then 1 Hz blink, with no UART output | The code is fine. Check the TX/RX wiring (try swapping them), ground, and baud rate. |
| 1 Hz blink, rates print as 0 | The mailbox request failed or the firmware did not answer that tag. |
| LED flashes fast (10 Hz) without stopping | A CPU exception occurred. The `FAULT:` line gives its type and address. |
| Garbled UART text | The UART clock is not 48 MHz. The banner's `uart` rate shows the real value. |

**First hardware try (2026-11-03).** The first Z0 image showed a short flicker and then a solid LED, with no UART connected. That image had no early flash and used the library mailbox code described in section 8. It is not yet known whether the code stalled in the mailbox, or GPIO 47 is not the LED. The second image adds the three-flash signal and its own mailbox code to tell these apart.

**Second hardware try (2026-11-04).** The second image gave three quick flashes, then a steady 1 Hz blink. So our code runs, GPIO 47 is the ACT LED, active low is the right polarity, and nothing stalls before the blink loop. The first image's solid LED was most likely the library mailbox stall, although that was not confirmed. The UART output has not been checked yet.

**Third hardware try (2026-11-04).** The UART, read through an RP2040 bridge, showed the mailbox numbers correctly (ARM 700000000 Hz, core 250000000 Hz) but every fixed string as runs of `U`. A load-check build showed why. The firmware log says `Loaded 'kernel.img' to 0x0`, and our code reported running at `0x00000000`. The image is linked for `0x8000`, so every absolute address pointed at memory filled with `0x55`, while position-relative code still ran. The board reports `boardrev 9000c1`, a Pi Zero W. `config.txt` now sets `kernel_address=0x8000` and no longer sets `disable_commandline_tags`, which is the likely trigger.

**Fourth hardware try (2026-11-04): Z0 passed.** The firmware logged `Loaded 'kernel.img' to 0x8000`, and the load check reported `run@0x00008000` with all 8188 filler bytes intact. The heartbeat then printed once per second for 26 seconds:

```
board revision 0x009000c1, firmware revision 0x6aa809c1
ARM memory base 0x00000000 size 448 MB
set rates: arm 700000000 Hz, core 250000000 Hz, uart 48000000 Hz
hb 1 s: arm 1000104000 Hz, core 400000000 Hz (measured)
hb 2 s: arm 1000000000 Hz, core 400000000 Hz (measured)
```

The "set" rates (`GET_CLOCK_RATE`) are the base clocks, and the measured rates are the turbo clocks the firmware is actually running. The diagnostic load check, with its 8 KB filler and memory dumps, is still in the passing image. It can be cut down to the one-line `run@` report at Z1.

The ACT LED is GPIO 47 and is taken to be active low. If the LED is dark when the code says on, the polarity is reversed, but the 1 Hz blink still shows. Report it, and `BOARD_ACT_LED_ACTIVE_LOW` in `firmware/PiZero/board.h` gets flipped.

## 7. Try USB boot with rpiboot

The BCM2835 boot ROM falls back to USB device boot when it cannot boot from the SD card. The `rpiboot` tool on the Mac then sends the boot files over USB. If this works, every later build can be loaded without moving the SD card. Whether it works on this unit is not yet known.

1. Install the tool.

   ```bash
   brew install rpiboot
   ```

2. Take the SD card out of the Zero and unplug its power.
3. Start the file server on the Mac, pointing it at the staged boot files.

   ```bash
   rpiboot -d firmware/PiZero/build/sdcard
   ```

   It prints `Waiting for BCM2835/6/7/2711/2712...` and waits. It also asks you to fit an `nRPIBOOT` jumper. That applies to Compute Modules only, and on the Zero, removing the SD card does the same job. Adding `-l` keeps it running, so it serves the files again each time the Zero resets.
4. Connect a data micro USB cable from the Mac to the Zero's **USB** port, not PWR. That port powers the board as well.
5. `rpiboot` should report sending `bootcode.bin`, and then each file the firmware asks for, `kernel.img` included. The UART should then show the same output as an SD card boot.

On 2026-11-03, Homebrew `rpiboot` version `20261002~115811` started and waited without `sudo` on this Mac. If it reports a permission or "Failed to open the requested device" error once the Zero is connected, run the same command with `sudo`. If it never sees the device, check the cable, because many micro USB cables carry power only.

A successful USB boot also confirms the data path through the **USB** port that Z1 needs.

## 8. Notes for Z1

These came up while writing Z0 and matter once TinyUSB's `broadcom_32bit` BSP is used.

- `broadcom-peripherals` `boot.s` sets the IRQ stack twice, so the FIQ stack is never set. It also never enables the VFP, which our `-mfloat-abi=hard` build needs. `firmware/PiZero/startup.S` replaces it and fixes both.
- The ARM1176's VFP11 needs support code for denormals and some underflow cases. Our startup sets RunFast mode (flush-to-zero and default NaN) so it never needs that code. DSP code at Z4 must not rely on denormals.
- `gpio_set_pull()` in `broadcom-peripherals` writes the BCM2711 pull registers at offset `0xE4`. The BCM2835 instead uses `GPPUD` and `GPPUDCLK0/1` at `0x94` to `0x9C`. TinyUSB's `family.c` calls it for its LED pin. Avoid it on the Zero, or write a BCM2835 version.
- TinyUSB's `family.c` drives an LED on GPIO 18, not the ACT LED, and uses the mini UART, whose baud rate depends on the core clock. Z0 uses the PL011 instead, which has its own 48 MHz clock.
- `vcmailbox_request()` has three problems, so `firmware/PiZero/mbox.c` replaces it. It waits on mailbox 0's FULL flag before writing, but writes go to mailbox 1, so it should check `STATUS1`. It times out after 10 million polls rather than a set time, which with caches off and `-O0` is tens of seconds per failed call. It also passes the buffer's ARM address with no bus alias. The BCM2835 firmware enables the VideoCore L2 cache for ARM accesses by default, so the GPU should be given `address | 0x40000000`. Its `.c` file also lacks a declaration in `vcmailbox.h`.
- `gpio.h` needs `stdbool.h` included before it.
- Do not rely on the firmware's default load address. With firmware `1.20260915`, no device tree and `disable_commandline_tags=1`, a Zero W loaded `kernel.img` at `0x0`. `config.txt` must set `kernel_address=0x8000` to match `pizero.ld`. The boot report line `LC ... run@` prints the address our code actually started at.
- With the MMU off, every data access is strongly ordered and an unaligned access faults. The build uses `-mno-unaligned-access` for that reason. Z1 still runs with the MMU and D-cache off. They come in once DMA or DSP load needs them.

## 9. Z1: TinyUSB vendor echo

*Added: 2026-11-04*

The Z1 image is `pizero_z1.img`, which the build stages as `sdcard/kernel.img`. It runs TinyUSB 0.18.0's DWC2 device driver with one vendor-class interface (VID:PID `cafe:5a31`, TinyUSB's test VID). The interface has a 64-byte bulk OUT and IN pair, and every byte received on OUT is sent back on IN. The UART prints the controller's ID and hardware configuration registers at boot, then one heartbeat a second with the mount state, link speed, event counts and byte counts.

Three choices differ from TinyUSB's own `broadcom_32bit` board support.

- **Full Speed is forced through the HS PHY.** With `CFG_TUD_MAX_SPEED = OPT_MODE_FULL_SPEED`, TinyUSB selects the dedicated Full Speed PHY (`GUSBCFG.PHYSEL`) and sets `DCFG.DSPD = 3`. It is not confirmed that the BCM2835 has that PHY. Z1 builds for High Speed so TinyUSB sets up the UTMI+ PHY, then sets `DCFG.DSPD = 1` (Full Speed on the HS PHY) between `tud_disconnect()` and `tud_connect()`. The host therefore sees a 12 Mbit/s device and never gets the High Speed chirp.
- **The controller is polled.** The main loop calls `tud_int_handler(0)` and `tud_task()`, and CPU interrupts stay masked. `BP_EnableIRQ()` and `BP_DisableIRQ()` are no-op stubs, so the library's `interrupts.c` is not needed. Moving to the USB interrupt is a later step, probably at Z3.
- **USB power comes from our mailbox.** `mbox_set_power_state(VCMAILBOX_DEVICE_USB_HCD, 1)` replaces the library's `vcmailbox_set_power_state()`.

The pass test runs on the Mac with Homebrew's Python 3.14, which has pyusb 1.3.1 and libusb.

```bash
/opt/homebrew/bin/python3.14 firmware/PiZero/tools/z1_vendor_echo.py
```

It checks that the device reports Full Speed. It then sends 17 payload sizes from 1 to 1024 bytes for 20 rounds, and compares every echoed byte.

The Zero's **USB** port (the one nearer the mini HDMI, not PWR IN) must connect to the Mac with a data cable. The first-generation Zero W never appeared on USB, either through the dock hub or for `rpiboot`, and its USB port is now suspected dead. The Zero 2 W passed through the same dock hub, so the hub is not the problem.

## 10. Zero 2 W bring-up

*Added: 2026-11-11*

The first-generation Zero W's USB port appears dead, so Z1 moved to a Zero 2 W. The same sources build for both boards, one build directory each.

```bash
cmake -S firmware/PiZero -B firmware/PiZero/build-zero2 -G Ninja -DPIZERO_BOARD=zero2
cmake --build firmware/PiZero/build-zero2
```

`PIZERO_BOARD=zero2` builds 32-bit code for the Cortex-A53 (`-mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard -marm`) with `BCM_VERSION=2837`. The register headers then use the `0x3F000000` peripheral base. The boot files are the same as the Zero's (`bootcode.bin`, `start.elf`, `fixup.dat` at `1.20260915`), and `config.txt` adds `arm_64bit=0`. The firmware boots the image from `0x8000` in 32-bit mode, with its built-in `armstub8-32`.

These are the differences from the BCM2835 build.

- **The firmware enters our code in HYP mode.** `CPS` cannot leave HYP, so `startup.S` drops to SVC with `ERET`. Before that it clears `HCPTR` (except its RES1 bits), `HSTR` and `HCR`, so floating-point and CP15 accesses cannot trap to an HYP vector we never set. Cores 1 to 3 stay parked by the firmware.
- **The ACT LED is GPIO 29.** `startup.S` flickers it for 4 s before any other setup, ending with the pin high, as a sign of life without a UART. The 3 flashes and 1 Hz blink from `board_led_init()` follow. Active low is assumed from the Linux device tree, but both states were visible, so polarity is not yet confirmed.
- **The mailbox uses the `0xC0000000` bus alias.** The BCM2837 ARM does not go through the VideoCore L2, so the GPU must read the buffer uncached.
- **The cache helpers are stubs.** The `broadcom-peripherals` BCM2837 cache code is AArch64 only, so `caches_v7.c` provides empty versions. The D-cache stays off, so there is nothing to maintain.
- **Barriers use `ISB`.** The ARMv6 CP15 prefetch flush stays for the BCM2835 build only.

The C library is newlib-nano's `thumb/v8-a+simd/hard` variant. Its `memcpy` and `memset` copy bytes, so they do not make unaligned accesses with the MMU off.

### rpiboot does not work from this Mac

The Zero 2 W appears as `BCM2710 Boot` (`0a5c:2764`) with no SD card, and `rpiboot -v -l -d firmware/PiZero/build-zero2/sdcard` sends `bootcode.bin`. `bootcode.bin` then serves `start.elf` and `fixup.dat` correctly. Every file `start.elf` asks for afterwards fails with `transfer error: timed out` and `sent 0 bytes`. `config.txt` never arrives, which shows because the firmware then searches the default kernel names (`kernel8.img`, `kernel8-32.img`, `kernel7.img`) instead of the `kernel=kernel.img` it sets. `kernel.img` never arrives either. The firmware still reports `Done: kernel.img`, but our code never runs, so the LED stays solid. Padding the files to a multiple of 512 bytes made no difference. The board was behind two dock hubs, so a direct Mac port and a Linux host are both untested. Without `-l`, `rpiboot` exits at the first timeout.

The Zero 2 W is therefore booted from the SD card, which is prepared as in section 4 from `build-zero2/sdcard`.

### Result

**2026-11-11: Z1 passed on the Zero 2 W.** Booted from the SD card, the LED gave the 4 s flicker, then the 3 flashes and the 1 Hz blink. The Mac enumerated `DSPi PiZero Z1 vendor echo` (`cafe:5a31`, serial `Z1-0001`) at Full Speed, through the dock hub. `z1_vendor_echo.py` passed 20 rounds (340 transfers, 113520 bytes each way). A 500-round soak also passed, with 8500 transfers and 2838000 bytes each way in 15.52 s, about 357 KiB/s round trip, and no mismatches. No UART was connected, so the heartbeat and register dumps were not read.

## 11. Z2: UAC1 enumeration as DSPi

*Added: 2026-11-11*

The Z2 image is `pizero_z2.img`, which the build now stages as `sdcard/kernel.img`. It enumerates with DSPi's real descriptors and answers the audio class requests, but does nothing with the audio yet.

- **Descriptors.** `firmware/DSPi/usb_descriptors.c` is used as it is, except that the product string becomes `Weeb Labs DSPi Zero`. CMake makes that change in a generated copy (`build*/z2/usb_descriptors_zero.c`) at configure time, regenerates it when the DSPi file changes, and stops if the original string is gone. Without `PICO_RP2350` it takes the RP2040 path, which offers stereo alts 1 (16-bit) and 2 (24-bit) at 44.1, 48 and 96 kHz, plus the vendor interface with its bulk notification endpoint `0x83`. Its only Pico SDK header, `pico/unique_id.h`, is replaced by an empty stand-in in `z2/shim`. The `z2` directory comes first in the include path, so its `tusb_config.h` wins over DSPi's Pico one.
- **Class driver.** `z2/uac1_z2.c` is cut down from `firmware/DSPi/usb_audio.c`. It keeps DSPi's open, `SET_INTERFACE` and `GET_INTERFACE` handling, the iso-alloc and iso-activate pattern, and the stall and clear-stall reset of the stream endpoints. It answers mute, volume (-60 to 0 dB, 1 dB steps) and sampling-rate requests, and stalls rates other than 44.1, 48 and 96 kHz. Feedback is the nominal 10.14 value for the selected rate, re-armed from the SOF callback. Received packets are counted and dropped. Vendor requests are not handled yet and stall.
- **Serial number.** The USB serial string is the board's 64-bit VideoCore serial (`mbox_board_serial()`), printed as 16 hex digits.
- **LED.** The LED blinks at 1 Hz while idle and 4 Hz while the host has alt 1 or 2 selected.

**2026-11-11: Z2 passed on the Zero 2 W**, booted from the SD card. This run used DSPi's product string unchanged. The rename to `Weeb Labs DSPi Zero` came afterwards. `system_profiler SPUSBDataType` showed `Weeb Labs DSPi`, `2E8B:FEAA`, version 2.04, serial `0000000065E417C0`, manufacturer `GitHub.com/WeebLabs`, at Full Speed. `system_profiler SPAudioDataType` listed `Weeb Labs DSPi` with 2 output channels, `Current SampleRate: 48000` and USB transport. macOS also made it the default output and system output device. No UART was connected.

## 12. Z3: isochronous data flowing

*Added: 2026-11-11*

Z3 runs on the Z2 image, with three fixes and a way to read its counters without a UART.

**Counters over USB.** The image answers vendor request `0x4F` (device recipient, IN, up to 64 bytes). `0x4F` is one of the few codes DSPi's command set leaves free. The reply starts with magic `0x5A33`, a version, uptime in ms, the selected rate, the alt and the mount state. It then holds counters for class requests, alt changes, rate sets, SOFs, feedback packets armed, sent and flushed, OUT packets and bytes, suspends, resumes and mounts. macOS passes this request to the board while its audio driver owns the device.

**Test.** `tools/z3_stream_check.py` needs pyusb and `SwitchAudioSource` (`brew install switchaudio-osx`).

```bash
/opt/homebrew/bin/python3.14 firmware/PiZero/tools/z3_stream_check.py 10
```

It plays a -20 dBFS 1 kHz tone with `afplay`, and requires 998 to 1002 packets per second for 3 s. It then switches the output to BlackHole 2ch and back ten times, and requires the same rate after each switch, with the device mounted and no suspends. BlackHole is silent, so nothing is heard. The output device in use at the start is restored at the end.

**Fix 1: hang when streaming restarted.** DSPi's driver re-activates the iso endpoints on every alt change. TinyUSB's DWC2 `edpt_disable()` then disables the still-enabled OUT endpoint, which sets `DCTL.SGONAK` and waits with no time limit for `GINTSTS.BOUTNAKEFF` (`dcd_dwc2.c` lines 309-310). In slave mode that flag only sets once the global OUT NAK entry is popped from the RX FIFO. Our main loop is the only reader, and it is the code waiting, so the first switch back hung the board. Both alts use the same data OUT (582 bytes) and feedback IN (3 bytes) endpoints, so `z2/uac1_z2.c` now activates them once per bus reset and leaves them enabled. Alt 0 only stops re-arming. DSPi's stall and clear-stall pair, an RP2040 workaround, is not used on the data endpoint.

**Fix 2: frames lost to UART output.** The board's clock matched the Mac's to 0.01%, yet it counted 994 SOFs and 994 packets per second. The 1 s heartbeat printed about 120 characters at 115200 baud and blocked for about 10 ms, and packets and SOFs arriving in that time were lost. `uart.c` now has a buffered mode. `uart_putc()` queues into a 2 KB ring, dropping on overflow, and `uart_poll()` in the main loop moves only what the TX FIFO can take. `fault_handler()` switches back to blocking output first.

**Fix 3: feedback stuck on the wrong frame parity.** TinyUSB queues an iso IN packet for the next frame's parity (`dcd_dwc2.c` line 366). The feedback endpoint has `bRefresh = 2`, so the Mac polls it every 4 ms, always on frames of one parity. A packet queued on the other parity after a restart is never sent, and feedback dropped to 1 to 6 packets per switch cycle. The SOF callback now flushes a feedback packet still pending after 6 frames (stall, then clear stall, which disables the IN endpoint and flushes its FIFO), and the next SOF queues it on the other parity.

**2026-11-11: Z3 passed on the Zero 2 W.** The steady stream ran at 1000.0 packets per second and 192000 bytes per second (alt 1, 16-bit, 48 kHz). After each of the ten switches the rate was 999.5 to 1000.5 packets per second, with 849 to 870 feedback packets per 3.5 s window, which is about 250 per second. The run ended with 49149 packets, 9436608 bytes, 22 alt changes, 12277 feedback packets sent, 72 feedback flushes, 0 suspends and 1 mount. Before fixes 2 and 3, the same test measured 993 to 994 packets per second, and feedback stalled after the first switch-away.

## 13. USB loader: new builds without moving the SD card

*Added: 2026-11-11*

`rpiboot` fails from this Mac (section 10), so builds went onto the SD card by hand. The loader replaces that. The card is written once, and every later image goes over USB.

**Set up the card (once).** Build, stage the boot files, and copy `sdcard-loader/` to a FAT32 card as in section 4.

```bash
cmake -S firmware/PiZero -B firmware/PiZero/build-zero2 -G Ninja -DPIZERO_BOARD=zero2
cmake --build firmware/PiZero/build-zero2
sh firmware/PiZero/tools/fetch_boot_firmware.sh "$PWD/firmware/PiZero/build-zero2"
```

`sdcard-loader/` holds the same `bootcode.bin`, `start.elf` and `fixup.dat`, the loader as `kernel.img`, and `boot/config_loader.txt` as `config.txt`. That config sets `kernel_address=0x02000000`, so the firmware loads the loader at 32 MB. `pizero_loader.ld` links it there, and `pizero.ld` still links every milestone image at `0x8000`. Both include `pizero_sections.ld`. With the loader card in, power the board from the Mac's USB. The LED flickers for 4 s, flashes 3 times, then gives two short flashes a second while the loader waits.

**Load an image.**

```bash
/opt/homebrew/bin/python3.14 firmware/PiZero/tools/pizero_load.py firmware/PiZero/build-zero2/pizero_z2.img
```

If the loader is not on USB, the script asks any running image whose product name contains "Zero" to reboot to it, with vendor request `0x40`. It then waits up to 30 s for the loader. Matching by name means a real DSPi on the same `2E8B:FEAA` is never touched. The script sends the image, waits until the loader reports every byte, and sends GO.

**Protocol** (`loader/loader_proto.h`). The loader enumerates as `cafe:5a4c`, "DSPi PiZero USB loader", with one vendor interface. All requests are vendor requests to the device recipient.

| Request | Direction | Data |
|---|---|---|
| `0x01` LOAD_BEGIN | OUT | 8 bytes: size, then zlib CRC-32 |
| `0x02` STATUS | IN | 16 bytes: state, size, bytes received, maximum size |
| `0x03` GO | OUT | none. Stalls unless every byte arrived and the CRC matches |
| `0x04` REBOOT | OUT | none. Watchdog reset |

Image bytes go to bulk OUT EP `0x01` and are written straight to `0x8000`, up to the loader at 32 MB. GO invalidates the I-cache and branch predictor, then jumps to `0x8000` in SYS mode with interrupts masked. It passes the firmware's original r0 to r2, and r3 = `0x4C4F4144` ("LOAD"), which makes `startup.S` skip the 4 s flicker. The D-cache is off, so no data-cache cleaning is needed.

**Reboot to the loader.** `board_reboot()` in `board.c` does a full reset through the power manager watchdog (`PM_WDOG`, then `PM_RSTC` full reset, at peripheral base + `0x100000`). The GPU firmware then boots the loader from the card again. Z2 calls it for vendor request `0x40` once the status stage is done. `0x40` is one of the codes DSPi's command set leaves free. A hung image cannot answer, so it still needs a power cycle.

**2026-11-11: tested on the Zero 2 W.** With the loader waiting, `pizero_load.py` sent `pizero_z2.img` (30048 bytes, CRC-32 `15054b53`) in 0.07 s, and the board enumerated as `Weeb Labs DSPi Zero`. `z3_stream_check.py` then passed 3 cycles at 1000.0 packets per second. A second load went through reboot-to-loader. The loader was back 7.1 s after the request, the whole round trip took 7 s, and the reloaded image passed the check again. A CRC mismatch and an oversized image are rejected by the loader in code, but neither case has been tried on hardware.

## 14. UAC2 at High Speed

*Added: 2026-11-11*

The Zero 2 W's USB runs at High Speed, so the audio front end moved from UAC1 at Full Speed to UAC2 at 480 Mbit/s, using TinyUSB's own audio class (`audio_device.c`). DSPi Console only uses EP0 and the vendor interface, which keep their layout, so the audio class change does not affect it. The image is `pizero_uac2.img`, loaded over USB:

```bash
cmake --build firmware/PiZero/build-zero2
/opt/homebrew/bin/python3.14 firmware/PiZero/tools/pizero_load.py firmware/PiZero/build-zero2/pizero_uac2.img
/opt/homebrew/bin/python3.14 firmware/PiZero/tools/uac2_stream_check.py 20
```

*Update (2026-11-12): `tusb_dwc2/dcd_dwc2.c` now carries a third patch, for a control-pipe hang under rapid stream switching. See section 21.*

*Update (2026-11-11): data OUT now runs every 1 ms (`bInterval` 4, 582 bytes maximum, an 8-packet FIFO), not every microframe. See section 15 for why, and for the OUT parity search that makes it work.*

**Device** (`uac2/usb_descriptors.c`). The device is `2E8B:FEAA`, bcdDevice `0x0300`, "Weeb Labs DSPi Zero", with the board serial. The audio function is a UAC2 speaker with these entities:

- clock source 4, with 44.1, 48 and 96 kHz
- input terminal 1
- feature unit 2, master mute and volume, -60 to 0 dB in 1 dB steps
- output terminal 3

Streaming alt 1 is stereo 16-bit and alt 2 is stereo 24-bit (3-byte subslots). Data OUT EP `0x01` runs every microframe (`bInterval` 1, 78 bytes maximum). The explicit feedback IN EP `0x82` runs every 1 ms (`bInterval` 4, 16.16 format). The vendor interface 2 keeps the bulk notify EP `0x83` at 512 bytes, the High Speed bulk size. A small app class driver in `uac2/main_uac2.c` claims it. Feedback uses TinyUSB's FIFO-count method. A timer-paced consumer in the main loop reads the sample rate's worth of audio each millisecond, standing in for an audio output. It starts once the FIFO is half full.

**Interrupts** (`irq.c`, `startup.S`). The USB controller is GPU IRQ 9 on the legacy interrupt controller, which reaches core 0. The IRQ entry saves r0-r3, r12 and lr, FPSCR and the VFP caller-saved registers (d0-d7, plus d16-d31 on the A53), then calls `irq_handler()`. `irq.c` dispatches to registered handlers and implements TinyUSB's `BP_EnableIRQ`/`BP_DisableIRQ`, which mask the USB IRQ around TinyUSB's event queue. Images without `irq.c` get a weak `irq_handler()` in `board.c` that reports a fault, as before. The main loop only runs `tud_task()`, the consumer and buffered UART output.

**Patched DWC2 driver** (`tusb_dwc2/dcd_dwc2.c`, a copy of TinyUSB 0.18.0's driver with each change marked "DSPi PiZero patch"). TinyUSB's audio class re-activates the iso endpoints on every `SET_INTERFACE` and re-arms feedback on completion. On this controller in slave mode, that hit the same two problems as Z3, so the fixes now live in the driver.

1. **Disabling an enabled OUT endpoint.** The driver waits for global OUT NAK effective. In slave mode that status only arrives through the RX FIFO, so the wait loop now pops the RX FIFO itself.
2. **Frame parity for iso endpoints with `bInterval` > 1.**
   - **The problem.** The host polls these endpoints on (micro)frames of one parity, and TinyUSB only sets parity for `bInterval` 1.
   - **Learning the parity.** The driver records the host's parity per endpoint. It reads it from the RX status frame number for OUT, and from the `EONUM` bit after a sent packet for IN. From then on it queues every packet on that parity.
   - **Until it is learned.** The SOF handler holds each parity for 16 (micro)frames in turn, which covers at least two polls, and keeps the SOF interrupt on. A first version followed every frame instead. It failed permanently once the host's IN token came before our SOF interrupt in the microframe, so the packet always carried the previous frame's parity.
   - **Where it applies.** It works for the feedback IN endpoint. For OUT, this controller fixes the parity when the endpoint is enabled and ignores later writes. A 1 ms data endpoint therefore only worked when the first guess was right (1 stream in 11), which is why data stays at `bInterval` 1. There, TinyUSB's own next-frame parity is used.

The patched driver also exports `dcd_dwc2_iso_parity()` for diagnostics.

**TinyUSB workaround in the app.** When a stream stops, a feedback packet is usually still queued. On the next `SET_INTERFACE` the class calls `usbd_edpt_clear_stall()`, which clears `busy` but not `claimed` (`usbd.c` 1418-1419). The class's `usbd_edpt_claim()` then fails for good. `tud_audio_set_itf_cb()` releases the feedback endpoint whenever a streaming alt is selected.

**Bench counters.** Vendor request `0x4F` returns a `0x5532` ("U2") block. It holds uptime, rate, alt, mount state and speed, then counters, then diagnostics:

- counters: class requests, alt changes, rate sets, feedback sent, OUT packets and bytes, consumed bytes, consumer underruns, FIFO level, suspends, resumes, mounts, IRQs taken and spurious
- diagnostics: `DIEPCTL`, `DIEPTSIZ` and `DIEPINT` of EP 2 IN, `DOEPCTL` of EP 1 OUT, `GINTMSK`, `DSTS`, the feedback busy flag, and both learned parities

Request `0x40` reboots to the loader.

**Test** (`tools/uac2_stream_check.py`). It opens the Pi's output by name through PortAudio (`sounddevice`) and plays a -20 dBFS 1 kHz tone, so the Mac's default output is never changed. It stops with `abort()`, because `stop()` once hung inside CoreAudio. Each cycle stops the stream, which makes the host drop to alt 0, and starts it again. A window passes when four things hold:

- the data is within 0.2% of the sample rate in bytes per second
- the paced consumer had no underruns
- feedback ran at over 900 packets per second
- the device stayed mounted with no suspends

Packet counts are reported but not judged. With feedback reaching the host, it varies the frames per microframe, so about 7990 to 8000 packets carry exactly the sample rate.

**2026-11-11: passed on the Zero 2 W.** The device enumerated at High Speed, and Core Audio listed "Weeb Labs DSPi Zero" with 2 output channels at 48 kHz. The 20-cycle run passed:

| Measure | Result |
|---|---|
| Steady stream | 191952 bytes per second |
| Each of 20 restarts | 191882 to 192102 bytes per second, feedback at 1000 to 1006 per second, 0 underruns in the measured windows |
| Whole run | 99376860 bytes, 517541 feedback packets, 94 alt changes, 0 suspends, 8.8 million IRQs |

The 40 underruns outside the windows come at stream edges, as the FIFO drains after the host stops. Earlier runs went wrong in three ways before the fixes above:

- Every-microframe retargeting lost feedback for good after 7 restarts.
- 1 ms data packets only started 1 time in 11.
- An earlier per-SOF write of the feedback endpoint control register lost about 16 packets a second after a while.

## 15. Z4a: DSPi's pipeline on received audio

*Added: 2026-11-11*

`pizero_z4.img` runs DSPi's real RP2350 float pipeline on the UAC2 stream. The DSPi files are compiled unchanged with `PICO_RP2350=1` against stand-in Pico SDK headers in `pico_shim/`. Those files are `audio_pipeline`, `usb_audio`, `dsp_pipeline`, `crossover`, `loudness`, `crossfeed`, `leveller`, `psybass`, `subharm`, `tube`, `limiter`, `upmix`, `siggen`, `rta`, `rta_fft`, `rta_bass`, `output_latency`, `pdm_generator`, `audio_input`, `adat_output`, `notify` and `bg_job`. Only the Zero 2 W builds it, because `usb_audio.c`'s 24-bit unpack uses `sbfx` and `bfi`, which the ARM1176 lacks.

**How the pieces connect.**

- **Front end.** `z4/main_z4.c` reads the sample rate's worth of frames from the TinyUSB FIFO each millisecond. It decodes them into DSPi's `buf_l` and `buf_r` with `global_preamp_linear`, then calls `process_input_block()`.
- **Globals.** `z4/z4_globals.c` defines what DSPi's `main.c` and `flash_storage.c` would normally provide.
- **`usb_audio.c`.** It is compiled for its globals, with its UAC1 hook renamed so it is never registered.
- **Output.** The S/PDIF and I2S output rings are the real pico-extras structures. Each enabled ring is drained at the sample rate by the system timer, which stands in for an audio output until Z5.
- **Single core.** Core 1 stays off, so every output runs on the single-core path.
- **Init.** The sequence follows DSPi's `core0_init` without the hardware parts. There is no preset load, so factory defaults apply, including -20 dB master volume.
- **Pending.** Vendor commands, bulk params and the EQ update path come in Z4b.
- **Counters.** Vendor request `0x4F` now returns version 2 (204 bytes). It adds `cpu0_load` and block timing, peaks, clip flags and output-ring fill.

**Lost packets with data every microframe.** In the first run only about 5970 of 8000 packets a second arrived, about 25% of the audio. TinyUSB re-arms the data endpoint from `tud_task()` in the main loop, and a 300 us DSP block let two or three 125 us microframes pass with the endpoint unarmed. Data now goes every 1 ms (`bInterval` 4), which gives a block up to 1 ms. This needed the OUT half of patch 2. An enabled OUT endpoint keeps the parity it was enabled with, so while the host's parity is unknown, the SOF handler disables and re-queues the untouched transfer every 16 microframes. Each new queue takes the other parity, and the first packet that lands fixes it.

**Results on the Zero 2 W (2026-11-11).**

- **UAC2 alone, 1 ms data.** 10 of 10 restarts passed at 1000.0 packets per second and 192000 bytes per second, with feedback at about 1002 per second and 0 underruns. The interrupt rate fell from about 15000 to about 1550 per second of streaming.
- **Z4a, 10 of 10 restarts.** All passed with the same figures. DSPi's input peak was 3275 (a -20 dBFS tone) and its output peak 327 (factory master volume -20 dB).
- **Load.** DSPi reported `cpu0_load` 35%, with 339.8 us per block on average and 387 us at most, for 47.5 frames. That is single core with the D-cache off, so every access goes to SDRAM. It is the first load figure on the Cortex-A53, not a tuned one.

**Possible DSPi bug found here (not changed, `firmware/DSPi` is read-only for the port).**

- **The bug.** `db_to_vol[]` (`usb_audio.c:480`) is `uint16_t`, and its last entry, for 0 dB, is `0x8000`. `audio_set_volume()` stores it into `int16_t audio_state.vol_mul` (`usb_audio.c:510`, `usb_audio.h:21`), which makes it -32768. The pipeline scales `vol_mul` by 1/32768 (`audio_pipeline.c:348`).
- **The effect.** At 0 dB host volume the gain would be -1.0, which inverts polarity, and the ramp from the 59 to the 60 step would pass through zero.
- **Scope.** It would affect the RP boards too.

## 16. Z4b: DSPi's control plane, the D-cache, and Console

*Added: 2026-11-12*

`pizero_z4.img` now runs DSPi's control plane as well as its pipeline.

**DSPi files added unchanged.** These are `vendor_commands`, `bulk_params`, `flash_storage`, `usb_feedback_controller`, `input_source_names`, `lg_sound_sync`, `dac_hw_mute`, `uart_control`, `i2c_control`, `control_surfaces` (with `_display`, `_ir` and `_nouns`) and `usb_descriptors`. `usb_descriptors.c` is linked only for `desc_ms_os_20` and the serial string. Its descriptor callbacks are renamed, because the UAC2 descriptors are the ones in use.

**How requests and state are handled.**

- **Vendor requests.** `z4/main_z4.c` answers its own bench requests, then passes every other vendor request at every stage to DSPi's handler (renamed `dspi_vendor_control_xfer_cb`). The bench requests are `0x4F` stats and `0x40` reboot to loader, and only when sent to the device recipient. DSPi Console and DSPi's test harness address interface 2, so for them those codes stall as on an RP2350.
- **Main-loop handlers.** `z4/z4_mainloop.c` ports DSPi's `main.c` handlers for the pending flags that vendor commands set, calling DSPi's own functions, gated the same way. That covers bulk params, EQ recompute, rate change, presets, factory reset, output config, dynamics coefficients and background jobs. Hardware-only work is stubbed in `z4/dspi_standins.c`. S/PDIF, ADAT and I2S inputs never lock. Control-surface and UART/I2C bindings are configurable but never act. The ADAT output never starts.
- **Presets.** Presets live in a 128 KB RAM window that stands in for the top of a 4 MB flash part, with NOR erase and program rules. They are lost at reboot until SD storage exists.
- **Notifications.** The vendor app driver drains them to EP `0x83`, using the same claim, peek, transfer and commit sequence as DSPi's `usb_notify_drain`, with a 1-byte keep-alive after 100 ms.

**MMU and D-cache** (`caches_v7.c`, `mmu.h`).

- **The map.** `mmu_enable()` identity-maps SDRAM below `0x3F000000` as Normal write-back write-allocate memory, in 1 MB sections. The peripherals and the ARM local block are Device and execute-never. It invalidates the data caches by set/way before turning on M, C, I and Z, with alignment checking off.
- **Cache maintenance.** `data_clean()` and `data_invalidate()` are now real (DCCMVAC/DCCIMVAC, 64-byte lines). `mbox_call()` uses them, because the GPU reads the buffer from SDRAM. USB slave mode uses no DMA, so nothing else needs maintenance.
- **Effect.** With the cache on, a DSP block takes 44.6 us on average and 59 us at most for 48 frames, against 340 to 420 us with it off. `cpu0_load` fell from 35 to 62% to 4%. The higher figure was after the control plane went in, which also cost about 3.5% of packets.
- **Spurious IRQs.** The faster handler returned before the USB controller's interrupt-clear writes landed, so `usb_irq()` now reads `GINTSTS` back before returning.

**Platform identity and System Information.**

- **Board byte.** A `GET_PLATFORM` request for 8 or more bytes gets DSPi's 7 bytes plus byte 7 = `1`, which means Raspberry Pi Zero 2 W. Byte 0 stays RP2350, the code base every Console feature gate uses. A 7-byte request still goes to DSPi unchanged. DSPi Console (`StatsView.swift`) asks for 8 bytes and shows the board name in Device Information. Its `platformName` gating is unchanged.
- **Sensor readings.** `GET_STATUS` 13, 14 and 16 return the VideoCore's measured ARM clock, core voltage (tag `0x00030003`, id 1) and SoC temperature (tag `0x00030006`). The main loop caches them once a second, because a mailbox round trip is too slow for a control request. Measured values were 1000000000 Hz, 1256 mV and 36.48 °C.

**ARM clock.** The VideoCore keeps the ARM at `arm_freq_min` (600 MHz on the Zero 2 W) until the OS asks for more, as Linux's cpufreq driver does. The 1000 MHz read just after boot dropped to 600 MHz once the firmware's boot period ended. Z4 now asks for the maximum at boot, using GET_MAX_CLOCK_RATE `0x00030004` then SET_CLOCK_RATE `0x00038002` with skip-turbo 0. The clock then held 1000 MHz over a minute of checks. The firmware still throttles on temperature. The earlier block timings were taken soon after boot, at 1000 MHz, so they stand.

**DSPi Console** found the board on its own and polled it at about 36 requests a second with none stalled.

**DSPi's control-plane harness** (`python3.14 -m tools.dspi_test.run`, no flash tests) passed 110 of 138 tests, with 12 failures, 0 errors and 16 skipped, over 2805 GETs and 1249 SETs. That run had the D-cache off, and the harness restored the state byte for byte. The failures broke down as follows:

| Failures | Cause |
|---|---|
| 1 | The bench `0x4F` answered an interface request. Fixed since. |
| 4 | GPIO 12 is not rejected as on an RP2350, because pin rules differ. Expected. |
| 1 | Core 1 modes stay off in this single-core build. Expected. |
| 1 | Clock diagnostics answer zero-length, as release RP2350 builds do. Part of the read-everything test. |
| 2 | Stale test expectations. The subharmonic headroom (4.14 dB, `subharm.c:499-546`) and the tube drive floor (-30 dB, `tube.h:42`) match DSPi's current code, so an RP2350 fails these the same way. |
| 1 | The RTA config validation does not stall over USB. DSPi deliberately acknowledges a rejected SET on USB (`vendor_commands.c:4629-4634`). |
| 1 | RTA busy time over budget, measured with the D-cache off. |
| 1 | The RTA did not auto-stop, most likely because Console's spectrum view was reading the analyser at the same time. |

**Rerun on 2026-11-12, with the D-cache on and the ARM at 1000 MHz.**

- **All groups except RTA, Console open:** 107 of 130 passed, with 8 failures, 0 errors and 15 skipped. The failures were exactly the expected and stale ones above, and the `0x4F` test now passes.
- **RTA group, Console closed:** 5 of 8 passed, with 2 failures and 1 skipped.
  - Auto-off now passes, which confirms Console's spectrum view caused that failure.
  - Sine placement, pink-noise flatness, channel rotation and frame sequence all pass.
  - Config validation still fails, for DSPi's USB acknowledgement reason above.
  - Busy time fell from 23049 to 13760 us per second. That is 1.4% of the CPU, but still over the harness's 5000 us RP2350 budget. Cache alone gave only 1.7 times, so the window probably includes interrupt time, or the FFT is not using NEON. This is the one item still to investigate.

**Console check on 2026-11-12.** With audio playing straight to "Weeb Labs DSPi Zero", DSPi Console showed the input and output meters moving, and an EQ change took effect on the output. That completes Z4 on the Zero 2 W.

## 17. Z4 block time: newlib-nano's memset

*Added: 2026-11-12*

The 4% CPU figure was too high for a 1 GHz core next to the RP2350's 8 to 9% at 307.2 MHz. In cycles, the Pi was spending 44,700 per 1 ms block against roughly 26,000 on the Cortex-M33.

**Tools.** Two bench tools now show where the time goes.
- **Counters.** `z4/pmu.h` turns on the Cortex-A53 cycle counter and five event counters. Startup now sets HDCR so SVC mode owns them. The `0x4F` stats reply (version 4, 264 bytes) adds per-block cycles, the USB interrupt's share, instructions and cache refills. `tools/z4_perf.py` reports them.
- **Sampling profiler.** `z4/z4_prof.c` takes system timer channel 3 interrupts about every 37 us and records the interrupted PC, which `startup.S` now saves as `irq_return_pc`, while `process_input_block()` runs. Bench requests `0x4D` (start or stop) and `0x4E` (read) serve it. `tools/z4_prof.py ELF` maps the samples to functions.

**Cause.** 45% of the block time was in `memset`. We link newlib-nano, whose `memset` and `memcpy` are Thumb byte loops. DSPi clears its output and staging buffers with `memset` on every block, and on the RP2350 the Pico SDK replaces both with fast versions. The USB interrupt was not a factor: 343 cycles per block, and 0.88% of the CPU overall.

**Fix.** `mem_ops.c` supplies NEON `memset` and `memcpy` for `pizero_z4`. The per-file flags now match DSPi's `CMakeLists.txt` exactly, which adds `-O3` for seven control-path files and `-O3 -ffp-contract=off` for `rta_bass.c`.

**Result.** A block now takes 24,200 cycles (24.2 us), and `cpu0_load` reads 2%. That is 17,900 instructions at 0.74 instructions per cycle, with few cache misses. With Console's spectrum view open, the remaining time splits as follows.

| Share | Where |
|---|---|
| 42% | `process_input_block()` itself, a third of it in the output peak meters |
| 26% | `rta_bass_push`, the RTA's bass path, which runs only while the analyser is read |
| 18% | The output ring stand-ins, which copy the real drivers' bookkeeping and memory traffic |
| 4% | `memset` |

Per clock cycle the A53 is now only about as fast as the M33 on this code. DSPi's DSP code is scalar, single-precision floating point with long dependency chains, and the in-order A53 has a 4-cycle floating-point latency, where the M33's is about 1. Going further would mean restructuring DSPi's loops, for example processing channels side by side with NEON, which is outside this port's no-changes rule.

## 18. Z5: I2S out on the PCM block

*Added: 2026-11-12. Superseded by section 19, which moves every output to the SMI. `pcm_out.c` is still built but nothing binds it, so the PCM block is free for inputs.*

`pizero_z4.img` played DSPi's I2S output on the Pi's PCM block.

**Wiring for a PCM5102 module.** The PCM block's pins are fixed, whatever pins DSPi has stored for the slot.

| DAC pin | Pi signal | Header pin |
|---|---|---|
| BCK | GPIO 18, PCM_CLK | 12 |
| LCK (LRCLK) | GPIO 19, PCM_FS | 35 |
| DIN | GPIO 21, PCM_DOUT | 40 |
| SCK | Ground, so the PCM5102 makes its clock from BCK | 39 |
| GND | Ground | 39 or 6 |
| VIN | 5 V or 3.3 V, as the module expects | 2 or 1 |

On most modules XSMT must be high (unmuted) and FMT low (I2S). Many boards strap these already.

**How it works.**

- **Which slot plays.** DSPi makes the first I2S slot the clock master. When that instance starts, `pico_shim.c` starts the PCM block on its ring at the frame DSPi expects the reader to be at. So setting slot 0 (outputs 1 and 2) to I2S in Console puts outputs 1 and 2 on the DAC.
- **Clock.** The clock manager divides the 500 MHz PLLD by about 162.76 for 64 fs (48 kHz), with MASH 1 dithering the 12-bit fraction. That gives 47,999.976 Hz nominal, 44,099.975 Hz at 44.1 kHz and 96,000.096 Hz at 96 kHz. It has jitter, which the plan accepts for bring-up. DSPi's rate changes retune the divider in place.
- **Format.** Philips I2S: 64 BCK per frame, two 32-bit slots, data one BCK after the LRCLK edge, changing on BCK falling, LRCLK low for left. The ring words are DSPi's 24-bit samples left-justified in 32 bits, so the PCM5102 reads them as 32-bit I2S.
- **DMA.** `pcm_out.c` loops eight 64-frame control blocks over the 512-frame ring into the PCM FIFO, on a DMA channel the firmware leaves free (it chose 5). There are no interrupts. The read position gives the frame count modulo the ring, and the system timer resolves whole wraps, so a stalled main loop cannot lose count.
- **Uncached memory.** The linker puts DSPi's slot rings and the DMA control blocks in their own megabyte (`.nocache`, now 1 to 2 MB), which `mmu_enable()` maps as normal, non-cacheable memory. So the DMA always reads what DSPi wrote, with no cache maintenance.
- **One output clock.** `shim_output_clock()` counts the DMA's frames while the PCM block runs, and system timer time otherwise. Every ring, including the S/PDIF stand-ins, and the USB consumer advance by it. The TinyUSB FIFO level, which sets the feedback, therefore follows the DAC's clock rather than the timer's.

**Bench checks** (`tools/z5_i2s_check.py`, which sets slot 0 to I2S and plays a quiet tone straight to the Pi). The stats reply is now version 5 (300 bytes), with the PCM status appended.

- **Rates.** The DMA read 48,000.40 frames per second over 20 s, and 96,000.40 and 44,101.13 over 10 s each, within the 1 ms uptime resolution. `--rate` asks Core Audio to switch the Pi's own device rate, not the Mac's default output.
- **Errors.** There were no PCM TX underflows, no DMA errors, and no slot 0 underrun frames or USB underruns inside any window. The slot 0 fill stayed near its 192-frame target.
- **Restarts.** The UAC2 stream check passed five stop/start cycles with slot 0 on I2S.
- **Not yet done.** The 30-minute fill run, and the listening test through a DAC.

## 19. All output slots on the SMI

*Added: 2026-11-12*

`pizero_z4.img` now drives all four of DSPi's output slots from one SMI stream. Each slot is S/PDIF or I2S, as DSPi's output type for the slot says, and every pin changes on the same SMI cycle.

### Pins

| Signal | GPIO | SMI bit | Header pin |
|---|---|---|---|
| Slot 1 (outputs 1-2) | 8 | SD0 | 24 |
| Slot 2 (outputs 3-4) | 9 | SD1 | 21 |
| Slot 3 (outputs 5-6) | 10 | SD2 | 19 |
| Slot 4 (outputs 7-8) | 11 | SD3 | 23 |
| I2S BCK | 12 | SD4 | 32 |
| I2S LRCLK | 13 | SD5 | 33 |
| ADAT (added in section 22) | 16 | SD8 | 36 |
| MCK (GPCLK0, not the SMI) | 4 | | 7 |

A slot's pin carries S/PDIF or I2S data according to its type. BCK and LRCLK run while any slot is I2S. The UART (GPIO 14/15) and PCM pins (GPIO 18 to 21) are untouched. DSPi's stored pin numbers are ignored, as they were for the PCM block. MCK was added later (`PiZero_PLLA_plan.md` section 14), and so was the ADAT output (section 22).

### What the bench showed about the SMI

These came from a temporary test request that streamed a pattern ring with the timing given.

- **Register layout.** From Linux's `bcm2835_smi.h`. The SMI clock is the clock manager's SMI clock (`0x3F1010B0`), with DIVI in bits 15:12 and DIVF in bits 11:4.
- **Fractional divider.** It works. Divider 13.5625 gave 12.291 M words/s against 12.289 predicted, where an integer 13 gave 12.818.
- **Cycle length.** One write cycle is setup + strobe + hold SMI clocks. Pace does not add. 1 + 1 + 1 streams cleanly.
- **Rate.** 12.288 M words/s (48 kHz at 256 fs) and 24.576 M (96 kHz) both stream with no DMA errors. With a pin driven, the real rate tops out near 37 M words/s at divider 2.
- **Length.** `L` reads back as a running count of words sent, and the transfer ends when the count equals the programmed target exactly. Setting a target below the count does not end it: the 32-bit counter wraps past `0xFFFFFFFF` and runs on to the target. Rewriting `L` mid-transfer sets `SETERR` but takes effect without a gap. So the driver keeps the stream running forever by setting `L` one word behind the count about once a second.
- **After the end.** Once the count reaches `L`, the SMI stops driving the pins but keeps draining its FIFO. A CPU probe of the pin level (bench request `0x4B`) showed this.

### How it works

- **One stream.** `smi_out.c` streams 16-bit words at 256 fs: 12.288 MHz at 48 kHz, 11.2896 MHz at 44.1 kHz and 24.576 MHz at 96 kHz. A 1 + 1 + 1 cycle runs from 500 MHz PLLD through the fractional divider. The 1/256 divider step leaves the rate up to about 70 ppm off nominal (48,003 Hz at 48 kHz), which the USB feedback absorbs.
  *Update (2026-11-12): the SMI clock now comes from PLLA with an integer divider and no fractional dither, exact at every rate, with this PLLD path kept as the fallback. See `PiZero_PLLA_plan.md`.*
- **Ring and interrupts.** A DMA loop runs over a 128-frame ring of 256-word frames, with an interrupt every 16 frames. The interrupt refreshes `L` and calls the composer, which keeps the ring 64 frames ahead of the DMA. The ring is cached, and each composed block is cleaned out to SDRAM for the DMA.
  *Update (2026-11-12): the ring is now 192 frames of uncached memory and nothing is cleaned. See section 27.*
- **Composer.** `pico_shim.c` turns each slot ring into its pin's bits. An S/PDIF cell lasts 2 words and an I2S bit clock 4, at every rate. Slot n is always bit n, so the four slots merge with NEON shift-and-insert, 16 words at a time, and the clocks are ORed in.
  *Update (2026-11-12): the composer is now `smi_frame_a32.S`, which uses no tables for the slots and leaves a frame the ring already holds. See section 27.*
- **Ring contents as on the RP2350.** The S/PDIF ring writes now encode exactly as `pico_audio_spdif_multi`'s software encoder: its lookup table, preambles, channel status (with the sample-rate byte) and parity. Each ring holds NRZI cells, where a 1 toggles the line, and the composer turns them into levels with a running XOR. I2S words are stored as `pico_audio_i2s_multi` stores them: `(sample << 8) | 0x01`. The earlier stand-in stored raw values, so section 18's PCM output was 48 dB too quiet.
  *Update (2026-11-13): the S/PDIF rings now hold two words a frame, a sample and its status bit each, and the composer does the encoding. The pins carry the same bits. See section 28.*
- **Philips I2S timing.** 64 BCK per frame, LRCLK low for left, data changing on BCK falling and one bit clock behind LRCLK.
- **Output clock.** `shim_output_clock()` is the composer's frame count. The composer is what reads the slot rings, so DSPi's fill, underrun and recovery accounting measure the real reader. Its lead adds about 1.3 ms of latency at 48 kHz.
- **Fill servo.** On the RP2350, DSPi's own feedback steers the slot fill. Here the consumer does it: each 1 ms tick it moves up to 2 frames more or fewer than the composer took, so slot 0's mid-block fill sits at DSPi's target.
- **Uncached memory.** Only DMA control blocks remain uncached. Uncached slot and SMI rings had made the composer cost 3,512 cycles a frame.
  *Update (2026-11-12): the SMI ring is uncached again, and the slot rings stay cached. Written only with 32-byte NEON stores and never read by the composer, it costs less than the cached ring and its clean. See section 27.*

### USB feedback: macOS reads it per service interval

The SMI's 64 ppm offset exposed a feedback fault that every earlier clock had hidden: the timer ran exactly at nominal and the PCM clock was within 1 ppm. TinyUSB's FIFO-count feedback is in samples per 125 µs microframe. The host ignored it and ran at nominal, the FIFO drained at 12 bytes/s, and underruns began after about 55 s.

Fixed values set with bench request `0x48` showed why:

| Feedback value | FIFO change |
|---|---|
| Per microframe, +500 ppm | +0.0 bytes/s |
| Per microframe, -500 ppm | +6.0 bytes/s |
| Per 1 ms, +500 ppm | +83.0 bytes/s |
| Per 1 ms, -500 ppm | -85.0 bytes/s |

macOS reads the value in samples per data-endpoint service interval: 1 ms here, with `bInterval` 4. Linux detects the feedback scale by its size. How Windows reads it at this `bInterval` is not yet checked. The first fix was a proportional term on the TinyUSB FIFO level; section 20 replaces it with DSPi's own controller.

### Results

`tools/smi_check.py` sets the slot types with DSPi's `REQ_SET_OUTPUT_TYPE` and plays a tone straight to the Pi. It snapshots composed frames (bench request `0x49`) and decodes every slot from the pin bits: I2S by sampling data on BCK rising edges, S/PDIF by finding preambles and checking every subframe's parity.

- **Decoding.** Every slot decodes at 44.1, 48 and 96 kHz in both I2S and S/PDIF, with zero parity or clock errors. The SMI frame rate matches the sample rate.
- **Stability.** With `--watch`, 30 s at each rate and 120 s at 48 kHz showed no slot underruns, ring drops, USB underruns or late compositions. The slot fill held near DSPi's target and the FIFO near half full.
- **Restarts.** The UAC2 stream check passed three stop/start cycles.
- **CPU at 48 kHz with four S/PDIF slots.** DSPi's block, including the S/PDIF encoding, takes 34.7 µs per 48 frames, about 3.5%. Interrupts (the composer and USB) take 8%. The composer alone is 930 cycles a frame plus 390 for the cache clean, about 6% at 48 kHz and 12% at 96 kHz. `z4_perf.py` now subtracts every interrupt, not only USB's.
  *Update (2026-11-12): the composer is now 350 cycles a frame while audio plays and 72 while the outputs are silent. See section 27.*

### Not done yet

- A listening test through a DAC or S/PDIF receiver on these pins.
- A logic-analyser look at pin-to-pin skew and the clock's jitter.
- Windows' reading of the feedback value.
- Moving the composer to a second core, if the load matters at 96 kHz.

## 20. USB feedback: DSPi's controller, and no FIFO latency

*Added: 2026-11-12*

The feedback now comes from DSPi's own controller (`usb_feedback_controller.c`, unchanged), and the TinyUSB FIFO no longer holds a buffer.

**Why.** The section 19 controller held the TinyUSB FIFO half full: about 12 ms of latency at 16-bit, 48 kHz (582 frames), on top of the slot rings. It also had no rate measurement, so it could reach the right rate only with a standing FIFO error, and a consumer servo held the slot fill separately. DSPi's controller does both jobs properly. Loop A measures the output rate against the host's 1 ms frames over an 8 to 16 s window. Loop B servos slot 0's fill to DSPi's target with a two-slope proportional term. It works in 16.16 samples per 1 ms, which is exactly the unit macOS reads at High Speed with our 1 ms data interval.

**Formats.** On the RP2350 the controller also works in 16.16. Only the value on the wire is rounded to 10.14 (`fb_ctrl_get_10_14()`), because a Full Speed feedback endpoint is 3 bytes. The Pi is at High Speed with a 4-byte endpoint, so it sends the 16.16 value with no rounding: one step is 1/65536 sample per ms, 0.3 ppm at 48 kHz. DSPi's `feedback_10_14` is still kept up to date for its own status reports.

**How it is fed.**

- **No 8 kHz interrupt.** On the RP2350, DSPi's SOF interrupt calls `fb_ctrl_sof_update()` every 1 ms. At High Speed a SOF interrupt would come every 125 µs microframe. Instead, `feedback_measure()` runs from the USB interrupt, which arrives at least once a millisecond, and acts once per new 1 ms frame. That frame is DSTS SOFFN >> 3.
- **Word-precise rate.** It latches the frame number with the SMI's consumption from the DMA position, in slot-0 I2S ring words (2 a frame, `rate_shift` 13). That is word-precise, not the composer's 16-frame steps.
- **Fill.** The fill comes from slot 0's snapshot pair, as on the RP2350. That is the fill just after each write, so it reads 15 to 30 frames above a random-moment sample.
- **Sending.** `feedback_send()` in the main loop sends the controller's value, nominal while another input is selected (as DSPi does), or the bench override from request `0x48`.

**Consumer.** `consume_task()` now drains everything the host has delivered into the pipeline on every main-loop pass, so the slot rings are the only buffer. When a stream opens, it first collects DSPi's fill target (scaled if the rate is about to change), so the rings start at target. The `underruns` count in the `0x4F` stats now means a gap of 3 ms or more with no data while streaming, since an empty FIFO is normal. The section 19 consumer servo and FIFO-level loop are gone. Stats version 8 (372 bytes) adds the controller's internals, which `smi_check.py --watch` prints.

**Results** (`smi_check.py --watch 30` at each rate, `uac2_stream_check.py 3`).

| Rate | Long-window rate | Divider offset | Fill error after 30 s | FIFO |
|---|---|---|---|---|
| 48 kHz | +64.8 to +65.8 ppm | +64 ppm | -4 frames | 0, or one packet |
| 96 kHz | +65.5 to +65.8 ppm | +64 ppm | -6 frames | 0 to 384 bytes |
| 44.1 kHz | +78.7 ppm | +76.6 ppm | -0.5 frames | 0 |

*Correction (2026-11-12): the 44.1 kHz prediction first read +101 ppm, from an arithmetic slip (500 / 33.868 instead of 500 / 33.8688). The right figure is +76.6 ppm, so all three rates leave the same 1 to 2 ppm residual, which is the Pi's crystal against the host. The SMI now runs from PLLA with an integer divider, and the residual alone remains (`PiZero_PLLA_plan.md` sections 10 to 13).*

There were no slot underruns, ring drops, host gaps or late compositions, and the restart cycles passed. Latency is now DSPi's fill target (192 frames, 4 ms at 48 kHz) plus the composer's lead of about 1.3 ms, down from about 17 ms.

**Settling.** DSPi's inner servo gain is deliberately gentle, 1/256 sample a millisecond per 48-frame bucket, so packet jitter stays out of the host's clock. A 30-frame fill error after a pipeline reset (a slot type switch, for example) takes about 30 s to clear, with no underruns on the way. That tuning is kept.


## 21. USB control transfers hung under rapid stream switching

*Added: 2026-11-12*

**Symptom.** Starting and stopping streams with a rate change every few seconds hung the board's control pipe after 15 to 60 switches. The board stayed enumerated and the main loop kept running (the LED kept blinking), but every control request timed out, including GET_STATUS and string descriptors. macOS logged plain timeouts on endpoint 0 (`status 0xe00002d6`), not bus errors, so the controller was accepting SETUP packets and then NAKing. Only a power cycle recovered it, and while it lasted `coreaudiod` stalled every audio device on the Mac. It happened with the SMI on PLLD and PLLA untouched, so it is not related to the PLLA work.

**Ruled out.** Bench request `0x45` (`tools/usb_health.py`) counts the events the DWC2 driver posts (through `-Wl,--wrap=dcd_event_handler`) against the events TinyUSB queues (`tud_event_hook_cb`), and records the longest gap between `tud_task()` calls. No SETUP or transfer completion was ever dropped from TinyUSB's 16-entry queue, and the longest gap stayed under 1.5 ms.

**Cause.** Patch 1 in `tusb_dwc2/dcd_dwc2.c` pops the RX FIFO from thread context while it waits for global OUT NAK effective, which happens whenever an enabled OUT endpoint is disabled, as on every stream start or stop. A FIFO entry is popped in two reads, the status word and then its data. Patch 1 did this with interrupts enabled, so the USB interrupt could pop the same FIFO between the two reads, or pop the entry first. The two readers then fall out of step, and every later entry, SETUPs included, is misread, while the iso data endpoint keeps running. DSPi Console's polling (a 1312-byte spectrum analyzer read about every 60 ms) makes this much more likely, but is not the cause.

**Fix, patch 3.**
- Each thread-context pop runs with interrupts off and checks RXFLVL only after they are off, so the main loop and the interrupt never interleave.
- The OUT endpoint-disable wait also pops while it waits.
- Every disable wait gives up after 5 ms and counts it in `dwc2_pizero_wait_timeouts`, which the `0x45` reply reports.

A first attempt ran the whole disable with interrupts off. That deadlocked the main loop (the LED went solid) after 19 switches, because the "endpoint disabled" wait also needs the RX FIFO drained.

**Result.** 300 switches through 44.1, 48 and 96 kHz with DSPi Console and its spectrum analyzer polling (69336 SETUPs): no hang, no dropped events, and no wait timeouts. A second run of 300 with the Console closed was also clean.

**Diagnostics left in for now.** These come out or become bench-only once the fix has had more time.
- The `0x45` accounting.
- A main-loop hang watch (`usb_watch()` in `z4/main_z4.c`). If SETUPs keep arriving with no EP0 completion for 3 s, if a USB interrupt is pending with no handler running for 150 ms, or (once armed by a `0x45` read with wValue 1) if EP0 completes nothing for 8 s, it snapshots the controller and the last eight SETUPs, then disconnects and reconnects so the record can be read.

## 22. ADAT output on the SMI

*Added: 2026-11-12*

DSPi's ADAT output now runs, on GPIO 16 (SMI SD8, header pin 36). As with the other slots, DSPi's stored ADAT pin is bookkeeping only. As on the RP2350, the pin is claimed only while ADAT is enabled. While DSPi has the stream stopped, at 96 kHz for example, the pin is held low, and disabling ADAT returns it to an input.

**How it works.** `adat_output.c` is compiled unchanged. On the RP2350 it encodes each frame into 256 NRZI line levels in a ring, and a DMA channel feeds them to a PIO state machine at 256 fs. One ADAT bit lasts exactly one SMI word, so the composer can play that ring directly.
- **Start and stop.** The shim watches `pio_sm_set_enabled()` for pio1 SM 1, which is ADAT's state machine. On enable it takes the ring from the read address and length given to DMA channel 13, and the frame from the output clock.
- **Composing.** Each SMI frame plays one ring frame on bit 8, MSB first. The composer stores the slot byte and the ADAT byte of each word together with one interleaving store (`vst2q_u8`).
- **Progress.** After each batch, the composer moves DMA channel 13's read address on, so DSPi's own lead and overflow accounting works as on the RP2350.
- **Main loop.** `z4_mainloop.c` gained the ADAT calls from `main.c`: `adat_output_resync()` at the end of `complete_pipeline_reset()` and at boot, `adat_output_task()` every pass, and `adat_output_release_moving_pin()` in a pin change. DSPi stops ADAT above 48 kHz, as it does on the RP2350.

**Test.** `tools/adat_check.py` puts slot 0 on I2S, enables ADAT with DSPi's `REQ_SET_ADAT_ENABLE` and plays a 1 kHz tone straight to the Pi. It decodes ADAT frames and slot 0's I2S words from the same composed SMI frames.
- **48 kHz:** every frame had a correct sync pattern and forced 1 bits. Channels 1 and 2 carried the tone, and channels 3 to 8 were silent, which matches the routing. DSPi holds ADAT 96 frames behind slot 0, which is exactly two periods of the tone, and channel 1 equalled the I2S word frame for frame.
- **44.1 kHz:** the amplitude matched the I2S word (83849 against 83848), and the lag read 7.80 frames modulo one period, which is 96 frames as expected.
- **96 kHz:** DSPi reported ADAT inactive with `rate_ok` 0, and the pin was held low. Returning to 48 kHz restarted it, again matching frame for frame. With ADAT disabled, GPIO 16 was an input again.
- **Stream health:** over 60 s each at 48 and 44.1 kHz, there were no underruns, ring drops, late compositions, ADAT resyncs or slips. `usb_health.py` shows no hang recoveries.

**Cost.** The composer takes 1185 cycles a frame with ADAT running, against 900 without it (885 before this change). The extra is about 1.4% of the 1 GHz CPU at 48 kHz.
*Update (2026-11-12): with the composer of section 27 it is 436 cycles a frame with ADAT running and every frame written, against 350 without it.*
*Update (2026-11-13): `adat_output.c` is no longer compiled. The port's `z4/adat_output_pi.c` gives DSPi the same API with a ring of samples, and the composer encodes them (section 29). The start and stop, the progress in DMA channel 13 and the main-loop calls described above are unchanged.*

**Not covered.** No ADAT receiver has decoded the pin. ADAT's 96 kHz mode (S/MUX) is not offered, because DSPi does not offer it on the RP2350 either.

## 23. S/PDIF input on the PCM block

*Added: 2026-11-12*

DSPi's S/PDIF input now runs on the Pi. `spdif_input.c` and `input_servo.c` are compiled unchanged. Underneath them, `z4/spdif_rx_pi.c` provides the API of the RP2350's receive library (`pico_spdif_rx`), and `z4/soft_vcxo_pi.c` turns the servo's VCXO command into a PLLA trim.

**Wiring.** The source's optical receiver module is powered from 3.3 V, and its output goes to GPIO 20 (PCM_DIN, header pin 38). DSPi's stored S/PDIF input pin is bookkeeping only, as the output pins are.

**Capture.** The PCM block is clock and frame master, with two 32-bit receive channels filling each 64-bit frame, so every PCM clock is one sample of the line. The clock is PLLD / 4 = 125 MHz, which input tracking never moves. Only GPIO 20 is routed; the clock and frame stay inside the block. A DMA loop fills a 128 KB ring of eight chunks of about 1 ms, and each chunk's interrupt decodes it. After the ring come 96 words that repeat its start, copied when the first chunk completes, so the decoder reads a subframe that starts near the ring's end without wrapping.

**Decoding** (`spdif_decode.c` and `spdif_decode_a32.S`, tested by `tests/spdif_decode_test.c` and `tests/spdif_decode_a32_test.c`).
- **Finding the stream:** the decoder times level changes, measures the cell from the longest runs, and looks for two preambles 64 cells apart. Their spacing gives the cell length precisely.
- **Tracking:** every time slot starts with a level change, so a slot's first cell is the inverse of the cell before it, and only the second cell's centre is read: a 1 is a second cell equal to the second cell of the slot before. The decoder re-anchors on a slot's leading edge every 8 cells, using one count-leading-zeros on a 32-sample window. That window starts on the centre of the cell before the edge, which is the fourth slot's sample, so each group of four slots costs three single samples and one window. If an anchor edge is missing, or a subframe does not start with a valid preamble, the stream counts as lost and the search starts again. Parity errors are counted, not dropped, as by the RP2350's library.
- **Mac test:** subframes decode exactly at 44.1, 48 and 96 kHz, clean and with ±0.2 cells of edge jitter (16 ns at 96 kHz). At 62.5 MHz sampling, 96 kHz with that jitter fails, so the sampling stays at 125 MHz.
- **Assembly:** on the Pi, the part that reads one subframe's samples is `spdif_decode_a32.S`. It returns exactly what the C version in `spdif_decode.c` returns, which the Mac test uses. `tests/spdif_decode_a32_test.c` compares the two on 200000 random reads under QEMU, and `tests/spdif_decode_test.c` also passes there with the assembly in place.

**Library behaviour.** Blocks of 384 subframes start at a B preamble. Four aligned blocks at one rate make the receiver STABLE and call DSPi's lock callback. A misaligned block, a rate change, or 10 ms without a block loses the lock. The FIFO is DSPi's input capture arena. Block timestamps come from each block's sample position, referred to the chunk interrupt's system-timer time, so the servo's long-window rate uses the crystal's time base, as on the RP2350.

**Main loop.** `z4_mainloop.c` gained `main.c`'s S/PDIF handshake: on lock, the outputs drain and are prefilled from the input; on loss, the output mutes. The servo runs every pass, and `z4_service_producer()` delivers the input.

**Result with a real source** (`tools/spdif_in_check.py --source`, 48 kHz from an optical source).
- **Lock:** DSPi locked within 0.1 s, at a measured 47998.08 Hz.
- **Two minutes of music:** 95996 subframes a second, no decode or parity errors, no output underruns or late compositions. The audio reached the outputs.
- **Tracking:** DSPi's servo held the PLLA trim at -43.2 to -43.9 ppm, following the source's clock. The trim returned to exactly 0 on the switch back to USB.
- **No signal:** with nothing connected, the receiver reports no signal, DSPi reports acquiring, and USB playback is clean afterwards.

**Cost.** The decoding interrupt takes 2.59% of the CPU at 48 kHz: 251 cycles a subframe decoding and 14 dropping each chunk from the cache. The cost came down in these steps, each measured on the board with the 48 kHz source.

| Version | CPU at 48 kHz | Cycles a subframe | Instructions |
|---|---|---|---|
| Every level change timed and sorted | 27.5% | | |
| Two samples a slot at the cell centres | 13.9% | 1368 | 1297 |
| The same, with offsets worked out once a subframe and each group of four slots unrolled | 10.1% | 968 | 967 |
| One sample a slot | 6.96% | 644 | 675 |
| The ring's tail (no index masking), the anchor window's first sample used as the fourth slot's, the sync code from a table | 5.49% | 490 | 586 |
| The subframe reader in assembly | 4.55% | 392 | 419 |
| The same without negative index loads | | 371 | 418 |
| Its instructions reordered for the Cortex-A53 (first attempt) | 3.86% | 320 | 431 |
| One tracking loop, and a leaner per-subframe callback | 3.58% | 290 | 399 |
| Chunks dropped from the cache with DCIMVAC (78 to 14 cycles a subframe) | 2.96% | 290 | 399 |
| Instructions reordered again from measured timings | 2.74% | 267 | 399 |
| The reader's results returned in registers, block start out of the callback, a better predicted C-bit test | 2.59% | 251 | 396 |

- **One sample a slot** gives up checking every slot boundary: a slip shows at the next anchor, at most four slots on, or at the next preamble.
- **Cache.** The decoder never writes a chunk, so its lines are never dirty and can be invalidated without cleaning. `data_invalidate` cleans as well, which cost 78 cycles a subframe.
- **Instruction order.** The assembly's first version ran at about one instruction a cycle. Small loops timed on the board showed why: the Cortex-A53 issues two instructions a cycle in program order, with one load and one shift by register among them, and a value is ready later for some uses than for others (a load's word after 3 cycles, a result after 1 cycle as a plain operand, 2 as a shifted operand and 3 as an address). An instruction whose input is not ready holds back everything behind it. Each group of four slots is 32 instructions, placed so that every cycle issues two: 16 cycles a group, as measured in a loop.
- **Where the rest goes.** Called a second time on the same subframe, the reader takes 172 cycles for 308 instructions. The other 79 cycles are the tracking loop, the callback that builds blocks and fills the FIFO, and the calls between them.
- **Tried, not kept.** Software prefetch ahead of the reader saved 3 cycles. Checking the edge limit with a sticky saturation flag instead of a branch, and comparing against a distance register instead of a loaded limit, were no faster in the timed loop.

The source decoded for 60 seconds with no decode or parity errors. With nothing connected, the search reads every word; that cost about 4.8% before the cheaper cache invalidation and has not been measured since. DSPi Console's CPU figure includes the decoding (section 24).

**Not covered.** The source was 48 kHz only. 44.1 and 96 kHz from a real source, unplugging and replugging, and a source changing rate are untested. 96 kHz should roughly double the decoding cost, and the edge-jitter cases have only run in the Mac and QEMU tests. ADAT input is not started.

## 24. A CPU figure that counts the whole core

*Added: 2026-11-12*

DSPi's own CPU figure (`global_status.cpu0_load`) is the time `process_input_block()` takes against the audio it handles. On the Pi much of the work happens elsewhere: the SMI composer and the S/PDIF decoder run in interrupts, and DSPi's services, such as the spectrum analyser, run in the main loop. `z4/cpu_meter.c` therefore replaces the figure with a whole-core one, which DSPi Console shows unchanged.

**How it is measured.** The Cortex-A53 cycle counter times the producer (USB and input delivery, with DSPi's blocks) and DSPi's services. Each region counts its own time, less interrupts and less any region nested inside it, so a settle loop that runs the producer from the services is not counted twice. Every cycle in an interrupt handler counts. A main-loop call with under 3 us of its own work is polling, not load, and is left out. Every 500 ms the total, as a share of the window, becomes `cpu0_load`. It is also written after every producer call, so DSPi's per-block figure never shows through. Bench request `0x43` and `tools/cpu_check.py` show the parts.

**Readings** (DSPi Console closed, 500 ms windows).

| Case | Load | Interrupts | Producer | Services |
|---|---|---|---|---|
| USB input, no stream | 7.5% | 7.0% | 0.6% | 0.0% |
| USB tone at 48 kHz | 13.8% | 8.3% | 4.0% | 1.4% |
| USB tone at 96 kHz | 21.2% | 13.8% | 4.6% | 2.8% |
| S/PDIF input at 48 kHz | 24.4% | 20.2% | 2.7% | 1.5% |

The S/PDIF row was taken before the decoder was made cheaper (section 23). It now reads 19.4%, with interrupts at 14.1%.

*Update (2026-11-12): with the composer of section 27 the first three rows read 1.2%, 8.3% and 14.1%. The S/PDIF input row has not been taken again.*

*Update (2026-11-13): with the S/PDIF encoding moved into the composer (section 28) they read 0.9%, 7.6% and 12.9%.*

`REQ_GET_STATUS` wValue 9, which DSPi Console reads, returned the same figure as the meter, rounded.

**Polling left out.** The main loop runs about 270,000 passes a second, and the calls that find nothing to do take 50 to 65% of the core. That is waiting, which a sleeping loop would not spend, so it is not counted.

## 25. Output toggles froze the main loop

*Added: 2026-11-12*

**Symptom.** Toggling outputs on and off in DSPi Console's matrix mixer froze the board until it was power cycled.

**Cause.** `REQ_SET_OUTPUT_ENABLE` recomputes DSPi's Core 1 mode. With any of outputs 3 to 8 enabled, that is the EQ worker mode, in which `process_input_block()` hands outputs 3 to 8 to Core 1 and waits, with `__wfe()`, for Core 1 to finish. The Pi runs one core, and the port held the mode idle only once per main-loop pass, in DSPi's services. But vendor requests are handled in `tud_task()` inside the producer, and the producer runs its blocks straight afterwards. So a toggle followed by a block in the same pass waited forever. The bug dates from Z4b; no earlier test toggled outputs.

**Fix.** The producer calls `z4_sync_core1_mode()` between `tud_task()` and any block, so no block ever sees a Core 1 mode.

**Check.** `tools/output_toggle_check.py` toggled random outputs, including outputs 3 to 8 and PDM, 400 times at 50 a second while a tone played. The board kept answering, `core1_mode` read 0 throughout, the stream ran with no late compositions, and the outputs were put back. The old image was not run against this test, to avoid another power cycle, so the cause comes from the code rather than from a reproduction.

## 26. A card for users, with the USB loader on board

*Added: 2026-11-12*

`tools/make_sd_image.sh` builds `dspi-zero2w.img(.zip)`, a 64 MB card image with one FAT32 partition. It holds the GPU boot files with Broadcom's licence, `boot/config_card.txt` as `config.txt`, DSPi (`pizero_z4.img`) as `kernel.img`, the USB loader as `loader.img`, and a short README. It is built on a Mac with `hdiutil` and `diskutil`, with no root needed.

**Boot.** The firmware starts DSPi at 0x8000 as the kernel, so a power-up never passes through the loader. `initramfs loader.img 0x02000000` has the firmware place the loader at 32 MB as well.

**Reaching the loader.** On `APP_REQ_REBOOT_TO_LOADER`, DSPi checks that a loader is at `LOADER_BASE` (its first word is startup.S's first instruction, as DSPi's is). It then writes `LOADER_HANDOFF_MAGIC` and its complement at `LOADER_HANDOFF_ADDR` (31 MB), cleans them from the cache and resets the board. `startup.S` reads the marker first thing, with the MMU and caches off. If it is there, startup.S clears it and jumps to the loader with the firmware's r0 to r2. An image the loader started (r3 = `LOADER_LAUNCH_MAGIC`) clears any marker and goes on, so a loader that does not know the marker, like the one on the loader-only card, still works. With no loader in RAM the reboot is plain, as before.

**On the board** (the image written with Raspberry Pi Imager):
- DSPi enumerated on its own after power-up. All four slots decoded on the SMI, with PLLA locked as the audio clock.
- `pizero_load.py` reached the loader in 7.1 and 7.4 s and started the image it sent, twice. So the marker survives the watchdog reset.
- The loader's own reboot request brought DSPi back from the card in 6.1 s, 4 s of which is startup.S's LED flicker.

An image sent over USB runs until the power is removed. Keeping it means copying it onto the card as `kernel.img`, because the loader cannot write the card.

## 27. A cheaper output path

*Added: 2026-11-12*

The SMI composer was the largest single load on the core: 48,000 frames a second at 930 cycles composing and 390 cleaning each, whether or not anything was playing, because the stream never stops. It now costs 72 cycles a frame while the outputs are silent and 350 while audio plays. The pins carry the same bits as before, and the composed-frame count, the lead and the interrupt rate are unchanged.

**What changed.**
- **The ring is not cached.** `smi_ring` is in `.nocache`, which `mmu_enable()` maps as Normal non-cacheable memory. The composer only writes it, with 32-byte NEON stores, and the DMA reads what was written, so `smi_out.c` no longer cleans anything. It ends each interrupt's composing with one `dsb`.
- **No tables for the slots.** `smi_frame.h` describes the frame. Each slot becomes 128 line levels, one per S/PDIF cell time (2 SMI words). An S/PDIF slot's levels are the running XOR of its NRZI cells (since section 28, of its two ring words, which are not cells). An I2S slot's are its 64 bits, each doubled by squaring as a polynomial (`vmull.p8` of a byte with itself moves bit i to bit 2i). Two more rounds of squaring interleave the four slots into one nibble per cell, slot n in bit n. The clocks are ORed in, and each cell is stored as two words.
- **Assembly.** `smi_frame_a32.S` is the version the Pi runs, and `smi_frames_c` in the header is the reference. `tests/smi_frame_test.c` checks the reference against the old table-driven composer, and the assembly against both under QEMU, over every mix of slot types with and without the clocks and ADAT.
- **A frame the ring already holds is left.** The ring is 192 frames, one S/PDIF channel-status block. Silence repeats with that period, and DSPi stamps silence with the block phase of the absolute frame number, so a silent stream finds every ring frame already holding the frame it is due. To know that, the composer keeps a key for each ring frame: the 16 slot ring words the frame was made from (and the 8 ADAT words while ADAT runs). Since section 28 an S/PDIF slot's 4 key words are the frame's two and the next frame's two. A frame whose words equal its key is not written. The comparison is of the inputs themselves, not of a hash, and nothing is assumed about what is playing: a 1 kHz tone at 48 kHz repeats every 48 frames and is left in the same way.
- **Runs.** The first frame of a run that differs is written, and so is the rest of that run (at most 16 frames) without comparing. Audio and silence both come in long stretches, so this spares the comparison while audio plays. A key is stored only when its frame is written.
- **When the keys are not trusted.** After a change of slot types, of the I2S clocks or of ADAT, and after any gap in what was composed (the late path), every frame is written for one lap of the ring.
- **Ring index.** The ring length is no longer a power of two, so `smi_out.c` keeps the composer's ring index in its own counter. The frame count wraps at 2^32, which is not a multiple of 192.

**What it costs now** (`smi_check.py`, slots I2S, S/PDIF, I2S, S/PDIF, cycles a frame on the 1 GHz core). `smi_check.py` now plays 997 Hz, which never repeats within the ring, and `--silent` plays nothing.

| Step | Playing | Silent |
|---|---|---|
| Before: 16 words at a time from tables, cached ring, cleaned | 924 + 390 | 924 + 390 |
| The ring uncached, nothing cleaned | 900 | 900 |
| Levels and polynomial squaring in C (GCC kept its vectors on the stack) | 489 | 489 |
| Keys and the 192-frame ring, one frame a call in assembly | 432 | 167 |
| The frame loop in assembly, runs of frames a call | 364 | 89 |
| The rest of a run written without comparing, keys stored only when written, I2S words read aligned | 350 | 72 |

All S/PDIF reads 355 playing and 91 silent, all I2S 353 and 81. At 44.1 and 96 kHz the cycles a frame are the same.

**Whole core** (`cpu_check.py`, the figure DSPi Console shows).

| Case | Before | Now | Interrupts now |
|---|---|---|---|
| USB input, no stream | 6.3 to 7.5% | 1.1 to 1.3% | 0.8 to 0.9% |
| USB tone at 48 kHz, 997 Hz | 13 to 14% | 8.3% | 3.5% |
| USB tone at 48 kHz, 1 kHz (repeats in the ring) | | 7.1% | 2.3% |
| USB tone at 96 kHz, 997 Hz | 21.2% | 14.1% | 5.8% |

**Where the rest goes.**
- **Playing.** The 512 bytes of a frame take about 275 cycles to reach the uncached ring, whatever the stores are: sixteen `vst4.8`, sixteen 32-byte `vst1` and thirty-two 16-byte `vst1` all measured the same, with nothing else in the loop. The composing hides under that. *(Update, section 29: it does not. Work added before or between the stores costs its full time.)* Taking the sixteen polynomial multiplies out, or the whole merge, changed the total by under 6 cycles. So the cost of a written frame is now the memory write, and only writing fewer bytes would lower it.
- **Silent.** Of the 72 cycles, 25 are the per-interrupt work in C (reading DSPi's slot state, 400 cycles an interrupt) and 47 the comparison: six NEON loads, which take about 20 cycles when repeated on lines already in the cache, and a remainder that prefetching did not move and that has not been traced.

**Tried, not kept.**
- The cached ring with the new composer and a clean: 685 cycles a frame against 347.
- Widening with `vmovl` and `vst2.16` instead of `vst4.8`: the same within 3 cycles. The shorter one stayed.
- Software prefetch of the keys and slot rings, one frame and eight frames ahead: no change. Alignment hints on the loads: no change.
- Comparing every frame while audio plays cost 11 cycles a frame more than writing the rest of a run blind.
- A bench request that composed every frame a second time in the interrupt and compared it with the ring. Reading the uncached ring back a byte at a time took longer than the interrupt period, the board stopped answering and needed a power cycle. It was removed. Anything that reads the ring must do it outside the interrupt, as the snapshot does.

**Checks on the board**, on the image described here.
- `smi_check.py` at 44.1, 48 and 96 kHz with I2S/S/PDIF mixed, all S/PDIF and all I2S: every slot decoded, no late compositions. With `--silent` every slot decoded from frames the composer had left.
- `adat_check.py`: channel 1 equalled slot 0's I2S word frame for frame at 48 kHz, the lag read 7.80 frames at 44.1 kHz, the pin was held low at 96 kHz, and 20 s at each rate showed no underruns, drops, resyncs or slips.
- `mck_check.py` and `output_toggle_check.py` (400 toggles while a tone played) passed.
- Tones started and stopped at 48 and 96 kHz in turn, with the load returning to 1.1% after the last.

**Not covered.** The board stopped answering during the removed bench request, before these were run on the final image: `spdif_in_check.py --source`, `smi_check.py --watch 60`, and a test that changes slot types, the rate and ADAT while the outputs are silent and checks the stream after each. The frame count's wrap at 2^32 (24.9 hours at 48 kHz) has not been run through. No receiver or DAC has listened to the pins since the change.

**Further, not done.**
- With ADAT off, every cell is two equal words, so a 128-word frame at 6 SMI clocks a word would put the same levels on the pins from half the bytes: about 140 cycles a frame less while playing. ADAT needs one bit a word, so enabling it would have to change the SMI's word timing and frame length in the running stream, which has not been tried.
- One DMA interrupt per 32 frames instead of 16 would halve the 400 cycles an interrupt and the interrupt entries, about 0.3% of the core. It was left because it coarsens the output clock's steps and lowers the composer's least lead from 48 frames to 32.


## 28. S/PDIF rings hold samples, and the composer encodes

*Added: 2026-11-13*

With the composer cheap (section 27), DSPi's audio blocks were the largest load while a tone played, and 44% of a block was the shim writing S/PDIF: `audio_spdif_ring_write_s32` encoded every sample into biphase cells through a lookup table, and `audio_spdif_stamp_silence_run` wrote the erase-ahead silence as cells. The rings now hold plain samples and the composer does the encoding. The pins carry the same bits as before.

**Who reads the rings.** Only the shim's ring writers and the composer touch a slot ring's contents. DSPi itself tests `inst->ring` for NULL and passes `slot_ring_store[i]` as the base, and it never reads a word. ADAT has its own ring (`adat_output.c`), the bench snapshot reads the SMI ring, and no tool reads a slot ring. So the format is the shim's own to choose.

*Update (2026-11-13): the ADAT ring went the same way in section 29. It holds samples, in the port's own `z4/adat_output_pi.c`, and the composer encodes them.*

**The ring format** (`smi_frame.h`). A frame is two words, left then right, where it was four. A word has one bit for each of its subframe's 32 time slots.

| Bits | Holds |
|---|---|
| 0 to 3 | 3 in a left word, 15 in a right word (the preamble's slots) |
| 4 to 27 | the 24-bit sample, inverted |
| 28, 29 | validity and user data, inverted (always 1) |
| 30 | the channel-status bit, inverted |
| 31 | left word only, set in the first frame of a channel-status block |

The parity bit is not stored. The 512-frame ring, the erase-ahead and DSPi's accounting are unchanged, and `consumed_words` still counts the RP2350's four words a frame. The ring uses the first half of its slot's store.

**Why inverted.** Biphase mark toggles the line at the start of every slot and again at its middle for a 1. So the level in the second half of slot k is the running XOR of the inverted bits up to k, and one running XOR over the stored word (five shift-and-XOR steps) gives all 32 second halves. The first half of a slot is the second half of the slot before, inverted. The low four bits are chosen so the same XOR gives the preamble's second halves, and the first halves need one constant XORed in, plus one more for preamble Z. The parity slot must return the line to the level the subframe began at, so its second half is simply cleared, and no parity is ever computed. The two sets of 32 levels are interleaved by squaring (`vmull.p8`), which gives the same 128 levels a frame the composer made from cells before.

**Writing.** `smi_spdif_write` keeps a table of the silent frame at each of the 192 block phases, rebuilt when `spdif_set_status_rate` changes the rate byte. Silence is a copy from the table. Audio is the table XORed with the sample shifted into bits 4 to 27. Both run four words at a time in NEON. The status bit is still fixed when the frame is written, from the absolute frame number, as before.

**Keys.** The composer still keys each SMI ring frame on 4 words a slot. For an S/PDIF slot those are now the frame's two words and the next frame's two, so a silent stream still finds every frame in place. At the slot ring's last frame the next frame is the ring's first, and the composer hands the kernel a 4-word copy, as it already did for I2S. The first build read past the ring's end there instead, and two frames in every 512 were rewritten while silent.

**I2S.** The I2S ring writes had no encoding to remove, but they were scalar loops. They now run four words at a time as well, which took an all-I2S block from 23.7 to 22.0 us.

**Proof that the bits are the same.** `tests/smi_frame_test.c` keeps the old encoder. It writes the same random samples, at random block phases with random channel status, through the old encoder and through `smi_spdif_write`, and requires the table-driven reference on the old cells to equal `smi_frames_c` and `smi_frames_a32` on the new words, for every mix of slot types, clocks and ADAT. It also writes whole 512-frame rings in runs of audio and silence that wrap the ring and the block, and under QEMU it checks the assembly against the C on arbitrary words. It prints PASS on the Mac and under QEMU. On the board, the new `tools/spdif_cs_check.py` reads 2,500 composed frames of every S/PDIF slot, rebuilds the 24 channel-status bytes and checks V, U, parity, the preambles and that the tone is unbroken. It read `04 00 00 02 0b` at 48 kHz, `04 00 00 00 0b` at 44.1 kHz and `04 00 00 0a 0b` at 96 kHz, the same on the image before this change.

**What it costs now** (four S/PDIF slots, 997 Hz tone, DSPi Console's analyser running).

| Measure | Before | Now |
|---|---|---|
| DSPi block at 48 kHz | 34.6 us | 22.4 us |
| DSPi block at 96 kHz | 58.4 us | 36.1 us |
| Whole core, no stream | 1.35% | 0.9% |
| Whole core, tone at 48 kHz | 8.85% | 7.6% |
| Whole core, tone at 96 kHz | 15.1% | 12.9% |
| Composer playing, cycles a frame (all S/PDIF) | 355 | 358 |
| Composer silent, cycles a frame (all S/PDIF) | 90 | 84 |

In the block profile (`z4_prof.py`) the two S/PDIF functions were 43% of the samples (34.6% writing, 8.8% stamping silence) and the one that remains is 7 to 11% of a block that is a third shorter. What is left of a block is DSPi's own `process_input_block` and the analyser's `rta_bass_push`. With no stream the producer fell from 0.45% to 0.01%. Part of that is the meter's rule that a call under 3 us is polling, which the idle silence pump now falls under at 48 kHz. With the device left at 96 kHz the idle figure went from 2.6% to 1.8%.

**Checks on the board**, on the image described here.
- `smi_check.py` with I2S/S/PDIF mixed, all S/PDIF and all I2S at 44.1, 48 and 96 kHz, and each with `--silent`: every slot decoded, no late compositions, and silent streams were left in the ring 100%.
- `smi_check.py --watch 60`: no underruns, drops or late compositions.
- `spdif_cs_check.py` at the three rates and silent, `adat_check.py`, `mck_check.py` and `output_toggle_check.py` passed.
- `spdif_in_check.py --source --watch 20`, with an optical source at 48 kHz steering the output clock: locked throughout, no decode or parity errors, no underruns.

**Tried or considered, not kept.**
- A key of only the frame's own two words would need the kernel's compare to know each slot's type. The 4-word load with the next frame in it costs nothing in the kernel.
- Feeding the composer's merge the first and second halves separately would save the eight extra `vmull.p8` that four S/PDIF slots now cost, about the 3 cycles a frame the playing figure rose by. It means rewriting the merge in both the C and the assembly for under 0.02% of the core, so it was left.

**Not covered.** No receiver or DAC has listened to the pins since the change; the checks decode the composed stream, not the wire. A change of slot type while audio plays is covered only by `smi_check.py` switching types between runs. Rates other than 44.1, 48 and 96 kHz (status byte 0x01) are covered by the unit test only.


## 29. The ADAT ring holds samples, and the composer encodes

*Added: 2026-11-13*

With ADAT enabled the core carried 2.3 points more load while a tone played (9.87% against 7.61%). DSPi's `adat_output.c` encoded every frame into 256 line levels as it was pushed, through a table and a chain of 24 dependent lookups, and that alone was 11 us of a 39 us block. The ADAT ring now holds plain samples, in a module of the port's own, and the composer encodes them. One compiler option then shortens DSPi's block itself. The pins carry the same bits as before, and the load with ADAT enabled is 8.35%.

**Where the load was** (four S/PDIF slots, 48 kHz, 997 Hz tone, DSPi Console's analyser running). Enabling ADAT added 0.57 points of interrupt time and 1.67 points of producer time.
- **The interrupts** were the composer, at 476 cycles a frame against 358.
- **The producer** was all inside `process_input_block`, which went from 22.4 to 39.1 us (`z4_perf.py`, which counts cycles; DSPi's own `block_us_avg` is a slow average and reads lower for a while after ADAT is enabled). The keep-alive and `adat_output_task` cost nothing that the meter can see, and the services figure did not move.
- **Inside the block**, `adat_output_push_block` took 11.1 us, which is 231 cycles a frame. Another 5 us is DSPi's S24 staging. While ADAT is active DSPi converts each output row to integers in place and then interleaves them, where it otherwise does both in one pass. The in-place loop is one chain of VFP operations a sample, and the in-order A53 waits out each chain, so it costs 16 cycles a sample where the fused loop's two chains cost 8. The rest is the cache. The ADAT ring is 18 KB, each block writes 24 lines of it that the cache no longer holds, and the block's L1D refills rose from 91 to 139.

**The module.** `z4/adat_output_pi.c` is DSPi's `adat_output.c` with the encoder taken out, and `adat_output.c` is no longer in the build. It keeps every function and every line of the accounting, so `diff` against DSPi's file shows only the ring's contents.
- **What DSPi's callers use**, read in `audio_pipeline.c`, `vendor_commands.c`, `bulk_params.c`, `flash_storage.c`, `dac_hw_mute.c`, `control_surfaces_nouns.c` and `main.c` (whose calls `z4_mainloop.c` repeats). They are the configuration with its dirty flag, which `bulk_params.c` and `flash_storage.c` also write directly, the status packet, `adat_output_is_active()`, which chooses DSPi's S24 staging for the block, the push, the slot-skip mirror, the keep-alive from the main loop and from the stall alarm, the task, the resync, the stop and the pin release. All are the same code as DSPi's.
- **PIO and DMA calls are kept.** The shim still starts and stops ADAT when pio1 SM 1 is enabled and disabled, takes the ring from DMA channel 13, and moves that channel's read address on as it composes. So the lead of 96 frames behind slot 0, the keep-alive, the resync and slip counters and the pin handling work as they did.
- **The ring** is aligned to a cache line. Left as DSPi declares it, it began 40 bytes into a line in this image, so every second frame lay across two lines. Aligning it took 8 cycles a frame off the composer and 0.5 us off the block.

**The ring format** (`smi_frame.h`). A frame is still 8 words, so the ring's size and the read address arithmetic are DSPi's.

| Word | Holds |
|---|---|
| 0 to 7 | channel 1 to 8's sample in bits 0 to 23 |
| 7, bit 31 | the line level the frame starts from |

A frame sends an even number of 1 bits apart from its samples' own (2 in the sync pattern and 6 before the nibbles of each sample). So it ends at its starting level XOR the parity of its eight samples, which is how `smi_adat_write` carries the level from frame to frame, and a silent frame leaves the level as it found it. Because each frame carries its own starting level it can be composed alone, which the composer needs since it skips frames that the SMI ring already holds.

**Writing.** `smi_adat_write` turns DSPi's eight rows into frames four at a time in NEON. It masks the samples to 24 bits, XORs the rows for the parities, and transposes rows to frames with `vzip`. It costs 1.4 us a block, of which 0.3 us is the ring's cache misses, against 11.1 us.

**Encoding** (`smi_adat_levels` in C, `ADATLEVELS` in `smi_frame_a32.S`). The 256 levels are built with the first one sent in bit 0, which is the order the composer stores words in.
- **Reversal.** A sample is sent MSB first, so each word is bit-reversed. AArch32 NEON has no bit reversal, so the eight words go through core registers (`rbit`).
- **The 1 before each nibble.** Nibble g of the reversed sample moves from bit 4g + 1 to bit 5g + 1 in three steps of shift and select (up 4, up 2, up 1), and the six 1 bits are ORed in. That is 30 bits a sample, all eight samples at once.
- **Packing.** Two samples make 60 bits (`vsli`), and the four 60-bit fields follow the 16 bits of sync into four 64-bit words.
- **NRZI.** A 1 is a change of level, so the levels are the running XOR of the bits, which is six shift-and-XOR steps in 64-bit lanes. Each 64 bits is then inverted if the level before it is 1, which is the frame's starting level XORed with the last level of each 64 before.
- **One level a word.** Squaring a byte as a polynomial (`vmull.p8`) moves bit i to bit 2i. Twice gives a byte for each cell time with its two words' levels in bits 0 and 4, which a shift and a mask split into the two high bytes that `vst4.8` stores beside the cell's low byte. The 256-entry table and its 32 loads a frame are gone.

**Proof that the bits are the same.** `tests/smi_frame_test.c` keeps DSPi's encoder (`adat_encode_frame` and its tables, as in `adat_output.c`). It prints PASS on the Mac and under QEMU.
- Runs of 1 to 192 frames of random, all-zero, all-one, sparse and sign-extended samples, and runs of silence, go through DSPi's encoder a frame at a time and through `smi_adat_write` and `smi_adat_silence`, from a random line level. Every frame's 256 levels must equal DSPi's words bit for bit, and the level carried out must agree.
- In the composer tests the table-driven reference plays DSPi's words, and `smi_frames_c` and `smi_frames_a32` compose the new ring frames. All 256 words must match for every mix of slot types and clocks, and the keys must skip only a repeat.
- Under QEMU the assembly is also checked against the C on arbitrary ring words.

**What it costs now** (four S/PDIF slots, 48 kHz, 997 Hz tone, analyser running). The middle column is the ring change alone.

| Measure | Before | Ring holds samples | And loops unrolled |
|---|---|---|---|
| Whole core, ADAT on | 9.87% | 9.07% | 8.35% |
| Interrupts, ADAT on | 4.05% | 4.09% | 4.14% |
| Producer, ADAT on | 4.43% | 3.60% | 2.83% |
| Composer, ADAT on, cycles a frame | 476 | 487 to 495 | 494 |
| DSPi block, ADAT on | 39.1 us | 31.1 us | 22.4 us |
| Whole core, ADAT off | 7.61% | 7.63% | 7.21% |
| Producer, ADAT off | 2.73% | 2.78% | 2.34% |
| Composer, ADAT off, cycles a frame | 358 | 357 | 358 |
| DSPi block, ADAT off | 22.4 us | 22.5 us | 18.2 us |
| Whole core, no stream, ADAT on | 1.08% | 1.05% | 1.06% |
| Whole core, no stream, ADAT off | 0.90% | 0.91% | 0.89% |
| Composer, no stream, ADAT on, cycles a frame | 121 | 116 | 117 |
| Whole core, tone at 96 kHz, ADAT off | 12.9% | | 12.1% |

The block profile (`z4_prof.py`) with ADAT on was `process_input_block` 38.8%, `adat_output_push_block` 28.4%, `rta_bass_push` 15.8% and `audio_spdif_ring_write_s32` 7.3% of 39.1 us. It is now 40.6%, 4.8%, 29.5% and 8.9% of 22.4 us.

**Unrolled loops.** `audio_pipeline.c` is now compiled with `-funroll-loops`, one line in `CMakeLists.txt`. This is separate from the ring change and can be dropped on its own. DSPi's sample loops (the mix, the gain, the delays, the peaks and the S24 conversion) are each one chain of VFP operations a sample. GCC does not vectorise float loops for 32-bit NEON without `-funsafe-math-optimizations`, which would change results, and on the in-order A53 a loop of one chain runs at the chain's latency. Unrolled, GCC overlaps the chains of several samples. No arithmetic changes. It takes 4.2 us off every block with ADAT off, and 8.7 us with ADAT on, where the in-place S24 pass gains most.

**The composer does not get cheaper.** Section 27 took the composer's arithmetic to hide under its stores to the uncached ring. It does not, as extra instructions put in the frame path showed.
- A NEON instruction on Q registers costs about 0.9 cycles and one on D registers 0.4, whether it comes before the stores or between them, and whether or not it depends on the one before. 140 on Q registers before the stores cost 134 cycles, and 128 between the stores cost 109.
- A NEON load costs a cycle for each 8 bytes.
- So the encoding costs what its instructions cost wherever it runs. In the composer it is 136 cycles a frame over a frame without ADAT, which is 18 more than the table lookups it replaced. Of the 136, 64 are the key, the pointers and the one-level-a-word step, 16 the reversal through core registers, and 56 the rest of the encoding.
- It stayed in the composer because a frame that the SMI ring already holds is then not encoded at all, as when a stream is open and silent.

**Tried, not kept.**
- The running XOR in 32-bit lanes, with the eight lane levels chained through `vext`: 537 cycles a frame against 519 for 64-bit lanes with the chain in D registers.
- The four constant vectors loaded from `.rodata` in every frame: two such loads cost 24 cycles more than the same loads from the stack, every frame, for a reason that was not traced. The function now copies them to its stack once a call (519 to 495).
- Prefetching the ADAT ring in the composer (`pld` two frames ahead), in the writer (`pldw` three iterations ahead), and at the end of a push for the lines the next block will write: no change in any of the three.
- Encoding in the writer and only spreading the levels in the composer was counted, not built. The composer would fall to about 420 cycles a frame and the block rise by about 3.5 us, the same total, and silent frames would still be encoded.
- Taking ADAT's samples from the slot rings, so that `adat_output_is_active()` could say no and DSPi keep its fused pass. A slot whose two outputs are disabled is written as zeros while ADAT still carries its channels, and a slot that is not enabled gets no ring writes at all, so ADAT would not be DSPi's in those cases. Not done.
- A change to DSPi's `adat_output.c` was not needed, and the submodule is untouched.

**Checks on the board**, on the image described here.
- `adat_check.py --watch 30`: at 48 kHz channel 1 equalled slot 0's I2S word frame for frame, at 44.1 kHz the lag read 7.80 frames, at 96 kHz the pin was held low, and with ADAT disabled GPIO 16 was an input. 30 s at each rate showed no underruns, drops, late compositions, resyncs or slips. With the 1 kHz tone, which repeats in the ring, the composer took 181 cycles a frame (168 before).
- `smi_check.py` with I2S/S/PDIF mixed and all S/PDIF, at 44.1, 48 and 96 kHz, with ADAT enabled and disabled: every slot decoded in all 12. `smi_check.py --watch 60` with ADAT enabled: no underruns, drops or late compositions.
- `spdif_cs_check.py` at the three rates and silent, `mck_check.py` and `output_toggle_check.py` passed with ADAT enabled.
- `spdif_in_check.py --source --watch 20` with ADAT enabled: locked throughout, no decode or parity errors, no underruns. This is also the case of 192-frame blocks.
- **Sample for sample against the image before.** The Mac plays a table of 48,000 samples that it repeats, and DSPi's default path maps each input sample to one output sample. A bench script aligns each snapshot to the table and collects that map for slot 0's left and right and for ADAT channels 1 and 2. On this image the map was a function over 900 snapshots with every run aligned, and ADAT agreed with slot 0 on 8,466 shared input values. Against the image before, with ADAT on and off, the maps agreed on every shared input value (between 1,200 and 2,300 for each of the four, and as many for ADAT against slot 0 across the two images).
- **Enable, disable and rate changes.** A second bench script toggled ADAT 12 times while a tone played, changed the rate six times through 44.1, 96 and 48 kHz, toggled it four times while silent at each rate, and toggled it 20 times at 8 a second. After each step it decoded 15 ADAT frames (sync, forced bits, user bits, channels 3 to 8 silent, and the tone's samples on channels 1 and 2, or silence), and read the status and the pin. Every step was right, with 29 resyncs, no slips and no late compositions. The image before gave the same 29 resyncs and the same pin at every step, including that ADAT enabled while already at 96 kHz leaves the pin an input.

**Not covered.**
- No ADAT receiver has decoded the pin. The checks decode the composed SMI stream.
- `adat_output_pi.c` must follow any change DSPi makes to `adat_output.c`. Nothing checks that it does.
- The unrolled loops were compared sample for sample only on the default path with a tone at 48 kHz. Loops in `audio_pipeline.c` that the default path does not run (several sources mixed into one output, gain ramps, output delays) were not compared between the two images.
- A change of the ADAT pin, and ADAT restored from a preset, from bulk parameters or from flash, were not run. That code is DSPi's, unchanged.
- The two bench scripts are not in the tree.

**Further, not done.**
- With ADAT on the block is still 4.2 us longer than with it off. About 1.1 us is the push and the rest is DSPi's in-place S24 pass and second interleave, which only a change in `output_s24.h` or `audio_pipeline.c` would remove.
- `rta_bass_push` is now 30% of the block. `rta_bass.c` and `dsp_pipeline.c` were not tried with `-funroll-loops`.

## 30. Eight-channel I2S input

DSPi's `i2s_input.c` is now compiled unchanged, and the Pi receives in place of its PIO state machines. One DMA channel copies the GPIO level register (GPLEV0) into a 64 MB cached ring at 128 MB, with no pacing, four snapshots to a 128-bit write. A 500 us system timer interrupt drops the new lines from the cache and decodes them (`i2s_rx_decode.c`, with the scan and frame builder in `i2s_rx_decode_a32.S`) straight into DSPi's four pair rings. It then moves the shim's DMA write addresses as DSPi's ring DMA would. All four data lines come from the same snapshots, so the eight channels cannot drift apart. The driver is `z4/i2s_input_pi.c`.

**Pins.** Data on GPIO 22-25 (pairs 1-4). In DSPi's master and slave roles BCK and LRCLK are the SMI's own, GPIO 12 and 13; in master role the composer now drives them even when no output slot is I2S. With an external master (DSPi's I2S slave mode) its BCK and LRCLK come in on GPIO 26 and 27. As with the outputs, these pins hold whatever DSPi Console's pin settings say.

**The shim.** `pio_enable_sm_mask_in_sync` on pio2 starts the receiver, or, while it runs, makes it frame afresh (DSPi's `i2s_input_resync`). The last `pio_sm_unclaim` on pio2 stops it. PIO IRQ 7, the checked program's framing slip, is raised by the receiver with an external master on a burst of bad LRCLK halves or when the clock follower loses lock. A lone bad half is a capture stall, not a slip. The PIO header comes from DSPi's `i2s_input.pio`, assembled at build time by pioasm from DSPi's own pico-sdk, and `dma.h` and `pio.h` gained the few helpers `i2s_input.c` calls. In DSPi's master role with more than one pair, DSPi reads pair 0 one frame on, since its PIO pair 0 starts a frame early. The decoder therefore writes that line one frame ahead in its ring (`lead` in `i2s_rx_t`).

**External master: shared clocks.** The SMI's clocks are phase-locked to the master's by trimming PLLA. Every output, S/PDIF and ADAT included, then runs on the master's clock, and a DAC may take its clocks from the master directly. In each fourth tick the receiver measures the lag of the SMI's BCK and LRCLK behind the master's from the same snapshots. It does so in the SMI's own bit clocks, since the snapshot rate is only known to a few percent. A loop in the main loop turns the lag into the trim, which is limited to ±250 ppm. Until the pipeline runs at the master's rate the follower waits, and DSPi's own rate detection makes that change. DSPi's soft VCXO commands are not applied while the follower holds PLLA. The prefill that starts the outputs also waits for the follower's lock, about 0.5 s after a start. The trim a rate last locked at is kept, so a restart begins from it.

**The control block and the VideoCore's cache.** The capture's reload reads its control block through the VideoCore's L1 and L2 (bus alias 0x0), the fast reload of the earlier study. The first image to run it fetched garbage: a destination of 0xEF059081 and a wait for a DMA request that never came. The cache held old contents for that address, probably from loading the image. Before each start the receiver now copies the block onto itself with the DMA, reading memory and writing through the same alias. A second channel reads the block in the last 2 ms before each reload, as in the study.

**The snapshot period.** A short calibration run at each start gives the snapshot period. Under load the period was 2.7% longer than the run measured, so each finished lap of the ring now measures it again and updates the decoder's bit clock. The run is 32768 snapshots (1-2 ms). At 131072, USB servicing showed gaps of up to 20 ms over the tests; at 32768 none passed 4 ms. With an external master the run also counts the master's BCK, which chooses 8 DMA wait cycles (16.5 M snapshots a second) at 44.1 and 48 kHz or none (30.9 M) at 96 kHz. Without the count, a 96 kHz master would arrive at the slow setting.

**The main loop.** `z4_mainloop.c` gained main.c's I2S parts:
- the role election;
- the rate-change bracket around a running input;
- the bring-up with its rate and MCK;
- the prefill handshake, lock-gated with an external master (where the outputs also wait for the follower) and silent in the slave role;
- the restart on a lost lock;
- the clock-mode change;
- the restarts around type switches, presets, bulk parameters, factory reset and flash writes, and for a changed channel count;
- the bring-up at boot.

The producer polls the input, lock-gated in the main loop as main.c does, and with an external master on the follower's lock as well. The VideoCore mailbox calls behind the once-a-second sensor readings are held while the receiver runs, since they stall the capture. The I2S stand-ins are gone from `dspi_standins.c`, and the bench captures `gpio_rx.c` and `z4/i2s_rx_pi.c` are replaced by the receiver.

**Measured on the board, before the audit below,** with `tools/i2s_input_check.py`, which needs no wiring. Its loop case puts every slot on I2S and plays DSPi's signal generator on all eight outputs, then decodes the Pi's own outputs (GPIO 8-11) as the input, with every word checked against the sine. Its master case puts every slot on S/PDIF. Its ext case runs the PCM block as an external master (BCK 18, LRCLK 19, a sine on 21) off nominal. Decode figures are cycles a frame; whole core includes DSPi processing eight inputs.

| Case | Rate | Run | Bad halves | Concealed | Stalls put right | Words checked | Decode | Whole core |
|---|---|---|---|---|---|---|---|---|
| loop (slave role) | 44.1 kHz | 20 s | 0 | 0 | 0 | 7.06 M, none wrong | 1285 | 14.6% |
| loop | 48 kHz | 15 s | 0 | 0 | 0 | 5.76 M, none wrong | 1183 | 15.1% |
| loop | 96 kHz | 20 s | 0 | 0 | 0 | 15.4 M, none wrong | 1161 | 28.0% |
| master | 44.1 kHz | 20 s | 0 | 0 | 1 | framing only | 1282 | 12.6% |
| master | 48 kHz | 15 s | 0 | 0 | 0 | framing only | 1197 | 13.1% |
| master | 96 kHz | 20 s | 2 | 2 | 7 | framing only | 1161 | 24.2% |
| ext, +40 ppm | 44.1 kHz | 20 s | 0 | 0 | 1 | 1.76 M, none wrong | 1294 | 14.7% |
| ext, -120 ppm | 48 kHz | 20 s | 0 | 0 | 0 | 1.92 M, none wrong | 1198 | 15.1% |
| ext, +60 ppm | 96 kHz | 40 s | 0 | 0 | 49 | 7.68 M, none wrong | 1171 | 26.8% |

- Decoding is 5.7% of the core at 44.1 and 48 kHz and 11.1% at 96 kHz.
- In the loop case DSPi's eight input peaks equalled its eight output peaks, so every pair reaches its own two channels.
- In the master and ext cases the outputs carried the input once the prefill finished.
- With the external master, DSPi locked and changed rate, and the follower locked 0.5 s after the restart. The SMI's BCK then stayed within -1.05 to +1.15 ns of the master's at 44.1 kHz, -0.69 to +0.72 ns at 48 kHz and -0.42 to +0.48 ns at 96 kHz, on trims of +44.9, -125.0 and +61.0 ppm.
- `smi_check.py`, `spdif_cs_check.py`, `mck_check.py`, `adat_check.py --watch 20` and `spdif_in_check.py --source --watch 20` passed afterwards. The idle load is unchanged at 0.87%, and `usb_health.py` showed no gap over 4 ms.
- `tests/i2s_rx_decode_test.c` passes its 36 cases on the Mac and under QEMU with the assembly, including the new ring lead.

**Zero loss: the runs of the final image.** The requirement is no concealed frame, wrong word, bad LRCLK half, slip, restart or output underrun at any rate. Each run below is 5 minutes on the final image, with `tools/i2s_input_check.py` as it now is. Every run passed.

| Rate | Case | Bad halves | Concealed | Words checked | Wrong | Stalls put right | Slips, restarts | Composer late | Slot 0 underrun |
|---|---|---|---|---|---|---|---|---|---|
| 44.1 kHz | loop | 0 | 0 | 105.8 M | 0 | 0 | 0 | 0 | 0 |
| 44.1 kHz | master | 0 | 0 | framing only | | 0 | 0 | 0 | 0 |
| 44.1 kHz | ext, +40 ppm | 0 | 0 | 26.5 M | 0 | 0 | 0 | 0 | 0 |
| 48 kHz | loop | 0 | 0 | 115.2 M | 0 | 0 | 0 | 0 | 0 |
| 48 kHz | master | 0 | 0 | framing only | | 0 | 0 | 0 | 0 |
| 48 kHz | ext, +40 ppm | 0 | 0 | 28.8 M | 0 | 0 | 0 | 0 | 0 |
| 96 kHz | loop | 0 | 0 | 230.4 M | 0 | 4 | 0 | 0 | 0 |
| 96 kHz | master | 0 | 0 | framing only | | 291 | 0 | 0 | 0 |
| 96 kHz | ext, +40 ppm | 0 | 0 | 57.6 M | 0 | 0 | 0 | 0 | 0 |

**96 kHz is not safe yet, though these runs are clean.** At 44.1 and 48 kHz, with the wide bursts, the capture lost no BCK rise in any run of any image, the added write load included. At 96 kHz it stalls for two to five snapshots (65 to 160 ns) and a rise goes unseen. A bit clock is 163 ns there. The decoder puts a lost rise back when the spacing of the rises shows where it was (a stall put right), and in the loop and ext cases every word that came out of that was right. When the stall is near a whole bit clock nothing shows where the bit went. The result is then a concealed frame or a bit put back wrongly with no counter raised. How often the capture stalls depends on the image, for a reason that was not found (below). Three earlier images of nearly the same code failed their 5-minute runs at 96 kHz:
- One had 610 stalls put right in the loop case and 12 wrong words with no concealed frame.
- One had 605 in the loop case with 1 concealed frame and 36 wrong words, and 1125 in the master case with 2 concealed frames.
- One, with the same layout as the final image, lost a frame to the decoder fault at the reload described under the fixes, which is now gone.

So the final image is clean at 96 kHz because its layout happens to stall rarely in the loop and ext cases, and the master case still leans on 291 repairs that no test can check for wrong bits. The table further up is from before the audit, and its 20 s runs understate the losses of that build: 60 s runs of it gave 5 concealed frames in the master case and 2 in the loop case.

**What the stalls are (audit, 2026-11-13).** The bench can count the snapshots between BCK rises over every snapshot (`--hist`), which shows each stall, and it keeps the snapshots around each bad half. The counting costs about 30% of the core in the tick interrupt and raises the stall rate, so its figures compare settings and are not rates in use. The figures in this table are from the master case at 96 kHz unless said, and each pair is from one image.

| Change | Lost rises or stalls | Without it |
|---|---|---|
| Main loop reading the timer for 100 us a pass | 28 lost rises in 15 s | 17 |
| Main loop reading 2000 cache lines of far SDRAM a pass | 104 | 17 |
| Main loop writing 16 KB of uncached memory a pass | 417 | 17 |
| Main loop reading 16 KB of uncached memory a pass | 26 | 17 |
| The same writes at 48 kHz (8 wait cycles) | 810 lost rises in 15 s | 0 |
| Wide bursts allowed, with those writes | 48 stalls a second | 138 |
| Wide bursts allowed, with those writes, another image | 1.4 stalls a second | 13 |
| Wide bursts allowed, no added load | 1 lost rise in 15 s | 68 |
| Wide bursts allowed, 48 kHz, with those writes | none in 15 s | 2359 lost rises |
| DMA priority and panic priority 0, 8 or 15 | no difference | |
| DMA channel 2 or 5 for the capture | no difference | |
| 32-bit destination writes | 49000 stalls a second | 13 |
| Waiting for each write response | 600000 stalls a second | 13 |
| Destination through the VideoCore's L2 (bus alias 0x4 or 0x8) | stale data, thousands of bad halves | |
| DMA bursts of 2 to 4 reads | 49 to 69 M snapshots a second, thousands of bad halves | |
| Wide bursts and 1 or 2 wait cycles (26.3 or 24.5 M snapshots a second) | 3 to 8 lost rises in 60 s | |
| SMI ring cached, composer cleaning it by line (60 s, loop, master, ext) | 36, 7, 51 stalls put right | 23, 3, 52 |
| Decoder scanning 512 snapshots a block instead of 2048 (30 s, loop) | 0 and 1 | 1 and 0 |

- ARM writes to SDRAM cause the stalls. Uncached writes do it most, and reading far memory does it through the dirty cache lines it pushes out. Reads of uncached memory and of peripherals do not.
- The capture's control block had the no-wide-bursts bit set, which makes each 128-bit write two transfers. Without the bit a stall long enough to lose a rise was 3 to 50 times rarer in every pair of runs, and the stalls that remain are shorter, so nearly all of them can be put right. 48 kHz stayed clean under a write load that lost 2359 rises before. The receiver now leaves the bit clear, in the calibration run as well.
- The composer's uncached stores are not the main cause. With the SMI ring mapped cached and written back by cache line, the stall counts did not change and the composer cost 383 cycles a frame against 325 to 355. That change was taken out again.
- The stalls come while the main loop and the other interrupts run, not during the tick. In 5-minute runs without the counting, 7 of 426 stalls put right fell in the first two sixteenths of the tick's snapshots, which is when the tick interrupt itself runs, and 84 fell in the two after it. An even spread would put 53 in each pair.
- The stall rate depends on the image far more than on anything else tried, and what decides it was not found. Images that differ by a few instructions or a few bytes of state give 0 to 4 stalls a second, differently for each role, and reloading the same image gives the same rates again. The table gives stalls put right a minute at 96 kHz, from runs of 30 s to 5 minutes. None of these explains it.

| Image | Loop | Master | Ext |
|---|---|---|---|
| The plain tree, three builds of it (stalls put right a minute) | 108 to 153 | 225 to 229 | 145 |
| One more field in the decoder's state, before its code buffer | 0 to 4 | | |
| The same field after the code buffer | 150 | | |
| The decoder's state moved by 4, 8, 12, 16 and 60 bytes | 2 to 16 | | |
| The code buffer on a cache line of its own | 0 | 256 | 2 |
| Every bss object of the port on a cache line of its own | 4 | 244 | |
| An unused branch added in the decoder (same data layout as the plain tree) | 0 | 16 | 0 |
| Every function on a 64-byte boundary (`-falign-functions=64`) | 0 | 9 | 24, one concealed |
| The decoder's two assembly routines on 64-byte boundaries (kept) | 1 | 61 | 0 |

- In one image, reading the code buffer before the scan wrote it, the scan block at 512 snapshots, and PLLA trimmed by 0, 30 and 200 ppm made no difference. The rates of L2 write-backs and of write streaming (PMU events 0x18, 0xC4 and 0xC5) differed by less than half between a case with no stalls and one with 4 a second.
- The two assembly routines of the decoder now start on 64-byte boundaries, as the composer's and the S/PDIF decoder's already did. That gave the best image measured, but it is one more layout, not a cure: any later change to the code may move the rates again.
- Not tried: the tick or the composer on another core, a shorter tick, a second capture channel offset from the first, and turning off the Cortex-A53's write streaming in CPUACTLR (the access may trap in the firmware's monitor, which would hang the board).

**The hangs, and the DMA abort.** The board stopped answering twice during the audit, each time straight after the input source went from I2S back to USB. It stayed enumerated, timed out every control request and needed a power cycle. The first time was on an image with the experiments in it, the second on the plain build. `stop()` reset the capture channel while it was mid-transfer, and both hangs came on images with the wide bursts. Every reset of a channel that may be active now pauses it first, waits for its outstanding writes (CS bit 6) and only then resets, as Linux's `bcm2835_dma_abort()` does. With that, 170 switches from I2S back to USB passed with no hang, 40 of them on the final image: master and ext roles at the three rates, with rate changes while the input ran, and 53 of them with MCK on. The hang was not reproduced on purpose without the change, since each try needs a power cycle, so the abort is the likely cure and not a proven one.

**Follower at a master that is exactly nominal.** With the bench master at exactly 48000 Hz the follower never locked (8 tries of 8): the lag swung between -57 and +107 ns and the trim between -0.4 and +16 ppm. At 48001 Hz and at 48002 Hz it locked at once, as it did at exactly 44100 and 96000 Hz. The 48 kHz PLLA row has a fraction of zero, so a trim that crosses zero changes NDIV and FRAC in two writes (`write_multiplier()` in `pll_out.c`), and the clock jumps between them. A master within a few tenths of a ppm of the Pi's own crystal at 48 kHz would do the same. This is in the PLL code, which the S/PDIF input's servo also uses, and was not changed.

**Fixed in the audit.**
- With an external master, the main loop delivered input blocks as soon as DSPi's rate detection locked, about 340 ms before the clock follower. Those blocks ran `preset_loading` out, so the prefill handshake never ran: the outputs were never restarted together and the DAC hardware mute was never released. The lock-gated poll now waits for the follower too. Checked on the board with the DAC hardware mute enabled on GPIO 17: in the ext, master and loop cases the outputs were drained once and started together once, the mute was released, and the follower kept its lock. Every run of the tool now checks the drain and the start.
- The deferred rate change passed `defer_output_to_input_prefill` as false with I2S as the source. It now passes true, as main.c does. Checked on the board: rate changes to 96, 44.1 and 48 kHz with the I2S input running each gave one input restart, one drain and one synchronized start, with no composer underrun.
- `prepare_pipeline_reset()` did not clear `i2s_prefilling` as main.c does. A reset during a prefill left the flag set, and the next pass enabled the outputs with no drain. The rate changes above pass through this code, but a reset in the middle of a prefill was not forced.
- MCK kept running after a switch away from an I2S input that was the clock master. It is now stopped there, as in main.c. Checked on the board: GPCLK0 ran with the I2S input as master and stopped on the switch back to USB.
- The decoder took two ordinary rises six snapshots apart for a lost rise when a lap's reload fell between them at the end of an LRCLK half. The next half then had 33 bit clocks and its frame was concealed, about once in 600 laps at 96 kHz (one in a 5-minute ext run, with 6 wrong words). The reload hardly lasts longer than a snapshot, so the distance that shows a lost rise across it is now the same as anywhere else (`gap_dist` in `i2s_rx_decode.c`). The host test has a new case with brief, frequent reloads, which the old distance fails (2 bad halves in 1265 reloads) and the new one passes.
- The bench settings and the follower's measurement are now copied with interrupts off.
- `tools/i2s_input_check.py` passed a run with up to two concealed frames and the wrong words they cause, and its sine tolerance of 3000 let a wrong low bit through. It now fails on any concealed frame, bad half, wrong word (tolerance 16 in the loop case, where a clean run's worst is 3, and 200 in the ext case, where it is 122), overrun, slip, restart, composer underrun or slot 0 underrun. It checks that the prefill handshake finished, prints how each stall was put right and why a bad half was not, and puts the I2S input rate back.

**The other checks on the final image.** `smi_check.py`, `spdif_cs_check.py`, `mck_check.py`, `adat_check.py --watch 20` and `spdif_in_check.py --source --watch 20` passed after the runs above. The idle load was 0.97 to 1.05% at 48 kHz, and `usb_health.py` showed no dropped event and one gap of 20 ms between `tud_task()` calls since boot. The decoder test passes its 37 cases on the Mac and under QEMU with the assembly.

**The decoder test passes by its random sequence.** A new case put before the others changed the sequence the later cases draw from. The case "96 kHz, stalls 145 ns, tco 6 ns" then gave 8 wrong words outside concealed frames, which its own rule (nothing silent) forbids, and one round of the fast-against-plain comparison differed. The first is the decoder's limit at stalls near a whole bit clock, as on the board. The second may be the test's own doing, since it refills one buffer while the fast path still holds codes scanned from it, but that was not confirmed. Neither was changed. The new case sits last so that the suite keeps its sequences.

**Not covered.**
- No real external master, source or DAC has been connected. The external master is the PCM block, on the Pi's own crystal, so a real master's drift and jitter are untested.
- The master case cannot check data. Its lines carry S/PDIF, and nothing synchronous to the SMI's clocks other than its outputs exists without wiring. The ring lead of the master role is checked only by the decoder test.
- A master that stops, returns or changes rate while locked, and the slip path, were not run on the board.
- With no master connected the follower never measures, so PLLA stays at the trim the rate last locked at (within 250 ppm) while the outputs wait muted.
- A master more than 250 ppm off cannot be followed, and the input then never starts. DSPi's own soft VCXO allows 300 ppm.
- DSPi measures the master's rate from the ring write address over 32 ms windows and allows 2%. The write address moves once a tick, which alone is up to 1.56% of a window, so a tick held off by 140 us at a window edge would drop the lock. None was seen in the runs.
- Restarts from presets, bulk parameters, flash writes, clock-mode changes and channel-count changes were not run with I2S as the source. The code follows main.c.
- DSPi's I2S slave status reports the measured rate from the receiver's word count, as on the RP2350, but its servo commands are ignored while the follower holds PLLA.
