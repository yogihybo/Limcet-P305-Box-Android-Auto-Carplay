#include "bootloader.h"

#define FLASH_KEY1 0x45670123UL
#define FLASH_KEY2 0xCDEF89ABUL

/* 128-byte de-obfuscation table confirmed byte-for-byte from factory
 * bootloader flash at 0x08001F72 (docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md §28/§35).
 * Transform formula: flash[i] = (~wire[i] + T[i % 128]) & 0xFF */
static const uint8_t g_deobfuscation_table[BLOCK_PAYLOAD_SIZE] = {
    0x78, 0xF8, 0x79, 0xF9, 0x7A, 0xFA, 0x7B, 0xFB, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0xFF, 0x7F, 0xFE, 0x7E, 0xFD, 0x7D, 0xFC, 0x7C, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
    0x7C, 0xFC, 0x7D, 0xFD, 0x7E, 0xFE, 0x7F, 0xFF, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88,
    0xFB, 0x7B, 0xFA, 0x7A, 0xF9, 0x79, 0xF8, 0x78, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
    0x78, 0xF8, 0x79, 0xF9, 0x7A, 0xFA, 0x7B, 0xFB, 0x1F, 0x2E, 0x3D, 0x4C, 0x5A, 0x69, 0x78, 0x87,
    0xFF, 0x7F, 0xFE, 0x7E, 0xFD, 0x7D, 0xFC, 0x7C, 0x96, 0xA5, 0xB4, 0xC3, 0xD2, 0xE1, 0xF0, 0x00,
    0x7C, 0xFC, 0x7D, 0xFD, 0x7E, 0xFE, 0x7F, 0xFF, 0xF1, 0xE2, 0xD3, 0xC4, 0xA5, 0x96, 0x87, 0x78,
    0xFB, 0x7B, 0xFA, 0x7A, 0xF9, 0x79, 0xF8, 0x78, 0x69, 0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F, 0x00
};

void flash_unlock(void) {
    /* Factory bootloader writes unlock keys directly to FLASH_KEYR */
    *((volatile uint32_t *)0x40022004UL) = FLASH_KEY1;
    *((volatile uint32_t *)0x40022004UL) = FLASH_KEY2;
}

void flash_lock(void) {
    *((volatile uint32_t *)0x40022010UL) |= (1UL << 7); /* CR LOCK */
}

static inline void flash_wait_busy(void) {
    while (*((volatile uint32_t *)0x4002200CUL) & (1UL << 0)) {
        iwdg_feed();
    }
}

bool flash_erase_page(uint32_t page_addr) {
    flash_wait_busy();
    *((volatile uint32_t *)0x40022010UL) |= (1UL << 1); /* PER (Page Erase) */
    *((volatile uint32_t *)0x40022014UL) = page_addr;   /* FLASH_AR */
    *((volatile uint32_t *)0x40022010UL) |= (1UL << 6); /* STRT */
    flash_wait_busy();
    *((volatile uint32_t *)0x40022010UL) &= ~(1UL << 1);
    return true;
}

bool flash_erase_app_pages(void) {
    for (uint32_t page_addr = APP_FLASH_BASE; page_addr < APP_FLASH_END; page_addr += FLASH_PAGE_SIZE) {
        flash_erase_page(page_addr);
        iwdg_feed();
        (void)millis();   /* keep the SysTick-based clock from wrapping unseen */
    }
    return true;
}

bool flash_write_halfword(uint32_t address, uint16_t data) {
    flash_wait_busy();
    *((volatile uint32_t *)0x40022010UL) |= (1UL << 0); /* PG */
    *((volatile uint16_t *)address) = data;
    flash_wait_busy();
    *((volatile uint32_t *)0x40022010UL) &= ~(1UL << 0);
    return true;
}

bool flash_write_block(uint32_t address, const uint8_t *data, uint32_t length) {
    const uint16_t *src = (const uint16_t *)data;
    uint32_t count = (length + 1) / 2;
    for (uint32_t i = 0; i < count; i++) {
        flash_write_halfword(address + (i * 2), src[i]);
    }
    return true;
}

/* Transmit 5-byte CMD 0xE2 ACK packet: 2E E2 01 <status> <chk> */
static void send_e2_status(uint8_t status) {
    uint8_t chk = (uint8_t)(~(MCU_CMD_UPDATE_ACK + 1 + status) & 0xFF);
    uart_putc(MCU_SYNC_BYTE);
    uart_putc(MCU_CMD_UPDATE_ACK);
    uart_putc(0x01);
    uart_putc(status);
    uart_putc(chk);
}

/* Transmit 6-byte CMD 0xE4 block status packet: 2E E4 02 <hi> <lo> <chk> */
static void send_e4_status(uint16_t last_block_idx) {
    uint8_t hi = (uint8_t)((last_block_idx >> 8) & 0xFF);
    uint8_t lo = (uint8_t)(last_block_idx & 0xFF);
    uint8_t chk = (uint8_t)(~(MCU_CMD_UPDATE_STATUS + 2 + hi + lo) & 0xFF);
    uart_putc(MCU_SYNC_BYTE);
    uart_putc(MCU_CMD_UPDATE_STATUS);
    uart_putc(0x02);
    uart_putc(hi);
    uart_putc(lo);
    uart_putc(chk);
}

