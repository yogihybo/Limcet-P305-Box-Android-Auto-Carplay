#include "bootloader.h"

#define FLASH_KEY1 0x45670123UL
#define FLASH_KEY2 0xCDEF89ABUL

void flash_unlock(void) {
    if (*((volatile uint32_t *)0x40022010UL) & (1UL << 7)) { /* FLASH_CR LOCK */
        *((volatile uint32_t *)0x40022004UL) = FLASH_KEY1;  /* FLASH_KEYR */
        *((volatile uint32_t *)0x40022004UL) = FLASH_KEY2;
    }
}

void flash_lock(void) {
    *((volatile uint32_t *)0x40022010UL) |= (1UL << 7); /* Set LOCK */
}

static void flash_wait_busy(void) {
    while (*((volatile uint32_t *)0x4002200CUL) & (1UL << 0)) {} /* FLASH_SR BSY */
}

bool flash_erase_app_pages(void) {
    flash_unlock();
    
    for (uint32_t page_addr = APP_FLASH_BASE; page_addr < APP_FLASH_END; page_addr += FLASH_PAGE_SIZE) {
        flash_wait_busy();
        *((volatile uint32_t *)0x40022010UL) |= (1UL << 1); /* PER (Page Erase) */
        *((volatile uint32_t *)0x40022014UL) = page_addr;   /* FLASH_AR */
        *((volatile uint32_t *)0x40022010UL) |= (1UL << 6); /* STRT */
        flash_wait_busy();
        *((volatile uint32_t *)0x40022010UL) &= ~(1UL << 1);
    }
    
    flash_lock();
    return true;
}

bool flash_write_page(uint32_t address, const uint8_t *data, uint32_t length) {
    flash_unlock();
    
    const uint16_t *src16 = (const uint16_t *)data;
    uint32_t count16 = (length + 1) / 2;
    volatile uint16_t *dst16 = (volatile uint16_t *)address;

    for (uint32_t i = 0; i < count16; i++) {
        flash_wait_busy();
        *((volatile uint32_t *)0x40022010UL) |= (1UL << 0); /* PG (Programming) */
        dst16[i] = src16[i];
        flash_wait_busy();
        *((volatile uint32_t *)0x40022010UL) &= ~(1UL << 0);
    }

    flash_lock();
    return true;
}

static uint16_t update_crc16(uint16_t crc, uint8_t byte) {
    crc ^= (uint16_t)byte << 8;
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 0x8000) {
            crc = (crc << 1) ^ 0x1021;
        } else {
            crc <<= 1;
        }
    }
    return crc;
}

bool ymodem_receive_and_flash(void) {
    uint8_t pkt_buf[1024 + 5];
    uint32_t write_addr = APP_FLASH_BASE;
    uint8_t expected_pkt_num = 0;
    bool session_started = false;

    flash_erase_app_pages();

    for (int retry = 0; retry < 30; retry++) {
        uart_putc(CRC16); /* Send 'C' to start YMODEM-CRC */

        uint8_t start_byte;
        if (!uart_getc_timeout(&start_byte, 1000)) {
            continue;
        }

        while (1) {
            uint32_t pkt_len = 0;
            if (start_byte == SOH) {
                pkt_len = 128;
            } else if (start_byte == STX) {
                pkt_len = 1024;
            } else if (start_byte == EOT) {
                uart_putc(NAK);
                uint8_t eot2;
                uart_getc_timeout(&eot2, 500);
                uart_putc(ACK);
                uart_putc(CRC16);
                /* Final empty packet */
                uint8_t final_start;
                if (uart_getc_timeout(&final_start, 1000) && final_start == SOH) {
                    for (int i = 0; i < 132; i++) {
                        uint8_t dummy;
                        uart_getc_timeout(&dummy, 50);
                    }
                    uart_putc(ACK);
                }
                return true;
            } else if (start_byte == CANCEL) {
                return false;
            } else {
                break;
            }

            /* Read packet header & payload */
            uint8_t pkt_num, pkt_num_inv;
            if (!uart_getc_timeout(&pkt_num, 500) || !uart_getc_timeout(&pkt_num_inv, 500)) {
                uart_putc(NAK);
                break;
            }

            if ((pkt_num ^ pkt_num_inv) != 0xFF) {
                uart_putc(NAK);
                break;
            }

            uint16_t calc_crc = 0;
            for (uint32_t i = 0; i < pkt_len; i++) {
                if (!uart_getc_timeout(&pkt_buf[i], 200)) {
                    uart_putc(NAK);
                    break;
                }
                calc_crc = update_crc16(calc_crc, pkt_buf[i]);
            }

            uint8_t crc_h, crc_l;
            if (!uart_getc_timeout(&crc_h, 200) || !uart_getc_timeout(&crc_l, 200)) {
                uart_putc(NAK);
                break;
            }
            uint16_t rx_crc = ((uint16_t)crc_h << 8) | crc_l;

            if (rx_crc != calc_crc) {
                uart_putc(NAK);
                break;
            }

            /* Packet 0: File Header Metadata */
            if (pkt_num == 0 && !session_started) {
                session_started = true;
                expected_pkt_num = 1;
                uart_putc(ACK);
                uart_putc(CRC16);
            } else if (pkt_num == expected_pkt_num) {
                /* Write payload to Flash */
                if (write_addr + pkt_len <= APP_FLASH_END) {
                    flash_write_page(write_addr, pkt_buf, pkt_len);
                    write_addr += pkt_len;
                }
                expected_pkt_num++;
                uart_putc(ACK);
            } else {
                uart_putc(ACK); /* Ignore duplicate */
            }

            /* Wait for next start byte */
            if (!uart_getc_timeout(&start_byte, 2000)) {
                break;
            }
        }
    }

    return false;
}
