#pragma once

// Whole-core CPU load for DSPi Console. DSPi's own figure counts only its block processing; this one adds
// every interrupt (outputs, S/PDIF decoding, USB) and the main loop's work. A main-loop call that finds
// nothing to do (under CPU_METER_IDLE_CYCLES of its own time) counts as polling, not load.

#include <stdint.h>

#define CPU_METER_IDLE_CYCLES 3000u     // 3 us at the 1 GHz ARM clock
#define CPU_METER_WINDOW_US 500000u

enum { CPU_METER_PRODUCER, CPU_METER_SERVICES, CPU_METER_REGIONS };

typedef struct {
    uint32_t magic;             // "CPUM"
    uint32_t load_x100;         // last window: busy share of the core, in 0.01 %
    uint32_t window_cycles;
    uint32_t irq_cycles;        // last window: in interrupt handlers
    uint32_t work_cycles[CPU_METER_REGIONS];    // last window: calls that did work, own time only
    uint32_t idle_cycles[CPU_METER_REGIONS];    // last window: calls that only polled
    uint32_t calls[CPU_METER_REGIONS];
    uint32_t max_cycles[CPU_METER_REGIONS];     // longest single call, own time
} cpu_meter_status_t;

#define CPU_METER_MAGIC 0x4D555043u

void cpu_meter_enter(void);
void cpu_meter_leave(uint32_t region);
void cpu_meter_tick(void);      // main loop: closes a window every CPU_METER_WINDOW_US
void cpu_meter_publish(void);   // puts the load where DSPi Console reads it
void cpu_meter_status(cpu_meter_status_t *out);
