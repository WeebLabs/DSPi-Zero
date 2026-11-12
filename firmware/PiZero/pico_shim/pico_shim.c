// Implementations behind the pico_shim headers: real time, interrupt masking and barriers on the BCM
// system timer and CPSR, output rings drained by the system timer instead of PIO and DMA, and inert
// stand-ins for the remaining RP2350 peripherals.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/structs/nvic.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/audio_spdif.h"
#include "pico/audio_i2s_multi.h"
#include "pico/i2c_slave.h"

#include "board.h"
#include "mbox.h"
#include "smi_out.h"
#include "adat_output.h"
#include "uart.h"

// After the Pi's uart.h: this one renames the SDK's uart_init.
#include "hardware/uart.h"

#include "broadcom/defines.h"

// ---------------------------------------------------------------------------------------------------
// panic

void panic(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    save_and_disable_interrupts();
    if (uart_ready()) {
        uart_set_buffered(0);
        uart_puts("\nPANIC: ");
        uart_puts(buf);
        uart_puts("\n");
    }
    for (;;) {
        board_led_write(1);
        board_delay_us(250000);
        board_led_write(0);
        board_delay_us(250000);
    }
}

void panic_unsupported(void) {
    panic("unsupported");
}

// A failed assert in DSPi or the SDK shim stops with the panic LED pattern instead of newlib's abort().
void __assert_func(const char *file, int line, const char *func, const char *expr) {
    (void)func;
    panic("assert %s at %s:%d", expr, file, line);
}

// stdout and stderr go to the bring-up UART, so DSPi's printf diagnostics appear in the log.
int _write(int fd, const char *buf, int len) {
    (void)fd;
    if (uart_ready()) {
        for (int i = 0; i < len; i++) {
            uart_putc(buf[i]);      // adds the CR before each LF
        }
    }
    return len;
}

// ---------------------------------------------------------------------------------------------------
// Time: the BCM 1 MHz free-running system timer

uint32_t time_us_32(void) {
    return SYSTMR->CLO;
}

uint64_t time_us_64(void) {
    uint32_t hi, lo;
    do {
        hi = SYSTMR->CHI;
        lo = SYSTMR->CLO;
    } while (hi != SYSTMR->CHI);
    return ((uint64_t)hi << 32) | lo;
}

void busy_wait_us_32(uint32_t delay_us) {
    uint32_t start = time_us_32();
    while (time_us_32() - start < delay_us) {}
}

void busy_wait_us(uint64_t delay_us) {
    uint64_t start = time_us_64();
    while (time_us_64() - start < delay_us) {}
}

void busy_wait_ms(uint32_t delay_ms) {
    busy_wait_us((uint64_t)delay_ms * 1000u);
}

timer_hw_t pico_shim_timer_hw;

int hardware_alarm_claim_unused(bool required) {
    (void)required;
    return -1;      // no RP2350 alarms; DSPi's only user (the flash stall bracket) is not linked
}

uint hardware_alarm_get_irq_num(uint alarm_num) {
    return alarm_num;
}

// ---------------------------------------------------------------------------------------------------
// Spin locks (single core, so they only mask interrupts; see sync.h)

static spin_lock_t spin_locks[32];

spin_lock_t *spin_lock_instance(uint lock_num) {
    return &spin_locks[lock_num & 31u];
}

uint spin_lock_claim_unused(bool required) {
    static uint next = 16;
    (void)required;
    return next < 32u ? next++ : 31u;
}

// ---------------------------------------------------------------------------------------------------
// RP2350 interrupt controller: nothing to drive

void irq_set_exclusive_handler(uint num, irq_handler_t handler) {
    (void)num, (void)handler;
}
void irq_add_shared_handler(uint num, irq_handler_t handler, uint8_t order_priority) {
    (void)num, (void)handler, (void)order_priority;
}
void irq_remove_handler(uint num, irq_handler_t handler) {
    (void)num, (void)handler;
}
void irq_set_enabled(uint num, bool enabled) {
    (void)num, (void)enabled;
}
bool irq_is_enabled(uint num) {
    (void)num;
    return false;
}
void irq_set_priority(uint num, uint8_t hardware_priority) {
    (void)num, (void)hardware_priority;
}
void irq_set_mask_enabled(uint32_t mask, bool enabled) {
    (void)mask, (void)enabled;
}
void irq_set_pending(uint num) {
    (void)num;
}

// ---------------------------------------------------------------------------------------------------
// DMA: register stand-in, claim bookkeeping, no transfers

dma_hw_t pico_shim_dma_hw;
static uint16_t dma_claimed;

void dma_channel_claim(uint channel) {
    dma_claimed |= (uint16_t)(1u << (channel & 15u));
}
void dma_channel_unclaim(uint channel) {
    dma_claimed &= (uint16_t)~(1u << (channel & 15u));
}
bool dma_channel_is_claimed(uint channel) {
    return (dma_claimed >> (channel & 15u)) & 1u;
}
int dma_claim_unused_channel(bool required) {
    for (uint ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
        if (!dma_channel_is_claimed(ch)) {
            dma_channel_claim(ch);
            return (int)ch;
        }
    }
    if (required) {
        panic("no DMA channel");
    }
    return -1;
}
dma_channel_config dma_channel_get_default_config(uint channel) {
    dma_channel_config c = {.ctrl = 1u | (2u << 2) | 0x10u | (0x3fu << 17) | ((channel & 15u) << 13)};
    return c;
}
dma_channel_config dma_get_channel_config(uint channel) {
    dma_channel_config c = {.ctrl = pico_shim_dma_hw.ch[channel & 15u].al1_ctrl};
    return c;
}
void dma_channel_set_config(uint channel, const dma_channel_config *config, bool trigger) {
    (void)trigger;
    pico_shim_dma_hw.ch[channel & 15u].al1_ctrl = config->ctrl;
}
void dma_channel_configure(uint channel, const dma_channel_config *config, volatile void *write_addr,
                           const volatile void *read_addr, uint32_t transfer_count, bool trigger) {
    (void)trigger;
    dma_channel_hw_t *ch = &pico_shim_dma_hw.ch[channel & 15u];
    ch->al1_ctrl = config->ctrl;
    ch->write_addr = (uint32_t)(uintptr_t)write_addr;
    ch->read_addr = (uint32_t)(uintptr_t)read_addr;
    ch->transfer_count = transfer_count;
}
void dma_channel_set_read_addr(uint channel, const volatile void *read_addr, bool trigger) {
    (void)trigger;
    pico_shim_dma_hw.ch[channel & 15u].read_addr = (uint32_t)(uintptr_t)read_addr;
}
void dma_channel_set_write_addr(uint channel, volatile void *write_addr, bool trigger) {
    (void)trigger;
    pico_shim_dma_hw.ch[channel & 15u].write_addr = (uint32_t)(uintptr_t)write_addr;
}
void dma_channel_set_trans_count(uint channel, uint32_t trans_count, bool trigger) {
    (void)trigger;
    pico_shim_dma_hw.ch[channel & 15u].transfer_count = trans_count;
}
void dma_channel_start(uint channel) {
    (void)channel;
}
void dma_channel_abort(uint channel) {
    (void)channel;
}
bool dma_channel_is_busy(uint channel) {
    (void)channel;
    return false;
}
void dma_channel_set_irq0_enabled(uint channel, bool enabled) {
    (void)channel, (void)enabled;
}
void dma_channel_set_irq1_enabled(uint channel, bool enabled) {
    (void)channel, (void)enabled;
}
void dma_start_channel_mask(uint32_t chan_mask) {
    (void)chan_mask;
}

// ---------------------------------------------------------------------------------------------------
// PIO: register stand-ins, no state machines

pio_hw_t pico_shim_pio_hw[3];
static uint8_t pio_sm_claimed[3];

