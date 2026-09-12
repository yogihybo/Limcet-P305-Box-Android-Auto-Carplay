#include "swc_driver.h"
#include "uart_protocol.h"

/* DMA target buffer for ADC1 conversions:
 * Index 0: Channel 1 (PA1 / SW1)
 * Index 1: Channel 9 (PB1 / SW2)
 * Disassembly location in SRAM: 0x20000044
 */
static volatile uint16_t g_swc_adc_buffer[2];

/* OEM threshold lookup table disassembled at 0x0800B9E4 */
typedef struct {
    uint8_t min_adc;
    uint8_t max_adc;
    uint8_t key_code;
} SwcKeyMap;

static const SwcKeyMap k_swc1_map[] = {
    { 0,   11,  SWC_KEY_NEXT },  /* GND / 0 ohm -> Next/Seek+ */
    { 43,  65,  SWC_KEY_PREV }   /* 330 ohm     -> Prev/Seek- */
};
#define SWC1_MAP_SIZE (sizeof(k_swc1_map) / sizeof(k_swc1_map[0]))

static const SwcKeyMap k_swc2_map[] = {
    { 43,  65,  SWC_KEY_VOL_UP },   /* 330 ohm  -> Volume Up */
    { 98,  120, SWC_KEY_VOL_DOWN }, /* 1k ohm   -> Volume Down */
    { 162, 184, SWC_KEY_MODE }      /* 3.1k ohm -> Mode / Source */
};
#define SWC2_MAP_SIZE (sizeof(k_swc2_map) / sizeof(k_swc2_map[0]))

/* State tracker matching OEM 10-byte struct at 0x200004D0 */
typedef struct {
    uint8_t  last_candidate;
    uint8_t  debounce_count;
    uint8_t  active_key;
    uint8_t  state;         /* 0=release, 1=press, 2=held */
    uint16_t hold_timer_ms;
} SwcChannelState;

static SwcChannelState g_swc_channels[2];

void swc_init(void) {
    /* 1. Enable peripheral clocks:
     * - DMA1 (AHB bit 0)
     * - GPIOA (APB2 bit 2)
     * - GPIOB (APB2 bit 3)
     * - ADC1  (APB2 bit 9)
     */
    RCC->AHBENR  |= (1UL << 0);
    RCC->APB2ENR |= (1UL << 2) | (1UL << 3) | (1UL << 9);

    /* 2. Configure pins as Analog Inputs (CNF=00, MODE=00):
     * - PA1: CRL bits [7:4]
     * - PB1: CRL bits [7:4]
     */
    GPIOA->CRL &= ~(0x0FUL << 4);
    GPIOB->CRL &= ~(0x0FUL << 4);

    /* 3. Configure DMA1 Channel 1 (ADC1 regular channel DMA):
     * Peripheral address: ADC1->DR (0x4001244C)
     * Memory address: g_swc_adc_buffer
     * Buffer size: 2 half-words
     * Circular mode, Memory increment, 16-bit data width, High priority
     */
    DMA1_Channel1->CCR = 0; /* Disable channel */
    DMA1_Channel1->CPAR = (uint32_t)&ADC1->DR;
    DMA1_Channel1->CMAR = (uint32_t)g_swc_adc_buffer;
    DMA1_Channel1->CNDTR = 2;

    /* CCR:
     * Bit 0:  EN = 1
     * Bit 5:  CIRC = 1
     * Bit 7:  MINC = 1
     * Bit 8:  PSIZE = 01 (16-bit)
     * Bit 10: MSIZE = 01 (16-bit)
     * Bit 12: PL = 10 (High priority)
     */
    DMA1_Channel1->CCR = (1UL << 0) | (1UL << 5) | (1UL << 7) |
                         (1UL << 8) | (1UL << 10) | (2UL << 12);

    /* 4. Configure ADC1 (matching disassembly at 0x080076D0):
     * - Independent mode (CR1 bits [19:16] = 0000)
     * - Scan mode enabled (CR1 bit 8 = 1)
     * - Continuous conversion mode enabled (CR2 bit 1 = 1)
     * - DMA request enabled (CR2 bit 8 = 1)
     * - Software trigger (CR2 bits [19:17] = 111, bit 20 EXTTRIG = 1)
     * - Right data alignment (CR2 bit 11 = 0)
     * - Sequence length: 2 conversions (SQR1 bits [23:20] = 0001)
     */
    ADC1->CR1 = (1UL << 8); /* SCAN mode */
    ADC1->CR2 = (1UL << 1) | (1UL << 8) | (7UL << 17) | (1UL << 20);

    /* Sequence configuration:
     * Rank 1 (SQR3 bits [4:0]): Channel 1 (PA1)
     * Rank 2 (SQR3 bits [9:5]): Channel 9 (PB1)
     */
    ADC1->SQR1 = (1UL << 20); /* L = 1 (2 conversions) */
    ADC1->SQR3 = (1UL << 0) | (9UL << 5);

    /* Sample times (disassembly specified sample time 5 = 55.5 cycles):
     * Channel 1: SMPR2 bits [5:3]   = 5
     * Channel 9: SMPR2 bits [29:27] = 5
     */
    ADC1->SMPR2 = (5UL << 3) | (5UL << 27);

    /* 5. Calibration sequence (disassembly 0x0800771E - 0x08007746):
     * - Enable ADC1 (ADON)
     * - Wait small delay
     * - Reset calibration and wait for completion
     * - Start calibration and wait for completion
     */
    ADC1->CR2 |= (1UL << 0); /* ADON */
    for (volatile int i = 0; i < 2000; i++) {}

    ADC1->CR2 |= (1UL << 3); /* RSTCAL */
    while ((ADC1->CR2 & (1UL << 3)) != 0) {}

    ADC1->CR2 |= (1UL << 2); /* CAL */
    while ((ADC1->CR2 & (1UL << 2)) != 0) {}

    /* 6. Start continuous conversion:
     * Write ADON again / SWSTART to begin continuous circular DMA conversion
     */
    ADC1->CR2 |= (1UL << 0) | (1UL << 22); /* ADON + SWSTART */

    /* Initialize channel state trackers */
    for (int i = 0; i < 2; i++) {
        g_swc_channels[i].last_candidate = SWC_KEY_NONE;
        g_swc_channels[i].debounce_count = 0;
        g_swc_channels[i].active_key     = SWC_KEY_NONE;
        g_swc_channels[i].state          = SWC_STATE_RELEASE;
        g_swc_channels[i].hold_timer_ms  = 0;
    }
}

