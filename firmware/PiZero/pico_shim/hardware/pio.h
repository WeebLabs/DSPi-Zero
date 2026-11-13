#pragma once

#include "hardware/address_mapped.h"
#include "hardware/gpio.h"

// RP2350 PIO API, so DSPi's output and PDM code compiles. The three blocks are RAM stand-ins and
// every call is a no-op: no state machine exists on the Pi.
typedef struct {
    io_rw_32 ctrl;
    io_ro_32 fstat;
    io_rw_32 fdebug;
    io_ro_32 flevel;
    io_wo_32 txf[4];
    io_ro_32 rxf[4];
    io_rw_32 irq;
    io_wo_32 irq_force;
} pio_hw_t;

typedef pio_hw_t *PIO;

extern pio_hw_t pico_shim_pio_hw[3];
#define pio0 (&pico_shim_pio_hw[0])
#define pio1 (&pico_shim_pio_hw[1])
#define pio2 (&pico_shim_pio_hw[2])
#define NUM_PIOS 3u
#define NUM_PIO_STATE_MACHINES 4u
#define PIO_NUM(pio) ((uint)((pio) - pico_shim_pio_hw))
#define PIO_INSTANCE(n) (&pico_shim_pio_hw[(n)])

typedef struct {
    uint32_t clkdiv;
    uint32_t execctrl;
    uint32_t shiftctrl;
    uint32_t pinctrl;
    uint32_t pinhi;
} pio_sm_config;

typedef struct pio_program {
    const uint16_t *instructions;
    uint8_t length;
    int8_t origin;
    uint8_t pio_version;
    uint32_t used_gpio_ranges;
} pio_program_t;

enum pio_fifo_join { PIO_FIFO_JOIN_NONE = 0, PIO_FIFO_JOIN_TX = 1, PIO_FIFO_JOIN_RX = 2 };
enum pio_src_dest {
    pio_pins = 0u,
    pio_x = 1u,
    pio_y = 2u,
    pio_null = 3u,
    pio_pindirs = 4u,
    pio_exec_mov = 4u,
    pio_status = 5u,
    pio_pc = 5u,
    pio_isr = 6u,
    pio_osr = 7u,
    pio_exec_out = 7u,
};

static inline pio_sm_config pio_get_default_sm_config(void) {
    pio_sm_config c = {0};
    return c;
}
static inline void sm_config_set_wrap(pio_sm_config *c, uint wrap_target, uint wrap) {
    (void)c, (void)wrap_target, (void)wrap;
}
static inline void sm_config_set_out_pins(pio_sm_config *c, uint out_base, uint out_count) {
    (void)c, (void)out_base, (void)out_count;
}
static inline void sm_config_set_set_pins(pio_sm_config *c, uint set_base, uint set_count) {
    (void)c, (void)set_base, (void)set_count;
}
static inline void sm_config_set_in_pins(pio_sm_config *c, uint in_base) {
    (void)c, (void)in_base;
}
static inline void sm_config_set_sideset_pins(pio_sm_config *c, uint sideset_base) {
    (void)c, (void)sideset_base;
}
static inline void sm_config_set_sideset(pio_sm_config *c, uint bit_count, bool optional, bool pindirs) {
    (void)c, (void)bit_count, (void)optional, (void)pindirs;
}
static inline void sm_config_set_jmp_pin(pio_sm_config *c, uint pin) {
    (void)c, (void)pin;
}
static inline void sm_config_set_out_shift(pio_sm_config *c, bool shift_right, bool autopull, uint pull_threshold) {
    (void)c, (void)shift_right, (void)autopull, (void)pull_threshold;
}
static inline void sm_config_set_in_shift(pio_sm_config *c, bool shift_right, bool autopush, uint push_threshold) {
    (void)c, (void)shift_right, (void)autopush, (void)push_threshold;
}
static inline void sm_config_set_fifo_join(pio_sm_config *c, enum pio_fifo_join join) {
    (void)c, (void)join;
}
static inline void sm_config_set_clkdiv_int_frac(pio_sm_config *c, uint16_t div_int, uint8_t div_frac) {
    (void)c, (void)div_int, (void)div_frac;
}
static inline uint pio_encode_set(enum pio_src_dest dest, uint value) {
    return 0xe000u | ((uint)dest << 5) | (value & 0x1fu);
}
static inline uint pio_encode_jmp(uint addr) {
    return addr & 0x1fu;
}
static inline uint pio_get_dreq(PIO pio, uint sm, bool is_tx) {
    return PIO_NUM(pio) * 8u + (is_tx ? 0u : 4u) + sm;
}
static inline uint pio_get_funcsel(PIO pio) {
    return GPIO_FUNC_PIO0 + PIO_NUM(pio);
}

uint pio_add_program(PIO pio, const pio_program_t *program);
uint pio_add_program_at_offset(PIO pio, const pio_program_t *program, uint offset);
void pio_remove_program(PIO pio, const pio_program_t *program, uint loaded_offset);
bool pio_can_add_program(PIO pio, const pio_program_t *program);
int pio_sm_init(PIO pio, uint sm, uint initial_pc, const pio_sm_config *config);
void pio_sm_set_enabled(PIO pio, uint sm, bool enabled);
void pio_set_sm_mask_enabled(PIO pio, uint32_t mask, bool enabled);
void pio_enable_sm_mask_in_sync(PIO pio, uint32_t mask);
void pio_sm_restart(PIO pio, uint sm);
void pio_sm_clkdiv_restart(PIO pio, uint sm);
void pio_sm_set_clkdiv_int_frac(PIO pio, uint sm, uint16_t div_int, uint8_t div_frac);
void pio_sm_set_clkdiv(PIO pio, uint sm, float div);
void pio_gpio_init(PIO pio, uint pin);
int pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin_base, uint pin_count, bool is_out);
void pio_sm_set_pins(PIO pio, uint sm, uint32_t pin_values);
void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint32_t pin_values, uint32_t pin_mask);
void pio_sm_set_pindirs_with_mask(PIO pio, uint sm, uint32_t pin_dirs, uint32_t pin_mask);
void pio_sm_clear_fifos(PIO pio, uint sm);
void pio_sm_drain_tx_fifo(PIO pio, uint sm);
void pio_sm_exec(PIO pio, uint sm, uint instr);
void pio_sm_exec_wait_blocking(PIO pio, uint sm, uint instr);
void pio_sm_claim(PIO pio, uint sm);
void pio_sm_unclaim(PIO pio, uint sm);
int pio_claim_unused_sm(PIO pio, bool required);
bool pio_sm_is_claimed(PIO pio, uint sm);
uint pio_sm_get_pc(PIO pio, uint sm);
void pio_sm_put(PIO pio, uint sm, uint32_t data);
uint32_t pio_sm_get(PIO pio, uint sm);
bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm);
bool pio_sm_is_tx_fifo_full(PIO pio, uint sm);
uint pio_sm_get_rx_fifo_level(PIO pio, uint sm);
uint pio_sm_get_tx_fifo_level(PIO pio, uint sm);
bool pio_interrupt_get(PIO pio, uint pio_interrupt_num);
void pio_interrupt_clear(PIO pio, uint pio_interrupt_num);
int pio_set_gpio_base(PIO pio, uint gpio_base);
uint pio_get_gpio_base(PIO pio);