uint pio_add_program(PIO pio, const pio_program_t *program) {
    (void)pio, (void)program;
    return 0;
}
uint pio_add_program_at_offset(PIO pio, const pio_program_t *program, uint offset) {
    (void)pio, (void)program;
    return offset;
}
void pio_remove_program(PIO pio, const pio_program_t *program, uint loaded_offset) {
    (void)pio, (void)program, (void)loaded_offset;
}
bool pio_can_add_program(PIO pio, const pio_program_t *program) {
    (void)pio, (void)program;
    return true;
}
int pio_sm_init(PIO pio, uint sm, uint initial_pc, const pio_sm_config *config) {
    (void)pio, (void)sm, (void)initial_pc, (void)config;
    return 0;
}
static void shim_adat_enable(bool on);

void pio_sm_set_enabled(PIO pio, uint sm, bool enabled) {
    if (PIO_NUM(pio) == 1u && sm == 1u) {
        shim_adat_enable(enabled);  // adat_output.c's state machine
    }
    if (enabled) {
        pio->ctrl |= 1u << sm;
    } else {
        pio->ctrl &= ~(1u << sm);
    }
}
void pio_set_sm_mask_enabled(PIO pio, uint32_t mask, bool enabled) {
    if (enabled) {
        pio->ctrl |= mask & 0xfu;
    } else {
        pio->ctrl &= ~(mask & 0xfu);
    }
}
void pio_enable_sm_mask_in_sync(PIO pio, uint32_t mask) {
    pio->ctrl |= mask & 0xfu;
}
void pio_sm_restart(PIO pio, uint sm) {
    (void)pio, (void)sm;
}
void pio_sm_clkdiv_restart(PIO pio, uint sm) {
    (void)pio, (void)sm;
}
void pio_sm_set_clkdiv_int_frac(PIO pio, uint sm, uint16_t div_int, uint8_t div_frac) {
    (void)pio, (void)sm, (void)div_int, (void)div_frac;
}
void pio_sm_set_clkdiv(PIO pio, uint sm, float div) {
    (void)pio, (void)sm, (void)div;
}
void pio_gpio_init(PIO pio, uint pin) {
    gpio_set_function(pin, (gpio_function_t)pio_get_funcsel(pio));
}
int pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin_base, uint pin_count, bool is_out) {
    (void)pio, (void)sm, (void)pin_base, (void)pin_count, (void)is_out;
    return 0;
}
void pio_sm_set_pins(PIO pio, uint sm, uint32_t pin_values) {
    (void)pio, (void)sm, (void)pin_values;
}
void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint32_t pin_values, uint32_t pin_mask) {
    (void)pio, (void)sm, (void)pin_values, (void)pin_mask;
}
void pio_sm_set_pindirs_with_mask(PIO pio, uint sm, uint32_t pin_dirs, uint32_t pin_mask) {
    (void)pio, (void)sm, (void)pin_dirs, (void)pin_mask;
}
void pio_sm_clear_fifos(PIO pio, uint sm) {
    (void)pio, (void)sm;
}
void pio_sm_drain_tx_fifo(PIO pio, uint sm) {
    (void)pio, (void)sm;
}
void pio_sm_exec(PIO pio, uint sm, uint instr) {
    (void)pio, (void)sm, (void)instr;
}
void pio_sm_exec_wait_blocking(PIO pio, uint sm, uint instr) {
    (void)pio, (void)sm, (void)instr;
}
void pio_sm_claim(PIO pio, uint sm) {
    pio_sm_claimed[PIO_NUM(pio)] |= (uint8_t)(1u << sm);
}
void pio_sm_unclaim(PIO pio, uint sm) {
    pio_sm_claimed[PIO_NUM(pio)] &= (uint8_t)~(1u << sm);
}
bool pio_sm_is_claimed(PIO pio, uint sm) {
    return (pio_sm_claimed[PIO_NUM(pio)] >> sm) & 1u;
}
int pio_claim_unused_sm(PIO pio, bool required) {
    for (uint sm = 0; sm < NUM_PIO_STATE_MACHINES; sm++) {
        if (!pio_sm_is_claimed(pio, sm)) {
            pio_sm_claim(pio, sm);
            return (int)sm;
        }
    }
    if (required) {
        panic("no PIO state machine");
    }
    return -1;
}
uint pio_sm_get_pc(PIO pio, uint sm) {
    (void)pio, (void)sm;
    return 0;
}
void pio_sm_put(PIO pio, uint sm, uint32_t data) {
    (void)pio, (void)sm, (void)data;
}
uint32_t pio_sm_get(PIO pio, uint sm) {
    (void)pio, (void)sm;
    return 0;
}
bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm) {
    (void)pio, (void)sm;
    return true;
}
bool pio_sm_is_tx_fifo_full(PIO pio, uint sm) {
    (void)pio, (void)sm;
    return false;
}
uint pio_sm_get_rx_fifo_level(PIO pio, uint sm) {
    (void)pio, (void)sm;
    return 0;
}
uint pio_sm_get_tx_fifo_level(PIO pio, uint sm) {
    (void)pio, (void)sm;
    return 0;
}
bool pio_interrupt_get(PIO pio, uint pio_interrupt_num) {
    (void)pio, (void)pio_interrupt_num;
    return false;
}
void pio_interrupt_clear(PIO pio, uint pio_interrupt_num) {
    (void)pio, (void)pio_interrupt_num;
}
int pio_set_gpio_base(PIO pio, uint gpio_base) {
    (void)pio, (void)gpio_base;
    return 0;
}
uint pio_get_gpio_base(PIO pio) {
    (void)pio;
    return 0;
}

// ---------------------------------------------------------------------------------------------------
// GPIO: remembered function and direction only

static uint8_t gpio_fn[NUM_BANK0_GPIOS];
static uint64_t gpio_out_dir, gpio_out_level;

static inline uint gpio_idx(uint gpio) {
    return gpio < NUM_BANK0_GPIOS ? gpio : NUM_BANK0_GPIOS - 1u;
}

void gpio_init(uint gpio) {
    gpio_set_dir(gpio, GPIO_IN);
    gpio_put(gpio, 0);
    gpio_set_function(gpio, GPIO_FUNC_SIO);
}
void gpio_deinit(uint gpio) {
    gpio_set_function(gpio, GPIO_FUNC_NULL);
}
void gpio_set_function(uint gpio, gpio_function_t fn) {
    gpio_fn[gpio_idx(gpio)] = (uint8_t)fn;
}
gpio_function_t gpio_get_function(uint gpio) {
    // The table starts zeroed, but the RP2350 resets every pin to NULL.
    uint8_t fn = gpio_fn[gpio_idx(gpio)];
    return fn ? (gpio_function_t)fn : GPIO_FUNC_NULL;
}
void gpio_set_dir(uint gpio, bool out) {
    uint64_t bit = 1ull << gpio_idx(gpio);
    gpio_out_dir = out ? (gpio_out_dir | bit) : (gpio_out_dir & ~bit);
}
bool gpio_get_dir(uint gpio) {
    return (gpio_out_dir >> gpio_idx(gpio)) & 1u;
}
void gpio_put(uint gpio, bool value) {
    uint64_t bit = 1ull << gpio_idx(gpio);
    gpio_out_level = value ? (gpio_out_level | bit) : (gpio_out_level & ~bit);
}
bool gpio_get(uint gpio) {
    return (gpio_out_level >> gpio_idx(gpio)) & 1u;
}
void gpio_pull_up(uint gpio) {
    (void)gpio;
}
void gpio_pull_down(uint gpio) {
    (void)gpio;
}
void gpio_disable_pulls(uint gpio) {
    (void)gpio;
}
void gpio_set_input_enabled(uint gpio, bool enabled) {
    (void)gpio, (void)enabled;
}

// ---------------------------------------------------------------------------------------------------
// Clocks: the RP2350 frequencies DSPi sets up, reported back for its divider arithmetic