/* Wait for head unit CMD 0xE1 "Ready to update" header frame */
static bool wait_for_e1_header(uint16_t *out_total_blocks) {
    /* State machine matching factory bootloader 0x080019EC
     * Accepts:
     * 1) 2E E1 02 <hi> <lo> <chk>
     * 2) 2E E1 08 <hi> <lo> "XINBAS" <chk> */
    uint8_t state = 0;
    uint8_t hi = 0, lo = 0;
    uint8_t xinbas_idx = 0;
    static const uint8_t xinbas_magic[6] = {'X', 'I', 'N', 'B', 'A', 'S'};
    uint32_t start = millis();
    uint32_t last_prompt = start;

    for (;;) {
        iwdg_feed();

        /* Factory 0x08001B3A-0x08001B5E: prompt the host with E2 00 while
         * nothing arrives, and give up after a fixed budget. */
        uint32_t now = millis();
        if ((now - start) >= HEADER_GIVE_UP_MS) {
            return false;
        }
        if ((now - last_prompt) >= HEADER_PROMPT_MS) {
            send_e2_status(UPDATE_STATUS_REJECT);
            last_prompt = now;
            state = 0;
        }

        uint8_t b;
        if (!uart_getc_timeout(&b, 1)) {
            continue;
        }

        switch (state) {
            case 0: /* Wait for sync byte 0x2E */
                if (b == MCU_SYNC_BYTE) {
                    state = 1;
                }
                break;

            case 1: /* CMD 0xE1 */
                if (b == SOC_CMD_UPDATE_HEADER) {
                    state = 2;
                } else if (b == MCU_SYNC_BYTE) {
                    state = 1;
                } else {
                    state = 0;
                }
                break;

            case 2: /* Length byte (0x02 or 0x08) */
                if (b == 0x02) {
                    state = 3; /* Standard 2-byte header */
                } else if (b == 0x08) {
                    state = 10; /* XINBAS 8-byte header */
                } else if (b == MCU_SYNC_BYTE) {
                    state = 1;
                } else {
                    state = 0;
                }
                break;

            case 3: /* Standard: Block count high byte */
                hi = b;
                state = 4;
                break;

            case 4: /* Standard: Block count low byte */
                lo = b;
                state = 5;
                break;

            case 5: /* Standard: Checksum */
                {
                    uint8_t expected_chk = (uint8_t)(~(SOC_CMD_UPDATE_HEADER + 0x02 + hi + lo) & 0xFF);
                    uint16_t n = ((uint16_t)hi << 8) | lo;
                    /* Factory 0x08001AA8: a zero block count is answered E2 00 and the
                     * wait goes on. Counts that can't fit the 116 KB region are refused too
                     * (the factory would program past the end of flash and fault). */
                    if (b == expected_chk && n != 0 && n <= MAX_UPDATE_BLOCKS) {
                        *out_total_blocks = n;
                        send_e2_status(UPDATE_STATUS_READY);
                        return true;
                    }
                    send_e2_status(UPDATE_STATUS_REJECT);
                    state = 0;
                }
                break;

            case 10: /* XINBAS: Block count high byte */
                hi = b;
                state = 11;
                break;

            case 11: /* XINBAS: Block count low byte */
                lo = b;
                xinbas_idx = 0;
                state = 12;
                break;

            case 12: /* XINBAS magic string verification */
                if (b == xinbas_magic[xinbas_idx]) {
                    xinbas_idx++;
                    if (xinbas_idx == 6) {
                        state = 13;
                    }
                } else {
                    state = 0;
                }
                break;

            case 13: /* XINBAS: Checksum */
                {
                    uint8_t sum = SOC_CMD_UPDATE_HEADER + 0x08 + hi + lo;
                    for (int i = 0; i < 6; i++) {
                        sum += xinbas_magic[i];
                    }
                    uint8_t expected_chk = (uint8_t)(~sum & 0xFF);
                    uint16_t n = ((uint16_t)hi << 8) | lo;
                    if (b == expected_chk && n != 0 && n <= MAX_UPDATE_BLOCKS) {
                        *out_total_blocks = n;
                        send_e2_status(UPDATE_STATUS_READY);
                        return true;
                    }
                    send_e2_status(UPDATE_STATUS_REJECT);
                    state = 0;
                }
                break;

            default:
                state = 0;
                break;
        }
    }
}

