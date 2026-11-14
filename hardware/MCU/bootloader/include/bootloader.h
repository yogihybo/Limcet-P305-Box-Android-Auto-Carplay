#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include "stm32f105.h"

/* Memory Map Constants
 * Confirmed via disassembly of factory bootloader (docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md)
 * 0x08000000 - 0x080027FF: Bootloader code/data (10 KB)
 * 0x08002800 - 0x08002FFF: Interrupted-update flag page (2 KB, Page 5)
 * 0x08003000 - 0x080037FF: Application vector table base (Page 6)
 * 0x08003000 - 0x0801FFFF: Application region (116 KB, 58 pages)
 */
#define FLASH_PAGE_SIZE         2048UL
#define BOOTLOADER_CODE_BASE    0x08000000UL
#define UPDATE_FLAG_PAGE_ADDR   0x08002800UL
#define UPDATE_FLAG_ADDR        0x08002FFEUL
#define UPDATE_FLAG_VAL         0x5AA5U

#define APP_FLASH_BASE          0x08003000UL
#define APP_FLASH_END           0x0801FFFFUL
#define APP_PAGE_COUNT          58U

#define BOOTLOADER_HANDSHAKE_ADDR ((volatile uint32_t *)0x20004000UL)
#define BOOTLOADER_HANDSHAKE_VAL  0x20141003UL

#define BOOTLOADER_MAGIC_ADDR     ((volatile uint32_t *)0x20004004UL)
#define BOOTLOADER_MAGIC_VAL      0x5555AAAAUL

/* OEM Protocol Constants (0x2E Framing) */
#define MCU_SYNC_BYTE           0x2EU
#define SOC_CMD_UPDATE_HEADER   0xE1U
#define MCU_CMD_UPDATE_ACK      0xE2U
#define SOC_CMD_UPDATE_BLOCK    0xE3U
#define MCU_CMD_UPDATE_STATUS   0xE4U

#define UPDATE_STATUS_READY     0x01U
#define UPDATE_STATUS_REJECT    0x00U
#define UPDATE_STATUS_COMPLETE  0x02U

#define BLOCK_PAYLOAD_SIZE      128U
#define MAX_UPDATE_BLOCKS       ((APP_FLASH_END + 1UL - APP_FLASH_BASE) / BLOCK_PAYLOAD_SIZE)  /* 928 */

/* Update-mode timing, measured on the bench against the factory bootloader
 * (docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md section 38). */
#define HEADER_PROMPT_MS        7000UL    /* E2 00 every 7.0 s while no E1 arrives */
#define HEADER_GIVE_UP_MS       358000UL  /* then gives up, clears the request, resets */
#define BLOCK_IDLE_MS           19600UL   /* re-request (E2 01 / E4) after 19.6 s of silence */
#define BLOCK_GIVE_UP_MS        314000UL  /* no complete E3 frame for this long: give up.
                                           * Derived (same 0x0FFFFFFF poll budget at the
                                           * measured 19.6 s per 0x00FFFFFF polls), not measured */

/* Flash Management */
void flash_unlock(void);
void flash_lock(void);
bool flash_erase_page(uint32_t page_addr);
bool flash_erase_app_pages(void);
bool flash_write_halfword(uint32_t address, uint16_t data);
bool flash_write_block(uint32_t address, const uint8_t *data, uint32_t length);

/* Hardware Management */
void clock_init(void);
uint32_t millis(void);
void update_gpio_rails_init(void);
void iwdg_init(void);
void iwdg_feed(void);

/* UART IAP Driver */
void uart_init(uint32_t baudrate);
void uart_putc(uint8_t c);
uint8_t uart_getc(void);
bool uart_getc_timeout(uint8_t *out_char, uint32_t timeout_ms);

/* Update Protocol Engine */
bool mcu_process_firmware_update(void);

/* Application Jump & Validation */
bool is_app_valid(void);
void jump_to_application(void);

#endif /* BOOTLOADER_H */