uint32_t clock_get_hz(clock_handle_t clock) {
    switch (clock) {
    case clk_sys:
    case clk_peri:
        return SHIM_CLK_SYS_HZ;
    case clk_ref:
        return 12000000u;
    case clk_usb:
    case clk_adc:
        return 48000000u;
    default:
        return 0;
    }
}
uint32_t frequency_count_khz(uint src) {
    (void)src;
    return SHIM_CLK_SYS_HZ / 1000u;
}
bool set_sys_clock_hz(uint32_t freq_hz, bool required) {
    (void)required;
    return freq_hz == SHIM_CLK_SYS_HZ;
}
bool set_sys_clock_khz(uint32_t freq_khz, bool required) {
    return set_sys_clock_hz(freq_khz * 1000u, required);
}
void set_sys_clock_pll(uint32_t vco_freq, uint post_div1, uint post_div2) {
    (void)vco_freq, (void)post_div1, (void)post_div2;
}
bool check_sys_clock_hz(uint32_t freq_hz, uint *vco_freq_out, uint *post_div1_out, uint *post_div2_out) {
    (void)vco_freq_out, (void)post_div1_out, (void)post_div2_out;
    return freq_hz == SHIM_CLK_SYS_HZ;
}
void clock_gpio_init_int_frac8(uint gpio, uint src, uint32_t div_int, uint8_t div_frac) {
    (void)gpio, (void)src, (void)div_int, (void)div_frac;
}
void clock_gpio_init_int_frac16(uint gpio, uint src, uint32_t div_int, uint16_t div_frac) {
    (void)gpio, (void)src, (void)div_int, (void)div_frac;
}
void clock_gpio_init(uint gpio, uint src, float div) {
    (void)gpio, (void)src, (void)div;
}

// ---------------------------------------------------------------------------------------------------
// ADC, regulator, multicore, boot ROM, board ID

static uint adc_input;

void adc_init(void) {}
void adc_gpio_init(uint gpio) {
    (void)gpio;
}
void adc_select_input(uint input) {
    adc_input = input;
}
uint adc_get_selected_input(void) {
    return adc_input;
}
void adc_set_temp_sensor_enabled(bool enable) {
    (void)enable;
}
// The sensor's code for 27 C: 0.706 V on a 3.3 V, 12-bit scale.
#define ADC_TEMP_27C_CODE 876u

uint16_t adc_read(void) {
    return adc_input == ADC_TEMPERATURE_CHANNEL_NUM ? ADC_TEMP_27C_CODE : 0;
}
void adc_set_round_robin(uint input_mask) {
    (void)input_mask;
}
void adc_fifo_setup(bool en, bool dreq_en, uint16_t dreq_thresh, bool err_in_fifo, bool byte_shift) {
    (void)en, (void)dreq_en, (void)dreq_thresh, (void)err_in_fifo, (void)byte_shift;
}
void adc_run(bool run) {
    (void)run;
}
void adc_set_clkdiv(float clkdiv) {
    (void)clkdiv;
}

static enum vreg_voltage vreg_level = VREG_VOLTAGE_DEFAULT;

void vreg_set_voltage(enum vreg_voltage voltage) {
    vreg_level = voltage;
}
enum vreg_voltage vreg_get_voltage(void) {
    return vreg_level;
}
void vreg_disable_voltage_limit(void) {}

void multicore_launch_core1(void (*entry)(void)) {
    (void)entry;
}
void multicore_reset_core1(void) {}
void multicore_lockout_victim_init(void) {}
bool multicore_lockout_victim_is_initialized(uint core_num) {
    (void)core_num;
    return false;
}
void multicore_lockout_start_blocking(void) {}
bool multicore_lockout_start_timeout_us(uint64_t timeout_us) {
    (void)timeout_us;
    return true;
}
void multicore_lockout_end_blocking(void) {}
bool multicore_lockout_end_timeout_us(uint64_t timeout_us) {
    (void)timeout_us;
    return true;
}
void multicore_fifo_push_blocking(uint32_t data) {
    (void)data;
}
uint32_t multicore_fifo_pop_blocking(void) {
    return 0;
}
bool multicore_fifo_rvalid(void) {
    return false;
}
bool multicore_fifo_wready(void) {
    return true;
}
void multicore_fifo_drain(void) {}

void reset_usb_boot(uint32_t usb_activity_gpio_pin_mask, uint32_t disable_interface_mask) {
    (void)usb_activity_gpio_pin_mask, (void)disable_interface_mask;
    board_reboot();     // back to the SD card's USB loader
}
void rom_reset_usb_boot(uint32_t usb_activity_gpio_pin_mask, uint32_t disable_interface_mask) {
    reset_usb_boot(usb_activity_gpio_pin_mask, disable_interface_mask);
}

void pico_get_unique_board_id(pico_unique_board_id_t *id_out) {
    uint64_t serial = mbox_board_serial();
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
        id_out->id[i] = (uint8_t)(serial >> (56 - 8 * i));
    }
}

void pico_get_unique_board_id_string(char *id_out, uint len) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    uint n = 0;
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES && n + 2 < len; i++) {
        static const char hex[] = "0123456789ABCDEF";
        id_out[n++] = hex[id.id[i] >> 4];
        id_out[n++] = hex[id.id[i] & 0xfu];
    }
    if (len) {
        id_out[n] = '\0';
    }
}

bool stdio_init_all(void) {
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Flash: a RAM window over the top of a 4 MB part (see hardware/flash.h)

uint8_t pico_shim_flash_window[PICO_SHIM_FLASH_WINDOW_BYTES] __attribute__((aligned(FLASH_SECTOR_SIZE)));
nvic_hw_t pico_shim_nvic_hw;

static uint8_t *flash_window_ptr(uint32_t flash_offs, size_t count) {
    const uint32_t base = PICO_FLASH_SIZE_BYTES - PICO_SHIM_FLASH_WINDOW_BYTES;
    if (flash_offs < base || count > PICO_FLASH_SIZE_BYTES - flash_offs) {
        panic("flash 0x%lx+%u outside the RAM window", (unsigned long)flash_offs, (unsigned)count);
    }
    return &pico_shim_flash_window[flash_offs - base];
}

void pico_shim_flash_init(void) {
    memset(pico_shim_flash_window, 0xff, sizeof(pico_shim_flash_window));
}

void flash_range_erase(uint32_t flash_offs, size_t count) {
    if ((flash_offs | count) & (FLASH_SECTOR_SIZE - 1u)) {
        panic("flash erase not sector aligned");
    }
    memset(flash_window_ptr(flash_offs, count), 0xff, count);
}

void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count) {
    if ((flash_offs | count) & (FLASH_PAGE_SIZE - 1u)) {
        panic("flash program not page aligned");
    }
    uint8_t *dst = flash_window_ptr(flash_offs, count);
    for (size_t i = 0; i < count; i++) {
        dst[i] &= data[i];      // programming only clears bits
    }
}

// ---------------------------------------------------------------------------------------------------
// UART, I2C, PWM and GPIO edge interrupts: Control Surfaces and the UART/I2C control interfaces

uart_hw_t pico_shim_uart_hw[2];
i2c_hw_t pico_shim_i2c_hw[2];

uint uart_init(uart_inst_t *uart, uint baudrate) {
    (void)uart;
    return baudrate;
}
void uart_deinit(uart_inst_t *uart) {
    (void)uart;
}
uint uart_set_baudrate(uart_inst_t *uart, uint baudrate) {
    (void)uart;
    return baudrate;
}
void uart_set_format(uart_inst_t *uart, uint data_bits, uint stop_bits, uart_parity_t parity) {
    (void)uart, (void)data_bits, (void)stop_bits, (void)parity;
}
void uart_set_hw_flow(uart_inst_t *uart, bool cts, bool rts) {
    (void)uart, (void)cts, (void)rts;
}
void uart_set_fifo_enabled(uart_inst_t *uart, bool enabled) {
    (void)uart, (void)enabled;
}
void uart_set_irq_enables(uart_inst_t *uart, bool rx_has_data, bool tx_needs_data) {
    (void)uart, (void)rx_has_data, (void)tx_needs_data;
}
bool uart_is_readable(uart_inst_t *uart) {
    (void)uart;
    return false;
}
bool uart_is_writable(uart_inst_t *uart) {
    (void)uart;
    return true;    // bytes written go nowhere
}

