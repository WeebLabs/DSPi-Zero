# DSPi on Raspberry Pi Zero: Port Intentions and Initial Testing

*Created: 2026-11-03*

## 1. What we are trying to do

We want to find out whether DSPi can run bare metal on a Raspberry Pi Zero 2 W, with no Linux underneath it. If it can, the Zero 2 W would be a much more capable DSPi platform than the RP2350. The aim is to keep the DSP pipeline and the vendor control interface, and to rebuild the I/O layer underneath them.

The first goal is deliberately small. The Pi should boot our own firmware, appear to the host as the DSPi USB audio device, receive the audio stream, and run it through the DSPi DSP pipeline. Everything else (S/PDIF, ADAT, multi-slot I2S, HDMI) comes after that works.

This fork lives at `~/DSPi-Zero` and is kept local. It was cloned from `~/DSPi` at `9ffce34` (`release/v1.1.6`). The `rp-upstream` remote points at `~/DSPi`, so portable DSP changes can be pulled across. Work happens on `zero-port-bringup`. The RP repo is never edited for this port.

## 2. Why the Zero 2 W, and why test on a first-generation Zero

| | RP2350 (today) | Pi Zero (bench unit) | Pi Zero 2 W (target) |
|---|---|---|---|
| CPU | 2x Cortex-M33 at 307.2 MHz | 1x ARM1176JZF-S at 1 GHz | 4x Cortex-A53 at 1 GHz |
| FPU | Single precision, DCP for doubles | VFPv2, single and double | VFPv4 and NEON, single and double |
| RAM | 520 KB SRAM | 512 MB DRAM | 512 MB DRAM |
| USB device | Full Speed (12 Mbit/s) | High Speed (480 Mbit/s), DWC2 | High Speed (480 Mbit/s), DWC2 |
| Programmable I/O | PIO | None (SMI, PCM, PWM, DMA) | None (SMI, PCM, PWM, DMA) |
| Boot | Onboard flash | SD card or USB boot via GPU firmware | SD card or USB boot via GPU firmware |

The Zero 2 W is estimated to be about 5 times faster per core than the RP2350 at 307.2 MHz, and about 10 times faster across the whole chip. These numbers are estimates from clock speed and instructions per cycle. They have not been measured.

*Update (2026-11-11): a Zero 2 W is now on hand and passed Z1. Milestones from Z2 on run on it.*

The first-generation Zero was the only unit on hand when this plan was written. It has the same SMI, PCM, DMA, USB and HDMI blocks as the Zero 2 W, so it is a valid platform for proving the USB path and measuring the I/O blocks. It is not useful for DSP performance work. It has one core, no NEON, and runs only 32-bit code. Its peripheral base address is `0x20000000`, while the Zero 2 W's is `0x3F000000`.

## 3. What carries over and what does not

These parts should port with little change:

- The RP2350 float DSP pipeline (`dsp_pipeline.c` and the filter, crossover, leveller, limiter and loudness modules). It is plain C. The DCP helpers in `dcp_inline.h` become ordinary `double` arithmetic, because both Pi chips have hardware double precision.
- The vendor command dispatcher, bulk params and notify endpoint. These sit on TinyUSB already. *Update (2026-11-11): the Zero uses UAC2 at High Speed through TinyUSB's own audio class instead of DSPi's UAC1 class driver. See "USB audio: UAC2 at High Speed" below.*
- The wire format, so DSPi Console can talk to the device unchanged apart from recognising a new platform ID.

These parts have to be rebuilt:

- Every PIO-based interface. That covers S/PDIF TX and RX, ADAT TX and RX, I2S slave, PDM and IR capture.
- Clock tracking. The soft VCXO trims the RP PLL. On the Pi this becomes a trim of a BCM PLL, whose registers are documented only through the Linux drivers.
- Preset storage and boot. There is no onboard flash. Presets would live on the SD card.
- Multicore. The first-generation Zero has one core, so the Core 1 EQ worker is disabled. The Zero 2 W brings back multicore later.

