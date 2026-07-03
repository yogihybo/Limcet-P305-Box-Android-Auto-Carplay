#ifndef __ASM_ARCH_ARK_COMMON_H
#define __ASM_ARCH_ARK_COMMON_H
void debug_send_string(const char * buf);
int ddr3_data_training(int ba);
void ddr3_sdramc_init(void);
void mem_init(void);
int mcu_serial_init(void);
void mcu_serial_putc(const char c);
void mcu_serial_puts(const char *s);
int mcu_serial_getc(void);
void mcu_serial_send(const unsigned char *buf, int len);
int mcu_serial_receive(unsigned char *buf, int len);
int mcu_serial_read(unsigned char *buf);
int early_printf(const char *fmt, ...);
typedef struct SYS_INFO {
	unsigned int  uboot_crc;
	unsigned int  fdt_crc;
	unsigned int  zImage_crc;
	unsigned int  rootfs_crc;
	unsigned int  bootanimation_crc;
	unsigned int  ubootspl_crc;
	unsigned int  bootlogo_crc;
	unsigned int  reserve[9];
} SYS_INFO;
#endif
