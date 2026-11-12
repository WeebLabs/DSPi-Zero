# PLLA output clock: evaluation and plan

Date: 2026-11-12. Scope: evaluation and planning only. Nothing in the repo was changed and nothing was run against the board.

Sources read: `firmware/PiZero/smi_out.c`, `pcm_out.c`, `mbox.c`, `pico_shim/pico_shim.c` (output clock and SMI start), `z4/main_z4.c` (ARM clock request, bench requests), `Documentation/Porting/PiZero_Z0_bringup.md` sections 19 and 20, `PiZero_port_plan.md` sections 4, 6 and 7. Linux references are to `drivers/clk/bcm/clk-bcm2835.c` and `drivers/gpu/drm/vc4/vc4_hdmi.c` at tag v6.6 (line numbers are for that tag). Local copies are in `/private/tmp/claude-501/-Users-weeblabs-DSPi-Zero/6d12383d-e934-4b4d-9be4-9ea9b3ef7e07/scratchpad/linux_src/`.

Register addresses below are ARM physical addresses, which are 0x3F101000 plus the Linux offset. Every write to a CM or A2W register needs the 0x5A000000 password (`cprman_write`, line 335).

## 1. Evaluation

### 1.1 Verdict

The proposal is sound and PLLA is the right PLL, provided the read-only dump shows that nothing the firmware has enabled is running from it. The numbers check out. Three of the stated facts are wrong or need a correction, and there are four risks the proposal does not list. None of them block the plan.

### 1.2 What is right

- **PLLA as the source.** PLLB is the ARM, PLLC is the VPU core and is retuned by the firmware for turbo, PLLD feeds the UART, SD card and the firmware's own assumptions (Linux marks `plld_per` critical because "VPU firmware assumes that PLLD_PER isn't disabled by the ARM core", line 1850), and PLLH is wanted for HDMI. PLLA is the only one left, and Linux describes it as "the auxiliary PLL, used to drive the CCP2 (Compact Camera Port 2) transmitter clock" (line 1667).
- **Source number.** Peripheral clocks select `plla_per` with source 4 (`bcm2835_clock_per_parents`, line 1554).
- **CM_SMI field widths.** Linux registers the SMI clock with `int_bits = 4`, `frac_bits = 8` (line 2161), so the integer divider is 1 to 15. Linux does not flag it as a MASH clock, so the minimum integer divider is 1.
- **VCO values.** 1179.648 MHz = 32 x 36.864 MHz and 1083.8016 MHz = 32 x 33.8688 MHz. Both lie inside Linux's PLLA range of 600 to 2400 MHz (lines 1686 and 1687) and below `BCM2835_MAX_FB_RATE` of 1750 MHz (line 293), so the feedback pre-divider stays off in both families.
- **Divider chain.** PER channel /4 gives 294.912 MHz, then CM_SMI /8 gives 36.864 MHz (48 kHz) and /4 gives 73.728 MHz (96 kHz). For 44.1 kHz the same /4 and /8 give 270.9504 MHz and 33.8688 MHz. The PER channel divider is 8 bits, where 0 means 256, with bit 8 as channel disable (`A2W_PLL_DIV_BITS`, `A2W_PLL_CHANNEL_DISABLE`, `CLK_DIVIDER_MAX_AT_ZERO` at line 1405). Linux places no even-only restriction on it and gives PLLA's channels no hidden fixed divider (`fixed_divider = 1`, line 1706). Only PLLH's channels have the fixed /10.
- **PLL multiplier.** The fraction is 20 bits (`A2W_PLL_FRAC_BITS`). The programmed values are:

| Family | VCO | NDIV | FRAC | Rounding error | One FRAC step |
|---|---|---|---|---|---|
| 48 and 96 kHz | 1179.648 MHz | 61 | 0x70A3D (461373) | -0.007 ppm | 0.0155 ppm |
| 44.1 kHz | 1083.8016 MHz | 56 | 0x72B02 (469762) | -0.001 ppm | 0.0169 ppm |

- **Fine trimming through FRAC.** A step of about 0.016 ppm is far finer than DSPi needs for the soft VCXO, and far finer than any divider after the PLL.
- **All PLLs share the crystal.** Correct, with the consequence in 1.3 item 3.

### 1.3 What is wrong or needs correcting

1. **The 44.1 kHz discrepancy is an arithmetic slip, not a hardware hint.** The exact divider is 500 / 33.8688 = 14.762850, not 14.76321 (that value is 500 / 33.868, with the last digit of 33.8688 dropped). Against the programmed 3779/256 = 14.761719 the prediction is +76.6 ppm, not +101 ppm. Measured was +78.7 ppm. The residuals are then consistent at all three rates: +0.8 to +1.8 ppm at 48 kHz, +1.5 to +1.8 ppm at 96 kHz, and +2.1 ppm at 44.1 kHz. That residual is the Pi crystal against the host's frame clock. The CM_SMI fraction and MASH behave as assumed. Section 20 of the bring-up notes should be corrected when the work is written up.

2. **The jitter figure is about half of what was stated, and 44.1 kHz is the worst case.** MASH 1 moves each edge by at most one source period, so the edge error spans 2 ns peak to peak, which is +/-1 ns about the mean, and the period steps by 2 ns. The pattern rate depends on the fraction:
   - 48 kHz: fraction 144/256 = 9/16, pattern repeats every 16 SMI clocks, 2.304 MHz.
   - 96 kHz: fraction 200/256 = 25/32, pattern repeats every 32 SMI clocks, 2.304 MHz.
   - 44.1 kHz: fraction 195/256 (odd numerator), pattern repeats every 256 SMI clocks, 132.3 kHz, which is exactly 3 fs.

   A pattern at 132.3 kHz is much closer to the loop bandwidth of a DAC or receiver PLL than one at 2.3 MHz. So the 44.1 kHz family benefits most, and it is the best rate at which to demonstrate the improvement.

3. **HDMI audio does not take its rate from PLLH.** In `vc4_hdmi.c` the audio sample pacing is `HDMI_MAI_SMP`, a rational N/M of the HSM clock (`vc4_hdmi_audio_set_mai_clock`, line 2294, with `audio_clock = hsm_clock` on the BCM2835 family, line 3426). CTS is written by software with `VC4_HDMI_CRP_CFG_EXTERNAL_CTS_EN` (`vc4_hdmi_set_n_cts`, line 2323). So the pixel clock (PLLH) only enters through the CTS number, and the thing that must be locked to the SMI is the HSM clock. HSM is an ordinary peripheral clock (`CM_HSMCTL` 0x3F101088, line 2125, 4 integer bits and 8 fractional), so it can select `plla_per` too. That makes HDMI audio lock to the SMI exactly, including under trim, with no matching trim of PLLH. `PiZero_port_plan.md` section 4 says the HDMI audio clock comes from the pixel clock, which should be corrected. The constraint this adds is in 1.5.