uint i2c_init(i2c_inst_t *i2c, uint baudrate) {
    (void)i2c;
    return baudrate;
}
void i2c_deinit(i2c_inst_t *i2c) {
    (void)i2c;
}
size_t i2c_get_write_available(i2c_inst_t *i2c) {
    (void)i2c;
    return 0;
}
size_t i2c_get_read_available(i2c_inst_t *i2c) {
    (void)i2c;
    return 0;
}
uint8_t i2c_read_byte_raw(i2c_inst_t *i2c) {
    (void)i2c;
    return 0;
}
void i2c_write_byte_raw(i2c_inst_t *i2c, uint8_t value) {
    (void)i2c, (void)value;
}
void i2c_slave_init(i2c_inst_t *i2c, uint8_t address, i2c_slave_handler_t handler) {
    (void)i2c, (void)address, (void)handler;
}
void i2c_slave_deinit(i2c_inst_t *i2c) {
    (void)i2c;
}

void pwm_set_wrap(uint slice_num, uint16_t wrap) {
    (void)slice_num, (void)wrap;
}
void pwm_set_clkdiv(uint slice_num, float divider) {
    (void)slice_num, (void)divider;
}
void pwm_set_gpio_level(uint gpio, uint16_t level) {
    (void)gpio, (void)level;
}
void pwm_set_enabled(uint slice_num, bool enabled) {
    (void)slice_num, (void)enabled;
}

void gpio_set_irq_enabled(uint gpio, uint32_t event_mask, bool enabled) {
    (void)gpio, (void)event_mask, (void)enabled;
}
void gpio_acknowledge_irq(uint gpio, uint32_t event_mask) {
    (void)gpio, (void)event_mask;
}

// ---------------------------------------------------------------------------------------------------
// Output slot rings, drained by the system timer

// Output clock: frames taken from the slot rings since boot. Once the SMI runs it is the composer's frame
// count: the composer is what reads the rings, so DSPi's fill and underrun accounting see the real reader,
// and every ring and the USB consumer follow the output hardware. Before that it runs from the system
// timer. Every output runs at one rate, as on the RP2350.
static uint32_t oc_frames, oc_last_us, oc_frac, oc_freq = 48000;
static bool oc_smi;

static void spdif_set_status_rate(uint32_t freq);

uint32_t shim_output_clock(void) {
    if (oc_smi) {
        return smi_out_composed();
    }
    uint32_t save = save_and_disable_interrupts();
    uint32_t now = time_us_32();
    {
        uint64_t acc = (uint64_t)(now - oc_last_us) * oc_freq + oc_frac;
        uint32_t frames = (uint32_t)(acc / 1000000u);
        oc_frac = (uint32_t)(acc - (uint64_t)frames * 1000000u);
        oc_frames += frames;
    }
    oc_last_us = now;
    restore_interrupts(save);
    return oc_frames;
}

static void shim_output_clock_set_rate(uint32_t freq) {
    shim_output_clock();    // frames so far count at the old rate
    oc_freq = freq;
    spdif_set_status_rate(freq);
    if (oc_smi) {
        smi_out_set_rate(freq);
    }
}

// Accounting as in pico_audio_spdif_multi and pico_audio_i2s_multi, field for field, so DSPi's
// admission, recovery and fill logic runs as on the RP2350. Only the reader differs: an enabled ring
// consumes what the output clock counts from the moment it was enabled. last_read_time_us holds the
// output clock at the last read.

// The ring contents match the RP2350 libraries': S/PDIF frames as NRZI-coded cells, I2S frames as
// left-justified 32-bit L and R words. consumed_words is frames times the real words per frame.
#define SHIM_RING_FUNCS(P, T, RING_FRAMES_, ERASE_AHEAD_, WORDS_PER_FRAME_)                              \
    static void P##_shim_stamp_silence(T *inst, uint32_t e, uint32_t n) {                                 \
        P##_stamp_silence_run((uint32_t *)inst->ring, e, n);                                              \
    }                                                                                                     \
                                                                                                          \
    /* Brings consumed_frames up to the output clock. Interrupts must be masked. */                      \
    static void P##_shim_advance(T *inst) {                                                               \
        uint32_t clk = shim_output_clock();                                                               \
        uint32_t frames = clk - inst->last_read_time_us;                                                  \
        inst->last_read_time_us = clk;                                                                    \
        if (!inst->enabled || inst->freq == 0) {                                                          \
            return;                                                                                       \
        }                                                                                                 \
        inst->consumed_frames += frames;                                                                  \
        inst->consumed_words += frames * (WORDS_PER_FRAME_);                                              \
    }                                                                                                     \
                                                                                                          \
    static void P##_shim_start(T *inst) {                                                                 \
        inst->last_read_time_us = shim_output_clock();                                                    \
        inst->last_read_addr = 0;                                                                         \
        inst->enabled = true;                                                                             \
    }                                                                                                     \
                                                                                                          \
    static void P##_shim_stop(T *inst) {                                                                  \
        P##_shim_advance(inst);                                                                           \
        inst->enabled = false;                                                                            \
    }                                                                                                     \
                                                                                                          \
    uint32_t P##_ring_consumed_words(T *inst) {                                                           \
        uint32_t save = save_and_disable_interrupts();                                                    \
        P##_shim_advance(inst);                                                                           \
        uint32_t out = inst->consumed_words;                                                              \
        restore_interrupts(save);                                                                         \
        return out;                                                                                       \
    }                                                                                                     \
                                                                                                          \
    uint32_t P##_ring_consumed_frames(T *inst) {                                                          \
        uint32_t save = save_and_disable_interrupts();                                                    \
        P##_shim_advance(inst);                                                                           \
        uint32_t out = inst->consumed_frames;                                                             \
        restore_interrupts(save);                                                                         \
        return out;                                                                                       \
    }                                                                                                     \
                                                                                                          \
    uint32_t P##_ring_fill_frames(T *inst) {                                                              \
        return inst->wr_frames - P##_ring_consumed_frames(inst);                                          \
    }                                                                                                     \
                                                                                                          \
    void P##_ring_reset(T *inst) {                                                                        \
        uint32_t save = save_and_disable_interrupts();                                                    \
        inst->wr_frames = 0;                                                                              \
        inst->erased_frames = 0;                                                                          \
        inst->consumed_words = 0;                                                                         \
        inst->consumed_frames = 0;                                                                        \
        inst->consumed_word_rem = 0;                                                                      \
        inst->last_read_addr = 0;                                                                         \
        inst->last_read_time_us = shim_output_clock();                                                    \
        inst->snap_produced_frames = 0;                                                                   \
        inst->snap_consumed_frames = 0;                                                                   \
        inst->recovery_prewrite_fill_frames = UINT32_MAX;                                                 \
        restore_interrupts(save);                                                                         \
        P##_ring_silence_pump(inst, (RING_FRAMES_));                                                      \
    }                                                                                                     \
                                                                                                          \
    void P##_ring_skip(T *inst, uint32_t frames) {                                                        \
        inst->underrun_frames += frames;                                                                  \
        inst->wr_frames += frames;                                                                        \
        uint32_t target = inst->wr_frames + (ERASE_AHEAD_);                                               \
        uint32_t e = inst->erased_frames;                                                                 \
        if ((int32_t)(target - e) > (int32_t)(RING_FRAMES_)) {                                            \
            e = target - (RING_FRAMES_);                                                                  \
        }                                                                                                 \
        if ((int32_t)(target - e) > 0) {                                                                  \
            P##_shim_stamp_silence(inst, e, target - e);                                                  \
            e = target;                                                                                   \
        }                                                                                                 \
        inst->erased_frames = e;                                                                          \
    }                                                                                                     \
                                                                                                          \
    void P##_ring_silence_pump(T *inst, uint32_t max_frames) {                                            \
        uint32_t consumed = P##_ring_consumed_frames(inst);                                               \
        uint32_t target = consumed + (RING_FRAMES_);                                                      \
        uint32_t e = inst->erased_frames;                                                                 \
        if ((int32_t)(e - inst->wr_frames) < 0) {                                                         \
            e = inst->wr_frames;                                                                          \
        }                                                                                                 \
        int32_t need = (int32_t)(target - e);                                                             \
        if (need <= 0) {                                                                                  \
            return;                                                                                       \
        }                                                                                                 \
        uint32_t n = ((uint32_t)need > max_frames) ? max_frames : (uint32_t)need;                         \
        P##_shim_stamp_silence(inst, e, n);                                                               \
        inst->erased_frames = e + n;                                                                      \
    }                                                                                                     \
                                                                                                          \
    void P##_ring_write_s32(T *inst, const int32_t *lr, uint32_t frames) {                                \
        uint32_t consumed = P##_ring_consumed_frames(inst);                                               \
        int32_t signed_fill = (int32_t)(inst->wr_frames - consumed);                                      \
        uint32_t fill = (signed_fill > 0) ? (uint32_t)signed_fill : 0u;                                   \
        uint32_t writable_max = (RING_FRAMES_) - (ERASE_AHEAD_);                                          \
        uint32_t writable = (fill < writable_max) ? (writable_max - fill) : 0u;                           \
        if (frames > writable) {                                                                          \
            inst->drop_frames += frames;                                                                  \
            inst->snap_produced_frames = inst->wr_frames;                                                 \
            inst->snap_consumed_frames = consumed;                                                        \
            return;                                                                                       \
        }                                                                                                 \
        uint32_t w = inst->wr_frames;                                                                     \
        P##_stamp_audio_run((uint32_t *)inst->ring, w, lr, frames);                                       \
        w += frames;                                                                                      \
        inst->wr_frames = w;                                                                              \
        uint32_t e = inst->erased_frames;                                                                 \
        if ((int32_t)(e - w) < 0) {                                                                       \
            e = w;                                                                                        \
        }                                                                                                 \
        uint32_t target = w + (ERASE_AHEAD_);                                                             \
        if ((int32_t)(target - e) > 0) {                                                                  \
            P##_shim_stamp_silence(inst, e, target - e);                                                  \
            e = target;                                                                                   \
        }                                                                                                 \
        inst->erased_frames = e;                                                                          \
        inst->recovery_prewrite_fill_frames = fill;                                                       \
        inst->snap_produced_frames = w;                                                                   \
        inst->snap_consumed_frames = consumed;                                                            \
    }

