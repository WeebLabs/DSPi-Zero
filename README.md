# DSPi Zero

A bare-metal port of [DSPi](https://github.com/WeebLabs/DSPi) to the Raspberry Pi Zero 2 W. DSPi's own firmware
is compiled unchanged from the `DSPi` submodule. This repository holds only the port: the Pi's platform code, a
shim that stands in for the RP2350's hardware, the USB stack's Broadcom driver, and bench tools.

The Zero 2 W enumerates as a USB Audio Class 2 device at High Speed, runs DSPi's processing pipeline, and sends
every output slot (I2S or S/PDIF) from one parallel SMI stream. The work is still in bring-up.

## Using a ready-made SD card

1. Download `dspi-zero2w.img.zip` from this repository's Releases page.
2. Write it to a micro SD card with Raspberry Pi Imager ("Use custom") or balenaEtcher.
3. Put the card in a Raspberry Pi Zero 2 W, and connect the Zero's USB port (not PWR) to a computer. After about
   six seconds it appears as the "Weeb Labs DSPi Zero" USB audio device, and DSPi Console connects to it.

| Signal | GPIO | Header pin |
|---|---|---|
| Output slot 1 (outputs 1-2), S/PDIF or I2S data | 8 | 24 |
| Output slot 2 (outputs 3-4) | 9 | 21 |
| Output slot 3 (outputs 5-6) | 10 | 19 |
| Output slot 4 (outputs 7-8) | 11 | 23 |
| I2S BCK | 12 | 32 |
| I2S LRCLK | 13 | 33 |
| MCK | 4 | 7 |
| ADAT out | 16 | 36 |
| S/PDIF in (3.3 V logic, such as an optical receiver module on 3.3 V) | 20 | 38 |

Pins follow this table whatever DSPi Console's pin settings say. Presets are not yet kept across a power cycle.
The card also carries the USB loader: `firmware/PiZero/tools/pizero_load.py` sends a newer build, which runs until
the power is removed; copy it onto the card as `kernel.img` to keep it. `firmware/PiZero/tools/make_sd_image.sh`
builds the card image on a Mac.

## Layout

- `DSPi/`: DSPi, pinned at the commit the port builds against. It brings the pico-sdk and TinyUSB with it.
- `firmware/PiZero/`: the port. `z4/` is the current application, `tools/` the host-side bench scripts.
- `Documentation/Porting/`: the port plan, the bring-up notes, and the PLLA clock plan.

## Building

```bash
git submodule update --init --recursive
```

```bash
(cd DSPi/firmware/pico-sdk/lib/tinyusb && python3 tools/get_deps.py broadcom_32bit)
```

```bash
cmake -S firmware/PiZero -B firmware/PiZero/build-zero2 -G Ninja -DPIZERO_BOARD=zero2
```

```bash
cmake --build firmware/PiZero/build-zero2 --target pizero_z4
```

For development, `firmware/PiZero/tools/pizero_load.py` sends `pizero_z4.img` to the board's USB loader, either
the loader-only card or the ready-made card above. `Documentation/Porting/PiZero_Z0_bringup.md` covers the SD card, the loader and each bring-up
stage.