4. **"Sigma-delta noise is filtered, so ns becomes tens of ps" is plausible but unproven, and the comparison point is wrong.** PLLD is already fractional (2000 MHz / 19.2 MHz = 104.1667), so today's clock already carries fractional-N PLL noise in addition to the divider pattern. Moving to PLLA removes the deterministic +/-1 ns divider pattern and leaves PLL noise of the same kind the Pi's integer-divided clocks have always had. I expect a large improvement, but the residual figure is unknown until measured. See 1.4 for an integer-N variant.

### 1.4 Alternatives considered

- **Stay on PLLD with integer division.** Not possible. 500 MHz / 12.288 MHz = 40.69 and 500 / 11.2896 = 44.29, so no combination of integer divider and cycle length works, and PLLD must not be retuned.
- **PLLC, PLLH, oscillator.** PLLC changes under turbo. PLLH conflicts with HDMI. 19.2 MHz is not related to any audio rate.
- **One VCO for both families.** The least common multiple of 36.864 MHz and 33.8688 MHz is 5.4 GHz. A retune per family is unavoidable.
- **Option B, integer-N for the 48 kHz family (worth an A/B test, not the baseline).** 1843.2 MHz = 96 x 19.2 MHz = 50 x 36.864 MHz. That is above 1750 MHz, so it uses the feedback pre-divider (NDIV 48, FRAC 0, `fb_prediv_mask` bit 14 of ANA1, line 455). PER /5 gives 368.64 MHz, CM_SMI /10 (48 kHz) or /5 (96 kHz). The matching 44.1 kHz VCO is 50 x 33.8688 = 1693.44 MHz (N = 88.2, fractional, no pre-divider), with the same PER and CM dividers. No integer-N solution exists for 44.1 kHz inside the VCO range (it needs N to be a multiple of 147, which is 2822 MHz).
  - For: the 48 kHz family has no fractional modulation at all while free running. 368.64 MHz also divides exactly to 184.32 MHz for HSM, 12.288 MHz for MCK and 3.072 MHz for a PCM bit clock.
  - Against: odd dividers (duty cycle is unknown), the pre-divider toggles between families (which is the case where Linux reverses the ANA write order, lines 713 to 722), and, most importantly, once input tracking trims N a few ppm away from an integer, fractional-N PLLs produce their worst spurs, at a low offset inside the loop bandwidth where the loop does not filter them. The proposal's N = 61.44 and 56.448 stay well away from an integer under any trim.
  - Recommendation: write the PLL function to take NDIV, FRAC, pre-divider, PER divider and CM divider as a table row, bring up with option A (the proposal), and compare option B only if phase 4 has an instrument that can tell them apart.
- **Cycle length.** Three clocks per word is the right choice. All pins change on one SMI clock edge per word, so only every third rising edge matters and divider duty cycle does not. Four clocks per word would need a 98.304 MHz SMI clock at 96 kHz, which is untested, and gains nothing with option A.
- **PER divider.** /4 is the right first choice: CM_SMI is then /8 and /4, both even, and a 48 to 96 kHz switch changes only CM_SMI. PER /8 with CM_SMI /4 and /2 is equivalent for the SMI but halves what other consumers can derive.

### 1.5 Risks the proposal does not list

1. **`plla_core` consumers.** For VPU-domain clocks (V3D, H264, ISP, SDRAM secondary, VPU) source 4 means `plla_core` (`bcm2835_clock_vpu_parents`, line 1598). From memory, not verified, Linux `clk_summary` on a Pi 2 or 3 with the stock firmware shows PLLA already running near 3 GHz with `plla_core` near 500 MHz, used by H264, ISP or V3D. If any enabled clock selects source 4, retuning the VCO changes that clock. The dump must look at CM_V3DCTL, CM_H264CTL, CM_ISPCTL, CM_CAM0CTL, CM_CAM1CTL, CM_CCP2CTL, CM_DSI0ECTL and CM_DSI0PCTL, not only at the PLLA registers. If V3D is ever wanted for the dashboard, this needs a decision.
2. **The ARM clock request may touch PLLA.** `main_z4.c` line 959 calls `mbox_set_clock_rate(ARM, max)` with skip-turbo 0. Turbo also raises the firmware's core, V3D, H264 and ISP targets, which may reprogram `plla_core` or the PLLA VCO. PLLA must be programmed after that call, the call must not be repeated afterwards, and the dump must be taken before and after it.
3. **`track()` breaks if the clock stops for more than about 1.3 ms.** `smi_out.c` `track()` adds whole rings (128 frames, 2.67 ms at 48 kHz) so that the advance matches what elapsed time predicts. Today the clock stops for microseconds. A PLL relock can take milliseconds (Linux allows 100 ms, `LOCK_TIMEOUT_NS`, line 292), and `track()` would then add rings that were never played, which corrupts `st.frames`, the feedback rate measurement and the composer position. `smi_out_set_rate()` also holds interrupts off for the whole change, which is not acceptable for a wait of milliseconds at High Speed USB.
4. **HSM range against PER /4.** Linux requires HSM to be at least 120 MHz and at least 101 % of the pixel clock (`vc4_hdmi.c` lines 107 and 1720). From 294.912 MHz the only integer choices are 294.912 MHz (higher than the 163.68 MHz the firmware uses, untested) and 147.456 MHz, which caps the pixel clock near 146 MHz and excludes 1080p60 (148.5 MHz) but allows 720p60 and 1080p30. This is a later decision. It is one reason to keep the dividers in a table, and one argument for option B (184.32 MHz).
5. **ANA register readback.** Linux reads ANA0 to ANA3 back before modifying them (line 697). I am not certain the readback is the live value on every PLL. Phase 0 should read them twice and compare with what Linux would write (KI 2, KP 8 in ANA1, KA 2 in ANA3, line 449).
6. **Firmware bookkeeping.** The firmware keeps its own idea of each PLL's rate. Any firmware service that later derives a clock from PLLA (camera, codec, firmware HDMI audio) will either compute dividers from a stale rate or retune PLLA under us. We use none of these today. A readback watchdog (phase 6) covers the second case.
7. **Power domain.** Linux notes that PLLA "is in the PX LDO power domain, which is on when the AUDIO domain is on" (line 1670). PLLD is in the same domain and is running, so this should already be satisfied, but a lock failure in phase 1 would point here first.

### 1.6 Things that are not at risk

USB takes its reference straight from the oscillator (`A2W_XOSC_CTRL_USB_ENABLE`). The ARM is on PLLB, SDRAM and the VPU on PLLC, the SD card and UART on PLLD. None of them depend on PLLA unless the dump shows a `plla_core` consumer.

## 2. Programming sequence (from Linux)

Addresses: CM_PLLA 0x3F101104, CM_LOCK 0x3F101114, A2W_PLLA_ANA0..3 0x3F102010..1C, A2W_PLLA_CTRL 0x3F102100, A2W_XOSC_CTRL 0x3F102190, A2W_PLLA_FRAC 0x3F102200, A2W_PLLA_PER 0x3F102500, CM_SMICTL 0x3F1010B0, CM_SMIDIV 0x3F1010B4.