// S/PDIF encoding, as pico_audio_spdif_multi's software encoder (pico-extras audio_spdif.c and
// sample_encoding.h, BSD-3-Clause, Raspberry Pi Ltd): each subframe is 64 NRZI cells, LSB first, where a
// 1 toggles the line. The channel-status bit comes from the absolute frame index modulo 192.
#define PREAMBLE_X 0b11001001u
#define PREAMBLE_Y 0b01101001u
#define PREAMBLE_Z 0b00111001u

static uint32_t spdif_lookup[256];
static uint8_t spdif_channel_status[5] = {0x04, 0x00, 0x00, 0x02, 0x0B};   // byte 3: sample rate

static void spdif_lookup_init(void) {
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t v = 0x5555u, p = 0;
        for (uint32_t j = 0; j < 8u; j++) {
            if (i & (1u << j)) {
                p ^= 1u;
                v |= 2u << (j * 2u);
            }
        }
        spdif_lookup[i] = v | (p << 16u);
    }
}

static void spdif_set_status_rate(uint32_t freq) {  // channel status byte 3
    switch (freq) {
        case 44100: spdif_channel_status[3] = 0x00; break;
        case 48000: spdif_channel_status[3] = 0x02; break;
        case 96000: spdif_channel_status[3] = 0x0A; break;
        default: spdif_channel_status[3] = 0x01; break;
    }
}

static inline uint32_t spdif_cs_bit(uint32_t phase) {
    return phase < 40u ? (spdif_channel_status[phase / 8u] >> (phase % 8u)) & 1u : 0u;
}

static inline void spdif_update_subframe(uint32_t *sf, int32_t sample) {
    uint32_t s0 = spdif_lookup[(uint8_t)sample];
    uint32_t s1 = spdif_lookup[(uint8_t)(sample >> 8u)];
    uint32_t s2 = spdif_lookup[(uint8_t)(sample >> 16u)];
    sf[0] = (sf[0] & 0xFFu) | ((uint32_t)(uint16_t)s0 << 8u) | (s1 << 24u);
    uint32_t ph = sf[1] >> 24u;
    uint32_t h = ((uint32_t)(uint16_t)s1 >> 8u) | ((uint32_t)(uint16_t)s2 << 8u);
    uint32_t p = (s0 >> 16u) ^ (s1 >> 16u) ^ (s2 >> 16u);
    p ^= (((ph & 0x2Au) * 0x2Au) >> 6u) & 1u;
    sf[1] = h | ((ph & 0x7Fu) << 24u) | (p << 31u);
}

static void audio_spdif_stamp_audio_run(uint32_t *ring, uint32_t w, const int32_t *lr, uint32_t n) {
    uint32_t phase = w % PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t *f = &ring[((w + i) & (PICO_AUDIO_SPDIF_RING_FRAMES - 1u)) * 4u];
        uint32_t c = spdif_cs_bit(phase);
        f[0] = phase == 0 ? PREAMBLE_Z : PREAMBLE_X;
        f[1] = 0x55000000u | (c << 29u);
        spdif_update_subframe(&f[0], lr[2u * i]);
        f[2] = PREAMBLE_Y;
        f[3] = 0x55000000u | (c << 29u);
        spdif_update_subframe(&f[2], lr[2u * i + 1u]);
        if (++phase == PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT) {
            phase = 0;
        }
    }
}

static void audio_spdif_stamp_silence_run(uint32_t *ring, uint32_t e, uint32_t n) {
    uint32_t phase = e % PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT;
    while (n--) {
        uint32_t *f = &ring[(e++ & (PICO_AUDIO_SPDIF_RING_FRAMES - 1u)) * 4u];
        uint32_t c = spdif_cs_bit(phase);
        uint32_t h = 0x55555555u | (c << 29u) | (c << 31u);
        f[0] = 0x55555500u | (phase == 0 ? PREAMBLE_Z : PREAMBLE_X);
        f[1] = h;
        f[2] = 0x55555500u | PREAMBLE_Y;
        f[3] = h;
        if (++phase == PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT) {
            phase = 0;
        }
    }
}

// I2S words as pico_audio_i2s_multi stores them: the 24-bit sample left-justified, with 0x01 in the pad
// byte so a DAC's zero-detect mute never latches.
#define I2S_PAD_PATTERN 0x01u

static void audio_i2s_stamp_audio_run(uint32_t *ring, uint32_t w, const int32_t *lr, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t *f = &ring[((w + i) & (PICO_AUDIO_I2S_RING_FRAMES - 1u)) * 2u];
        f[0] = (uint32_t)lr[2u * i] << 8 | I2S_PAD_PATTERN;
        f[1] = (uint32_t)lr[2u * i + 1u] << 8 | I2S_PAD_PATTERN;
    }
}

static void audio_i2s_stamp_silence_run(uint32_t *ring, uint32_t e, uint32_t n) {
    while (n--) {
        uint32_t *f = &ring[(e++ & (PICO_AUDIO_I2S_RING_FRAMES - 1u)) * 2u];
        f[0] = I2S_PAD_PATTERN;
        f[1] = I2S_PAD_PATTERN;
    }
}

