#include "uart_protocol.h"
#include "can_driver.h"
#include "tea_crypto.h"

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
    /* Check RXNE */
    if (USART2->SR & (1UL << 5)) {
        uint8_t byte = (uint8_t)(USART2->DR & 0xFF);
        
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

void uart_send_key_event(uint8_t key_code, bool pressed) {
    uint8_t payload[2];
    payload[0] = key_code;
    payload[1] = pressed ? 0x01 : 0x00;
    uart_send_packet(MCU_CMD_INPUT_EVENT, payload, 2);
}

void uart_send_reverse_state(bool reverse_active) {
    uint8_t payload[2];
    payload[0] = reverse_active ? 0x01 : 0x00;
    payload[1] = 0x00;
    uart_send_packet(MCU_CMD_REVERSE_GEAR, payload, 2);
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
    /* 1. Send MCU Firmware Version Report (Matches stock 2E 01 06 130000000000 E5 -> Limcet-V1.0-1302) */
    uint8_t ver_payload[6] = { 0x13, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uart_send_packet(MCU_CMD_HANDSHAKE_VER, ver_payload, 6);

    /* 2. Send Vehicle Profile & DIP Switch Report (Matches stock 2E 12 03 010400 E5 -> Toyota Prado 150 Mode) */
    uint8_t dip_payload[3] = { 0x01, 0x04, 0x00 };
    uart_send_packet(MCU_CMD_DIP_PROFILE, dip_payload, 3);
}

static void handle_app_state(const UartPacket *p) {
    /* 0x82: App mode change (e.g. CarPlay active vs OEM head unit active) */
    if (p->len >= 3) {
        uint8_t mode = p->payload[2];
        if (mode == 0x01) {
            /* Switch relays to CarPlay / Android Auto */
            GPIOB->BSRR = (1UL << 0); /* PB0 TOUCH_SEL -> SoC */
            GPIOB->BSRR = (1UL << 6); /* PB6 MIC_SEL   -> SoC */
        } else if (mode == 0x00) {
            /* Bypass relays back to OEM Factory Radio */
            GPIOB->BRR = (1UL << 0);  /* PB0 TOUCH_SEL -> OEM */
            GPIOB->BRR = (1UL << 6);  /* PB6 MIC_SEL   -> OEM */
        }
    }
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
    uint8_t value = p->payload[0] & 0x0F;
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

static void handle_diag_read_mem(const UartPacket *p) {
    /* 0x90: Diagnostic Memory Readback [Addr_B3, Addr_B2, Addr_B1, Addr_B0, Length] */
    if (p->len >= 5) {
        uint32_t addr = ((uint32_t)p->payload[0] << 24) |
                        ((uint32_t)p->payload[1] << 16) |
                        ((uint32_t)p->payload[2] << 8)  |
                        ((uint32_t)p->payload[3]);
        uint8_t count = p->payload[4];
        if (count > (UART_MAX_PAYLOAD - 4)) {
            count = (UART_MAX_PAYLOAD - 4);
        }

        uint8_t reply[UART_MAX_PAYLOAD];
        reply[0] = p->payload[0];
        reply[1] = p->payload[1];
        reply[2] = p->payload[2];
        reply[3] = p->payload[3];

        const uint8_t *src = (const uint8_t *)addr;
        for (uint8_t i = 0; i < count; i++) {
            reply[4 + i] = src[i];
        }
        uart_send_packet(SOC_CMD_DIAG_READ_MEM, reply, count + 4);
    }
}

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
    if (USART3->SR & (1UL << 5)) { /* RXNE */
        uint8_t byte = (uint8_t)(USART3->DR & 0xFF);
        if (g_usart3_rx_idx < UART_MAX_PAYLOAD) {
            g_usart3_rx_buf[g_usart3_rx_idx++] = byte;
        }
        if (byte == '\n' || g_usart3_rx_idx >= UART_MAX_PAYLOAD) {
            uart_send_packet(SOC_CMD_BT_AT_RELAY, g_usart3_rx_buf, g_usart3_rx_idx);
            g_usart3_rx_idx = 0;
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
                    * drives LOW for any non-1 value as a reasonable simplification. */
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

        case 0x09: /* mic/audio input mux -- real, already-shipped custom_ui feature */
            g_settings.mic_mux_38 = value;
            if (value != 0) {
                GPIOB->BSRR = (1UL << 6); /* PB6 -> SoC */
            } else {
                GPIOB->BRR = (1UL << 6);  /* PB6 -> OEM */
            }
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
                     * docs/1.3.1_MCU_FIRMWARE_DECOMPILATION.md's claim alone. */
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
    can_reset_rx_ring();
    uint8_t ack = 0x7F;
    uart_send_packet(SOC_CMD_SYSTEM_RESET, &ack, 1);
}

static const UartCmdDispatchEntry g_uart_cmd_table[] = {
    { SOC_CMD_INIT_HANDSHAKE,  {0}, handle_init_handshake },
    { SOC_CMD_APP_STATE,       {0}, handle_app_state },
    { SOC_CMD_AUDIO_ROUTE,     {0}, handle_audio_route },
    { SOC_CMD_DIAG_READ_MEM,   {0}, handle_diag_read_mem },
    { SOC_CMD_BT_AT_RELAY,     {0}, handle_bt_at_relay },
    { SOC_CMD_CRYPTO_CHALLENGE,{0}, handle_crypto_challenge },
    { SOC_CMD_SYNC_SETTINGS,   {0}, handle_sync_settings },
    { SOC_CMD_REBOOT_BOOTLDR,  {0}, handle_reboot_bootloader },
    { SOC_CMD_APP_PROTOCOL,    {0}, handle_app_protocol },
    { SOC_CMD_SYSTEM_RESET,    {0}, handle_system_reset }
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
