// Whole-core CPU load (cpu_meter.h), timed with the Cortex-A53 cycle counter. Regions nest, as when a
// settle loop in DSPi's services runs the producer: each counts its own time, less interrupts and less
// the regions inside it, so nothing is counted twice.

#include "cpu_meter.h"

#include "irq.h"
#include "pmu.h"
#include "config.h"     // global_status

typedef struct {
    uint32_t start, irq0, child;
} frame_t;

static frame_t stack[4];
static uint32_t depth;
static uint32_t work[CPU_METER_REGIONS], idle[CPU_METER_REGIONS];
static uint32_t calls[CPU_METER_REGIONS], longest[CPU_METER_REGIONS];
static uint32_t win_start, win_irq0;
static uint8_t load_pct;
static cpu_meter_status_t last = {.magic = CPU_METER_MAGIC};

void cpu_meter_enter(void) {
    if (depth < 4u) {
        stack[depth] = (frame_t){pmu_cycles(), irq_cycles(), 0};
    }
    depth++;
}

void cpu_meter_leave(uint32_t region) {
    if (depth == 0u || --depth >= 4u) {
        return;
    }
    frame_t *f = &stack[depth];
    // An interrupt that began before the region is credited whole when it ends, so a short region can
    // lose more interrupt time than it lasted; it then counts as nothing.
    int32_t span = (int32_t)((pmu_cycles() - f->start) - (irq_cycles() - f->irq0));
    uint32_t total = span > 0 ? (uint32_t)span : 0u;
    uint32_t own = total > f->child ? total - f->child : 0u;
    if (own >= CPU_METER_IDLE_CYCLES) {
        work[region] += own;
    } else {
        idle[region] += own;
    }
    calls[region]++;
    if (own > longest[region]) {
        longest[region] = own;
    }
    if (depth) {
        stack[depth - 1u].child += total;
    }
}

void cpu_meter_tick(void) {
    uint32_t now = pmu_cycles();
    uint32_t elapsed = now - win_start;
    if (elapsed < CPU_METER_WINDOW_US * 1000u) {
        return;
    }
    uint32_t irq = irq_cycles() - win_irq0;
    uint64_t busy = irq;
    for (uint32_t r = 0; r < CPU_METER_REGIONS; r++) {
        busy += work[r];
        last.work_cycles[r] = work[r];
        last.idle_cycles[r] = idle[r];
        last.calls[r] = calls[r];
        last.max_cycles[r] = longest[r];
        work[r] = idle[r] = calls[r] = longest[r] = 0;
    }
    last.window_cycles = elapsed;
    last.irq_cycles = irq;
    last.load_x100 = (uint32_t)(busy * 10000u / elapsed);
    load_pct = (uint8_t)((last.load_x100 + 50u) / 100u);
    win_start = now;
    win_irq0 = irq_cycles();
    cpu_meter_publish();
}

void cpu_meter_publish(void) {
    global_status.cpu0_load = load_pct;
}

void cpu_meter_status(cpu_meter_status_t *out) {
    *out = last;
}