Cold start (PLLA off or in an unknown state):

1. Stop the consumer: clear ENAB in CM_SMICTL and wait for BUSY to clear (`bcm2835_clock_off`, line 1057). Use KILL only on timeout.
2. Hold the channel: set `CM_PLLA_HOLDPER` (bit 7) in CM_PLLA (`bcm2835_pll_divider_off`, line 823).
3. `bcm2835_pll_set_rate` (line 681): set `A2W_XOSC_CTRL_PLLA_ENABLE` (bit 6) by read-modify-write. Read ANA3..0, set KI = 2 and KP = 8 in ANA1 and KA = 2 in ANA3, clear the pre-divider bit 14 in ANA1, and write ANA3, ANA2, ANA1, ANA0 in that order (`bcm2835_pll_write_ana`, line 665). Write FRAC. Write CTRL with NDIV in bits 9:0 and PDIV = 1 in bits 14:12, keeping the other bits.
4. `bcm2835_pll_on` (line 628): clear `A2W_PLL_CTRL_PWRDN` (bit 16) in CTRL, clear `CM_PLL_ANARST` (bit 8) in CM_PLLA, wait for `CM_LOCK_FLOCKA` (bit 8 of CM_LOCK), then set `A2W_PLL_CTRL_PRST_DISABLE` (bit 17) in CTRL.
5. `bcm2835_pll_divider_set_rate` (line 857): write the PER divider (4) to A2W_PLLA_PER, which also clears channel disable, then pulse `CM_PLLA_LOADPER` (bit 6) in CM_PLLA high and low. Clear HOLDPER (`bcm2835_pll_divider_on`, line 839).
6. Program CM_SMIDIV with the integer in bits 15:12 and a zero fraction, CM_SMICTL with source 4 and bit 9 clear, then set ENAB (`bcm2835_clock_set_rate`, line 1100, clears `CM_FRAC` when the fraction is zero).

Family retune: steps 1 and 2, then FRAC and CTRL only (ANA is unchanged because the pre-divider does not change in option A), wait for lock plus a measured settle time, then steps 5 (hold release only) and 6. Whether the lock bit actually drops during an 8 % step is unknown, so the settle time has to be measured in phase 3.

48 to 96 kHz: CM_SMI only, as today.

Fallback: CM_SMI back to source 6 with the fractional divider and MASH 1. This is the existing `clock_set()` and must remain in the build.

## 3. Phased plan

Each phase has a single pass criterion and the next does not start until it passes. Every phase keeps PLLD with MASH as the boot default until phase 6.

### Phase 0. Read-only register dump

**Change.** Add a bench request (next free number, for example 0x4C) that returns named clock registers, and a host script that decodes them. Registers: CM_PLLA, CM_PLLB, CM_PLLC, CM_PLLD, CM_PLLH, CM_LOCK. For each of PLLA, B, C, D and H: A2W CTRL, FRAC and ANA0..3 (ANA read twice). All channel dividers (A2W_PLLA_DSI0, CORE, PER, CCP2 and the PLLC, PLLD, PLLH equivalents). A2W_XOSC_CTRL. Every CM CTL and DIV pair that Linux names from 0x008 to 0x1D4. Read only the registers Linux names, not the whole block. Also report the mailbox clock rates for every firmware clock id. Take one snapshot into a static buffer as the first thing in `main()`, before the ARM clock request, and return it alongside the live values. The script decodes each PLL as 19.2 MHz x (NDIV + FRAC / 2^20) x (2 if the pre-divider bit is set) / PDIV, and lists every clock with ENAB set together with its source.

**Test.** Capture: (a) the early snapshot, (b) after the ARM clock request, (c) while streaming at 44.1, 48 and 96 kHz, (d) after 10 minutes of streaming, (e) booted with and without an HDMI monitor attached, (f) after the board has been warmed until the firmware throttles, if that is easy to provoke.

**Pass.** PLLD decodes to 2000 MHz with PER /4, which confirms the 19.2 MHz reference and the decode. No enabled clock selects source 4. PLLA's registers are identical in captures (b) to (f). If an enabled clock does use source 4, stop and decide (see open questions) before phase 1.

### Phase 1. PLLA running, nothing using it

**Change.** A new `pll_out.c` with one table-driven function (NDIV, FRAC, pre-divider, PER divider) following section 2, called from a bench request, after the ARM clock request. The SMI stays on PLLD. To observe PLLA, drive the otherwise unused PCM clock from it: CM_PCM source 4, integer divider 96, no MASH, giving 3.072 MHz, and measure it with the clock manager's frequency counter as Linux does in `bcm2835_measure_tcnt_mux` (line 348: CM_TCNTCTL 0x3F1010C0 with mux value 23 for PCM, CM_OSCCOUNT 0x3F101100 loaded with a count of oscillator cycles, result in CM_TCNTCNT 0x3F1010C4). Optionally also put it on GPCLK0 (GPIO 4, CM_GP0CTL 0x3F101070, source 4, /24 = 12.288 MHz) as a probe point. Record the time from releasing ANARST to FLOCKA. Do not call the mailbox "measured clock rate" tag while using the counter, since the firmware may use the same counter.

**Test.** Program 1179.648 MHz, then 1083.8016 MHz. Read the counter, the lock bit and all PLLA registers once a second for 10 minutes while `smi_check.py --watch` streams on PLLD. Repeat the phase 0 dump.

**Pass.** FLOCKA sets within 10 ms. The counter reads 3.072 MHz and 2.8224 MHz to within one count (this also proves there is no hidden fixed divider on PER). PLLA's registers and lock bit do not change in 10 minutes. The stream on PLLD shows no new underruns, drops or late compositions, and the mailbox temperature and ARM clock queries still answer.

### Phase 2. SMI on PLLA at 48 kHz

**Change.** `clock_set()` gains a source choice: PLLA with integer CM_SMI (/8), or the existing PLLD path. A bench request switches between them at run time. Fix `track()` for a stopped clock: after any clock change, reload `last_us` and `last_off_words` before the next service so that no elapsed-time rings are added, and do the lock wait with interrupts enabled behind a "clock paused" flag that makes `track()` return early.

**Test.** `smi_check.py` decode of all four slots in I2S and S/PDIF, then `--watch` for 10 minutes on PLLA. Toggle PLLA and PLLD ten times while streaming.

**Pass.** Zero parity or clock errors, no underruns, drops or late compositions. The feedback controller's long-window rate reads between 0 and +3 ppm (the crystal against the host) instead of +65 ppm. `st.frames` shows no jump across a toggle.

### Phase 3. Rate families and switching

**Change.** 96 kHz uses CM_SMI /4. 44.1 kHz retunes PLLA by the retune sequence in section 2. `smi_out_set_rate()` picks the table row from the rate. Measure the relock: time to FLOCKA, and the counter reading at 1, 2, 5 and 10 ms after the write, to set the settle delay. Try both the in-place retune (FRAC and CTRL written while running) and a full reset through ANARST, and keep the one that settles faster without losing the lock bit afterwards.

