#include "uart_protocol.h"
#include "can_driver.h"
#include "tea_crypto.h"
#include "gpio_driver.h"

static uint8_t g_rx_state = 0;
static uint8_t g_rx_cmd = 0;
static uint8_t g_rx_len = 0;
static uint8_t g_rx_idx = 0;
static uint8_t g_rx_buf[UART_MAX_PAYLOAD];

static UartPacket g_rx_ring[UART_RX_RING_SIZE];
static volatile uint8_t g_rx_head = 0;
static volatile uint8_t g_rx_tail = 0;

static uint8_t calc_checksum(uint8_t cmd, uint8_t len, const uint8_t *payload) {
    uint32_t sum = (uint32_t)cmd + (uint32_t)len;
    for (uint8_t i = 0; i < len; i++) {
        sum += payload[i];
    }
    return (uint8_t)(~sum & 0xFF);
}

void USART2_IRQHandler(void) {
    uint32_t sr = USART2->SR;
    /* Check RXNE or ORE: read SR followed by DR clears ORE cleanly */
    if (sr & ((1UL << 5) | (1UL << 3))) {
        uint8_t byte = (uint8_t)(USART2->DR & 0xFF);
        
        /* Only process payload byte if RXNE is set (data is valid) */
        if (sr & (1UL << 5)) {
            switch (g_rx_state) {
            case 0: /* Wait for header 0x2E */
                if (byte == UART_HEADER_SIG) {
                    g_rx_state = 1;
                }
                break;
                
            case 1: /* Command byte */
                g_rx_cmd = byte;
                g_rx_state = 2;
                break;
                
            case 2: /* Length byte */
                if (byte <= UART_MAX_PAYLOAD && byte > 0) {
                    g_rx_len = byte;
                    g_rx_idx = 0;
                    g_rx_state = 3;
                } else if (byte == 0) {
                    g_rx_len = 0;
                    g_rx_state = 4; /* Checksum directly */
                } else {
                    g_rx_state = 0; /* Invalid length -> resync */
                }
                break;
                
            case 3: /* Payload bytes */
                g_rx_buf[g_rx_idx++] = byte;
                if (g_rx_idx >= g_rx_len) {
                    g_rx_state = 4;
                }
                break;
                
            case 4: /* Checksum byte */
                if (byte == calc_checksum(g_rx_cmd, g_rx_len, g_rx_buf)) {
                    uint8_t next_head = (g_rx_head + 1) % UART_RX_RING_SIZE;
                    if (next_head != g_rx_tail) {
                        g_rx_ring[g_rx_head].cmd = g_rx_cmd;
                        g_rx_ring[g_rx_head].len = g_rx_len;
                        for (uint8_t i = 0; i < g_rx_len; i++) {
                            g_rx_ring[g_rx_head].payload[i] = g_rx_buf[i];
                        }
                        g_rx_head = next_head;
                    }
                }
                g_rx_state = 0;
                break;
                
            default:
                g_rx_state = 0;
                break;
        }
        }
    }
}

void uart_protocol_init(uint32_t baudrate) {
    g_rx_head = 0;
    g_rx_tail = 0;
    g_rx_state = 0;
    
    /* Enable Clocks: USART2, GPIOA */
    RCC->APB1ENR |= (1UL << 17); /* USART2EN */
    RCC->APB2ENR |= (1UL << 2);  /* IOPAEN */
    
    /* USART2 Pins: PA2 TX, PA3 RX */
    /* PA2: Alternate Function Push-Pull 50MHz (Mode 11, CNF 10 -> 0x0B) */
    GPIOA->CRL &= ~(0x0FUL << 8);
    GPIOA->CRL |=  (0x0BUL << 8);
    
    /* PA3: Input Floating / Pull-Up (Mode 00, CNF 01 / 10 -> 0x08) */
    GPIOA->CRL &= ~(0x0FUL << 12);
    GPIOA->CRL |=  (0x08UL << 12);
    GPIOA->ODR |=  (1UL << 3);
    
    /* Configure Baud Rate (Assuming APB1 = 36 MHz) */
    /* 36000000 / 38400 = 937.5 -> Mantissa = 937 (0x3A9), Fraction = 0.5 * 16 = 8 -> 0x3A98 */
    if (baudrate == 38400) {
        USART2->BRR = 0x03A98;
    } else {
        /* Standard calculation */
        uint32_t pclk1 = 36000000;
        USART2->BRR = (pclk1 + (baudrate / 2)) / baudrate;
    }
    
    /* Enable Transmitter, Receiver, and RXNE Interrupt */
    USART2->CR1 = (1UL << 13) | (1UL << 5) | (1UL << 3) | (1UL << 2); /* UE, RXNEIE, TE, RE */
    
    /* Enable NVIC IRQ 38 (USART2) */
    nvic_enable_irq(38);
}

void uart_send_byte(uint8_t byte) {
    while ((USART2->SR & (1UL << 7)) == 0) {} /* Wait for TXE */
    USART2->DR = byte;
}

void uart_send_packet(uint8_t cmd, const uint8_t *payload, uint8_t len) {
    uart_send_byte(UART_HEADER_SIG);
    uart_send_byte(cmd);
    uart_send_byte(len);
    for (uint8_t i = 0; i < len; i++) {
        uart_send_byte(payload[i]);
    }
    uart_send_byte(calc_checksum(cmd, len, payload));
}

