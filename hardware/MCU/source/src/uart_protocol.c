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

static void handle_audio_route(const UartPacket *p) {
    /* 0x84: Audio amplifier mute/unmute control */
    if (p->len >= 1) {
        bool mute = (p->payload[0] != 0);
        if (mute) {
            GPIOA->BSRR = (1UL << 1); /* PA1 AMP_MUTE -> HIGH (Muted) */
        } else {
            GPIOA->BRR = (1UL << 1);  /* PA1 AMP_MUTE -> LOW (Unmuted) */
        }
    }
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

/* 0x88: TEA-cipher anti-clone challenge/response. Real algorithm structure
 * confirmed via disassembly (see tea_crypto.h/.c). Runs against an explicit
 * placeholder key -- NOT expected to match real hardware's response until
 * the real key is recovered (see tea_crypto.h for why it's not guessed). */
static void handle_crypto_challenge(const UartPacket *p) {
    if (p->len < 8) {
        return;
    }
    uint32_t v0 = ((uint32_t)p->payload[0] << 24) | ((uint32_t)p->payload[1] << 16) |
                  ((uint32_t)p->payload[2] << 8)  |  (uint32_t)p->payload[3];
    uint32_t v1 = ((uint32_t)p->payload[4] << 24) | ((uint32_t)p->payload[5] << 16) |
                  ((uint32_t)p->payload[6] << 8)  |  (uint32_t)p->payload[7];

    tea_decrypt_block(&v0, &v1, tea_key_placeholder);

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
        case 0x00: /* real target: GPIOB Pin 1 */
            g_settings.mode_3b = value;
            if (value == 1) {
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
                     * calls a shared 4-state dispatcher (0x080058A4) with r0=2 (value==0)
                     * or r0=3 (value!=0), which resolves to: GPIOC Pin 13 = HIGH when
                     * value!=0 else LOW; GPIOC Pin 2 stays LOW either way at this call
                     * site (other r0 states drive Pin 2 HIGH too, reached from a
                     * different caller -- id=0x00's value==2 branch queues an outbound
                     * reply via the same ring-buffer mechanism as CMD 0x87, a separate
                     * finding not yet implemented here).
                     *
                     * DELIBERATELY NOT wiring the physical pin toggle: GPIOC Pin 13 is
                     * the SAME pin this clean-room source already uses as the ArkMicro
                     * ARK1668 SoC hardware-reset line (main.c's gpio_hardware_init).
                     * Toggling it again at runtime from here could unexpectedly assert
                     * SoC reset. Struct bookkeeping only, until this collision is
                     * resolved against a real schematic or scope capture -- consistent
                     * with this project's zero-unverified-hardware-action policy. */
            g_settings.value_45 = value;
            if (g_settings.flag_5e == 1) {
                /* Real target confirmed: GPIOC Pin 13 (value!=0 -> HIGH, else LOW).
                 * NOT driven here -- see comment above. */
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

static const UartCmdDispatchEntry g_uart_cmd_table[] = {
    { SOC_CMD_INIT_HANDSHAKE,  {0}, handle_init_handshake },
    { SOC_CMD_APP_STATE,       {0}, handle_app_state },
    { SOC_CMD_AUDIO_ROUTE,     {0}, handle_audio_route },
    { SOC_CMD_DIAG_READ_MEM,   {0}, handle_diag_read_mem },
    { SOC_CMD_BT_AT_RELAY,     {0}, handle_bt_at_relay },
    { SOC_CMD_CRYPTO_CHALLENGE,{0}, handle_crypto_challenge },
    { SOC_CMD_SYNC_SETTINGS,   {0}, handle_sync_settings },
    { SOC_CMD_REBOOT_BOOTLDR,  {0}, handle_reboot_bootloader }
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