## 4. The longer-term I/O plan (not part of initial testing)

This section records the current thinking so later steps start from it. None of it is proven.

- **All outputs on SMI.** SMI (secondary memory interface) is an undocumented parallel port that can stream a DMA buffer onto up to 16 GPIO pins at a set rate. Running it at 256 fs (12.288 MHz at 48 kHz) lets one stream carry four S/PDIF lines, I2S BCK, LRCLK and data, and ADAT. Every pin changes on the same bus cycle, so slot alignment is structural. Pin functions are chosen per pin, so SMI can avoid the UART (GPIO 14/15) and the PCM pins (GPIO 18 to 21).
- **Inputs on the PCM block.** SMI works in one direction at a time, so it cannot sample inputs while it streams outputs. The PCM block's data input would act as a one-bit oversampler for S/PDIF, with software decoding. ADAT input is the riskiest item because it needs roughly 50 to 100 MHz sampling.
- **I2S slave is stereo only.** The PCM block can follow an external master for one stereo pair. SMI cannot be clocked externally, so multi-slot outputs cannot be locked to an external I2S master.
- **HDMI.** The HDMI block supports 8-channel LPCM, and the Linux vc4 driver shows how to set it up. Its audio samples are paced by the HSM clock (`HDMI_MAI_SMP`, a fraction of HSM), and the sink rebuilds its audio clock from the pixel clock and the N/CTS numbers. So the clock that must stay locked to the SMI is HSM, which can run from `plla_per` like the SMI does (`PiZero_PLLA_plan.md` section 1.3). An earlier version of this line said the audio clock came from the pixel clock. The receiver's own processing delay is outside our control.

## 5. Initial testing

The initial target is the first-generation Zero. Each milestone has a single pass condition, and the next one does not start until it passes.

### Hardware needed

- The Pi Zero and a micro SD card.
- A 3.3 V USB-to-UART adapter on GPIO 14 (TX) and GPIO 15 (RX) for logging.
- A micro USB data cable from the Mac to the Zero's **USB** port (not the PWR port).
- The I2S DAC module already used for the STM32 bring-up, for milestone Z5.

### Z0. Bare-metal boot and logging

Build a minimal image with `arm-none-eabi-gcc` (`-mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard`). Boot it from the SD card with the GPU firmware files (`bootcode.bin`, `start.elf`, `fixup.dat`) and a `config.txt` that names our `kernel.img`. Blink the ACT LED and print over the UART.

**Pass:** the LED blinks at 1 Hz and the UART prints a heartbeat with the ARM and core clock rates read from the mailbox.

Worth checking at this step is whether USB boot (`rpiboot` from the Raspberry Pi `usbboot` tools) works on this unit. It would let us load new builds over USB instead of swapping SD cards, which would speed up every later milestone.

**Status (2026-11-04): Z0 passed.** On a Pi Zero W (`boardrev 9000c1`, firmware `1.20260915`) the ACT LED blinks at 1 Hz and UART0 prints one heartbeat per second. The heartbeat shows the measured ARM clock at 1000000000 Hz and the core at 400000000 Hz. USB boot with `rpiboot` is still untested, because the Mac never saw the board through the hub. The history below records how it got there.

**Status (2026-11-03):** The Z0 image builds cleanly from `firmware/PiZero/` (3916-byte `kernel.img`). The first image on hardware left the LED solid instead of blinking. On 2026-11-04 a second image, with an early LED signal and its own mailbox code, blinked the ACT LED at 1 Hz, so the LED half of the pass condition is met. An RP2040 bridge then showed the UART heartbeat's numbers correctly but its text garbled. The firmware had loaded the image at `0x0` instead of `0x8000`. `config.txt` now sets `kernel_address=0x8000`, which is not yet confirmed on hardware. The board is a Zero W (`boardrev 9000c1`). USB boot with `rpiboot` has not worked yet, because the Mac never saw the board. SD card preparation, UART wiring, the LED failure codes and the `rpiboot` steps are in `PiZero_Z0_bringup.md`.

