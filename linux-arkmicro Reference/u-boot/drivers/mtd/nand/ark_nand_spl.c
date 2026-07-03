// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2014 Gateworks Corporation
 * Author: Tim Harvey <tharvey@gateworks.com>
 */
#include <common.h>
#include <malloc.h>
#define NAND_BASE		CONFIG_SYS_NAND_BASE
/* NAND Flash Controller */
#define rNAND_CR									(*(volatile unsigned int  *) (NAND_BASE + 0x00))
#define rNAND_CLE_WR								(*(volatile unsigned char *) (NAND_BASE + 0x04))
#define rNAND_ALE_WR								(*(volatile unsigned char *) (NAND_BASE + 0x08))
#define rNAND_ID_RD									(*(volatile unsigned char *) (NAND_BASE + 0x0c))
#define rNAND_STATUS_RD								(*(volatile unsigned char *) (NAND_BASE + 0x10))
#define rNAND_DATA									(*(volatile unsigned int  *) (NAND_BASE + 0x14))
#define rNAND_TX_FIFO								(*(volatile unsigned int  *) (NAND_BASE + 0x18))
#define rNAND_RX_FIFO								(*(volatile unsigned int  *) (NAND_BASE + 0x1c))
#define rNAND_WRBLK_START							(*(volatile unsigned int  *) (NAND_BASE + 0x20))
#define rNAND_RDBLK_START							(*(volatile unsigned int  *) (NAND_BASE + 0x24))
#define rEX_BCH_ENCODE_STATUS						(*(volatile unsigned int  *) (NAND_BASE + 0x274))
#define rEX_BCH_DECODE_STATUS						(*(volatile unsigned int  *) (NAND_BASE + 0x278))
#define rBCH_CR										(*(volatile unsigned int  *) (NAND_BASE + 0x27c))
#define rBCH_NAND_STATUS							(*(volatile unsigned int  *) (NAND_BASE + 0x280))
#define rBCH_DECODE_STATUS							(*(volatile unsigned int  *) (NAND_BASE + 0x284))
#define rBCH_INT									(*(volatile unsigned int  *) (NAND_BASE + 0x288))
#define rBCH_INT_MASK								(*(volatile unsigned int  *) (NAND_BASE + 0x28c))
#define EX_BCH_DECODE_RESULT_ADDR					(NAND_BASE + 0x29c)
#define NAND_INT_GLOBAL			(1<<3)
#define NAND_INT_DECODE_ERR		(1<<2)
#define NAND_INT_DECODE_END		(1<<1)
#define NAND_INT_ENCODE_END		(1<<0)
//BCH_CR register fields defination
#define BCH_CR_SECTOR_MODE			(1<<8)
#define BCH_CR_ENCODER_RESET		(1<<3)
#define BCH_CR_DECODER_RESET		(1<<2)
#define BCH_CR_SOFT_ECC_ENABLE		(1<<1)
#define BCH_CR_BCH_ENABLE			(1<<0)
#define CMD_ERASE1				0x60
#define CMD_ERASE2				0xD0
#define CMD_READ1				0x00
#define CMD_READ2				0x30
#define CMD_PAGEPROGRAM1		0x80
#define CMD_PAGEPROGRAM2		0x10
#define CMD_READ_ID				0x90
#define CMD_RESET				0xFF
#define CMD_RADOM_OUTPUT1		0x05
#define CMD_RADOM_OUTPUT2		0xE0
#define CMD_RADOM_INPUT			0x85
#define CMD_READ_STATUS			0x70
#define NAND_MFR_TOSHIBA		0x98
#define NAND_MFR_SAMSUNG		0xec
#define NAND_MFR_FUJITSU		0x04
#define NAND_MFR_NATIONAL		0x8f
#define NAND_MFR_RENESAS		0x07
#define NAND_MFR_STMICRO		0x20
#define NAND_MFR_HYNIX			0xad
#define NAND_MFR_MICRO			0x2c
#define NAND_MFR_AMD			0x01
struct nand_maker{
	int id;
	const char *name;
	struct nand_dev *dev_desc;
	int (*match_id)(unsigned char *id);
};
struct nandflash_info {
	const char *name;
	unsigned int id;
	unsigned int id2;
	int pagesize;
	int chipsize;
	int blocksize;
	int oobsize;
	int options;
};
struct nand_info{
	int chipsize;
	int blksize;
	int blknum;
	int pagesize;
	int pagesblk;
	int oobsize;
	int colcycle;
	int rowcycle;
	int bchecccodesize;
	int bchsegsize;
};
struct nand_dev {
	const char *name;
	int id;
	int pagesize;
	int chipsize;
	int blocksize;
};
enum nand_chip_sel {
	NAND_CHIP0 = 0,
	NAND_CHIP1,
	NAND_CHIP2,
	NAND_CHIP3,
};
enum nand_data_width {
	NAND_DATA_WIDTH8 = 0,
	NAND_DATA_WIDTH16,
};
struct nand_dev nand_flash_ids[] = {
//		name					id			pagesize   chipsize		blocksize
	{"NAND 64MiB 3,3V 8-bit",	0x76,		512,		 64,		0x4000},
	{"NAND 128MiB 3,3V 8-bit",	0x79, 		512, 		128, 		0x4000},
	{"NAND 256MiB 3,3V 8-bit",	0x71, 		512, 		256, 		0x4000},
	/*
	 * These are the new chips with large page size. The pagesize and the
	 * blocksize is determined from the extended id bytes
	 */
	/*512 Megabit */
	{"NAND 64MiB 3,3V 8-bit",	0xF2, 		0,  		64, 			0},
	{"NAND 64MiB 3,3V 8-bit",	0xD0, 		0,  		64, 			0},
	/* 1 Gigabit */
	{"NAND 128MiB 3,3V 8-bit",	0xF1, 		0, 			128, 			0},
	{"NAND 128MiB 3,3V 8-bit",	0xD1, 		0, 			128, 			0},
	/* 2 Gigabit */
	{"NAND 256MiB 3,3V 8-bit",	0xDA, 		0, 			256, 			0},
	/* 4 Gigabit */
	{"NAND 512MiB 3,3V 8-bit",	0xDC, 		0, 			512, 			0},
	/* 8 Gigabit */
	{"NAND 1GiB 3,3V 8-bit",	0xD3, 		0, 			1024, 			0},
	/* 16 Gigabit */
	{"NAND 2GiB 3,3V 8-bit",	0xD5, 		0, 			2048, 			0},
	/* 32 Gigabit */
	{"NAND 4GiB 3,3V 8-bit",	0xD7, 		0, 			4096, 			0},
	/* 64 Gigabit */
	{"NAND 8GiB 3,3V 8-bit",	0xDE, 		0, 			8192, 			0},
	/*
	 * Renesas AND 1 Gigabit. Those chips do not support extended id and
	 * have a strange page/block layout !  The chosen minimum blocksize is
	 * 4 * 2 * 2048 = 16384 Byte, as those chips have an array of 4 page
	 * planes 1 block = 2 pages, but due to plane arrangement the blocks
	 * 0-3 consists of page 0 + 4,1 + 5, 2 + 6, 3 + 7 Anyway JFFS2 would
	 * increase the eraseblock size so we chose a combined one which can be
	 * erased in one go There are more speed improvements for reads and
	 * writes possible, but not implemented now
	 */
	{"AND 128MiB 3,3V 8-bit",	0x01, 		2048, 		128, 		0x4000},
	{NULL,},
};
static const struct nandflash_info nandflash_devices[] = {
	{"TC58BVG0S3HTAI0",	0x98, 0xF18015F2, 2048, 128, 128, 64,  0},
	{"K9F1G08U0C",      0xec, 0xf1009540, 2048, 128, 128, 64,  0},
	{"K9F1G08U0E",      0xec, 0xf1009541, 2048, 128, 128, 64,  0},
	{"HY27UF081G2A",    0xad, 0xf1801dad, 2048, 128, 128, 64,  0},
	{"TC58NVG0S3ETA00",	0x98, 0xD1901576, 2048, 128, 128, 64,  0},
	{"TC58NVG0S3HTA00",	0x98, 0xF1801572, 2048, 128, 128, 128, 0},
	{"TC58NVG1S3HTA00",	0x98, 0xDA901576, 2048, 256, 128, 128, 0},
	{"W29N01GV",	    0xEF, 0xF1809500, 2048, 128, 128, 64,  0},
	{"FMND1GXXX3D",	    0xF8, 0xF1809500, 2048, 128, 128, 64,  0},
	{"XT27G02ETSIGA",	0x2C, 0xDA909506, 2048, 256, 128, 128, 0},
	//此款flash芯片不支持，在此处强制将OOB128字节
	{"9FU1G8F2AMGF",	0xC8, 0xF1801D42, 2048, 128, 128, 128, 0},
	{"TC58NVG2S0HTAI0",	0x98, 0xDC902676, 4096, 512, 256, 256, 0},
	{"FMND4G08U3C-IA",	0xF8, 0xDC909546, 2048, 512, 128, 128, 0},
};
static struct nand_info nand_info;
static void nand_reset(void)
{
	rNAND_CLE_WR = CMD_RESET;
	do {
		rNAND_CLE_WR = CMD_READ_STATUS;
		udelay(10);//do not delete this delay avoiding to elapse the time for tWHR
	} while (!(rNAND_STATUS_RD & 0x40));
}
static void nand_readid(unsigned char *id)
{
	int i;
	rNAND_CLE_WR = CMD_READ_ID;
	rNAND_ALE_WR = 0x00;
	udelay(100);
	for(i = 0; i < 8; i++)
		*id++ = rNAND_ID_RD;
}
static int nand_scan_table(unsigned char *flashid)
{
	int i;
	unsigned int id = flashid[0];
	unsigned int id2 = (flashid[1] << 24) | (flashid[2] << 16) | (flashid[3] << 8) | flashid[4];
	printf("Flashid = 0x%x%x\n",id,id2);
	for(i = 0; i < sizeof(nandflash_devices) / sizeof(nandflash_devices[0]); i++) {
		if(id == nandflash_devices[i].id && id2 == nandflash_devices[i].id2) {
			nand_info.blksize = nandflash_devices[i].blocksize * 1024;
			nand_info.chipsize = nandflash_devices[i].chipsize * 1024 * 1024;
			nand_info.pagesize = nandflash_devices[i].pagesize;
			nand_info.oobsize = nandflash_devices[i].oobsize;
			return 0;
		}
	}
	puts("NO Device in Table\n");
	return -1;
}
static int get_cycle(unsigned int addr)
{
	int cycle;
	for (cycle = 0; cycle < 4; cycle++) {
		if(addr & (0xff000000 >> (8 * cycle)))
			return 4-cycle;
	}
	return 0;
}
static void get_samsung_nandinfo(unsigned char *id)
{
	int extid = id[3];
	nand_info.pagesize = 2048 << (extid & 0x03);
	extid >>= 2;
	//K9GBG08U0A ID:0xECD794766443
	switch ((extid & 0x03) | ((extid >> 2) & 0x04)) {
	case 1:
		nand_info.oobsize= 128;
		break;
	case 2:
		nand_info.oobsize = 218;
		break;
	case 3:
		nand_info.oobsize = 400;
		break;
	case 4:
		nand_info.oobsize = 436;
		break;
	default:
		nand_info.oobsize = 640;
		break;
	}
	extid >>= 2;
	/* Calc blocksize */
	nand_info.blksize = (128 * 1024) << (((extid >> 1) & 0x04) | (extid & 0x03));
}
static void get_hynix_nandinfo(unsigned char *id)
{
	int extid = id[3];
	/* Calc pagesize */
	nand_info.pagesize = 2048 << (extid & 0x03);
	extid >>= 2;
	switch ((extid & 0x03) | ((extid>>2) & 0x04)) {
	case 0:
		nand_info.oobsize= 128;
		break;
	case 1:
		nand_info.oobsize = 224;
		break;
	case 2:
		nand_info.oobsize = 448;
		break;
	case 3:
		nand_info.oobsize = 64;
		break;
	case 4:
		nand_info.oobsize = 32;
		break;
	case 5:
		nand_info.oobsize = 16;
		break;
	case 6:
		nand_info.oobsize = 640;
		break;
	}
	extid >>= 2;
	switch(((extid >> 1) & 0x04) | (extid & 0x03)) {
	case 0:
		nand_info.blksize = 128 << 10;
		break;
	case 1:
		nand_info.blksize = 256 << 10;
		break;
	case 2:
		nand_info.blksize = 512 << 10;
		break;
	case 3:
		nand_info.blksize = 768 << 10;
		break;
	case 4:
		nand_info.blksize = 1 << 20;
		break;
	case 5:
		nand_info.blksize = 2 << 20;
		break;
	case 6:
		nand_info.blksize = 4 << 20;
		break;
	}
}
static int nand_readoob(int block, int page, int offset, unsigned int *buf, int size)
{
	int i;
	unsigned int page_addr;
	page_addr = block * nand_info.pagesblk + page;
	rBCH_NAND_STATUS = 0x1;
	if(nand_info.pagesize > 512) {
		rNAND_CLE_WR = CMD_READ1;//0x00;
		rNAND_ALE_WR = offset;
		rNAND_ALE_WR = nand_info.pagesize >> 8;
	} else {
		rNAND_CLE_WR = 0x50;
		rNAND_ALE_WR = offset;
	}
	//send address
	for (i = 0; i < nand_info.rowcycle; i++)
		rNAND_ALE_WR = (page_addr >> (i * 8)) & 0xff;
	if (nand_info.pagesize >= 2048)
		rNAND_CLE_WR = CMD_READ2;
	while(!(rBCH_NAND_STATUS & (1 << 18)));
	for ( i = 0; i < size; i ++)
		*buf++ = rNAND_DATA;
	return 0;
}
static int nand_is_bad_block(int block)
{
	unsigned int val;
	nand_readoob(block, 0, 0, &val, 1);
	if ((val & 0xffff) != 0xffff)
		return 1;
	return 0;
}
static int nand_read_page(int block, int page, void *dst)
{
	unsigned int PhyAddr;
	int i,j;
	unsigned int *data = (unsigned int *)dst;
	unsigned char *buf = (unsigned char*)dst;
	unsigned int ECCData;
	unsigned int BchDecode;
	unsigned char BchDecodeStatus;
	unsigned int ErrByteAddr[2];
	unsigned int ErrBitAddr[2];
	int nBCH_DECODE_REGs = 4, nBCH_ENCODE_REGs = 4;
	unsigned int nMaxBCHSectors;
	int result = 0;
	int nECCCodeSize;
	nECCCodeSize = nand_info.bchecccodesize;
	PhyAddr = block * nand_info.pagesblk + page;
	//clear rb bit status
	rBCH_NAND_STATUS = 0x1; //add by frank
	//send cmd
	rNAND_CLE_WR = CMD_READ1;//0x00;
	//send address
	for (i = 0; i < nand_info.colcycle; i++)
		rNAND_ALE_WR = 0x00;
	for(i = 0; i < nand_info.rowcycle; i++)
		rNAND_ALE_WR = (PhyAddr >> (i*8)) & 0xff;
	if(nand_info.pagesize >= 2048)
		rNAND_CLE_WR = CMD_READ2;
	nMaxBCHSectors = nand_info.pagesize/nand_info.bchsegsize;
	while (!(rBCH_NAND_STATUS & (1 << 18)));
	for(i = 0; i < nMaxBCHSectors; i++) {
		rBCH_CR |= BCH_CR_SECTOR_MODE;
		//enable ecc controller, it will auto caculate the ecc code while data reading in the following code
		rBCH_CR |= BCH_CR_DECODER_RESET;//reset
		rBCH_CR &= ~BCH_CR_DECODER_RESET;
		rBCH_CR |= (BCH_CR_SOFT_ECC_ENABLE|BCH_CR_BCH_ENABLE);
		//read data from fifo with block process
		rNAND_RDBLK_START = 1;
		for (j = 0; j < (nand_info.bchsegsize >> 2); j++)
			*data++ = rNAND_RX_FIFO;
		//wait for data decode end while useing block process
		while(!(rBCH_INT & NAND_INT_DECODE_END));
		rBCH_INT = 1;
		//read ecc data stored in spare area
		rNAND_CLE_WR = CMD_RADOM_OUTPUT1;//0x05;
		for (j = 0; j < nand_info.colcycle; j++)
			rNAND_ALE_WR = ((nand_info.pagesize + nand_info.oobsize - nMaxBCHSectors * nECCCodeSize
								+ i * nECCCodeSize) >> (8 * j)) & 0xff;
		rNAND_CLE_WR = CMD_RADOM_OUTPUT2;//0xE0;
		switch(nECCCodeSize) {
		case 13:
			nBCH_ENCODE_REGs = 4;
			nBCH_DECODE_REGs = 4;//7 BITS
			break;
		case 23:
			nBCH_ENCODE_REGs = 6;
			nBCH_DECODE_REGs = 7;//13 BITS
			break;
		case 42:
			nBCH_ENCODE_REGs = 11;//24 BITS
			nBCH_DECODE_REGs = 12;
			break;
		case 53:
			nBCH_ENCODE_REGs = 14;//30 BITS
			nBCH_DECODE_REGs = 15;
			break;
		case 84:
			nBCH_ENCODE_REGs = 21;//48 BITS
			nBCH_DECODE_REGs = 24;
			break;
		}
		for (j=0; j < nBCH_ENCODE_REGs; j++)
			ECCData = rNAND_DATA;
		ECCData = ECCData; //remove compile warning
		//wait for ecc data read end
		while (rEX_BCH_DECODE_STATUS & 0x01);
		//begin to check data read at the forward step according the ecc decord result just now.
		BchDecode = rEX_BCH_DECODE_STATUS;
		BchDecodeStatus = (char)(BchDecode&0x7);
		if(BchDecodeStatus&0x2) {
			//All Data is right
			;
		} else if(BchDecodeStatus&0x4) {
			//err more than 8 bit
			rBCH_CR &= ~BCH_CR_BCH_ENABLE;
			puts("uncorrectable ECC error\n");
			result = -1;
		} else {
			for(j=0;j<nBCH_DECODE_REGs;j++) {
				unsigned int val;

				val = *(volatile unsigned int*)(EX_BCH_DECODE_RESULT_ADDR + 4 * j);
				ErrByteAddr[0] = (val&0x3ff8)>>3;
				ErrBitAddr[0] = val&0x7;
				val = val >> 14;
				ErrByteAddr[1] = (val&0x3ff8)>>3;
				ErrBitAddr[1] = val&0x7;

				if(ErrByteAddr[0] < 1024)
					*(buf + nand_info.bchsegsize * i + ErrByteAddr[0]) ^= 1 << ErrBitAddr[0];
				if(ErrByteAddr[1] < 1024)
					*(buf + nand_info.bchsegsize * i + ErrByteAddr[1]) ^= 1 << ErrBitAddr[1];
			}
		}
		rBCH_INT = 1;
		//if there are some other sector in this page to read, then begin to send their row address
		if(i<(nMaxBCHSectors-1) && nMaxBCHSectors>1) {
			rNAND_CLE_WR = CMD_RADOM_OUTPUT1;//0x85;
			rNAND_ALE_WR = 0x00;
			rNAND_ALE_WR = ((i + 1) * nand_info.bchsegsize) >> 8;
			rNAND_CLE_WR = 	CMD_RADOM_OUTPUT2;
		}
	}
	rBCH_INT = 1;
	rBCH_CR &= ~BCH_CR_BCH_ENABLE;
	return result;
}
int nand_spl_load_image(uint32_t offs, unsigned int size, void *dst)
{
	unsigned int block, lastblock;
	unsigned int page, page_offset;
	/* offs has to be aligned to a page address! */
	block = offs / nand_info.blksize;
	lastblock = (offs + size - 1) / nand_info.blksize;
	page = (offs % nand_info.blksize) / nand_info.pagesize;
	page_offset = offs % nand_info.pagesize;
	while (block <= lastblock) {
		if (!nand_is_bad_block(block)) {
			/* Skip bad blocks */
			while (page < nand_info.pagesblk) {
				nand_read_page(block, page, dst);
				/*
				 * When offs is not aligned to page address the
				 * extra offset is copied to dst as well. Copy
				 * the image such that its first byte will be
				 * at the dst.
				 */
				if (unlikely(page_offset)) {
					memmove(dst, dst + page_offset,
						nand_info.pagesize);
					dst = (void *)((int)dst - page_offset);
					page_offset = 0;
				}
				dst += nand_info.pagesize;
				page++;
			}
			page = 0;
		} else {
			lastblock++;
		}
		block++;
	}
	return 0;
}
void nand_init(void)
{
	unsigned char flash_id[8];
	int i, num;
	struct nand_dev nand_dev;
	unsigned char deviceid, devicecellid, extid;
	unsigned int val;
	rNAND_CR = 0;
	rNAND_CR = (1<<22)|(1<<21)|(0<<20)|(0x4<<16)|(0x3<<12)|(0x3<<8)|(0x3<<4)|(0x4<<0);
	nand_reset();
	nand_readid(flash_id);
	if(!nand_scan_table(flash_id)) {
		nand_info.blknum = nand_info.chipsize / nand_info.blksize;
		nand_info.pagesblk = nand_info.blksize / nand_info.pagesize;
		if(nand_info.pagesize == 512)
			nand_info.colcycle = 1;
		else
			nand_info.colcycle = get_cycle(nand_info.pagesize);
		nand_info.rowcycle = get_cycle(nand_info.chipsize / nand_info.pagesize - 1);
	} else {
		deviceid = flash_id[1];
		num = sizeof(nand_flash_ids) / sizeof(nand_flash_ids[0]);
		for(i = 0; i < num; i++) {
			if(deviceid ==nand_flash_ids[i].id) {
				nand_dev = nand_flash_ids[i];
				nand_info.chipsize = nand_flash_ids[i].chipsize << 20;
				break;
			}
		}
		/* Unkown Maker */
		if(i == num) {
			puts("Unknown Maker,Not Support The Chip!!\n");
			return;
		}
		if (!nand_dev.pagesize) {
			/* The 3rd id byte holds MLC / multichip data */
			devicecellid = flash_id[2];
			/* The 4th id byte is the important one */
			extid = flash_id[3];
			/*
			 * Field definitions are in the following datasheets:
			 * Old style (4,5 byte ID): Samsung K9GAG08U0M (p.32)
			 * New style (6 byte ID): Samsung K9GBG08U0M   (p.40)
			 *
			 * Check for wraparound + Samsung ID + nonzero 6th byte
			 * to decide what to do.
			 */
			if ((flash_id[0] == flash_id[6]) && (flash_id[1] == flash_id[7]) &&
			//					FlashId[0] == NAND_MFR_SAMSUNG &&
					(devicecellid & 0x0C) &&
					(flash_id[5] != 0x00)) {
				if(flash_id[0] == NAND_MFR_SAMSUNG) {
					get_samsung_nandinfo(flash_id);
					goto nandinfo_over;
				} else if(flash_id[0] == NAND_MFR_HYNIX) {
					get_hynix_nandinfo(flash_id);
					goto nandinfo_over;
				}
			}
			/* Calc pagesize */
			nand_info.pagesize = 1024 << (extid & 0x03);
			extid >>= 2;
			/* Calc oobsize */
			nand_info.oobsize = (8 << (extid & 0x01)) *
				(nand_info.pagesize >> 9);
			extid >>= 2;
			/* Calc blocksize. Blocksize is multiples of 64KiB */
			nand_info.blksize= (64 * 1024) << (extid & 0x03);
		}
		else {
			/*
			 * Old devices have chip data hardcoded in the device id table
			 */
			nand_info.blksize = nand_dev.blocksize;
			nand_info.pagesize = nand_dev.pagesize;
			nand_info.oobsize = nand_info.pagesize / 32;
			/*
			 * Check for Spansion/AMD ID + repeating 5th, 6th byte since
			 * some Spansion chips have blocksize that conflicts with size
			 * listed in nand_ids table
			 * Data sheet (5 byte ID): Spansion S30ML-P ORNAND (p.39)
			 */
			if (flash_id[0] == NAND_MFR_AMD && flash_id[4] != 0x00 &&
					flash_id[5] == 0x00 && flash_id[6] == 0x00 &&
					flash_id[7] == 0x00 && nand_info.pagesize == 512) {
				nand_info.blksize = 128 * 1024;
				nand_info.blksize <<= ((flash_id[3] & 0x03) << 1);
			}
		}
nandinfo_over:
		if(nand_info.pagesize == 512)
			nand_info.colcycle = 1;
		else
			nand_info.colcycle = get_cycle(nand_info.pagesize);
		nand_info.rowcycle = get_cycle(nand_info.chipsize / nand_info.pagesize - 1);
		nand_info.pagesblk = nand_info.blksize / nand_info.pagesize;
		nand_info.blknum = nand_info.chipsize / nand_info.blksize;
	}
	if(nand_info.oobsize == 64) {
		val = rNAND_CR;
		val &= ~(3 << 25);
		rNAND_CR = val;
		nand_info.bchsegsize = 1024;
		nand_info.bchecccodesize = 23;
		val = rBCH_CR;
		val &= ~((1 << 7) | (7 << 4));
		val |= (1 << 7) | (1 << 4);
		rBCH_CR = val;
		debug("ECC 13 bit !!\n");
	} else if(nand_info.oobsize >= 128) {
		val = rNAND_CR;
		val &= ~(3<<25);
		rNAND_CR = val;
		nand_info.bchsegsize = 1024;
		nand_info.bchecccodesize = 42;
		val = rBCH_CR;
		val &= ~((1 << 8) | (1 << 7) | (7 << 4));
		val |= (1 << 7) | (2 << 4);
		rBCH_CR = val;
		debug("ECC 24 bit !!\n");
	}
	return;
}
void nand_deselect(void)
{
}
