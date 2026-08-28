#include "uart_protocol.h"
#include "can_driver.h"

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

static void handle_sync_settings(const UartPacket *p) {
    (void)p;
    /* 0xA0: Settings sync ACK */
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