### Z1. TinyUSB enumeration

Fetch TinyUSB's Broadcom dependency (`python3 tools/get_deps.py broadcom_32bit` inside `DSPi/firmware/pico-sdk/lib/tinyusb`, which pulls Adafruit's `broadcom-peripherals` into `hw/mcu/broadcom`). Build a vendor-class device on the `raspberrypi_zero` board. Force Full Speed with `CFG_TUD_MAX_SPEED = OPT_MODE_FULL_SPEED`, so the existing UAC1 descriptors and the 3-byte 10.14 feedback format carry over unchanged. High Speed is a later step.

**Pass:** macOS enumerates the device at 12 Mbit/s, and a pyusb vendor echo test round-trips byte-perfect.

**Status (2026-11-11): Z1 passed on a Zero 2 W.** The first-generation Zero W never showed up on USB, either for `rpiboot` or for our image, and its USB port is suspected dead. The same sources now build for the Zero 2 W with `-DPIZERO_BOARD=zero2`. Booted from an SD card, it enumerated at Full Speed and passed the echo test, plus a 500-round soak with no mismatches. Full Speed is forced with `DCFG.DSPD = 1` on the High Speed PHY rather than `OPT_MODE_FULL_SPEED`. `rpiboot` from this Mac loads `start.elf`, but every later file transfer times out, so SD boot remains the workflow. Details are in `PiZero_Z0_bringup.md` sections 9 and 10.

### Z2. UAC1 enumeration as DSPi

Bring across the DSPi UAC1 class driver and descriptors with the stereo alts only. Return a fixed nominal feedback value.

**Pass:** Core Audio lists the DSPi output device, and `system_profiler SPAudioDataType` shows it at 48 kHz.

**Status (2026-11-11): Z2 passed on the Zero 2 W.** `firmware/DSPi/usb_descriptors.c` is compiled through its RP2040 (stereo-only) path, with a one-line stand-in for `pico/unique_id.h`. The product string is renamed to `Weeb Labs DSPi Zero` in a generated copy, so the DSPi file stays untouched. A cut-down copy of DSPi's UAC1 class driver (`firmware/PiZero/z2/uac1_z2.c`) claims the audio function and the vendor interface. The Mac enumerated `Weeb Labs DSPi` (`2E8B:FEAA`, bcdDevice 2.04) at Full Speed with the board's VideoCore serial, and `system_profiler SPAudioDataType` listed it with 2 output channels at a current rate of 48000 Hz. macOS also made it the default output device. Details are in `PiZero_Z0_bringup.md` section 11.

### Z3. Isochronous data flowing

Count isochronous OUT packets and bytes, and report them on the UART. Exercise switching the device away and back in Audio MIDI Setup. The STM32 port found that DWC2 isochronous endpoints need TinyUSB's `iso_alloc` and `iso_activate` pattern and that the feedback endpoint must be armed from the SOF callback. The same rules are expected to apply here.

**Pass:** the packet count rises at 1000 per second while the host plays, and streaming survives ten switch-away and switch-back cycles without the host suspending the device.

**Status (2026-11-11): Z3 passed on the Zero 2 W.** With a 1 kHz tone playing, the board received 1000.0 packets per second (192000 bytes per second, alt 1, 48 kHz), and the rate was 999.5 to 1000.5 after each of ten switches to BlackHole 2ch and back. There were no suspends, and feedback kept flowing at about 250 per second. Three DWC2 problems had to be fixed first: a hang when streaming restarted, frames lost to a blocking UART print, and feedback packets queued on the wrong frame parity. Counters are read over EP0 with a bench-only vendor request, so no UART is needed. Details are in `PiZero_Z0_bringup.md` section 12.

### Loading builds over USB

*Added: 2026-11-11.* The SD card now holds a USB loader instead of a milestone image. `tools/pizero_load.py` reboots a running image to the loader and sends the new build over USB, so no card swaps are needed. A round trip takes about 7 s. See `PiZero_Z0_bringup.md` section 13.