// S/PDIF frames are two subframes of two words; I2S frames are two words.
SHIM_RING_FUNCS(audio_spdif, audio_spdif_instance_t, PICO_AUDIO_SPDIF_RING_FRAMES, PICO_AUDIO_SPDIF_ERASE_AHEAD, 4u)
SHIM_RING_FUNCS(audio_i2s, audio_i2s_instance_t, PICO_AUDIO_I2S_RING_FRAMES, PICO_AUDIO_I2S_ERASE_AHEAD, 2u)

// ---------------------------------------------------------------------------------------------------
// S/PDIF instances

static audio_spdif_instance_t *spdif_instances[PICO_AUDIO_SPDIF_MAX_INSTANCES];
static uint spdif_instance_count;
static int8_t spdif_encoder_loaded[3] = {-1, -1, -1};

static PIO pio_for_index(uint8_t index) {
    return index < NUM_PIOS ? PIO_INSTANCE(index) : pio0;
}

void audio_spdif_setup(audio_spdif_instance_t *inst, uint32_t sample_freq, const audio_spdif_config_t *config) {
    inst->pio = pio_for_index(config->pio);
    inst->pio_sm = config->pio_sm;
    inst->dma_channel = config->dma_channel;
    inst->dma_reload_channel = config->dma_reload_channel;
    inst->pin = config->pin;
    inst->freq = sample_freq;
    shim_output_clock_set_rate(sample_freq);
    inst->enabled = false;
    inst->instance_index = config->dma_channel;
    inst->ring = (uint32_t *)config->ring_base;
    inst->underrun_frames = 0;
    inst->drop_frames = 0;
    inst->pio_bmc = true;
    audio_spdif_ring_reset(inst);
    gpio_set_function(config->pin, (gpio_function_t)pio_get_funcsel(inst->pio));
    pio_sm_claim(inst->pio, inst->pio_sm);
    dma_channel_claim(inst->dma_channel);
    if (config->pio < 3u) {
        spdif_encoder_loaded[config->pio] = 1;
    }
    if (spdif_instance_count < PICO_AUDIO_SPDIF_MAX_INSTANCES) {
        spdif_instances[spdif_instance_count++] = inst;
    }
}

void audio_spdif_set_enabled(audio_spdif_instance_t *inst, bool enabled) {
    if (enabled == inst->enabled) {
        return;
    }
    uint32_t save = save_and_disable_interrupts();
    if (enabled) {
        audio_spdif_shim_start(inst);
        pio_sm_set_enabled(inst->pio, inst->pio_sm, true);
    } else {
        audio_spdif_shim_stop(inst);
        pio_sm_set_enabled(inst->pio, inst->pio_sm, false);
    }
    restore_interrupts(save);
}

void audio_spdif_change_pin(audio_spdif_instance_t *inst, uint new_pin) {
    audio_spdif_attach_pin(inst, new_pin);
}

void audio_spdif_attach_pin(audio_spdif_instance_t *inst, uint new_pin) {
    gpio_set_function(new_pin, (gpio_function_t)pio_get_funcsel(inst->pio));
    inst->pin = (uint8_t)new_pin;
}

void audio_spdif_teardown(audio_spdif_instance_t *inst) {
    audio_spdif_set_enabled(inst, false);
    inst->ring = NULL;
    dma_channel_unclaim(inst->dma_channel);
    pio_sm_unclaim(inst->pio, inst->pio_sm);
    uint32_t save = save_and_disable_interrupts();
    for (uint i = 0; i < spdif_instance_count; i++) {
        if (spdif_instances[i] == inst) {
            for (uint j = i; j + 1 < spdif_instance_count; j++) {
                spdif_instances[j] = spdif_instances[j + 1];
            }
            spdif_instance_count--;
            break;
        }
    }
    restore_interrupts(save);
    inst->enabled = false;
}

uint32_t audio_spdif_enable_sync_prepare(audio_spdif_instance_t *instances[], uint count) {
    uint32_t mask = 0;
    uint32_t save = save_and_disable_interrupts();
    for (uint i = 0; i < count; i++) {
        audio_spdif_shim_start(instances[i]);
        mask |= 1u << instances[i]->pio_sm;
    }
    restore_interrupts(save);
    return mask;
}

void audio_spdif_enable_sync(audio_spdif_instance_t *instances[], uint count) {
    if (count == 0) {
        return;
    }
    uint32_t mask = audio_spdif_enable_sync_prepare(instances, count);
    pio_enable_sm_mask_in_sync(instances[0]->pio, mask);
}

void audio_spdif_apply_pio_frequency(audio_spdif_instance_t *inst, uint32_t sample_freq) {
    uint32_t save = save_and_disable_interrupts();
    audio_spdif_shim_advance(inst);     // frames so far count at the old rate
    inst->freq = sample_freq;
    shim_output_clock_set_rate(sample_freq);
    restore_interrupts(save);
}

void audio_spdif_select_encoder(uint pio_index, bool pio_bmc) {
    (void)pio_index, (void)pio_bmc;
}

int audio_spdif_loaded_encoder(uint pio_index) {
    return pio_index < 3u ? spdif_encoder_loaded[pio_index] : -1;
}

void audio_spdif_reset_dma_starvations(void) {
    for (uint i = 0; i < spdif_instance_count; i++) {
        spdif_instances[i]->underrun_frames = 0;
        spdif_instances[i]->drop_frames = 0;
    }
}

uint32_t audio_spdif_get_dma_starvations(void) {
    uint32_t total = 0;
    for (uint i = 0; i < spdif_instance_count; i++) {
        total += spdif_instances[i]->underrun_frames;
    }
    return total;
}