**Test.** `smi_check.py` at all three rates. A loop of 50 switches through 44.1, 48 and 96 kHz from the host, checking decode after each. `uac2_stream_check.py 3`.

**Pass.** All three rates read the same long-window residual, within 1 ppm of each other. Every switch ends with clean decode and no SETERR or AFERR growth beyond what a PLLD switch shows today. The clock is stopped for less than 20 ms per family switch. DSPi's pipeline reset on a rate change already mutes and refills, so no extra handling is expected, and the fill error after a switch is no worse than on PLLD.

### Phase 4. Jitter verification

Frequency is proven by phases 1 to 3. Jitter needs one of these, in order of preference:

1. **Oscilloscope, 200 MHz and 1 GS/s or better.** Trigger on a BCK edge (GPIO 12, 3.072 MHz) and view an edge several hundred nanoseconds later with infinite persistence at 1 ns per division. With MASH the later edge shows as two or more distinct positions up to 2 ns apart. With PLLA it should be a single edge whose width is the scope's own trigger jitter.
2. **RTL-SDR receiver (about 30 USD), the cheapest instrument that gives a number.** Compose a square wave on a spare SMI pin and tune to a harmonic near 30 MHz through a few picofarads or an attenuator. At 44.1 kHz the MASH pattern appears as discrete sidebands spaced 132.3 kHz around the carrier, which is inside the receiver's bandwidth. A 1 ns peak-to-peak deterministic jitter on a 30 MHz carrier is about 0.09 radians of phase modulation, so the sidebands should total roughly -26 dBc and be unmistakable. On PLLA they should vanish into the noise floor. The remaining close-in skirt compares option A against option B.
3. **No instrument: the PCM block as a sampler.** Jumper a slot pin to PCM DIN, clock the PCM receiver from PLLD at an integer rate, and histogram edge positions modulo the sample period. Because the two clocks are not harmonically related, the edges sweep through every phase, and a 2 ns bimodal pattern separates from a single clean edge. This needs PCM receive with DMA, which the input work needs anyway, so do it only if neither instrument is available.
4. **Audio-band check.** Play a J-test signal (fs/4 tone with an LSB square wave at fs/192) through the PCM5102 and through an S/PDIF DAC and capture with the best ADC to hand. I expect no visible difference on the PCM5102, because its PLL already filters the pattern. This is a regression check, not proof.

**Pass.** With instrument 1 or 2: the MASH signature is present on PLLD and absent on PLLA at 44.1 kHz. If no instrument is available, this phase is recorded as not verified and the change proceeds on the frequency results alone, since the PLLD fallback remains.

### Phase 5. Fine trim

**Change.** `pll_out_trim(int32_t ppb)` writes FRAC only (and NDIV if the fraction carries), with no gate or hold. Expose it on a bench request. It is not connected to DSPi's clock tracking yet.

**Test.** While streaming at 48 kHz and 44.1 kHz with an S/PDIF receiver locked: single FRAC steps, then a staircase of +/-100 ppm in 1 ppm steps at 10 steps a second, then one 100 ppm jump. Read the long-window rate (or the counter) after each plateau.

**Pass.** The measured rate follows the commanded trim within 1 ppm. FLOCKA never clears, the SMI shows no errors, and the receiver stays locked through the staircase. Record whether it survives the 100 ppm jump, which sets the slew limit the tracking loop must respect.

### Phase 6. Make it the default, with a fallback

**Change.** PLLA becomes the boot default after the ARM clock request. Fall back to PLLD with MASH, and set a flag in the `0x4F` stats, if FLOCKA does not set within the timeout. A once-a-second watchdog reads back A2W_PLLA_CTRL, FRAC, PER, CM_PLLA and FLOCKA. On a mismatch it counts the event, reprograms once, and falls back to PLLD if the mismatch returns. Update bring-up sections 19 and 20 and port plan section 4 (the 44.1 kHz arithmetic, the HDMI audio clock source).

**Test.** Cold boot 10 times. One hour of streaming with a rate switch every minute. Force the fallback with a bench request.

**Pass.** PLLA locks on every boot. The watchdog counts zero events in the hour. A forced fallback resumes clean audio on PLLD within one pipeline reset.

### Later, not part of this work

- MCK on a GPCLK pin from `plla_per` with an integer divider (/24 for 256 fs), which is locked to the SMI clock by construction.
- The PCM block clocked from `plla_per` with an integer divider when it is used as an I2S master for inputs, so that input and output sampling share one clock. Linux restricts PCM to the oscillator and PLLD only because other sources "may have varying rates or be switched off" (line 1572), which does not apply once we own PLLA.
- HSM from `plla_per` for HDMI audio, and the PER divider decision in 1.5 item 4.
- Connecting `pll_out_trim()` to DSPi's clock tracking.

## 4. Open questions

For the hardware (answered by phases 0 to 3):

1. Is PLLA running at boot on this firmware, at what rate, and does any enabled clock select source 4 (`plla_core` or `plla_per`)?
2. Does the ARM clock request with turbo, a monitor on HDMI, or thermal throttling change any PLLA register?
3. Do the ANA registers read back live values, and what KA, KI and KP has the firmware set on PLLA and PLLD?
4. Is there any hidden divider on `plla_per`, and are odd PER dividers usable (needed only for option B)?
5. How long does lock take from cold and across an 8 % retune, and does FLOCKA drop during an in-place retune?
6. How large a FRAC step can be written while streaming without a receiver losing lock?
7. How wide is the CM_OSCCOUNT counter, which sets the resolution of the on-chip frequency measurement?

For you:

1. What can you measure with: an oscilloscope (bandwidth and sample rate), a logic analyser (sample rate), an RTL-SDR, or an ADC good enough for a J-test capture? If none, is an RTL-SDR purchase acceptable?
2. Will the HDMI dashboard be driven by our own code in the vc4 style, or through the firmware's framebuffer? The second leaves the firmware in charge of HSM and PLLH and changes how HDMI audio can be locked.
3. Is 1080p60 required for the dashboard, or is 720p60 or 1080p30 enough? That decides whether HSM at 147.456 MHz from PER /4 is acceptable.
4. Will the dashboard ever use V3D, H264 or the camera? Those are the likely `plla_core` users.
5. Is a mute of up to about 20 ms on a 44.1 to 48 kHz family switch acceptable? I assume yes, since DSPi resets the pipeline anyway.
6. Do you want option B (integer-N at 48 and 96 kHz) tested in phase 4, or only kept as a note?
7. `start.elf` is the full firmware. Would you accept `start_cd.elf` (the cut-down firmware without codec and camera support) if the dump shows the full firmware holding PLLA for those blocks?

## 5. Revision after the phase 0 dump (2026-11-12)

### 5.1 What the dump showed

