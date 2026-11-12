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
- **Composer.** `pico_shim.c` turns each slot ring into its pin's bits. An S/PDIF cell lasts 2 words and an I2S bit clock 4, at every rate. Slot n is always bit n, so the four slots merge with NEON shift-and-insert, 16 words at a time, and the clocks are ORed in.
- **Ring contents as on the RP2350.** The S/PDIF ring writes now encode exactly as `pico_audio_spdif_multi`'s software encoder: its lookup table, preambles, channel status (with the sample-rate byte) and parity. Each ring holds NRZI cells, where a 1 toggles the line, and the composer turns them into levels with a running XOR. I2S words are stored as `pico_audio_i2s_multi` stores them: `(sample << 8) | 0x01`. The earlier stand-in stored raw values, so section 18's PCM output was 48 dB too quiet.
- **Philips I2S timing.** 64 BCK per frame, LRCLK low for left, data changing on BCK falling and one bit clock behind LRCLK.
- **Output clock.** `shim_output_clock()` is the composer's frame count. The composer is what reads the slot rings, so DSPi's fill, underrun and recovery accounting measure the real reader. Its lead adds about 1.3 ms of latency at 48 kHz.
- **Fill servo.** On the RP2350, DSPi's own feedback steers the slot fill. Here the consumer does it: each 1 ms tick it moves up to 2 frames more or fewer than the composer took, so slot 0's mid-block fill sits at DSPi's target.
- **Uncached memory.** Only DMA control blocks remain uncached. Uncached slot and SMI rings had made the composer cost 3,512 cycles a frame.

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