uint32_t audio_spdif_get_dma_starvations_instance(uint index) {
    for (uint i = 0; i < spdif_instance_count; i++) {
        if (spdif_instances[i]->instance_index == index) {
            return spdif_instances[i]->underrun_frames;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------
// I2S instances and MCK

static audio_i2s_instance_t *i2s_instances[PICO_AUDIO_I2S_MAX_INSTANCES];
static uint i2s_instance_count;


void audio_i2s_setup(audio_i2s_instance_t *inst, uint32_t sample_freq, const audio_i2s_config_t *config) {
    inst->pio = pio_for_index(config->pio);
    inst->pio_sm = config->pio_sm;
    inst->dma_channel = config->dma_channel;
    inst->dma_reload_channel = config->dma_reload_channel;
    inst->data_pin = config->data_pin;
    inst->clock_pin_base = config->clock_pin_base;
    inst->external_clock = config->external_clock;
    inst->clock_master = config->external_clock ? false : config->clock_master;
    inst->freq = sample_freq;
    shim_output_clock_set_rate(sample_freq);
    inst->enabled = false;
    inst->instance_index = config->dma_channel;
    inst->ring = (int32_t *)config->ring_base;
    inst->underrun_frames = 0;
    inst->drop_frames = 0;
    audio_i2s_ring_reset(inst);
    gpio_set_function(config->data_pin, (gpio_function_t)pio_get_funcsel(inst->pio));
    pio_sm_claim(inst->pio, inst->pio_sm);
    dma_channel_claim(inst->dma_channel);
    if (i2s_instance_count < PICO_AUDIO_I2S_MAX_INSTANCES) {
        i2s_instances[i2s_instance_count++] = inst;
    }
}

void audio_i2s_set_enabled(audio_i2s_instance_t *inst, bool enabled) {
    if (enabled == inst->enabled) {
        return;
    }
    uint32_t save = save_and_disable_interrupts();
    if (enabled) {
        audio_i2s_shim_start(inst);
        pio_sm_set_enabled(inst->pio, inst->pio_sm, true);
    } else {
        audio_i2s_shim_stop(inst);
        pio_sm_set_enabled(inst->pio, inst->pio_sm, false);
    }
    restore_interrupts(save);
}

void audio_i2s_change_data_pin(audio_i2s_instance_t *inst, uint new_pin) {
    audio_i2s_attach_data_pin(inst, new_pin);
}

void audio_i2s_attach_data_pin(audio_i2s_instance_t *inst, uint new_pin) {
    gpio_set_function(new_pin, (gpio_function_t)pio_get_funcsel(inst->pio));
    inst->data_pin = (uint8_t)new_pin;
}

uint32_t audio_i2s_enable_sync_prepare(audio_i2s_instance_t *instances[], uint count) {
    uint32_t mask = 0;
    uint32_t save = save_and_disable_interrupts();
    for (uint i = 0; i < count; i++) {
        audio_i2s_shim_start(instances[i]);
        mask |= 1u << instances[i]->pio_sm;
    }
    restore_interrupts(save);
    return mask;
}

void audio_i2s_enable_sync(audio_i2s_instance_t *instances[], uint count) {
    if (count == 0) {
        return;
    }
    uint32_t mask = audio_i2s_enable_sync_prepare(instances, count);
    pio_enable_sm_mask_in_sync(instances[0]->pio, mask);
}

void audio_i2s_reset_dma_starvations(void) {
    for (uint i = 0; i < i2s_instance_count; i++) {
        i2s_instances[i]->underrun_frames = 0;
        i2s_instances[i]->drop_frames = 0;
    }
}

uint32_t audio_i2s_get_dma_starvations(void) {
    uint32_t total = 0;
    for (uint i = 0; i < i2s_instance_count; i++) {
        total += i2s_instances[i]->underrun_frames;
    }
    return total;
}

uint32_t audio_i2s_get_dma_starvations_instance(uint index) {
    for (uint i = 0; i < i2s_instance_count; i++) {
        if (i2s_instances[i]->instance_index == index) {
            return i2s_instances[i]->underrun_frames;
        }
    }
    return 0;
}

void audio_i2s_teardown(audio_i2s_instance_t *inst) {
    audio_i2s_set_enabled(inst, false);
    dma_channel_unclaim(inst->dma_channel);
    pio_sm_unclaim(inst->pio, inst->pio_sm);
    uint32_t save = save_and_disable_interrupts();
    for (uint i = 0; i < i2s_instance_count; i++) {
        if (i2s_instances[i] == inst) {
            for (uint j = i; j + 1 < i2s_instance_count; j++) {
                i2s_instances[j] = i2s_instances[j + 1];
            }
            i2s_instance_count--;
            break;
        }
    }
    restore_interrupts(save);
    memset(inst, 0, sizeof(*inst));
}

void audio_i2s_update_all_frequencies(uint32_t sample_freq) {
    uint32_t save = save_and_disable_interrupts();
    for (uint i = 0; i < i2s_instance_count; i++) {
        audio_i2s_shim_advance(i2s_instances[i]);
        i2s_instances[i]->freq = sample_freq;
    }
    shim_output_clock_set_rate(sample_freq);
    restore_interrupts(save);
}

int8_t audio_i2s_get_clock_master_index(void) {
    for (uint i = 0; i < i2s_instance_count; i++) {
        if (i2s_instances[i]->clock_master) {
            return (int8_t)i2s_instances[i]->instance_index;
        }
    }
    return -1;
}

bool audio_i2s_extclk_framing_slipped(void) {
    return false;
}

// MCK always comes out on GPIO 4 (GPCLK0), from the SMI's clock (smi_out_mck()). DSPi's MCK pin is kept
// as bookkeeping only, as the output pins are.
static uint8_t mck_pin = 0xff;
static bool mck_enabled;
static uint32_t mck_mult = 256;

void audio_i2s_mck_setup(uint pin) {
    mck_pin = (uint8_t)pin;
}
void audio_i2s_mck_set_enabled(bool enabled) {
    mck_enabled = enabled && mck_pin != 0xff;
    smi_out_mck(mck_enabled ? mck_mult : 0u);
}
void audio_i2s_mck_update_frequency(uint32_t sample_freq, uint32_t multiplier) {
    (void)sample_freq;
    mck_mult = multiplier;
    if (mck_enabled) {
        smi_out_mck(mck_mult);
    }
}
void audio_i2s_mck_change_pin(uint new_pin) {
    mck_pin = (uint8_t)new_pin;
}
uint8_t audio_i2s_mck_get_pin(void) {
    return mck_pin;
}
bool audio_i2s_mck_is_enabled(void) {
    return mck_enabled;
}
void audio_i2s_mck_apply_state(uint pin, bool enabled, uint32_t sample_freq, uint32_t multiplier) {
    if (mck_pin == 0xff) {
        return;
    }
    mck_pin = (uint8_t)pin;
    audio_i2s_mck_update_frequency(sample_freq, multiplier);
    audio_i2s_mck_set_enabled(enabled);
}

// ---------------------------------------------------------------------------------------------------
// SMI composer: every slot ring onto one parallel stream, 256 words per frame, so all outputs change on
// the same SMI cycle. Slot n's data is SD n (GPIO 8 + n); the I2S bit clock is SD4 (GPIO 12) and LRCLK
// SD5 (GPIO 13). An S/PDIF cell lasts 2 words and an I2S bit clock 4, at every sample rate.

#include <arm_acle.h>
#include <arm_neon.h>

#define SMI_SLOTS 4u
#define SMI_BIT_BCK 4u
#define SMI_BIT_LRCLK 5u
#define SMI_BIT_ADAT 8u   // SD8, GPIO 16, claimed only while ADAT is enabled
#define SMI_PIN_MASK (0x3Fu << 8)
#define SMI_RING_FRAMES 128u

static uint16_t smi_ring[SMI_RING_FRAMES * SMI_FRAME_WORDS] __attribute__((aligned(64)));   // cached; smi_out cleans it

extern uint8_t slot_ring_store[];   // usb_audio.c: one PICO_AUDIO_SPDIF_RING_BYTES ring per slot

static uint8_t lut_cell2[256][16] __attribute__((aligned(16)));    // 8 cells, bit 0 first, to 16 words
static uint8_t lut_bck4[256][32] __attribute__((aligned(16)));     // 8 I2S bits to 32 words
static uint8_t i2s_clock_bytes[SMI_FRAME_WORDS] __attribute__((aligned(16)));   // BCK and LRCLK, a byte a word
static uint8_t lut_msb8[256][8] __attribute__((aligned(8)));     // 8 bits, MSB first, to 8 words

// adat_output.c's stream: DMA channel 13 into pio1 SM 1, one bit per PIO cycle at 256 fs. The composer
// plays the ring that channel was given from the frame the state machine was enabled, one bit per SMI
// word on SD8, and moves the channel's read address on as it composes, as the DMA would.
#define ADAT_DMA_CH 13u
#define ADAT_FRAME_WORDS 8u
static const uint32_t *adat_ring;
static uint32_t adat_ring_frames, adat_start;
static volatile bool adat_on;

static void shim_adat_enable(bool on) {
    if (on) {
        dma_channel_hw_t *ch = &pico_shim_dma_hw.ch[ADAT_DMA_CH];
        adat_ring = (const uint32_t *)(uintptr_t)ch->read_addr;
        adat_ring_frames = ch->transfer_count / ADAT_FRAME_WORDS;
        adat_start = shim_output_clock();
    }
    adat_on = on && adat_ring && adat_ring_frames;
    // As on the RP2350, a stopped stream holds the pin low until ADAT is disabled, which frees it.
    if (adat_on || !adat_output_config_enabled()) {
        smi_out_pin(8u + SMI_BIT_ADAT, adat_on);
    }
}

typedef struct {
    const uint32_t *ring;
    uint32_t offset;    // ring frame = SMI frame + offset
    uint8_t bit;
    uint8_t i2s;
} comp_slot_t;

static void smi_tables_init(void) {
    for (uint32_t v = 0; v < 256u; v++) {
        for (uint32_t j = 0; j < 8u; j++) {
            uint8_t b = (uint8_t)((v >> j) & 1u);
            lut_cell2[v][2u * j] = lut_cell2[v][2u * j + 1u] = b;
            for (uint32_t k = 0; k < 4u; k++) {
                lut_bck4[v][4u * j + k] = b;
            }
        }
        for (uint32_t j = 0; j < 8u; j++) {
            lut_msb8[v][j] = (uint8_t)((v >> (7u - j)) & 1u);
        }
    }
    // Philips I2S: BCK low then high within each bit (data changes on the falling edge), LRCLK low for
    // the left half. Data runs one bit clock behind LRCLK, which the composer builds into the bit stream.
    for (uint32_t w = 0; w < SMI_FRAME_WORDS; w++) {
        uint32_t k = w / 4u;
        i2s_clock_bytes[w] = (uint8_t)(((w % 4u) >= 2u) << SMI_BIT_BCK | (k >= 32u) << SMI_BIT_LRCLK);
    }
}

// NRZI cells (1 = toggle) to line levels: a running XOR. Every subframe has an even number of
// transitions, so each frame starts from the same level and needs no state from the last one.
static inline uint32_t nrzi_levels(uint32_t x, uint32_t *carry) {
    x ^= x << 1;
    x ^= x << 2;
    x ^= x << 4;
    x ^= x << 8;
    x ^= x << 16;
    if (*carry) {
        x = ~x;
    }
    *carry = x >> 31;
    return x;
}

static uint32_t smi_gather(comp_slot_t *slots, bool *any_i2s) {
    uint32_t n = 0;
    *any_i2s = false;
    for (uint i = 0; i < spdif_instance_count; i++) {
        audio_spdif_instance_t *inst = spdif_instances[i];
        uint32_t slot = (uint32_t)((uint8_t *)inst->ring - slot_ring_store) / PICO_AUDIO_SPDIF_RING_BYTES;
        if (inst->enabled && inst->ring && slot < SMI_SLOTS) {
            slots[n++] = (comp_slot_t){(const uint32_t *)inst->ring, inst->consumed_frames - inst->last_read_time_us,
                                       (uint8_t)slot, 0};
        }
    }
    for (uint i = 0; i < i2s_instance_count; i++) {
        audio_i2s_instance_t *inst = i2s_instances[i];
        uint32_t slot = (uint32_t)((uint8_t *)inst->ring - slot_ring_store) / PICO_AUDIO_SPDIF_RING_BYTES;
        if (inst->enabled && inst->ring && slot < SMI_SLOTS) {
            slots[n++] = (comp_slot_t){(const uint32_t *)inst->ring, inst->consumed_frames - inst->last_read_time_us,
                                       (uint8_t)slot, 1};
            *any_i2s = true;
        }
    }
    return n;
}

static const uint8_t zero_row[32] __attribute__((aligned(16)));

// Builds each frame 16 words at a time. Slot n is always bit n, so the four slots merge with shift-and-
// insert (a disabled slot reads a row of zeros), then the clocks are ORed in and the block is widened.
static void smi_compose(uint32_t frame, uint16_t *dst, uint32_t n) {
    comp_slot_t found[2u * SMI_SLOTS];
    bool any_i2s;
    uint32_t nf = smi_gather(found, &any_i2s);
    const comp_slot_t *by_slot[SMI_SLOTS] = {NULL, NULL, NULL, NULL};
    for (uint32_t s = 0; s < nf; s++) {
        by_slot[found[s].bit] = &found[s];
    }
    bool adat = adat_on;
    for (uint32_t k = 0; k < n; k++, frame++, dst += SMI_FRAME_WORDS) {
        const uint8_t *rows[SMI_SLOTS][16];   // per slot, the 16-byte table row for each block
        for (uint32_t s = 0; s < SMI_SLOTS; s++) {
            const comp_slot_t *c = by_slot[s];
            if (!c) {
                for (uint32_t q = 0; q < 16u; q++) {
                    rows[s][q] = zero_row;
                }
            } else if (c->i2s) {
                uint32_t rf = (frame + c->offset) & (PICO_AUDIO_I2S_RING_FRAMES - 1u);
                uint32_t prev = (rf - 1u) & (PICO_AUDIO_I2S_RING_FRAMES - 1u);
                // Bit clock 0 carries the last frame's right LSB, 1-32 the left word MSB first, 33-63 the
                // right word's top 31 bits.
                uint32_t lo = (c->ring[prev * 2u + 1u] & 1u) | __rbit(c->ring[rf * 2u]) << 1;
                uint32_t hi = __rbit(c->ring[rf * 2u]) >> 31 | __rbit(c->ring[rf * 2u + 1u]) << 1;
                for (uint32_t q = 0; q < 8u; q++) {
                    rows[s][q] = &lut_bck4[(lo >> (4u * (q & ~1u))) & 0xFFu][16u * (q & 1u)];
                    rows[s][q + 8u] = &lut_bck4[(hi >> (4u * (q & ~1u))) & 0xFFu][16u * (q & 1u)];
                }
            } else {
                uint32_t rf = (frame + c->offset) & (PICO_AUDIO_SPDIF_RING_FRAMES - 1u);
                const uint32_t *w = &c->ring[rf * 4u];
                uint32_t carry = 0;
                for (uint32_t j = 0; j < 4u; j++) {
                    uint32_t lv = nrzi_levels(w[j], &carry);
                    rows[s][4u * j] = lut_cell2[lv & 0xFFu];
                    rows[s][4u * j + 1u] = lut_cell2[(lv >> 8) & 0xFFu];
                    rows[s][4u * j + 2u] = lut_cell2[(lv >> 16) & 0xFFu];
                    rows[s][4u * j + 3u] = lut_cell2[lv >> 24];
                }
            }
        }
        // ADAT words are line levels, MSB first: each 16-word block takes half a word. They fill the
        // high byte of each SMI word, so the store interleaves the two byte vectors.
        const uint32_t *aw = adat ? &adat_ring[((frame - adat_start) % adat_ring_frames) * ADAT_FRAME_WORDS] : NULL;
        for (uint32_t q = 0; q < 16u; q++) {
            uint8x16_t acc = vld1q_u8(rows[0][q]);
            acc = vsliq_n_u8(acc, vld1q_u8(rows[1][q]), 1);
            acc = vsliq_n_u8(acc, vld1q_u8(rows[2][q]), 2);
            acc = vsliq_n_u8(acc, vld1q_u8(rows[3][q]), 3);
            if (any_i2s) {
                acc = vorrq_u8(acc, vld1q_u8(&i2s_clock_bytes[16u * q]));
            }
            uint8x16x2_t out = {{acc, vdupq_n_u8(0)}};
            if (aw) {
                uint32_t bits = aw[q >> 1] >> ((q & 1u) ? 0u : 16u);
                out.val[1] = vcombine_u8(vld1_u8(lut_msb8[(bits >> 8) & 0xFFu]), vld1_u8(lut_msb8[bits & 0xFFu]));
            }
            vst2q_u8((uint8_t *)&dst[16u * q], out);
        }
    }
    if (adat) {
        pico_shim_dma_hw.ch[ADAT_DMA_CH].read_addr =
            (uint32_t)(uintptr_t)&adat_ring[((frame - adat_start) % adat_ring_frames) * ADAT_FRAME_WORDS];
    }
}

// Starts the SMI stream (before DSPi creates its slots), and from then on the output clock is its frame count.
void shim_smi_start(void) {
    spdif_lookup_init();
    smi_tables_init();
    if (smi_out_start(smi_ring, SMI_RING_FRAMES, oc_freq, SMI_PIN_MASK, smi_compose)) {
        oc_smi = true;
    }
}

// Bench: copies n frames that are composed but not yet sent, and returns the first one's frame number.
uint32_t shim_smi_snapshot(uint16_t *out, uint32_t n) {
    uint32_t save = save_and_disable_interrupts();
    uint32_t first = smi_out_frames() + 4u;     // composed (the composer is ahead) and not yet sent
    for (uint32_t k = 0; k < n; k++) {
        const uint16_t *src = &smi_ring[((first + k) & (SMI_RING_FRAMES - 1u)) * SMI_FRAME_WORDS];
        memcpy(&out[k * SMI_FRAME_WORDS], src, SMI_FRAME_WORDS * 2u);
    }
    restore_interrupts(save);
    return first;
}