static uint8_t swc_lookup_key(uint8_t channel_idx, uint8_t adc_8bit) {
    if (adc_8bit >= SWC_IDLE_ADC_THRESHOLD) {
        return SWC_KEY_NONE;
    }

    if (channel_idx == 0) {
        for (uint32_t i = 0; i < SWC1_MAP_SIZE; i++) {
            if (adc_8bit >= k_swc1_map[i].min_adc && adc_8bit <= k_swc1_map[i].max_adc) {
                return k_swc1_map[i].key_code;
            }
        }
    } else {
        for (uint32_t i = 0; i < SWC2_MAP_SIZE; i++) {
            if (adc_8bit >= k_swc2_map[i].min_adc && adc_8bit <= k_swc2_map[i].max_adc) {
                return k_swc2_map[i].key_code;
            }
        }
    }
    return SWC_KEY_NONE;
}

void swc_process(void) {
    for (uint8_t ch_idx = 0; ch_idx < 2; ch_idx++) {
        SwcChannelState *ch = &g_swc_channels[ch_idx];

        /* 1. Scale 12-bit raw ADC to 8-bit matching disassembly 0x08007414:
         * ubfx r5, r0, #4, #8 -> (raw >> 4) & 0xFF
         */
        uint8_t adc_8bit = (uint8_t)((g_swc_adc_buffer[ch_idx] >> 4) & 0xFF);

        /* 2. Look up key code in OEM threshold table */
        uint8_t detected_key = swc_lookup_key(ch_idx, adc_8bit);

        /* 3. Debounce filter (2 consecutive 25ms samples matching 0x0800746E) */
        if (detected_key == ch->last_candidate) {
            if (ch->debounce_count < SWC_DEBOUNCE_THRESHOLD) {
                ch->debounce_count++;
            }
        } else {
            ch->last_candidate = detected_key;
            ch->debounce_count = 1;
        }

        /* 4. Process debounced key events */
        if (ch->debounce_count >= SWC_DEBOUNCE_THRESHOLD) {
            if (detected_key != SWC_KEY_NONE) {
                if (ch->active_key == SWC_KEY_NONE) {
                    /* Initial Button Press */
                    ch->active_key = detected_key;
                    ch->state = SWC_STATE_PRESS;
                    ch->hold_timer_ms = 0;
                    uart_send_key_event(ch->active_key, SWC_STATE_PRESS);
                } else if (ch->active_key == detected_key) {
                    /* Button Held: increment hold counter */
                    ch->hold_timer_ms += SWC_POLL_INTERVAL_MS;

                    /* Hold repeat threshold: 1200 ms (0x080074C0, cmp r0, #0x4B0)
                     * Decrements timer by 100 ms (0x080074D0, subs r0, #0x64)
                     * Emits state 2 (SWC_STATE_HELD)
                     */
                    if (ch->hold_timer_ms >= SWC_HOLD_DELAY_MS) {
                        uint16_t hold_elapsed = ch->hold_timer_ms - SWC_HOLD_DELAY_MS;
                        if ((hold_elapsed % SWC_REPEAT_INTERVAL_MS) == 0) {
                            ch->state = SWC_STATE_HELD;
                            uart_send_key_event(ch->active_key, SWC_STATE_HELD);
                        }
                    }
                }
            } else {
                /* Button Released: voltage returned to resting pull-up */
                if (ch->active_key != SWC_KEY_NONE) {
                    uart_send_key_event(ch->active_key, SWC_STATE_RELEASE);
                    ch->active_key = SWC_KEY_NONE;
                    ch->state = SWC_STATE_RELEASE;
                    ch->hold_timer_ms = 0;
                }
            }
        }
    }
}

uint16_t swc_get_raw_adc(uint8_t channel) {
    if (channel < 2) {
        return g_swc_adc_buffer[channel];
    }
    return 0;
}