- PLLA is running and locked at 2400.000 MHz: NDIV 62, FRAC 0x80000, PDIV 1, feedback pre-divider on (19.2 MHz x 2 x 62.5).
- `plla_core` is /4 = 600 MHz and feeds V3D, which is enabled at /2 = 300 MHz. PER, DSI0 and CCP2 are disabled and held (CM_PLLA 0x08A is HOLDPER, HOLDCCP2 and HOLDDSI0, with ANARST clear).
- No other enabled clock uses source 4. The registers did not change across our turbo request or across streaming at the three rates.
- The mailbox reports V3D as 250 MHz while the registers give 300 MHz. The firmware's reported rate is therefore not derived from the registers, which suggests it programs PLLA once at boot and does not track it afterwards. This is a hint, not proof.

The phase 0 criterion as written fails. The finding does not rule PLLA out. It changes how PLLA should be programmed.

### 5.2 Recommendation: retune in place, close to the firmware's own setting

Keep the firmware's configuration of PLLA exactly as it is (pre-divider on, ANA registers untouched, CORE /4, V3D /2, no reset) and change only NDIV and FRAC by a small amount:

| Family | NDIV | FRAC | VCO | Change from 2400 | V3D becomes | `plla_per` /8 | CM_SMI |
|---|---|---|---|---|---|---|---|
| 48 and 96 kHz | 61 | 0x70A3D | 2359.296 MHz | -1.7 % | 294.9 MHz | 294.912 MHz | /8 and /4 |
| 44.1 kHz | 56 | 0x72B02 | 2167.6032 MHz | -9.7 % | 270.9 MHz | 270.9504 MHz | /8 |

These are the same NDIV and FRAC values as in section 1.2. With the pre-divider on, the VCO is twice the earlier figure, so the PER divider becomes /8 instead of /4 and everything after `plla_per` is unchanged. The trim step in ppm is also unchanged (about 0.016 ppm).

Why this is better than the original 1179.648 MHz plan and than the listed options:

- V3D only ever gets slower, by 2 to 10 %. That is safe for a block that is idle, and it would still be acceptable if the dashboard later used V3D.
- No ANA write, no pre-divider change, no ANARST reset and no power-down. The PLL stays in the operating region the firmware chose, with the firmware's loop settings, so the Linux ANA sequence and its ordering quirk are not needed at all.
- Both VCO values are at or below Linux's 2400 MHz limit for PLLA (line 1687), and the firmware already runs it at that limit.
- If the firmware ever rewrites PLLA to 2400 MHz, the audio clock goes 1.7 % or 10 % fast rather than stopping, and the watchdog can restore it.

Write order for a retune, so the VCO never passes above the higher of the two targets: when going down, write NDIV (CTRL) first and then FRAC. When going up, write FRAC first and then NDIV. Neither intermediate value exceeds 62.5.

The PER channel is enabled as in section 2 step 5: write 8 to A2W_PLLA_PER (which clears the disable bit), pulse LOADPER in CM_PLLA, then clear HOLDPER. The other bits of CM_PLLA must be preserved.

### 5.3 The coordinator's options

- **(a) Retune to 1179.648 MHz and accept V3D at 147 MHz.** Workable, but it needs the pre-divider turned off, an ANA write and a 2:1 VCO move, for no benefit over 5.2. Replaced by 5.2.
- **(b) Ask the firmware to turn V3D off.** Not recommended as the configuration. If the firmware manages PLLA by reference count, removing the last consumer invites it to power PLLA down, and its bookkeeping would then say PLLA is off while we use it. Leaving V3D nominally on is the more stable state. It is useful as a probe, see 5.4.
- **(c) config.txt.** `v3d_freq` or `gpu_freq` might move V3D to PLLC, but the firmware's PLL choice is undocumented, each guess costs a boot, and PLLA may stay at 2400 MHz regardless. `avoid_pwm_pll` is obsolete and unrelated. Not worth pursuing first. One setting is worth a later look for a different reason: `force_turbo=1` removes the firmware's turbo transitions, which removes one class of clock events.
- **(d) start_cd.elf.** Possibly the cleanest ownership, if it leaves PLLA off and V3D disabled. It needs `fixup_cd.dat` and `gpu_mem=16`, and its HDMI support is narrower, which matters for the dashboard. Worth one dump as information and as a backup route. Not the primary path.
- **(e) `plla_per` at 2400 MHz.** 2400 / 36.864 is 65.1, so this is fractional again and no better than PLLD. PLLH is in use for the composite encoder (VEC at 108 MHz) and is wanted for HDMI. Neither helps.

### 5.4 Proving the firmware leaves PLLA alone

Nothing can prove this for all time, so the plan is evidence plus a watchdog. Each test compares A2W_PLLA_CTRL, A2W_PLLA_FRAC, A2W_PLLA_CORE, A2W_PLLA_PER, CM_PLLA, CM_LOCK, CM_V3DCTL and CM_V3DDIV before and after.

Before the retune (firmware values in place), to learn what the firmware writes:

1. Finish the 10-minute soak, the HDMI attach capture and a throttling capture.
2. Mailbox SET_CLOCK_RATE for V3D (id 5) to 250 MHz and back to 300 MHz. Record whether it rewrites only CM_V3DDIV, or also PLLA CTRL and FRAC.
3. Mailbox SET_CLOCK_STATE off and on for V3D. Record whether PLLA is powered down or its CORE channel disabled when V3D goes off. If it is, the firmware manages PLLA actively and option (b) is ruled out for good.
4. ARM clock request to the minimum and back to the maximum.
5. One boot with `start_cd.elf`, dump only.

After the retune (2359.296 MHz, PER /8 enabled), repeat 2, 3 and 4, plus a one-hour soak that includes a throttling episode and an HDMI hot-plug.

