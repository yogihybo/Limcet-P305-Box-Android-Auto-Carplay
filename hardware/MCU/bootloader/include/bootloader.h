#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include "stm32f105.h"

#define APP_FLASH_BASE          0x08004000UL
#define APP_FLASH_END           0x0801FFFFUL
#define FLASH_PAGE_SIZE         2048UL

#define BOOTLOADER_MAGIC_ADDR   ((volatile uint32_t *)0x20004004UL)
#define BOOTLOADER_MAGIC_VAL    0x5555AAAAUL

/* YMODEM Protocol Constants */
#define SOH                     0x01
#define STX                     0x02
#define EOT                     0x04
#define ACK                     0x06
#define NAK                     0x15
#define CANCEL                  0x18
#define CRC16                   0x43  /* 'C' */

/* Flash Management */
void flash_unlock(void);
void flash_lock(void);
bool flash_erase_app_pages(void);
bool flash_write_page(uint32_t address, const uint8_t *data, uint32_t length);

/* UART IAP Driver */
void uart_init(uint32_t baudrate);
void uart_putc(uint8_t c);
uint8_t uart_getc(void);
bool uart_getc_timeout(uint8_t *out_char, uint32_t timeout_ms);

/* YMODEM Receiver */
bool ymodem_receive_and_flash(void);

/* Application Jump */
bool is_app_valid(void);
void jump_to_application(void);

#endif /* BOOTLOADER_H */