### USB audio: UAC2 at High Speed

*Added: 2026-11-11.* The Zero 2 W's USB is High Speed, so the Zero moves from UAC1 at Full Speed (Z2 and Z3) to UAC2 at 480 Mbit/s, using TinyUSB's built-in audio class. DSPi Console is unaffected, because it talks to the board only over EP0 and the vendor interface, which keep the same layout. The work is in `firmware/PiZero/uac2`, with these supporting pieces.

- **Interrupt-driven USB.** At High Speed the bus runs in 125 us microframes, too fast for a polled main loop that will also run DSP blocks. GPU IRQ 9 now reaches the ARM, and `startup.S` saves the VFP caller-saved registers in the IRQ entry.
- **A patched DWC2 driver** (`firmware/PiZero/tusb_dwc2/dcd_dwc2.c`). It fixes the slave-mode OUT disable hang and isochronous frame parity in the driver, so any class driver benefits.
- **The test.** `tools/uac2_stream_check.py` plays straight to the Pi's output by name, so the Mac's default output is never changed.

Details are in `PiZero_Z0_bringup.md` section 14. Z4 builds on this front end, with the DSP pipeline, vendor commands and bulk params compiled unchanged against stand-in Pico SDK headers.

### Z4. DSPi base running on the received audio

Bring in the RP2350 float pipeline, the vendor command dispatcher and bulk params. Add a new platform ID (`WIRE_PLATFORM` value to be chosen; the STM32 fork already uses 2). Drive the pipeline from the USB receive path. With no output hardware yet, pace consumption from the 1 MHz system timer so the buffer fill behaves the way it will with a real output.

**Pass:** DSPi Console connects, reads back valid bulk params and shows live input meters. EQ, gain and matrix changes made in Console take effect, as confirmed by the output meters. The CPU meter (`GET_STATUS` wValue 9) gives a first ARM1176 load figure at 48 kHz stereo.

DSPi Console will need a small patch to name the new platform. The STM32 port needed that in three places in the Console source.

**Status (2026-11-11): Z4a done on the Zero 2 W (pipeline only).** DSPi's unchanged float pipeline ran on the UAC2 stream through 10 stream restarts with no lost audio. DSPi reported `cpu0_load` 35%, with blocks averaging 340 us for 1 ms of audio. That is single core with the D-cache off, so it is a starting figure, not a tuned one. Z4b brings in vendor commands, bulk params and DSPi Console, with a new platform ID. Details are in `PiZero_Z0_bringup.md` section 15.

**Status (2026-11-12): Z4 passed on the Zero 2 W.** Z4b added DSPi's vendor commands, bulk params, presets and notifications, all compiled unchanged, plus the MMU and D-cache and the full 1000 MHz ARM clock. DSPi Console connects, names the board "Raspberry Pi Zero 2 W", shows live input and output meters, and applies EQ changes. `cpu0_load` is 2% at 48 kHz stereo, with blocks averaging 24.2 us, after replacing newlib-nano's byte-at-a-time `memset` (bring-up doc section 17). DSPi's control-plane harness passes everything except platform differences, stale expectations and the RTA busy-time budget, which is still to investigate. Details are in `PiZero_Z0_bringup.md` section 16.

### Z5. Stereo I2S out (listening test)

Drive the PCM block as I2S master into the DAC module, clocked from PLLD through the clock manager's fractional divider. This clock has jitter, which is acceptable for bring-up. Replace the timer pacing with PCM DMA consumption, and derive the USB feedback from it.

**Pass:** host audio plays cleanly through the DAC with DSP applied, and the buffer fill stays centred over a 30-minute run.

**Status (2026-11-12): output path working, listening test pending.** When slot 0 is set to I2S in DSPi (Console, or `REQ_SET_OUTPUT_TYPE`), DSPi's clock-master I2S slot plays on the PCM block through a DMA loop over DSPi's own slot ring, as the RP2350's free-running DMA does. Every other ring and the USB consumer now follow the DMA's frame count, so the host's feedback tracks the DAC clock. The DMA reads at the sample rate at 44.1, 48 and 96 kHz with no PCM underflows or DMA errors. Details are in `PiZero_Z0_bringup.md` section 18.

