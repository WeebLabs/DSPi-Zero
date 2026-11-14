# GPIO assignment on the Pi Zero 2 W: plan

DSPi lets the user move every function to the GPIO they choose. On the Pi, every function sits on a fixed pin today. DSPi stores the pins it is given, but the port ignores them. This plan makes DSPi's pin settings real on the Pi, within what the Pi's hardware allows.

## Where things stand

- **DSPi's pin code runs unchanged.** `vendor_commands.c` handles every pin request: output slots and PDM (0x7C), I2S BCK in both roles (0xC2), the clock-pin mode (0xFE), MCK (0xC6), ADAT out (0xCC), ADAT in (0x6A), the four S/PDIF inputs (0xE4, 0xE9) and the four I2S input pairs (0xF1). It also checks pins for the control surfaces, the UART and I2C control interfaces and the DAC hardware mute. All of them check pins with `is_valid_gpio_pin()` and `is_pin_in_use()`, which follow the RP2350's rules (GPIO 0 to 29, less 23 to 25).
- **The defaults are the RP2350's.** `config.h`, `audio_input.h` and `dac_hw_mute.h` define them without `#ifndef` guards, so S/PDIF out 1 defaults to GPIO 6, MCK to 13, BCK to 14, and so on. A preset saved on an RP2350 board carries those numbers too.
- **The port ignores them.**
  - The SMI composer puts slot n on SD n (GPIO 8 + n), BCK on GPIO 12, LRCLK on GPIO 13 and ADAT on GPIO 16, whatever `output_pins[]` says. `audio_spdif_change_pin()` and `audio_i2s_change_data_pin()` in `pico_shim.c` only record the pin.
  - MCK always uses GPCLK0 on GPIO 4.
  - The I2S input reads data on GPIO 22 to 25, and an external master's clocks on 26 and 27.
  - S/PDIF and ADAT input read GPIO 20.
- **The general GPIO calls do nothing.** `gpio_put()`, `gpio_get()`, `gpio_set_function()` and the pull calls in `pico_shim.c` only update tables in memory. The DAC hardware mute, the control surfaces and the UART and I2C control interfaces therefore never reach a real pin. The mute check in section 30 confirmed DSPi's mute state, not a pin level.

## What the Pi's hardware allows

| Function | Block | Possible pins on the header | Notes |
|---|---|---|---|
| Output slots 1 to 4 (S/PDIF or I2S data) | SMI data lines | GPIO 8 to 23 (SD0 to SD15) | The SMI runs 16 bits wide. Any slot can take any line. |
| I2S BCK and LRCLK out | SMI data lines | GPIO 8 to 23 | DSPi requires LRCLK = BCK + 1, which the SMI can meet anywhere in the range. |
| ADAT out | SMI data line | GPIO 8 to 23 | |
| MCK | GPCLK0, 1 or 2 | GPIO 4, 5, 6 (also 20, 21) | GPIO 4 is proven. Whether the VideoCore leaves GPCLK1 and GPCLK2 free has to be checked. 20 and 21 collide with the PCM block. |
| S/PDIF and ADAT in | PCM block's PCM_DIN | GPIO 20 only | The other PCM_DIN pins are not on the header. |
| I2S input data, BCK and LRCLK in | GPIO snapshot (GPLEV0) | Any 16 consecutive GPIOs that hold all of them | The decoder reads a 16-bit window of each snapshot. |
| DAC mute, control surfaces, general I/O | Plain GPIO | GPIO 2 to 27 | GPIO 0 and 1 are kept for a HAT's ID EEPROM. |
| UART control | PL011 or mini UART | GPIO 14 and 15 | They are the only UART pins on the header. |
| I2C control and displays | BSC1 | GPIO 2 and 3 | They have fixed 1.8 kΩ pull-ups on the board. |

Every pin is 3.3 V only.

## The decision this plan needs first

DSPi's rules and defaults are compiled into DSPi's own files, and the preset-restore paths call them directly. The port can catch requests from DSPi Console before DSPi sees them, but it cannot catch a preset restore. There are two ways forward:

1. **Small hooks in DSPi (recommended).** These would go into DSPi upstream, and the port would then move its submodule to that commit:
   - `#ifndef` guards on the default pin macros, so the port can supply the Pi's defaults.
   - A platform hook behind `is_valid_gpio_pin()`, so the Pi can supply its own per-function rules.

   This is a few dozen lines, and the RP2350 build stays unchanged.
2. **No DSPi change.** The port would check every pin request itself, and after each preset load it would rewrite any invalid pin. This is more code in the port, it has gaps (a preset could install a pin the Pi cannot honour until the port catches it), and it duplicates DSPi's conflict rules.