void uart_send_key_event(uint8_t key_code, uint8_t state) {
    uint8_t payload[2];
    payload[0] = key_code;
    payload[1] = state;
    uart_send_packet(MCU_CMD_INPUT_EVENT, payload, 2);
}

void uart_send_touch_coordinates(uint16_t x, uint16_t y, uint8_t state) {
    /* 5-byte payload matching OEM disassembly 0x08006A98:
     * payload[0] = X & 0xFF
     * payload[1] = X >> 8
     * payload[2] = Y & 0xFF
     * payload[3] = Y >> 8
     * payload[4] = state (0=release, 1=press)
     */
    uint8_t payload[5];
    payload[0] = (uint8_t)(x & 0xFF);
    payload[1] = (uint8_t)((x >> 8) & 0xFF);
    payload[2] = (uint8_t)(y & 0xFF);
    payload[3] = (uint8_t)((y >> 8) & 0xFF);
    payload[4] = state;
    uart_send_packet(MCU_CMD_STATUS_QUERY, payload, 5);
}

static uint8_t g_vehicle_status_byte = MCU_STATUS_BASE_FLAGS; /* 0x11 resting state */

void uart_send_reverse_state(bool reverse_active) {
    if (reverse_active) {
        g_vehicle_status_byte |= MCU_STATUS_BIT_REVERSE; /* Bit 2 -> 0x15 (reversing) */
    } else {
        g_vehicle_status_byte &= ~MCU_STATUS_BIT_REVERSE; /* Clear Bit 2 -> 0x11 */
    }
    uint8_t payload[6] = { g_vehicle_status_byte, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uart_send_packet(MCU_CMD_ILLUMINATION_STATUS, payload, 6);
}

void uart_send_headlights_state(bool lights_on) {
    if (lights_on) {
        g_vehicle_status_byte |= MCU_STATUS_BIT_ILLUM; /* Bit 1 -> 0x13 (headlights ON) */
    } else {
        g_vehicle_status_byte &= ~MCU_STATUS_BIT_ILLUM; /* Clear Bit 1 -> 0x11 */
    }
    uint8_t payload[6] = { g_vehicle_status_byte, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uart_send_packet(MCU_CMD_ILLUMINATION_STATUS, payload, 6);
}

void uart_send_lcd_source(uint8_t mode) {
    uint8_t payload[3] = { mode, 0x04, 0x00 };
    uart_send_packet(MCU_CMD_LCD_SOURCE_REPORT, payload, 3);
}

void uart_send_version_report(void) {
    static const uint8_t kMcuVersion[28] = "DCn32-VOLVO-V2.10-20240909  ";
    uart_send_packet(MCU_CMD_VERSION_REPORT, kMcuVersion, 28);
}

void uart_send_steering_angle(int16_t angle_deci_degrees) {
    uint8_t payload[4];
    payload[0] = (angle_deci_degrees >= 0) ? 0x00 : 0x01; /* Direction bit */
    uint16_t mag = (angle_deci_degrees >= 0) ? angle_deci_degrees : -angle_deci_degrees;
    payload[1] = (uint8_t)(mag & 0xFF);
    payload[2] = (uint8_t)((mag >> 8) & 0xFF);
    payload[3] = 0x00;
    uart_send_packet(MCU_CMD_STEERING_ANGLE, payload, 4);
}

void uart_send_radar_levels(uint8_t left, uint8_t mid_left, uint8_t mid_right, uint8_t right) {
    uint8_t payload[4];
    payload[0] = left;
    payload[1] = mid_left;
    payload[2] = mid_right;
    payload[3] = right;
    uart_send_packet(MCU_CMD_RADAR_LEVEL, payload, 4);
}

/* Vehicle Telemetry State (matching factory McuSettings 0x200001BC layout) */
static uint8_t g_steering_sign = 0;
static uint8_t g_steering_mag = 0;
static uint8_t g_gear_field = 0;
static uint8_t g_front_radar[4] = { 12, 12, 12, 12 };
static uint8_t g_rear_radar[4] = { 12, 12, 12, 12 };
static bool    g_door_open = false;

void uart_update_steering_telemetry(uint8_t sign, uint8_t angle_raw) {
    g_steering_sign = sign;
    g_steering_mag = angle_raw;
}

void uart_update_transmission_telemetry(uint8_t gear_field) {
    g_gear_field = gear_field;
}

void uart_update_door_state(bool door_open) {
    g_door_open = door_open;
}

void uart_send_radar_telemetry(uint8_t subtype, const uint8_t levels[4]) {
    /* CMD 0x04 for rear radar, CMD 0x05 for front radar matching factory routines */
    uint8_t cmd = (subtype == 0) ? MCU_CMD_RADAR_LEVEL : MCU_CMD_FRONT_RADAR;
    uart_send_packet(cmd, levels, 4);
}

void uart_update_radar_telemetry(bool is_front, const uint8_t raw_levels[4]) {
    bool changed = false;
    uint8_t *target = is_front ? g_front_radar : g_rear_radar;
    for (uint8_t i = 0; i < 4; i++) {
        if (target[i] != raw_levels[i]) {
            target[i] = raw_levels[i];
            changed = true;
        }
    }
    if (changed) {
        uart_send_radar_telemetry(is_front ? 1 : 0, raw_levels);
        uart_send_composite_vehicle_status();
    }
}

void uart_send_composite_vehicle_status(void) {
    uint8_t payload[8];
    /* byte 0: status bits: base 0x03, bit 4 if door open (0x0800A834 - 0x0800A850) */
    payload[0] = 0x03;
    if (g_door_open) {
        payload[0] |= 0x10;
    }
    
    /* byte 1: gear flags: bit 0 = Drive/Neut, bit 1 = Reverse, bit 2 = Park (0x0800A858 - 0x0800A884) */
    payload[1] = 0x00;
    if ((g_gear_field & 0x38) == 0x10) payload[1] |= 0x01; /* Neutral / Drive */
    if ((g_gear_field & 0x38) == 0x20) payload[1] |= 0x02; /* Reverse */
    if ((g_gear_field & 0x38) == 0x38) payload[1] |= 0x04; /* Park */
    
    /* byte 2: speed / moving indicator (0x0800A888) */
    payload[2] = 0x00;
    
    /* byte 3: steering angle centered at 0x80 (0x0800A890 - 0x0800A8C8) */
    if (g_steering_sign != 0) {
        /* Turning right */
        if (g_steering_mag <= 127) {
            payload[3] = (uint8_t)(128 + g_steering_mag);
        } else {
            payload[3] = 255;
        }
    } else {
        /* Turning left */
        if (g_steering_mag <= 128) {
            payload[3] = (uint8_t)(128 - g_steering_mag);
        } else {
            payload[3] = 0;
        }
    }
    
    /* byte 4: front radar packed levels (0x0800A8D2 - 0x0800A9B6)
     * Channels: FL (g_front_radar[0]), FML (g_front_radar[1]), FR (g_front_radar[3]), FMR (g_front_radar[2]) */
    payload[4] = 0;
    if (g_front_radar[0] < 5) payload[4] |= 0x03;
    else if (g_front_radar[0] < 8) payload[4] |= 0x02;
    else if (g_front_radar[0] < 12) payload[4] |= 0x01;
    
    if (g_front_radar[1] < 5) payload[4] |= 0x0C;
    else if (g_front_radar[1] < 8) payload[4] |= 0x08;
    else if (g_front_radar[1] < 12) payload[4] |= 0x04;
    
    if (g_front_radar[3] < 5) payload[4] |= 0x30;
    else if (g_front_radar[3] < 8) payload[4] |= 0x20;
    else if (g_front_radar[3] < 12) payload[4] |= 0x10;
    
    if (g_front_radar[2] < 5) payload[4] |= 0xC0;
    else if (g_front_radar[2] < 8) payload[4] |= 0x80;
    else if (g_front_radar[2] < 12) payload[4] |= 0x40;
    
    /* byte 5: rear radar packed levels (0x0800A9C0 - 0x0800AA9C)
     * Channels: RL (g_rear_radar[0]), RML (g_rear_radar[1]), RR (g_rear_radar[3]), RMR (g_rear_radar[2]) */
    payload[5] = 0;
    if (g_rear_radar[0] < 5) payload[5] |= 0x03;
    else if (g_rear_radar[0] < 8) payload[5] |= 0x02;
    else if (g_rear_radar[0] < 12) payload[5] |= 0x01;
    
    if (g_rear_radar[1] < 5) payload[5] |= 0x0C;
    else if (g_rear_radar[1] < 8) payload[5] |= 0x08;
    else if (g_rear_radar[1] < 12) payload[5] |= 0x04;
    
    if (g_rear_radar[3] < 5) payload[5] |= 0x30;
    else if (g_rear_radar[3] < 8) payload[5] |= 0x20;
    else if (g_rear_radar[3] < 12) payload[5] |= 0x10;
    
    if (g_rear_radar[2] < 5) payload[5] |= 0xC0;
    else if (g_rear_radar[2] < 8) payload[5] |= 0x80;
    else if (g_rear_radar[2] < 12) payload[5] |= 0x40;
    
    /* byte 6: vehicle setting / status (0x0800AAA0) */
    payload[6] = 0x00;
    /* byte 7: padding (0x0800AAA8) */
    payload[7] = 0x00;
    
    uart_send_packet(MCU_CMD_HVAC_STATUS, payload, 8);
}

void uart_trigger_bootloader_reset(void) {
    /* Set bootloader update magic in RAM */
    *BOOTLOADER_MAGIC_ADDR = BOOTLOADER_MAGIC_VAL;
    
    /* Disable interrupts and spin to let IWDG / Software Reset fire */
    __asm__ volatile("cpsid i");
    SCB->AIRCR = (0x5FAUL << 16) | (1UL << 2); /* SYSRESETREQ */
    while (1) {}
}

/* Inbound Command Handlers */
static void handle_init_handshake(const UartPacket *p) {
    (void)p;
    /* On init handshake (CMD 0x81), broadcast current vehicle status, LCD source, and version */
    uint8_t status_payload[6] = { g_vehicle_status_byte, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uart_send_packet(MCU_CMD_ILLUMINATION_STATUS, status_payload, 6);

    uint8_t lcd_payload[3] = { MCU_LCD_SOURCE_AFTERMARKET, 0x04, 0x00 };
    uart_send_packet(MCU_CMD_LCD_SOURCE_REPORT, lcd_payload, 3);

    uart_send_version_report();
}

/* 0x82: App mode change (e.g. CarPlay/Android Auto active vs OEM head unit
 * active). CORRECTED (2026-09-14): an earlier version of this handler drove
 * GPIOB->BSRR/BRR on pins 0 and 6 directly ("TOUCH_SEL"/"MIC_SEL" relays).
 * That was never actually in the real firmware -- direct disassembly of the
 * real handler (0x08008bd4, docs/MCU_COMMAND_REFERENCE.md's "CMD 0x82 real
 * MCU-side trace") shows it only writes an internal struct (0x20000282:
 * payload[0]==1 -> {1,4}, else -> {2,1}) that feeds a CAN-bus mode-
 * announcement / MCU->SoC status message -- it never drives any GPIO
 * output. GPIOB Pin 0 (TOUCH_SEL) is driven entirely autonomously elsewhere
 * (touch_driver.c's touch_process_digitizer(), based on real PA0/PC4
 * sensing), and GPIOB Pin 6 was never a real output at all -- see
 * gpio_driver.h's GPIO_PIN_MIC_SENSE comment and
 * docs/BOOTLOADER_HANG_TRACE_2026-09-14.md section 28. Reimplemented here
 * to match: track the mode locally (so mcu_settings_get()-style future
 * consumers have it available) with no direct hardware side effect,
 * consistent with what the real firmware actually does. */
static uint8_t g_app_mode = 0;

static void handle_app_state(const UartPacket *p) {
    if (p->len < 1) {
        return;
    }
    uint8_t mode = p->payload[0];
    if (mode == 0 && p->len >= 3 && p->payload[2] != 0) {
        mode = p->payload[2];
    }
    g_app_mode = mode;
}

/* Real firmware's shared 4-state dispatcher (0x080058A4), disassembled this
 * session -- drives GPIOC Pin 13 and GPIOC Pin 2 together, reached from BOTH
 * CMD 0xA0 id=0x11 (r0=2/3) and CMD 0x84's audio-route handler (r0=0/1).
 * Real truth table, traced instruction-by-instruction (0x080058F8 drives
 * Pin 13, 0x0800591C drives Pin 2):
 *   state 0: Pin13=LOW,  Pin2=LOW
 *   state 1: Pin13=LOW,  Pin2=HIGH
 *   state 2: Pin13=LOW,  Pin2=LOW   (same physical result as state 0)
 *   state 3: Pin13=HIGH, Pin2=LOW
 * Two real call sites sharing one relay pair is consistent with GPIOC13/PC2
 * being a combined audio+video OEM-bypass relay, not video-only -- see
 * MCU_FIRMWARE_VERIFIED_FINDINGS.md's discussion of the CMD 0x84 finding
 * that prompted this refactor. */
static void shared_relay_dispatch(uint8_t state) {
    switch (state) {
        case 1:
            GPIOC->BRR  = (1UL << 13);
            GPIOC->BSRR = (1UL << 2);
            break;
        case 3:
            GPIOC->BSRR = (1UL << 13);
            GPIOC->BRR  = (1UL << 2);
            break;
        case 0:
        case 2:
        default:
            GPIOC->BRR = (1UL << 13);
            GPIOC->BRR = (1UL << 2);
            break;
    }
}

/* 0x84: Audio Route. Real firmware (0x08008808) is NOT a simple PA1 mute
 * toggle -- that was never independently disassembly-confirmed (its
 * originally-cited address, 0x0800599C, was already proven wrong for a
 * closely related claim -- see MCU_FIRMWARE_VERIFIED_FINDINGS.md). The real
 * handler:
 *   - Masks the incoming value to 4 bits (0-15), ignores it entirely if >=6.
 *   - Stores it into a debounced/shadowed state field (separate SRAM struct,
 *     base 0x20000238 -- NOT the same struct CMD 0xA0 uses, base 0x200001D8).
 *   - On a real change, value==0 sends the literal ASCII string
 *     "AT+AUDROUTE=1\r\n" over USART3 (same channel as CMD 0x87's Bluetooth
 *     relay and id=0x00's "AT+UPGRADE" command), then calls the shared relay
 *     dispatcher above with state=0 -- but ONLY if this handler's own gate
 *     byte (struct offset 0x5e IN THIS STRUCT) == 0.
 *   - value==3 sends "AT+AUDROUTE=2\r\n", dispatcher state=1, same gate.
 *   - value==1/2/4/5: state stored, no further action (matches the real
 *     firmware's own no-op there).
 * Real, notable, unresolved finding: this handler's gate condition
 * ("proceed if ==0") is the OPPOSITE polarity of CMD 0xA0 id=0x11's own gate
 * ("proceed if ==1") -- and it's genuinely unclear whether these are the
 * same underlying flag at two different relative offsets into overlapping
 * SRAM structs, or two independent flags. Implemented here as a SEPARATE
 * local state field (not reusing McuSettings.flag_5e) to avoid conflating
 * two real values this session couldn't confirm are the same variable. */
static uint8_t g_audio_route_state = 0;
static uint8_t g_audio_route_shadow = 0;
static uint8_t g_audio_route_gate = 0; /* real POR/bss default is 0, which
                                          * means the dispatcher call fires
                                          * immediately by default here --
                                          * unlike id=0x11's own gate, which
                                          * defaults closed. Real firmware
                                          * behavior, not an inconsistency. */

static void handle_audio_route(const UartPacket *p) {
    if (p->len < 1) {
        return;
    }
    /* Real firmware reads payload[1] (offset +3 in frame buffer).
     * Support both 2-byte payloads (value at payload[1]) and 1-byte payloads (value at payload[0]). */
    uint8_t value = (p->len >= 2) ? (p->payload[1] & 0x0F) : (p->payload[0] & 0x0F);
    if (value >= 6) {
        return;
    }

    g_audio_route_state = value;
    if (g_audio_route_state == g_audio_route_shadow) {
        return;
    }
    g_audio_route_shadow = g_audio_route_state;

    if (value == 0) {
        static const uint8_t kAudRoute1[] = "AT+AUDROUTE=1\r\n";
        usart3_relay_send(kAudRoute1, sizeof(kAudRoute1) - 1);
        if (g_audio_route_gate == 0) {
            shared_relay_dispatch(0);
        }
    } else if (value == 3) {
        static const uint8_t kAudRoute2[] = "AT+AUDROUTE=2\r\n";
        usart3_relay_send(kAudRoute2, sizeof(kAudRoute2) - 1);
        if (g_audio_route_gate == 0) {
            shared_relay_dispatch(1);
        }
    }
    /* value == 1,2,4,5: state stored above, no further action -- matches
     * real firmware exactly. */
}

/* handle_diag_read_mem() (CMD 0x90) REMOVED 2026-08-30 -- see the
 * dispatch table's own comment below for the full explanation. This was
 * a fictional arbitrary-memory-read handler ([Addr_B3,B2,B1,B0,Length]
 * -> raw pointer dereference -> echo bytes back), disproven by direct
 * disassembly of this device's own can_app.bin plus 4 other real
 * DCn32-family firmware variants -- none of them have it. */

/* USART3 / Bluetooth AT-command relay (CMD 0x87). Real pins confirmed via
 * disassembly this session (see uart_protocol.h's SOC_CMD_BT_AT_RELAY comment):
 * PB10 = TX (AF push-pull), PB11 = RX (input floating/pull-up). Baud is an
 * unconfirmed best-guess (9600), matching common BT-module AT-mode defaults. */
static bool g_usart3_initialized = false;

void usart3_relay_init(void) {
    if (g_usart3_initialized) {
        return;
    }

    RCC->APB1ENR |= (1UL << 18); /* USART3EN */
    RCC->APB2ENR |= (1UL << 3);  /* IOPBEN */

    /* PB10: Alternate Function Push-Pull 50MHz (Mode 11, CNF 10 -> 0x0B) */
    GPIOB->CRH &= ~(0x0FUL << 8);
    GPIOB->CRH |=  (0x0BUL << 8);

    /* PB11: Input Floating / Pull-Up (Mode 00, CNF 10 -> 0x08) */
    GPIOB->CRH &= ~(0x0FUL << 12);
    GPIOB->CRH |=  (0x08UL << 12);
    GPIOB->ODR |=  (1UL << 11);

    /* 36 MHz / 9600 = 3750 -> Mantissa = 234 (0xEA), Fraction = 6 -> 0xEA6 */
    USART3->BRR = 0x00000EA6;

    USART3->CR1 = (1UL << 13) | (1UL << 5) | (1UL << 3) | (1UL << 2); /* UE, RXNEIE, TE, RE */
    nvic_enable_irq(39);

    g_usart3_initialized = true;
}

void usart3_relay_send(const uint8_t *data, uint8_t len) {
    usart3_relay_init();
    for (uint8_t i = 0; i < len; i++) {
        while ((USART3->SR & (1UL << 7)) == 0) {} /* TXE */
        USART3->DR = data[i];
    }
}

/* Bluetooth module responses come back asynchronously over USART3 RX; relay them
 * to the SoC as further SOC_CMD_BT_AT_RELAY-tagged outbound packets. This mirrors
 * the "relay" concept but the exact reply framing back to the SoC is NOT
 * independently confirmed from disassembly -- flagged, not asserted as verified. */
static uint8_t g_usart3_rx_buf[UART_MAX_PAYLOAD];
static uint8_t g_usart3_rx_idx = 0;

void USART3_IRQHandler(void) {
    uint32_t sr = USART3->SR;
    if (sr & ((1UL << 5) | (1UL << 3))) { /* RXNE or ORE */
        uint8_t byte = (uint8_t)(USART3->DR & 0xFF);
        if (sr & (1UL << 5)) {
            if (g_usart3_rx_idx < UART_MAX_PAYLOAD) {
                g_usart3_rx_buf[g_usart3_rx_idx++] = byte;
            }
            if (byte == '\n' || g_usart3_rx_idx >= UART_MAX_PAYLOAD) {
                uart_send_packet(SOC_CMD_BT_AT_RELAY, g_usart3_rx_buf, g_usart3_rx_idx);
                g_usart3_rx_idx = 0;
            }
        }
    }
}

static void handle_bt_at_relay(const UartPacket *p) {
    if (p->len > 0) {
        usart3_relay_send(p->payload, p->len);
    }
}

/* 0x88: TEA-cipher anti-clone challenge/response. Real algorithm AND real key
 * confirmed via disassembly (see tea_crypto.h/.c for the full derivation,
 * including the real firmware's .data init-table trace that located the key
 * bytes in flash). */
static void handle_crypto_challenge(const UartPacket *p) {
    if (p->len < 8) {
        return;
    }
    uint32_t v0 = ((uint32_t)p->payload[0] << 24) | ((uint32_t)p->payload[1] << 16) |
                  ((uint32_t)p->payload[2] << 8)  |  (uint32_t)p->payload[3];
    uint32_t v1 = ((uint32_t)p->payload[4] << 24) | ((uint32_t)p->payload[5] << 16) |
                  ((uint32_t)p->payload[6] << 8)  |  (uint32_t)p->payload[7];

    tea_decrypt_block(&v0, &v1, tea_real_key);

    uint8_t reply[8];
    reply[0] = (uint8_t)(v0 >> 24); reply[1] = (uint8_t)(v0 >> 16);
    reply[2] = (uint8_t)(v0 >> 8);  reply[3] = (uint8_t)(v0);
    reply[4] = (uint8_t)(v1 >> 24); reply[5] = (uint8_t)(v1 >> 16);
    reply[6] = (uint8_t)(v1 >> 8);  reply[7] = (uint8_t)(v1);
    uart_send_packet(SOC_CMD_CRYPTO_CHALLENGE, reply, 8);
}

static McuSettings g_settings;

const McuSettings *mcu_settings_get(void) {
    return &g_settings;
}

/* 0xA0: UI settings sync. Real firmware format: [settingId, value] -- see
 * docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md for the full decoded 18-entry TBB jump
 * table this switch mirrors (settingId 0x00-0x11). Handlers with a confirmed
 * physical pin actually drive it; the rest only update the settings struct so
 * a future custom_ui build that queries mcu_settings_get() stays forward-compatible
 * once the remaining pins/subsystems are traced. */
static void handle_sync_settings(const UartPacket *p) {
    if (p->len < 2) {
        return;
    }
    uint8_t setting_id = p->payload[0];
    uint8_t value = p->payload[1];

    switch (setting_id) {
        case 0x00: /* Real firmware (0x080089F8) is a real 4-way branch, not a simple
                    * binary flag -- re-traced precisely this session:
                    *   value==1 -> struct[0x3b]=1
                    *   value==2 -> struct[0x3b]=0, AND sends the literal ASCII string
                    *               "AT+UPGRADE\r\n" out over USART3 (the same
                    *               Bluetooth-module UART CMD 0x87 uses) -- a real,
                    *               concrete finding, not a guess: read directly from
                    *               the real firmware's own embedded string constant.
                    *   value==3 -> struct[0x3b]=3
                    *   else     -> struct[0x3b]=0
                    * GPIOB Pin 1 itself is driven by a SEPARATE poll site
                    * (0x08005E4C) that fires HIGH specifically when struct[0x3b]==1
                    * -- that part of the original finding stands; the LOW case for
                    * struct[0x3b]==0/3 wasn't individually traced, so this still
                    * drives LOW for any non-1 value as a reasonable simplification.
                    *
                    * REAL-WORLD MEANING (2026-08-29, app-side trace): this GPIOB1
                    * toggle (value 0/1, never touching the value==2 upgrade path) is
                    * very likely THE REAL microphone-source relay -- the stock head
                    * unit app's own MCUAdapter_BoxP300::getSetItemValueTexts(0)
                    * returns exactly ["OEM Microphone","AfterMarket Microphone"] for
                    * this same id=0x00, sent unmodified by syncSettingDataToMcu().
                    * Real, separately confirmed vendor bug in the STOCK app (not this
                    * firmware): its own Settings UI shows this row as "Reversing
                    * camera", not "Microphone" -- a different function
                    * (getSetItemText()) switches on the same id and has drifted out
                    * of sync with the one that actually builds the wire frame.
                    * Strong candidate explanation for a real "stock factory mic never
                    * worked" complaint -- see custom_ui's "Microphone Source
                    * (OEM/AfterMarket)" toggle and
                    * docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md's "COMPLETE" section for
                    * the full chain. */
            switch (value) {
                case 1: g_settings.mode_3b = 1; break;
                case 2: {
                    g_settings.mode_3b = 0;
                    static const uint8_t kAtUpgrade[] = "AT+UPGRADE\r\n";
                    usart3_relay_send(kAtUpgrade, sizeof(kAtUpgrade) - 1);
                    break;
                }
                case 3: g_settings.mode_3b = 3; break;
                default: g_settings.mode_3b = 0; break;
            }
            if (g_settings.mode_3b == 1) {
                GPIOB->BSRR = (1UL << 1);
            } else {
                GPIOB->BRR = (1UL << 1);
            }
            break;

        case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
        case 0x0e: /* real firmware: shared/no-op target -- genuinely unimplemented there too */
            break;

        case 0x07:
            g_settings.flag_3a = value;
            break;

        case 0x08:
            g_settings.flag_39 = value;
            break;

        case 0x09: /* mic-mux setting. CORRECTED (2026-09-14): this used to
                    * drive GPIOB Pin 6 as an output directly here, which was
                    * never real -- direct disassembly of the real apply
                    * chain (0x08006B28 -> 0x080085F0 -> 0x0800598C, traced
                    * from hardware/MCU/can_app.bin) shows it only ever
                    * READS GPIOC Pin 0 and, on a debounced change, forwards
                    * a notification -- it never writes a GPIO output. The
                    * real read+notify behavior now lives in
                    * uart_protocol_poll_mic_sense() below, called
                    * periodically from main.c's loop exactly like the real
                    * firmware's own poll site. This handler just stores the
                    * setting, matching the real CMD 0xA0 id=0x09 handler
                    * (which is a plain struct write, nothing else). See
                    * gpio_driver.h's GPIO_PIN_MIC_SENSE comment and
                    * docs/BOOTLOADER_HANG_TRACE_2026-09-14.md section 28. */
            g_settings.mic_mux_38 = value;
            break;

        case 0x0a:
            g_settings.value_3c = (value < 10) ? value : 9;
            break;

        case 0x0b: /* coordinated 3-pin enable when cleared to 0 (real finding) */
            g_settings.group_3d = value;
            if (value == 0) {
                GPIOA->BSRR = (1UL << 15);
                GPIOB->BSRR = (1UL << 8);
                GPIOB->BSRR = (1UL << 9);
            } else {
                GPIOA->BRR = (1UL << 15);
                GPIOB->BRR = (1UL << 8);
                GPIOB->BRR = (1UL << 9);
            }
            break;

        case 0x0c:
            g_settings.value_40 = value;
            break;

        case 0x0d:
            g_settings.flag_42 = value;
            break;

        case 0x0f: /* real firmware (0x08008B46): plain store to struct offset 0x43,
                    * no GPIO effect at all -- corrects an earlier, wrong "feeds a
                    * PA15 threshold compare" guess; re-traced precisely this session */
            g_settings.value_43 = value;
            break;

        case 0x10: /* real firmware (0x08008B52): plain store to offset 0x44, same
                    * correction as 0x0f -- no GPIO effect */
            g_settings.value_44 = value;
            break;

        case 0x11: /* Real firmware (0x08008B5E): stores to offset 0x45, then --
                     * ONLY if struct offset 0x5e (whatever sets it is untraced) == 1 --
                     * calls the shared_relay_dispatch() helper above (real address
                     * 0x080058A4) with state=2 (value==0) or state=3 (value!=0).
                     * Now correctly drives BOTH GPIOC13 and GPIOC2 via the shared
                     * helper (previously only GPIOC13 was wired; GPIOC2 was left
                     * unimplemented) -- corrected as part of the CMD 0x84 audio-route
                     * finding, which uses the exact same dispatcher and revealed the
                     * full real truth table.
                     *
                     * GPIOC Pin 13 was previously believed to collide with the SoC
                     * hardware-reset line; re-verified this session that the real SoC
                     * reset pin is GPIOB Pin 14 (0x08005A18), a different port/pin
                     * entirely -- see main.c's gpio_hardware_init(). GPIOC13/PC2's
                     * real function is most plausibly a combined audio+video OEM-
                     * bypass relay (CMD 0x84 sends real "AT+AUDROUTE=1/2" over USART3
                     * alongside driving this same dispatcher -- see handle_audio_route()
                     * above), not video-only as first guessed from
                     * docs/historical/1.3.1_MCU_FIRMWARE_DECOMPILATION.md's claim alone.
                     *
                     * REAL, UNRESOLVED CROSS-REFERENCE (2026-08-29): the stock app's
                     * own Settings UI labels THIS id (0x11) "Microphone" -- but its
                     * real value options (Off/On/12V Active, from
                     * getSetItemValueTexts(17)) have nothing to do with OEM/
                     * AfterMarket mic selection, and the REAL "OEM Microphone"/
                     * "AfterMarket Microphone" value pair actually lives at id=0x00
                     * (see that case above), which the stock UI itself mislabels
                     * "Reversing camera". So id=0x11's "Microphone" label is very
                     * likely just another instance of the same stock-app labeling
                     * bug, not independent evidence this GPIOC13/C2 relay is mic-
                     * related -- not chased further; flagged here so a future pass
                     * doesn't rediscover the same false lead. */
            g_settings.value_45 = value;
            if (g_settings.flag_5e == 1) {
                shared_relay_dispatch(value != 0 ? 3 : 2);
            }
            break;

        default: /* settingId >= 0x12: out-of-range in the real firmware too */
            break;
    }
}

static void handle_reboot_bootloader(const UartPacket *p) {
    (void)p;
    /* 0xE1: Enter resident bootloader for YMODEM upgrade */
    uart_trigger_bootloader_reset();
}

/* 0x85: App Protocol response/ACK. Real firmware (0x08008BA8) stores 3
 * payload bytes into persistent state, then queues an outbound packet via
 * an indexed lookup into an 81-byte-stride descriptor table (0x80062FC,
 * index=3, length=5) -- that table was NOT mapped this pass, so the exact
 * real ACK byte content isn't reproduced here. Real, confirmed part
 * (storing the 3 bytes) is implemented; the ACK below is a reasonable
 * protocol-consistent approximation (echo the command back), not a
 * byte-exact match to real hardware. */
static uint8_t g_app_protocol_state[3];

static void handle_app_protocol(const UartPacket *p) {
    if (p->len < 3) {
        return;
    }
    g_app_protocol_state[0] = p->payload[0];
    g_app_protocol_state[1] = p->payload[1];
    g_app_protocol_state[2] = p->payload[2];

    uart_send_packet(SOC_CMD_APP_PROTOCOL, g_app_protocol_state, 3);
}

/* 0xFF: System State Reset. Real firmware (0x080088E8) is a sub-command
 * dispatch on payload[0] (values 0-9 are genuinely no-op there too; only
 * sub-id 0x7F triggers real action -- the same indexed-table queue call as
 * CMD 0x85 above, index=0, length=0xC). Matches that real dispatch shape:
 * only sub-id 0x7F does anything here, not every 0xFF frame. The real
 * action (per the doc's "clears CAN buffers" description, plausible but
 * not byte-verified against the queued packet content) is approximated as
 * resetting the CAN RX ring -- a real, safe, self-contained effect, not a
 * byte-exact reproduction of the real queued response. */
static void handle_system_reset(const UartPacket *p) {
    if (p->len < 1 || p->payload[0] != 0x7F) {
        return;
    }
    /* Sub-ID 0x7F resets CAN rx ring/buffers. Real firmware returns no UART ACK frame. */
    can_reset_rx_ring();
}

/* Mic-mux sense polling. Mirrors the real firmware's own poll site
 * (0x08006B28, traced from hardware/MCU/can_app.bin): only samples
 * GPIOC Pin 0 while the mic-mux setting (CMD 0xA0 id=0x09) is enabled
 * (== 1), debounces the reading, and on a confirmed change notifies the
 * SoC. The real notify content (its own internal event-queue call,
 * 0x08006228) was not byte-mapped to a specific outbound UART/CAN
 * payload in this project's disassembly work -- approximated here by
 * re-broadcasting the composite vehicle status, a real, already-existing
 * status packet, rather than inventing an unconfirmed dedicated frame.
 * Called from main.c's loop at the same cadence class as the other
 * discrete-input polling task (GPIO_POLL_INTERVAL_MS). */
static uint8_t s_mic_sense_debounce = 0;
static bool s_mic_sense_last_state = false;

void uart_protocol_poll_mic_sense(void) {
    if (g_settings.mic_mux_38 != 1) {
        s_mic_sense_debounce = 0;
        return;
    }
    bool raw = gpio_get_mic_sense();
    if (raw == s_mic_sense_last_state) {
        s_mic_sense_debounce = 0;
        return;
    }
    if (s_mic_sense_debounce < 2) {
        s_mic_sense_debounce++;
        return;
    }
    s_mic_sense_debounce = 0;
    s_mic_sense_last_state = raw;
    uart_send_composite_vehicle_status();
}

static const UartCmdDispatchEntry g_uart_cmd_table[] = {
    { SOC_CMD_INIT_HANDSHAKE,  {0}, handle_init_handshake },
    { SOC_CMD_APP_STATE,       {0}, handle_app_state },
    { SOC_CMD_AUDIO_ROUTE,     {0}, handle_audio_route },
    { SOC_CMD_BT_AT_RELAY,     {0}, handle_bt_at_relay },
    { SOC_CMD_CRYPTO_CHALLENGE,{0}, handle_crypto_challenge },
    { SOC_CMD_SYNC_SETTINGS,   {0}, handle_sync_settings },
    { SOC_CMD_REBOOT_BOOTLDR,  {0}, handle_reboot_bootloader },
    { SOC_CMD_APP_PROTOCOL,    {0}, handle_app_protocol },
    { SOC_CMD_SYSTEM_RESET,    {0}, handle_system_reset }
    /* SOC_CMD_DIAG_READ_MEM (0x90) REMOVED 2026-08-30 -- disproven, not
     * just unconfirmed. Directly read the real 9-entry (cmd,handler_ptr)
     * dispatch table + its exact bounding loop (cmp r4,#9) out of this
     * device's own can_app.bin, AND cross-checked against 4 other real
     * DCn32-family firmware variants (2 more Toyota/generic builds byte-
     * identical to this device's own, 1 Acura build with a genuinely
     * different 7-entry table missing 0x84/0x87 too) -- 0x90 appears in
     * NONE of them. This command never existed; it was a clean-room
     * fabrication (likely inherited from this source's original,
     * "largely unverified handoff document" per this file's own header)
     * that read arbitrary [address,length] and echoed the bytes back --
     * a real, working RDP-bypass/full-flash-dump primitive, had it been
     * real. See docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md's "CMD 0x90 --
     * disproven" section for the full multi-firmware trace. */
};
#define UART_CMD_COUNT (sizeof(g_uart_cmd_table) / sizeof(g_uart_cmd_table[0]))

void uart_process_rx(void) {
    while (g_rx_head != g_rx_tail) {
        UartPacket p = g_rx_ring[g_rx_tail];
        g_rx_tail = (g_rx_tail + 1) % UART_RX_RING_SIZE;
        
        for (uint8_t i = 0; i < UART_CMD_COUNT; i++) {
            if (g_uart_cmd_table[i].cmd == p.cmd && g_uart_cmd_table[i].handler) {
                g_uart_cmd_table[i].handler(&p);
                break;
            }
        }
    }
}
