#pragma once

#include "hardware/address_mapped.h"

// RP2350 DMA API and register layout. dma_hw points at a RAM stand-in, so register reads give
// whatever was last stored there (zero at boot) and no transfer ever runs.
#define NUM_DMA_CHANNELS 16u

typedef struct {
    io_rw_32 read_addr;
    io_rw_32 write_addr;
    io_rw_32 transfer_count;
    io_rw_32 ctrl_trig;
    io_rw_32 al1_ctrl;
    io_rw_32 al1_read_addr;
    io_rw_32 al1_write_addr;
    io_rw_32 al1_transfer_count_trig;
    io_rw_32 al2_ctrl;
    io_rw_32 al2_transfer_count;
    io_rw_32 al2_read_addr;
    io_rw_32 al2_write_addr_trig;
    io_rw_32 al3_ctrl;
    io_rw_32 al3_write_addr;
    io_rw_32 al3_transfer_count;
    io_rw_32 al3_read_addr_trig;
} dma_channel_hw_t;

typedef struct {
    dma_channel_hw_t ch[NUM_DMA_CHANNELS];
    io_rw_32 intr;
    io_rw_32 inte0;
    io_rw_32 intf0;
    io_rw_32 ints0;
    io_rw_32 multi_channel_trigger;
    io_rw_32 sniff_ctrl;
    io_rw_32 sniff_data;
    io_rw_32 fifo_levels;
    io_rw_32 abort;
} dma_hw_t;

extern dma_hw_t pico_shim_dma_hw;
// adat_input.c is built with dma_hw defined as adat_input_pi_dma_hw(), which brings channel 15's write
// address up to date before each read (z4/adat_input_pi.h).
dma_hw_t *adat_input_pi_dma_hw(void);
#ifndef dma_hw
#define dma_hw (&pico_shim_dma_hw)
#endif

#define DMA_CH0_CTRL_TRIG_BUSY_BITS (1u << 26)
#define DMA_CH0_TRANS_COUNT_MODE_LSB 28u
#define DMA_CH0_TRANS_COUNT_MODE_VALUE_ENDLESS 0xFu

static inline dma_channel_hw_t *dma_channel_hw_addr(uint channel) {
    return &dma_hw->ch[channel & 15u];
}
static inline uint32_t dma_encode_endless_transfer_count(void) {
    return 0xFu << 28;
}

enum dma_channel_transfer_size { DMA_SIZE_8 = 0, DMA_SIZE_16 = 1, DMA_SIZE_32 = 2 };

typedef struct {
    uint32_t ctrl;
} dma_channel_config;

static inline void channel_config_set_read_increment(dma_channel_config *c, bool incr) {
    c->ctrl = incr ? (c->ctrl | 0x10u) : (c->ctrl & ~0x10u);
}
static inline void channel_config_set_write_increment(dma_channel_config *c, bool incr) {
    c->ctrl = incr ? (c->ctrl | 0x40u) : (c->ctrl & ~0x40u);
}
static inline void channel_config_set_dreq(dma_channel_config *c, uint dreq) {
    c->ctrl = (c->ctrl & ~(0x3fu << 17)) | ((dreq & 0x3fu) << 17);
}
static inline void channel_config_set_chain_to(dma_channel_config *c, uint chain_to) {
    c->ctrl = (c->ctrl & ~(0xfu << 13)) | ((chain_to & 0xfu) << 13);
}
static inline void channel_config_set_transfer_data_size(dma_channel_config *c, enum dma_channel_transfer_size size) {
    c->ctrl = (c->ctrl & ~(3u << 2)) | ((uint32_t)size << 2);
}
static inline void channel_config_set_ring(dma_channel_config *c, bool write, uint size_bits) {
    c->ctrl = (c->ctrl & ~0xf80u) | ((size_bits & 0xfu) << 8) | (write ? 0x800u : 0u);
}
static inline void channel_config_set_high_priority(dma_channel_config *c, bool high) {
    c->ctrl = high ? (c->ctrl | 0x2u) : (c->ctrl & ~0x2u);
}
static inline void channel_config_set_enable(dma_channel_config *c, bool enable) {
    c->ctrl = enable ? (c->ctrl | 0x1u) : (c->ctrl & ~0x1u);
}
static inline void channel_config_set_irq_quiet(dma_channel_config *c, bool quiet) {
    c->ctrl = quiet ? (c->ctrl | (1u << 23)) : (c->ctrl & ~(1u << 23));
}

void dma_channel_claim(uint channel);
void dma_channel_unclaim(uint channel);
int dma_claim_unused_channel(bool required);
bool dma_channel_is_claimed(uint channel);
dma_channel_config dma_channel_get_default_config(uint channel);
dma_channel_config dma_get_channel_config(uint channel);
void dma_channel_set_config(uint channel, const dma_channel_config *config, bool trigger);
void dma_channel_configure(uint channel, const dma_channel_config *config, volatile void *write_addr,
                           const volatile void *read_addr, uint32_t transfer_count, bool trigger);
void dma_channel_set_read_addr(uint channel, const volatile void *read_addr, bool trigger);
void dma_channel_set_write_addr(uint channel, volatile void *write_addr, bool trigger);
void dma_channel_set_trans_count(uint channel, uint32_t trans_count, bool trigger);
void dma_channel_start(uint channel);
void dma_channel_abort(uint channel);
bool dma_channel_is_busy(uint channel);
void dma_channel_set_irq0_enabled(uint channel, bool enabled);
void dma_channel_set_irq1_enabled(uint channel, bool enabled);
void dma_start_channel_mask(uint32_t chan_mask);
static inline void dma_irqn_set_channel_enabled(uint irq_index, uint channel, bool enabled) {
    (void)irq_index, (void)channel, (void)enabled;
}
static inline void dma_irqn_acknowledge_channel(uint irq_index, uint channel) {
    (void)irq_index, (void)channel;
}