**Pass.** After the retune, none of these events changes NDIV, FRAC, the PER channel or the lock bit. If test 2 or 3 does rewrite PLLA, that is acceptable provided we never issue those requests ourselves and the soak, throttling and hot-plug cases stay clean. If any event that we cannot avoid (throttling, hot-plug, the firmware's own periodic work) rewrites PLLA, stop: the choices are then `start_cd.elf` if its dump is clean, or staying on PLLD with MASH.

### 5.5 Changes to phases 1 to 6

- **Phase 0.** Extended by the pre-retune tests in 5.4. New pass criterion: the only enabled source 4 consumer is V3D, and no unavoidable event rewrites PLLA.
- **Phase 1.** No cold start. Retune in place to 2359.296 MHz and enable PER /8. Observe through the unused PCM clock (source 4, /96, 3.072 MHz) and the frequency counter as before. Stop the once-a-second mailbox "measured rate" query (`main_z4.c` line 550) while using the counter: the TD0 and TD1 changes in the dump suggest the firmware uses the same measurement hardware, and that query returned 0 in this capture anyway. Added pass conditions: CM_V3DCTL, CM_V3DDIV and A2W_PLLA_CORE are unchanged, and the post-retune tests in 5.4 pass. Record whether FLOCKA drops during the 1.7 % step and for how long.
- **Phase 2.** CM_SMI source 4, /8, from `plla_per` at 294.912 MHz. Otherwise unchanged, including the `track()` fix.
- **Phase 3.** The family switch is an in-place move between 2359.296 and 2167.6032 MHz with the write order in 5.2. There is no ANARST path to compare any more. The SMI clock is still gated and the PER channel held during the move. Same pass criterion.
- **Phase 4.** Unchanged. Option B (integer-N) becomes 1843.2 MHz with the pre-divider on (NDIV 48, FRAC 0, V3D 230 MHz, PER /5), still only as an A/B experiment.
- **Phase 5.** Unchanged.
- **Phase 6.** The watchdog is now required rather than a precaution. It also checks A2W_PLLA_PER and HOLDPER, restores our values once, and falls back to PLLD with MASH if they are overwritten again. The stats report the event count.

### 5.6 Answers this changes

- Open question 4 for the user (will the dashboard use V3D) no longer blocks anything. V3D keeps a valid clock within 10 % of nominal.
- Open question 7 for the user (`start_cd.elf`) becomes a backup route only.
- The HSM point in 1.5 item 4 is unchanged: `plla_per` is still 294.912 MHz. The dump confirms the firmware runs HSM at 163.683 MHz from `plld_per` with a fractional divider, so moving HSM to `plla_per` later means taking HSM away from the firmware, which ties into user question 2.

## 6. Phase 0 results (2026-11-12)

Captured with bench request `0x4C` (`tools/clk_dump.py`), whose boot snapshot is taken first thing in `main()`, and the mailbox probes of bench request `0x4A` (`tools/clk_probe.py`).

**Clock state as the firmware leaves it.**

| PLL | VCO | Channels in use | Consumers |
|---|---|---|---|
| PLLA | 2400 MHz (NDIV 62, FRAC 0x80000, pre-divider on) | CORE /4 = 600 MHz; PER, DSI0 and CCP2 disabled | V3D from `plla_core` /2 = 300 MHz |
| PLLB | 2000 MHz | ARM /2 = 1000 MHz | ARM |
| PLLC | 2400 MHz | PER /2, CORE0 /2 = 1200 MHz | EMMC from `pllc_per` /6 |
| PLLD | 2000 MHz exactly | CORE /4, PER /4 = 500 MHz | SMI, HSM (163.683 MHz, MASH 1), UART |
| PLLH | 864 MHz | AUX /4 = 216 MHz | VEC /2 = 108 MHz |

The decode is confirmed by PLLD at exactly 2000 MHz. ANA registers read back identically twice. Between boot and now, only our SMI clock and the TD0/TD1 test clocks changed. The firmware probably uses those to measure clocks when we query the mailbox.

**Stability.** PLLA's registers and V3D's stayed identical while streaming at 44.1, 48 and 96 kHz, and through a 10-minute streaming soak checked every 30 s.

**Firmware probes.**

| Mailbox request | Registers the firmware changed |
|---|---|
| V3D rate 250 or 300 MHz | none |
| V3D rate 400 MHz | PLLA CORE /4 to /6 and V3D DIV /2 to /1; the VCO stayed at 2400 MHz |
| V3D clock off, then on | none |
| ARM clock to minimum, then maximum | none on PLLA |

**Verdict.** V3D is the only enabled consumer of PLLA, and the firmware treats PLLA's VCO as fixed: it re-divides the CORE channel and never touches PER. That supports section 5's in-place retune. Still not captured: an HDMI hot-plug and thermal throttling. Phase 6's watchdog covers the rest.

## 7. Answers to the open questions (2026-11-12)

1. Instruments: none for now. Phase 4 is recorded as not verified, and the choice between options rests on the frequency results and on theory.
2. HDMI dashboard: our own driver and display code, if that is simplest. That leaves HSM and PLLH to us.
3. Resolution: 1080p60 is wanted if possible, 1080p30 is acceptable, and the options should stay open.
4. V3D, H264 and the camera will never be used. A slower V3D clock does not matter.
5. A mute of up to a second on a sample rate switch is acceptable.
6. Option B: compare it if there is a reason to prefer it. See section 8.
7. `start_cd.elf` is acceptable if it does what we need. It stays the backup route.

## 8. Option A against option B

Both options were brought up in phase 1 (section 9) and both lock and count exactly, so the comparison is on paper.

| | Option A | Option B |
|---|---|---|
| 48 and 96 kHz VCO | 2359.296 MHz, N = 61.44 (fractional) | 1843.2 MHz, N = 48 exactly (integer) |
| 44.1 kHz VCO | 2167.6032 MHz, N = 56.448 | 1693.44 MHz, N = 44.1 |
| PER divider | /8 (294.912 or 270.9504 MHz) | /5 (368.64 or 338.688 MHz) |
| CM_SMI | /8 at 48 and 44.1 kHz, /4 at 96 kHz | /10 at 48 and 44.1 kHz, /5 at 96 kHz |
| V3D | 295 or 271 MHz | 230 or 212 MHz |
| Integer HSM from `plla_per` | 147.456 MHz (/2), below 1080p60's 150 MHz | 184.32 or 169.344 MHz (/2), enough for 1080p60 |

What separates them:

- **The PLL's own modulator.** Both remove the clock manager's MASH pattern, which is the dominant jitter today (±1 ns). What is left is the PLL's own noise. With a fractional N the PLL's sigma-delta modulator runs all the time and adds shaped noise that its loop mostly filters. With N = 48 exactly that modulator has nothing to do. So B is cleaner at 48 and 96 kHz, by an amount no instrument here can show. At 44.1 kHz both are fractional and equivalent.
- **HDMI.** B gives an integer HSM that covers 1080p60. With A, HSM would need a fractional divider from `plla_per`. That still works for audio, because an HDMI sink rebuilds its audio clock from the pixel clock and the N/CTS numbers, so HSM's fine timing never reaches the audio. B is simpler, though.
- **Trim.** If the output ever has to follow an external clock (an S/PDIF input, for example), N is trimmed a few ppm. A trimmed N just off an integer is the worst case for fractional spurs. USB playback never trims, because the asynchronous feedback handles rate matching. If input tracking arrives later, the trimmed case can use the A rows, which stay in the table.
- **Odd divider.** PER /5 and CM_SMI /5 are odd. The counter confirms the rate. Duty cycle does not matter, because the SMI acts on one edge per three-clock word.
- **One structure.** With B both families use PER /5 and the same CM_SMI dividers. Only NDIV and FRAC change on a family switch.

**Decision.** Option B (rows `b48` and `b44`) becomes the baseline for phase 2. The A rows stay in the table for trimming and for comparison.

## 9. Phase 1 results (2026-11-12)

Code: `firmware/PiZero/pll_out.c` (table-driven in-place retune, PER channel enable, PCM probe clock, frequency counter), bench request `0x46` in `z4/main_z4.c`, and `tools/pll_check.py`. Retunes and counter runs are carried out from the main loop, not the USB interrupt. The once-a-second mailbox "measured ARM rate" query stops once the counter has been used.

**Frequency counter.** It was checked against PLLD first. The PCM clock from `plld_per` /20 reads exactly 250000, 2500000 and 10000000 counts over 10, 100 and 400 ms. `CM_TCNTCNT` is 24 bits wide, so a gate of 800 ms wraps at 36.864 MHz. Gates are capped at 400 ms, which gives 0.07 ppm per count. The SMI clock on PLLD with MASH reads 36.866360 MHz, which is +64 ppm from 36.864 MHz, matching the divider's error in section 1.3.

**Every row locks and counts exactly.** The PCM clock was driven from `plla_per` with an integer divider and no MASH:

| Row | VCO | PER | PCM divider | Count over 400 ms | Error |
|---|---|---|---|---|---|
| a48 | 2359.2960 MHz | /8 | /8 | 14745600 | +0.007 ppm (FRAC rounding) |
| a44 | 2167.6032 MHz | /8 | /8 | 13547520 | +0.001 ppm |
| b48 | 1843.2000 MHz | /5 | /10 | 14745600 | 0.000 ppm |
| b44 | 1693.4400 MHz | /5 | /10 | 13547521 | +0.065 ppm (one count) |

This also proves there is no hidden divider on `plla_per`, and that odd PER dividers work.

**Lock behaviour.** FLOCKA stays set through every downward move, including the 23 % step from 2400 to 1843.2 MHz. On an upward move it drops for a few microseconds: it was first seen clear 9 µs after the write and set again by 12 µs. So FLOCKA cannot tell when the VCO has settled, and phase 3 has to measure settling with short counter gates instead.

**Untouched.** A2W_PLLA_CORE, CM_V3DCTL and CM_V3DDIV kept their values through every retune. V3D runs at the reduced rate the table predicts.

**Soaks.** a48 and b48 were each held for 10 minutes while the SMI streamed all four slots on PLLD at 48 kHz. Both gave zero register changes, no lost lock, and a counter error of 0.264 ppm at worst on a48 (one count at the 100 ms gate) and 0.000 ppm on b48. The stream showed no underruns, ring drops, USB underruns or late compositions, and every slot decoded.

**Firmware probes after the retune (b48).** V3D at 250 or 300 MHz, and V3D off and on, changed nothing. V3D at 400 MHz again only re-divided CORE (/4 to /6) and V3D's own divider. The ARM probes left PLLA alone. They did swap the multipliers of PLLB and PLLC (2000 and 2400 MHz) and swapped them back. PLLA's NDIV, FRAC, PER and lock bit were unchanged after every probe.

**Not done.** HDMI hot-plug and thermal throttling are still not captured. The phase 6 watchdog covers them.

**Verdict.** Phase 1 passes for both options. Phase 2 moves the SMI onto `plla_per` with row b48.

## 10. Phase 2 results (2026-11-12)

Code:
- **`smi_out.c` `clock_set()`** takes PLLA when it is the preferred source and the rate allows it. `pll_out_smi_row()` picks row b48 or b44 and an integer CM_SMI divider (10 fs base / rate, 1 to 15). Any other rate, or a PLLA that fails to lock, falls back to PLLD with MASH.
- **Live moves:** a move between rows with the same PER divider happens live. The PLL slews while the SMI keeps running, so a rate change never waits for lock with interrupts off. Only the first start holds the PER channel and waits for lock, and `smi_out_set_source()` does that with interrupts on while the SMI is still on PLLD.
- **Bench guard:** once the SMI runs from `plla_per`, bench retunes that would hold the channel are refused.
- **`track()`:** after a clock change it counts the words the DMA has read but does not infer whole rings from the elapsed time.
- **Clock changes** stop CM_SMI at the end of a cycle (ENAB cleared, BUSY gone within 1 µs) instead of killing it. KILL remains only as a fallback.
- **Bench request** `0x46` op 4 selects the source, checked with `tools/smi_pll_check.py`. PLLD stays the boot default until phase 6.

**Exact clock.** The counter reads the SMI clock at 36.864000 MHz (0.000 ppm) at 48 kHz, 73.728000 MHz (0.000 ppm) at 96 kHz, and 33.868803 MHz (+0.074 ppm, one count, from FRAC rounding) at 44.1 kHz. On PLLD the same counter reads 36.866360 MHz, which is +64.0 ppm.

**CM_SMI's MASH bit.** CTL reads 0x294 on PLLA, with bit 9 set, whatever is written. The clock was confirmed stopped and not BUSY when CTL was written. The PCM clock clears the same bit normally, so on CM_SMI the bit appears fixed. With DIVF 0, a first-order MASH accumulator never carries, so the output is a pure integer division.

**Toggles.** 20 switches between PLLA and PLLD while streaming at 48 kHz. The SMI frame count stayed within 4 frames of what the two clock rates predict (a ring is 128 frames, and timestamp rounding is 48). Composer lateness, slot underruns, ring drops and USB underruns did not move.

**Soak.** 10 minutes on PLLA at 48 kHz with I2S and S/PDIF slots: no underruns, ring drops or late compositions, and every slot decodes. The feedback controller's long-window rate reads +1.9 to +2.5 ppm, against +65 ppm on PLLD. That residual is the Pi crystal against the host's USB frame clock, as predicted in section 1.3.

**Rates.** 44.1, 96 and 48 kHz each decode with zero parity or clock errors after a rate change from the host. The 44.1 kHz switch moves PLLA live from b48 to b44 and back.

**Verdict.** Phase 2 passes. Phase 3 (switch timing and a 50-switch loop) is next.

## 11. Phase 3 results (2026-11-12)

Code:
- **Settling capture.** `pll_out.c` counts the SMI clock in back-to-back 1 ms windows across a family move, with PLLA moved at the start of the third window. It runs from bench request `0x46` op 5, read back with an IN request (wValue 1), and is checked with `tools/pll_settle.py`. It blocks the main loop for the capture, so it is for the bench only.
- **`smi_check.py`** now takes the frame count and the timestamp from the same stats reply. Before, they came from two requests, so host latency between them looked like lost frames.

**Settling.**
- **Moving down (b48 to b44):** the window that contains the write already reads within one count (27 to 30 ppm) of the new rate, and FLOCKA never clears.
- **Moving up (b44 to b48):** that window is short by one or two counts, a few tens of nanoseconds of clock time, and the next window is exact.
- **Against the plan:** the clock never stops, and a family switch settles in well under 1 ms, against the 20 ms the plan allowed.

**51 rate changes from the host.** The rates cycled through 44.1, 48 and 96 kHz, 17 times each. Every change decoded with zero parity or clock errors on both S/PDIF slots, stayed on PLLA, and counted exactly on the SMI clock: 33.868800 MHz (twice 33.868803 MHz, one count), 36.864000 MHz and 73.728000 MHz. After the `smi_check.py` fix, the frame count over the 3 s check stays within 1 ms of timestamp rounding at every rate. `uac2_stream_check.py 3` passes.

**Residual at each rate.** These are 3-minute soaks; the figure is the mean of the feedback controller's long-window rate over the last 90 s.

| Rate | Mean | Range |
|---|---|---|
| 44.1 kHz | +2.36 ppm | +1.5 to +2.9 |
| 48 kHz | +2.47 ppm | +1.9 to +4.1 |
| 96 kHz | +2.44 ppm | +0.8 to +4.0 |

The three means agree within 0.11 ppm, inside the 1 ppm criterion. As expected, the residual is the crystal against the host's USB frame clock and does not depend on the rate.

**PLLA against PLLD across rate switches.** Nine switches with the stream stopped between them gave the same slot 0 underrun frames on PLLD (568907) and PLLA (569180), no ring drops, and no slot drops. The underruns are the gaps between streams.

**USB hang.** The runs above exposed a hang of USB control transfers under rapid stream starts, stops and rate changes. It happened on PLLD with PLLA untouched as well, so it is not part of this work. It was caused by and fixed in the DWC2 driver; see `PiZero_Z0_bringup.md` section 21.

**Verdict.** Phase 3 passes. Phase 4, jitter verification, needs an instrument and is recorded as not verified for now, as its pass criterion allows. Phase 5 (fine trim) is next.

## 12. Phase 5 results (2026-11-12)

Code:
- **`pll_out_trim(ppb)`** scales the active row's multiplier (NDIV.FRAC) by the trim, rounded to one FRAC step, and writes it live with no hold, so the PER channel and the SMI keep running. The trim is kept across family moves, so it applies to whichever row is in use.
- **Bench request** `0x46` op 6 sets the trim in 0.01 ppm units. `tools/pll_trim.py` runs the test while a tone streams straight to the Pi.
- **Not yet connected** to anything that tracks an input clock.

**Tracking.** These are measured on the SMI clock with the on-chip counter, against the reading at trim 0, which is 0.000 ppm from nominal at both rates.

| Commanded | 48 kHz (b48) | 44.1 kHz (b44) |
|---|---|---|
| +0.02 ppm (about one FRAC step) | +0.068 | +0.000 |
| +1 ppm | +1.017 | +0.960 |
| +10 / -10 ppm | +10.037 / -9.969 | +9.965 / -9.965 |
| +100 / -100 ppm | +100.030 / -99.962 | +100.018 / -100.018 |

Every reading is within the counter's resolution of the command (0.07 ppm over 400 ms), far inside the 1 ppm criterion. A single FRAC step is below that resolution, so it is confirmed by the register readback rather than the frequency.

**Staircase and jump.**
- **Staircase:** 0 to +100 to -100 to 0 ppm in 1 ppm steps at 10 steps a second, about 40 s, with plateaus read every 50 ppm. FLOCKA never cleared, and `lock_lost` stayed 0.
- **Jump:** 0 to +100 ppm in one write already reads +100 ppm in the first 10 ms window after it (one count is 2.7 ppm there).
- **Slew limit:** no slew limit appears up to 100 ppm on the PLL side.

**Stream.** The tone played through every run.
- **Clean runs:** six of the seven runs at 48 and 44.1 kHz showed no composer lateness, slot underruns, ring drops, USB underruns or lost lock.
- **One gap:** the first 44.1 kHz run, which came straight after the 48 kHz one, lost 4356 frames (about 99 ms) with one USB underrun. That run had no per-phase counters, so it is not known whether this happened during the trim or as macOS settled the rate change. It did not recur in five later 44.1 kHz runs, which had per-phase counters.

**Not verified.**
- **Receiver lock:** there is no S/PDIF receiver or DAC on the bench, so whether a receiver stays locked through the staircase and the jump is untested. The plan wanted this to set the slew limit for a tracking loop.
- **Trimming off an integer:** the test ran on the B rows, which become fractional as soon as they are trimmed. That is the spur case section 8 warns about, and it can only be judged with an instrument (phase 4). A tracking loop should use the A rows.

**Verdict.** Phase 5 passes on frequency, lock and the stream. Receiver lock is outstanding. Phase 6 (PLLA as the boot default, with the watchdog and PLLD fallback) is next.

## 13. Phase 6 results (2026-11-12)

Code:
- **Boot default.** The SMI's preferred source in `smi_out.c` is now PLLA. Its first clock setup happens in `shim_smi_start()`, after the ARM clock request and before interrupts are enabled. That setup is PLLA's first start: it holds the PER channel, retunes, waits for lock and releases the channel. If PLLA does not lock, `clock_set()` falls back to PLLD with MASH as before.
- **`pll_out_select()`** now checks PLLA's registers before trusting a row it already holds. Without that check, returning to PLLA after the firmware had rewritten it left the SMI 4.7 % fast.
- **Watchdog.** `pll_out_watchdog()` runs from the main loop once a second while the SMI runs from PLLA, skipping the first 100 ms after a move or trim. It reads back NDIV, FRAC, the PER divider, HOLDPER and FLOCKA.
  - On a mismatch it records what it saw and restores our values once.
  - If a mismatch comes back before ten clean checks have passed, the main loop moves the SMI to PLLD, where it stays until reboot or until bench request `0x46` op 4 asks for PLLA again.
  - Stats version 9 (`0x4F`, 388 bytes) reports the source in use and the watchdog's events, restores and fallbacks.
- **Bench request** `0x46` op 7 stands in for the firmware by rewriting PLLA to its boot multiplier, once or every 100 ms for a given time. `tools/pll_failover.py` drives it, and `tools/pll_soak.py` runs the soak.

**Boots.** Ten reboots through the USB loader each reset the whole SoC, so the VideoCore firmware set PLLA to 2400 MHz again every time. Every boot came up with the SMI on PLLA, row b48 locked, and the SMI clock at exactly 36.864000 MHz. These were not power cycles, but the firmware starts from scratch either way.

**Forced fallback, with a tone playing.**
- **One rewrite:** the watchdog counted one event, restored PLLA within a second, and the SMI clock read exact again.
- **Rewrites every 100 ms for 5 s:** one restore, then a fallback to PLLD within the next second. All four slots decoded on PLLD.
- **Asking for PLLA again:** the SMI returned to PLLA with an exact clock.

**One-hour soak.** A rate switch every minute cycled 44.1, 48 and 96 kHz, with DSPi Console and its spectrum analyzer polling throughout. In each minute's 48 s window, which leaves out the stream restart, there were no slot underruns, ring drops, USB underruns or late compositions. The SMI stayed on PLLA with an exact clock in all 60 minutes, and the watchdog saw no event.

**Documentation.** Bring-up section 20's 44.1 kHz prediction is corrected to +76.6 ppm, and section 19 notes the PLLA clock. Port plan section 4 now says HDMI audio is paced by HSM, not by the pixel clock.

**Not covered.** No HDMI hot-plug or thermal throttling episode happened during the soak; the watchdog is the protection against both. Phase 4 (jitter) needs an instrument, and the receiver-lock part of phase 5 needs a receiver.

**Verdict.** Phase 6 passes. PLLA with integer dividers is the audio clock, with a watchdog and a PLLD fallback.
