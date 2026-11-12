#pragma once

// Interface between the UAC2 front end (main_z4.c) and the port of DSPi's main loop (z4_mainloop.c).

#include <stdbool.h>
#include <stdint.h>

// main_z4.c: one pass of the audio side, which the settle loops call while they wait for a fade.
// USB task, stream consumer, test-signal pump, idle ring pump and notification drain.
void z4_service_producer(void);
bool z4_stream_running(void);   // the consumer is feeding received audio into the pipeline
uint32_t z4_host_rate(void);    // the rate the host set on the UAC2 clock
uint32_t shim_output_clock(void);   // pico_shim.c: frames played at the output rate, from the SMI once it runs
void shim_smi_start(void);          // pico_shim.c: start every output on the SMI
uint32_t shim_smi_snapshot(uint16_t *out, uint32_t n);  // bench: n composed frames, returns the first

// z4_mainloop.c
void z4_dspi_init(void);        // main.c's core0_init(), less what needs RP2350 hardware
void z4_dspi_service(void);     // one pass of main.c's deferred work
void z4_stream_restart(void);   // a host stream is starting: host rate, then a synchronized restart
void z4_sync_core1_mode(void);  // z4_mainloop.c: Core 1 does not exist, so its mode is held idle