bool mcu_process_firmware_update(void) {
    uint16_t total_blocks = 0;

    flash_unlock();

    /* Step 1: Wait for head unit CMD 0xE1 header */
    if (!wait_for_e1_header(&total_blocks)) {
        flash_lock();
        return false;
    }

    /* Step 2: Receive and flash each 128-byte data block (CMD 0xE3) */
    uint16_t expected_block = 0;
    uint32_t curr_flash_addr = APP_FLASH_BASE;
    bool erased = false;
    uint8_t wire_buf[BLOCK_PAYLOAD_SIZE];
    uint8_t deobf_buf[BLOCK_PAYLOAD_SIZE];

    /* Packet parser state for CMD 0xE3 */
    uint8_t state = 0;
    uint8_t b_hi = 0, b_lo = 0;
    uint16_t payload_idx = 0;
    uint8_t running_chk = 0;
    uint32_t last_byte = millis();
    uint32_t last_frame = last_byte;

    while (expected_block < total_blocks) {
        iwdg_feed();

        /* Factory 0x08001E76-0x08001EAC. After a stretch of silence, drop any
         * partial frame and re-request: E2 01 ("go", restart at block 0) until
         * the first block has been taken, then E4 (resend the next block).
         * Without this a single lost frame leaves both ends waiting forever. */
        uint32_t now = millis();
        if ((now - last_frame) >= BLOCK_GIVE_UP_MS) {
            flash_lock();
            return false;
        }
        if ((now - last_byte) >= BLOCK_IDLE_MS) {
            state = 0;
            if (erased) {
                send_e4_status(expected_block - 1);
            } else {
                send_e2_status(UPDATE_STATUS_READY);
            }
            last_byte = now;
        }

        uint8_t b;
        if (!uart_getc_timeout(&b, 1)) {
            continue;
        }
        last_byte = millis();

        switch (state) {
            case 0: /* Sync byte */
                if (b == MCU_SYNC_BYTE) {
                    state = 1;
                }
                break;

            case 1: /* CMD 0xE3 */
                if (b == SOC_CMD_UPDATE_BLOCK) {
                    running_chk = SOC_CMD_UPDATE_BLOCK;
                    state = 2;
                } else if (b == MCU_SYNC_BYTE) {
                    state = 1;
                } else {
                    state = 0;
                }
                break;

            case 2: /* Length byte (0x82 = 130 bytes payload: 2 index + 128 data) */
                if (b == 0x82) {
                    running_chk += 0x82;
                    state = 3;
                } else if (b == MCU_SYNC_BYTE) {
                    state = 1;
                } else {
                    state = 0;
                }
                break;

            case 3: /* Block index high */
                b_hi = b;
                running_chk += b;
                state = 4;
                break;

            case 4: /* Block index low */
                b_lo = b;
                running_chk += b;
                payload_idx = 0;
                state = 5;
                break;

            case 5: /* Payload data bytes (128 bytes) */
                wire_buf[payload_idx++] = b;
                running_chk += b;
                if (payload_idx == BLOCK_PAYLOAD_SIZE) {
                    state = 6;
                }
                break;

            case 6: /* Checksum */
                {
                    uint8_t expected_chk = (uint8_t)(~running_chk & 0xFF);
                    uint16_t blk_num = ((uint16_t)b_hi << 8) | b_lo;

                    last_frame = millis();
                    if (b != expected_chk) {
                        /* Factory 0x08001E6A: a corrupt frame gets no reply; the idle
                         * re-request above recovers. (Replying E2 00 here made the head
                         * unit abandon the whole update.) */
                    } else if (blk_num == expected_block) {
                        /* On first valid data block: erase app pages and set interrupted-update flag */
                        if (!erased) {
                            flash_erase_app_pages();
                            flash_write_halfword(UPDATE_FLAG_ADDR, UPDATE_FLAG_VAL);
                            erased = true;
                            last_byte = millis();   /* the erase takes 1-2 s */
                        }

                        /* De-obfuscate payload: flash[i] = (~wire[i] + T[i % 128]) & 0xFF */
                        for (uint32_t i = 0; i < BLOCK_PAYLOAD_SIZE; i++) {
                            deobf_buf[i] = (uint8_t)((~wire_buf[i] + g_deobfuscation_table[i]) & 0xFF);
                        }

                        /* Program de-obfuscated halfwords into flash */
                        flash_write_block(curr_flash_addr, deobf_buf, BLOCK_PAYLOAD_SIZE);
                        curr_flash_addr += BLOCK_PAYLOAD_SIZE;

                        expected_block++;

                        /* Send ACK for this block */
                        send_e4_status(expected_block - 1);
                    } else {
                        /* Factory 0x08001C72 / 0x08001EE0: a good frame for the wrong block
                         * is ignored and answered with the last block taken, so the host
                         * resends the one after it. */
                        send_e4_status(expected_block - 1);
                    }
                    state = 0;
                }
                break;

            default:
                state = 0;
                break;
        }
    }

    /* Step 3: All blocks received!
     * 1. Send final E4 report (done by loop completion)
     * 2. Erase flag page (0x08002800) to clear 0x5AA5 flag
     * 3. Send 2E E2 01 02 1A (update complete)
     * 4. Lock flash & clear SRAM handoff magic
     * 5. Spin to allow IWDG reset into normal boot */
    flash_erase_page(UPDATE_FLAG_PAGE_ADDR);
    send_e2_status(UPDATE_STATUS_COMPLETE);
    flash_lock();

    *BOOTLOADER_MAGIC_ADDR = 0;

    /* Spin until IWDG forces soft reset into new application */
    while (1) {
        /* Do NOT feed IWDG here -- allow watchdog to fire */
    }

    return true;
}
