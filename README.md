# DSPi Zero

A bare-metal port of [DSPi](https://github.com/WeebLabs/DSPi) to the Raspberry Pi Zero 2 W. DSPi's own firmware
is compiled unchanged from the `DSPi` submodule. This repository holds only the port: the Pi's platform code, a
shim that stands in for the RP2350's hardware, the USB stack's Broadcom driver, and bench tools.

The Zero 2 W enumerates as a USB Audio Class 2 device at High Speed, runs DSPi's processing pipeline, and sends
every output slot (I2S or S/PDIF) from one parallel SMI stream. The work is still in bring-up.

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

The board boots a small USB loader from its SD card, and `firmware/PiZero/tools/pizero_load.py` sends it
`pizero_z4.img`. `Documentation/Porting/PiZero_Z0_bringup.md` covers the SD card, the loader and each bring-up
stage.
