#include <common.h>
#include <dm.h>
#include <asm/arch/ark-common.h>
struct SYS_INFO sys_info;
#if 1
//watchdog
//#define PCLK_FREQ  80000000  //APBPLL_CLK/6
#define WDT_BASE    0xe4b00000
#define rWDT_CR											(*(volatile unsigned int *)(WDT_BASE + 0x00))
#define rWDT_PSR										(*(volatile unsigned int *)(WDT_BASE + 0x04))
#define rWDT_LDR										(*(volatile unsigned int *)(WDT_BASE + 0x08))
#define rWDT_VLR										(*(volatile unsigned int *)(WDT_BASE + 0x0C))
#define rWDT_ISR										(*(volatile unsigned int *)(WDT_BASE + 0x10))
#define rWDT_RCR										(*(volatile unsigned int *)(WDT_BASE + 0x14))
#define rWDT_TMR										(*(volatile unsigned int *)(WDT_BASE + 0x18))
#define rWDT_TCR										(*(volatile unsigned int *)(WDT_BASE + 0x1C))
void ark_watchdog_start(int time_ms)
{
	u32 freq = ark_get_apb_clock();
	printf("wdt reset timer %d,freq %d\n",time_ms,freq);
	rWDT_CR = 0;
	rWDT_CR = 0x3 << 4;		/* 128 div */
	rWDT_PSR = 0x400;		//0x100;/* 256 prescale */
	rWDT_LDR = (freq/1000)*time_ms  / 128 / 0x400;
	rWDT_CR |= 3 << 0;
}
void ark_watchdog_stop(void){
	rWDT_CR = 0;
}
#endif
//crc校验从卡或者u盘以及其他设备，用来升级的源数据
int ark_check_data_from_devide(char *file_name,unsigned int crc_data)
{
		char *update_fdt = NULL;
		unsigned int size;
		update_fdt = env_get("boardfdt");
		size = strlen(update_fdt);
		printf("update_fdt:%s,size %d,file_name %s\n", update_fdt,size,file_name);
		if(!strncmp(file_name, "u-boot.img", 10)){
			if(sys_info.uboot_crc != 0){
					if(crc_data != sys_info.uboot_crc)
						return 1;
			}else
				sys_info.uboot_crc = crc_data;
		}
		else if(!strncmp(file_name, "zImage", 6)){
			if(sys_info.zImage_crc != 0){
					if(crc_data != sys_info.zImage_crc)
						return 1;
			}else
				sys_info.zImage_crc = crc_data;
		}
		else if(!strncmp(file_name, "rootfs.ubi", 10)){
			if(sys_info.rootfs_crc != 0){
					if(crc_data != sys_info.rootfs_crc)
						return 1;
			}else
				sys_info.rootfs_crc = crc_data;
		}
		else if(!strncmp(file_name, "bootanimation", 13)){
			if(sys_info.bootanimation_crc != 0){
					if(crc_data != sys_info.bootanimation_crc)
						return 1;
			}else
				sys_info.bootanimation_crc = crc_data;

		}
		else if(!strncmp(file_name, "bootlogo", 8)){
			if(sys_info.bootlogo_crc != 0){
					if(crc_data != sys_info.bootlogo_crc)
						return 1;
			}else
				sys_info.bootlogo_crc = crc_data;
		}
		else if(!strncmp(file_name, update_fdt, size)){
			if(sys_info.fdt_crc != 0){
					if(crc_data != sys_info.fdt_crc)
						return 1;
			}else
				sys_info.fdt_crc = crc_data;
		}
		else if(!strncmp(file_name, "ubootspl.bin", 12)){
			if(sys_info.ubootspl_crc != 0){
					if(crc_data != sys_info.ubootspl_crc)
						return 1;
			}else
				sys_info.ubootspl_crc = crc_data;
		}
		return 0;
}
//crc校验从分区中读取的数据，保证烧写的数据的一致性
int ark_check_data_from_partition(char *part_name,unsigned int crc_data)
{
		if(!strncmp(part_name, "bootloader", 10)){
			if(sys_info.zImage_crc != 0)
					if(crc_data != sys_info.uboot_crc)
						return 1;
		}
		if(!strncmp(part_name, "bootloader_bak", 14)){
			if(sys_info.zImage_crc != 0)
					if(crc_data != sys_info.uboot_crc)
						return 1;
		}
		else if(!strncmp(part_name, "kernel", 6)){
			if(sys_info.zImage_crc != 0)
					if(crc_data != sys_info.zImage_crc)
						return 1;
		}
		else if(!strncmp(part_name, "rootfs", 6)){
			if(sys_info.rootfs_crc != 0)
					if(crc_data != sys_info.rootfs_crc)
						return 1;
		}
		else if(!strncmp(part_name, "fdt", 13)){
			if(sys_info.bootanimation_crc != 0)
					if(crc_data != sys_info.fdt_crc)
						return 1;
		}
		else if(!strncmp(part_name, "bootanimation", 13)){
			if(sys_info.bootanimation_crc != 0)
					if(crc_data != sys_info.bootanimation_crc)
						return 1;
		}
		else if(!strncmp(part_name, "bootstrap", 9)){
			if(sys_info.ubootspl_crc != 0)
					if(crc_data != sys_info.ubootspl_crc)
						return 1;
		}
		else if(!strncmp(part_name, "bootlogo", 8)){
			if(sys_info.bootlogo_crc != 0)
					if(crc_data != sys_info.bootlogo_crc)
						return 1;
		}
		return 0;
}
int get_crc_data_from_device(char *file_name)
{
	int ret = -1;
	unsigned int file_size;
	char cmd[128] = { 0 };
	unsigned int *srcdata = (unsigned int *)(env_get_hex("loadaddr", 0));
	memset(&sys_info, 0, sizeof(sys_info));
	printf("file_name=%s\n", file_name);
	sprintf(cmd, "fatload %s %s %s %s",env_get("update_dev_type"),env_get("update_dev_part"),env_get("loadaddr"), file_name);
	printf("cmd=%s\n", cmd);
	ret = run_command(cmd, 0);
	if(ret)
		return 1;
	file_size = env_get_ulong("filesize", 16, 0x2000);
	memcpy(&sys_info, srcdata, file_size);
//	printf("uboot_crc 		  = 0x%x\n",sys_info.uboot_crc);
//	printf("fdt_crc			  = 0x%x\n",sys_info.fdt_crc);
//	printf("zImage_crc 		  = 0x%x\n",sys_info.zImage_crc);
//	printf("rootfs_crc		  = 0x%x\n",sys_info.rootfs_crc);
//	printf("ubootspl_crc	  = 0x%x\n",sys_info.ubootspl_crc);
//	printf("bootanimation_crc = 0x%x\n",sys_info.bootanimation_crc);
//	printf("bootlogo_crc	  = 0x%x\n",sys_info.bootlogo_crc);
	return 0;
}
int get_data_from_media(char *file_name)
{
	int ret = -1;
	unsigned int file_size,crc_src;
	char cmd[128] = { 0 };
	unsigned int *srcdata = (unsigned int *)(env_get_hex("loadaddr", 0));
	sprintf(cmd, "fatload %s %s %s %s",env_get("update_dev_type"),env_get("update_dev_part"),env_get("loadaddr"), file_name);
	printf("cmd=%s\n", cmd);
	ret = run_command(cmd, 0);
	if(ret)
	{
		printf("Load %s from tf/usb error!!\n",file_name);
		return 1;
	}
	file_size = env_get_ulong("filesize", 16, 0x2000);
	crc_src = crc32(0, (const unsigned char *)srcdata, file_size);
	ret = ark_check_data_from_devide(file_name,crc_src);
	if(ret)
	{
		printf("check data %s from tf/usb crc error!!\n",file_name);
		return 1;
	}
	return 0;
}