**Status (2026-11-12): all four output slots on the SMI, listening test pending.** The section 4 output design now runs. One SMI stream at 256 fs carries every slot as S/PDIF or I2S, on GPIO 8 to 11, with I2S BCK and LRCLK on GPIO 12 and 13. Every slot decodes correctly from the composed stream at 44.1, 48 and 96 kHz, and the stream held steady with no underruns. The PCM block is free again for inputs. Along the way it turned out macOS reads High Speed feedback per data-endpoint service interval. The feedback now comes from DSPi's own controller in full 16.16, fed from the USB interrupt and the SMI's DMA count, and the TinyUSB FIFO no longer adds about 12 ms of latency. Details are in `PiZero_Z0_bringup.md` sections 19 and 20.

## 6. Bench measurements for the I/O plan

These come after Z5. They decide whether the section 4 design is possible or only the output half of it.

| Measurement | Why it matters |
|---|---|
| Maximum sustained SMI write rate, with DMA, on 8 and 16 pins | Must reach 12.288 MHz (256 fs at 48 kHz) for ADAT and S/PDIF on one stream. Higher rates need more. **Measured 2026-11-12 on 16 pins: 12.288 and 24.576 MHz stream cleanly, and the real ceiling is about 37 MHz (bring-up section 19).** |
| Maximum sustained SMI read rate | Decides whether SMI could ever capture inputs. |
| Maximum PCM bit clock in master receive mode | Sets how fast S/PDIF and ADAT can be oversampled, and therefore which input rates are possible. **Throughput measured 2026-11-12: the block and its DMA capture every bit clock up to 250 MHz, the fastest clock tried, with no FIFO overflow (section 6.1). Sampling a real signal at those rates is not yet checked.** |
| Pin-to-pin skew across SMI outputs | Confirms the slot alignment claim with a logic analyser. |

### 6.1 PCM receive rate (2026-11-12)

`pcm_rx.c` runs the PCM block as clock and frame master with two 32-bit receive channels filling each 64-bit frame, so every bit clock is one captured bit. A DMA channel loops over a 256 KB ring in the uncached region. Bench request `0x47` runs a capture from the main loop, and `tools/pcm_rx_check.py` sweeps the PCM clock. For each clock, it compares the bits captured per second with the clock read by the clock manager's counter.

| Source | PCM clock | Captured | RX FIFO overflows |
|---|---|---|---|
| PLLA /30 to /2 | 12.288 to 184.320 MHz | equal to the clock (+0.0 ppm) | 0 |
| PLLD /4, /3, /2 | 125.0, 166.7, 250.0 MHz | equal to the clock within 2.6 ppm | 0 |

At 250 MHz the DMA writes about 31 MB/s with no errors. No faster clock is available from an integer divider of these sources, so 250 MHz is a lower bound on the limit, not the limit itself. The 2.6 ppm on PLLD appears at every PLLD divider and not on PLLA. Its cause is not known.

**Not yet checked.** GPIO 20 (PCM_DIN) had nothing connected, so every captured bit was 0. Whether the block samples a real signal correctly at these rates needs a signal on the pin. The simplest is a jumper from an SMI output, such as slot 1 on GPIO 8 (header pin 24), to GPIO 20 (header pin 38), and then decoding the captured S/PDIF.

## 7. Open risks

- SMI is missing from Broadcom's public datasheet and is known mainly from reverse-engineering write-ups.
- DMA buffers need cache maintenance or an uncached memory region. A mistake here produces corrupted audio that looks like a USB fault.
- First-generation Zero performance for the full 8-channel pipeline is unknown. Single-core load at Z4 will show how much headroom there is before the Zero 2 W is needed.
- The BCM PLL and clock manager registers needed for clock tracking are documented only through Linux driver source.