**Decided: option 1, and the hooks are in place.** They are DSPi commit a10ca91 on `release/v1.1.6`, which the submodule now points at. DSPi's new `platform_pins.h` declares them, and every change to DSPi sits behind `DSPI_PLATFORM_PINS`, which only the port defines. The port supplies its defaults in `firmware/PiZero/platform_pins/dspi_platform_pins.h` and its rules in `z4/platform_pins_pi.c`. One rule had to be shared rather than per-pin: a platform build's ADAT input may take S/PDIF input 1's pin, because the Pi has one receiver input. The RP2040 and RP2350 builds were checked to be unchanged in four configurations (RP2040 and RP2350, each with and without `DSPI_LOOPBACK`):

- Every source file's preprocessed output, with whitespace normalised, is the same as before, apart from the build's own describe string.
- With that string held equal, `DSPi.bin` and `DSPi.uf2` are byte-identical, and so are the ELFs once debug info is stripped, as are their symbol tables.
- An RP build that defined `DSPI_PLATFORM_PINS` by mistake would stop with a missing `dspi_platform_pins.h`, rather than build differently.

A second, smaller question concerns DSPi Console. It decides which pins to offer from the platform ID, and the port reports itself as an RP2350. The Console needs either a Pi platform ID or, better, a request the device answers with its own list of pins for each function. That is a change to DSPi and to the Console.

## Phases

1. **Real GPIO.** The shim's GPIO calls would drive the pins through GPFSEL, GPSET, GPCLR, GPLEV and the BCM2837's pull sequence (GPPUD, GPPUDCLK0). An ownership table would keep a general GPIO call from touching a pin that the SMI, PCM, GPCLK or the I2S snapshot owns. This unblocks the DAC hardware mute at once, and it gives the control surfaces the buttons and encoders they read. The control surfaces' other parts (IR, displays, the UART and I2C interfaces) are a separate plan.
2. **The Pi's pin rules and defaults.** This adds the table above as a per-function check, applied through the hooks of option 1 or the request filter of option 2. It also adds Pi defaults that match today's fixed pins, so nothing moves for an existing user. When a preset holds a pin the Pi cannot use, that function falls back to its default, as DSPi already does for an RP2040 preset with MCK on 13.
3. **Outputs on any SMI line.** The composer would take a bit position for each slot, for BCK, for LRCLK and for ADAT, in place of today's fixed bits. The SMI's GPFSEL claims would follow the assigned pins, and a released pin would go back to an input. DSPi already applies a slot pin change through a muted, synchronised pipeline reset (`process_pin_changes()`), so the move is live. The composer is hand-written NEON, so the per-pin bits have to cost nothing measurable. I'll measure that against today's composer before and after.
4. **MCK on GPIO 4, 5 or 6.** This picks GPCLK0, 1 or 2 by pin, after checking on the board that the VideoCore leaves GPCLK1 and GPCLK2 alone.
5. **I2S input on any pins within the window.** The decoder would take a bit position for each data line, in place of `data + k`, and the window would be placed from the assigned pins. A set of pins that doesn't fit in 16 consecutive GPIOs is refused. I'll measure the decode cost before and after, as for the composer.
6. **S/PDIF and ADAT input stay on GPIO 20.** Any other pin is refused, and so are S/PDIF inputs 2 to 4, since the Pi has one PCM_DIN. Reading S/PDIF from the GPIO snapshot instead would free the pin, but at 16.5 or 30.9 M snapshots a second it has far less margin than the PCM block's 125 MHz capture. That would be a later study, not part of this plan.
7. **DSPi Console.** This adds the platform ID or the pin-list request from the decision above, so that the Console offers only pins the Pi accepts. Until then the Console shows RP2350 pins, and the Pi refuses the ones it can't use with `PIN_CONFIG_INVALID_PIN`.
8. **Bench tool and docs.** `pin_assign_check.py` would move each function across its valid pins and check that the signal appears there and nowhere else. It would read the pins back with the I2S input's GPLEV0 snapshot, so no wiring is needed. It would also check that invalid pins and conflicts are refused, and that a pin change during playback restarts the outputs once and in phase. The README's pin table becomes the default map.

## What it depends on

Presets are kept in RAM until SD storage exists, so an assigned pin lasts only until the next power cycle. Pin assignment works without that, but it is of limited use to a real user until presets are kept.

## Risks

- **GPCLK1 and GPCLK2.** If the VideoCore uses them, MCK stays on GPIO 4.
- **Composer and decoder cost.** Both are tuned hand-written code. If per-pin bits cost too much, a fallback is to permute the bits once per SMI word with a lookup table, and measure that instead.
- **GPIO 2 and 3** have fixed pull-ups to 3.3 V, which suit I2C and buttons, but not a pin that must idle low.
