#pragma once

#include "pico.h"

// RP2350 NVIC interrupt API. Nothing DSPi asks for here exists on the Pi, so these record nothing
// and never enable an interrupt. The Pi's own interrupt controller is driven through irq.h.
typedef void (*irq_handler_t)(void);

void irq_set_exclusive_handler(uint num, irq_handler_t handler);
void irq_add_shared_handler(uint num, irq_handler_t handler, uint8_t order_priority);
void irq_remove_handler(uint num, irq_handler_t handler);
void irq_set_enabled(uint num, bool enabled);
bool irq_is_enabled(uint num);
void irq_set_priority(uint num, uint8_t hardware_priority);
void irq_set_mask_enabled(uint32_t mask, bool enabled);
void irq_set_pending(uint num);

#define PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY 0x80
#define PICO_HIGHEST_IRQ_PRIORITY 0x00
#define PICO_DEFAULT_IRQ_PRIORITY 0x80
#define PICO_LOWEST_IRQ_PRIORITY  0xff

#define DMA_IRQ_0 10u     // RP2350 numbering, only ever passed back to the calls above
#define DMA_IRQ_1 11u
