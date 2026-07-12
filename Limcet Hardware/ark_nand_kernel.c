/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/mtd/partitions.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/reset.h>

/* NAND Flash Controller */
#define rNAND_CR                                (0x00)
#define rNAND_CLE_WR                            (0x04)
#define rNAND_ALE_WR                            (0x08)
#define rNAND_ID_RD                             (0x0c)
#define rNAND_STATUS_RD                         (0x10)
#define rNAND_DATA                              (0x14)
#define rNAND_TX_FIFO                           (0x18)
#define rNAND_RX_FIFO                           (0x1c)
#define rNAND_WRBLK_START                       (0x20)
#define rNAND_RDBLK_START                       (0x24)

#define rEX_BCH_ENCODE_STATUS                   (0x274)
#define rEX_BCH_DECODE_STATUS                   (0x278)
#define rBCH_CR                                 (0x27c)
#define rBCH_NAND_STATUS                        (0x280)
#define rBCH_DECODE_STATUS                      (0x284)
#define rBCH_INT                                (0x288)
#define rBCH_INT_MASK                           (0x28c)
#define rNAND_DMA_CTRL                          (0x290)
#define rNAND_GLOBAL_CTL                        (0x294)
#define rNAND_JUMP_CTL                          (0x298)


#define EX_BCH_ENCODE_RESULT_ADDR       (0x1d0)
#define EX_BCH_DECODE_RESULT_ADDR       (0x29c)

#define NAND_INT_GLOBAL                 (1<<3)
#define NAND_INT_DECODE_ERR             (1<<2)
#define NAND_INT_DECODE_END             (1<<1)
#define NAND_INT_ENCODE_END             (1<<0)

//BCH_CR register fields defination
#define BCH_CR_SECTOR_MODE				(1<<8)
#define BCH_CR_SECTOR_LENGTH  			(1<<7)
#define BCH_CR_ENCODER_RESET				(1<<3)
#define BCH_CR_DECODER_RESET				(1<<2)
#define BCH_CR_SOFT_ECC_ENABLE			(1<<1)
#define BCH_CR_BCH_ENABLE				(1<<0)

#define BCH_7BIT_SEL  	 	0x0
#define BCH_13BIT_SEL   	 	0x1
#define BCH_24BIT_SEL   	 	0x2
#define BCH_30BIT_SEL   	 	0x3
#define BCH_36BIT_SEL  	 	0x4

#define BCH_BIT_SEL(x)		((x)<<4)

#define BIT_7_ECC_BYTE		13
#define BIT_13_ECC_BYTE		23
#define BIT_24_ECC_BYTE		42
#define BIT_30_ECC_BYTE		53
#define BIT_48_ECC_BYTE		84

#define ARK_NAND_MAX_CHIPS		1

#define ARK_NAND_ADDR_UNALIGNED		0x1
#define ARK_NAND_LEN_UNALIGNED		0x2

//#define USE_DATA_INTERFACE		1

struct ark_nfc {
	struct nand_controller		controller;
	struct device *dev;
	void __iomem *regs;
	struct reset_control *reset;
	unsigned long assigned_cs;
	unsigned long clk_rate;
	int max_chips;
	int chip_num;
	int raw_rw;
	struct nand_chip nand;
	struct completion complete;
};

static inline struct ark_nfc *to_ark_nfc(struct nand_chip *nand)
{
	return container_of(nand, struct ark_nfc, nand);
}

static irqreturn_t ark_nfc_interrupt(int irq, void *dev_id)
{

	return IRQ_HANDLED;
}

static void ark_nand_enable_hwecc(struct mtd_info *mtd, int mode)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct ark_nfc *nfc = to_ark_nfc(chip);

	//printk("ark_nand_enable_hwecc\n");

    switch(mode){
    case NAND_ECC_READ:
        writel(readl(nfc->regs + rBCH_CR) | BCH_CR_SECTOR_MODE,
               nfc->regs + rBCH_CR);
        writel(readl(nfc->regs + rBCH_CR) | BCH_CR_DECODER_RESET,
               nfc->regs + rBCH_CR);
        writel(readl(nfc->regs + rBCH_CR) & ~BCH_CR_DECODER_RESET,
               nfc->regs + rBCH_CR);
        writel(readl(nfc->regs + rBCH_CR) | (BCH_CR_SOFT_ECC_ENABLE|BCH_CR_BCH_ENABLE),
               nfc->regs + rBCH_CR);

#ifndef USE_DATA_INTERFACE
        //spin_lock(&nfc->lock);   
        //nfc->bch_int_status = 0;
        //spin_unlock(&nfc->lock);
        writel(1, nfc->regs + rNAND_RDBLK_START);
#endif
        break;

	case NAND_ECC_WRITE:
        writel(readl(nfc->regs + rBCH_CR) | BCH_CR_SECTOR_MODE,
               nfc->regs + rBCH_CR);
        writel(readl(nfc->regs + rBCH_CR) | BCH_CR_ENCODER_RESET,
               nfc->regs + rBCH_CR);
        writel(readl(nfc->regs + rBCH_CR) & ~BCH_CR_ENCODER_RESET,
               nfc->regs + rBCH_CR);
        writel(readl(nfc->regs + rBCH_CR) | (BCH_CR_SOFT_ECC_ENABLE|BCH_CR_BCH_ENABLE),
               nfc->regs + rBCH_CR);
#ifndef USE_DATA_INTERFACE
		//spin_lock(&nfc->lock);
		//nfc->bch_int_status = 0;
		//spin_unlock(&nfc->lock);
		writel(1, nfc->regs + rNAND_WRBLK_START);
#endif
        break;

    case NAND_ECC_READSYN:
#ifndef USE_DATA_INTERFACE
        /* wait for data decode end while useing block process */
		//wait_event(nfc->irq_waitq, nfc->bch_int_status & NAND_INT_DECODE_END);
		while(!(readl(nfc->regs + rBCH_INT) & NAND_INT_DECODE_END));
		writel(1, nfc->regs + rBCH_INT);
#endif
		break;
    }

	return;
}


static int ark_nand_calculate_ecc(struct mtd_info *mtd, const uint8_t *dat,
                                    uint8_t *ecc_code)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct ark_nfc *nfc = to_ark_nfc(chip);

	unsigned int bch_encode_no = 0;
	int i;

	union{
		unsigned int word;
		unsigned char byte[4];
        } tmp;


	//printk("ark_nand_calculate_ecc\n");
#ifndef USE_DATA_INTERFACE
	//wait_event(nfc->irq_waitq, info->bch_int_status & NAND_INT_ENCODE_END);
	while(!(readl(nfc->regs + rBCH_INT) & NAND_INT_ENCODE_END));
	writel(1, nfc->regs + rBCH_INT);
#endif

	writel(readl(nfc->regs + rBCH_CR) & ~BCH_CR_BCH_ENABLE,
		nfc->regs + rBCH_CR);  //disable BCH for write ECC Code

#ifdef USE_DATA_INTERFACE
	while((readl(nfc->regs + rBCH_NAND_STATUS) & 0x3F) != 0 ); //wait for fsm to idle state
#else
	while((readl(nfc->regs + rBCH_NAND_STATUS) >> 14) & 0x0F); //wait for all data in fifo to be transfered over.
	while((readl(nfc->regs + rBCH_NAND_STATUS) & 0x3F)  != 0 );//wait for fsm to idle state
#endif

	switch(chip->ecc.bytes){
	case BIT_7_ECC_BYTE:
		bch_encode_no = 4;
		break;
	case BIT_13_ECC_BYTE:
		bch_encode_no = 6;
		break;
	case BIT_24_ECC_BYTE:
		bch_encode_no = 11;
		break;
	case BIT_30_ECC_BYTE:
		bch_encode_no = 14;
		break;
	case BIT_48_ECC_BYTE:
		bch_encode_no = 21;
		break;
	}

	for(i = 0; i < bch_encode_no; i++){
		tmp.word = readl(nfc->regs + (EX_BCH_ENCODE_RESULT_ADDR + i*4));
		*ecc_code++ = tmp.byte[0];
		*ecc_code++ = tmp.byte[1];
		*ecc_code++ = tmp.byte[2];
		*ecc_code++ = tmp.byte[3];
	}

	return 0;
}

static int ark_nand_correct_data(struct mtd_info *mtd, uint8_t *dat, uint8_t *read_ecc,
				  uint8_t *calc_ecc)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct ark_nfc *nfc = to_ark_nfc(chip);

	unsigned int bch_encode_no = 0;
	unsigned int bch_decode_no = 0;
	unsigned char bch_decode_status;
	unsigned int err_byte_addr[2];
	unsigned int err_bit_addr[2];
	int i, ret = 0;

	//printk("ark_nand_correct_data\n");

	switch(chip->ecc.bytes){
	case BIT_7_ECC_BYTE:
		bch_encode_no = 4;
		bch_decode_no = 4; /* no. of loops to correct 2 bits per loop */
		break;
	case BIT_13_ECC_BYTE:
		bch_encode_no = 6;
		bch_decode_no = 7;
		break;
	case BIT_24_ECC_BYTE:
		bch_encode_no = 11;
		bch_decode_no = 12;
		break;
	case BIT_30_ECC_BYTE:
		bch_encode_no = 14;
		bch_decode_no = 15;
		break;
	case BIT_48_ECC_BYTE:
		bch_encode_no = 21;
		bch_decode_no = 24;
		break;
	}

	//wait for ecc data read end
	while(readl(nfc->regs + rEX_BCH_DECODE_STATUS) & 0x01);

	bch_decode_status = (unsigned char)(readl(nfc->regs + rEX_BCH_DECODE_STATUS) & 0x7);
	if(bch_decode_status & 0x2){
		/* All data correct */
	}else if(bch_decode_status & 0x4){   //err more than 8 bit
		writel(readl(nfc->regs + rBCH_CR) & ~BCH_CR_BCH_ENABLE,
			nfc->regs + rBCH_CR);
		pr_err("%s: uncorrectable ECC error\n", __func__);
		return -EBADMSG;
	}else{
		for(i=0; i<bch_decode_no; i++){
			unsigned int val;
			val = readl(nfc->regs + (EX_BCH_DECODE_RESULT_ADDR+4*i));

			err_byte_addr[0] = (val&0x3ff8)>>3;
			err_bit_addr[0] = val&0x7;

			val = val >> 14;

			err_byte_addr[1] = (val&0x3ff8)>>3;
			err_bit_addr[1] = val&0x7;

			if(err_byte_addr[0] < 1024){
				(*(dat+err_byte_addr[0]) )^= (1<<err_bit_addr[0]);
				ret++;
			}

			if(err_byte_addr[1] < 1024){
				(*(dat+err_byte_addr[1]) )^= (1<<err_bit_addr[1]);
				ret++;
			}
		}

	}

	return ret;
}

/**
 * ark_nand_read_page_syndrome - hardware ecc syndrom based page read
 * @mtd:        mtd info structure
 * @chip:       nand chip info structure
 * @buf:        buffer to store read data
 * @page:       page number to read
 *
 */
static int ark_nand_read_page_syndrome(struct mtd_info *mtd, struct nand_chip *chip,
				       uint8_t *buf, int oob_required, int page)
{
	struct ark_nfc *nfc = to_ark_nfc(chip);
	struct mtd_oob_region oobregion;
	int i, eccsize = chip->ecc.size;
	int eccbytes = chip->ecc.bytes;
	int eccsteps = chip->ecc.steps;
	uint8_t *p = buf;
	uint8_t *oob = chip->oob_poi;
	int oob_pos = 0, pos = 0;

	//printk("ark_nand_read_page_syndrome\n");
	chip->cmdfunc(mtd, NAND_CMD_READ0, 0x00, page);

	oob_pos += mtd->writesize;

	mtd_ooblayout_ecc(mtd, 0, &oobregion);
	oob_pos += oobregion.offset;
	oob += oobregion.offset;

	//printk("ark_nand_read_page_syndrome oobregion.offset = %d\n",oobregion.offset);
	
	//writel(1,nfc->regs + rBCH_NAND_STATUS);

	for (i = 0; eccsteps; eccsteps--, i += eccbytes, p += eccsize) {
		int stat;

		chip->ecc.hwctl(mtd, NAND_ECC_READ);
		chip->read_buf(mtd, p, eccsize);

		chip->ecc.hwctl(mtd, NAND_ECC_READSYN);

		if (mtd->writesize > 512){
			chip->cmdfunc(mtd, NAND_CMD_RNDOUT, oob_pos, -1);
		}else{
			chip->cmdfunc(mtd, NAND_CMD_READ0, oob_pos, page);
		}
		oob_pos += eccbytes;
		chip->read_buf(mtd, oob, eccbytes);
		stat = chip->ecc.correct(mtd, p, oob, NULL);
		oob += eccbytes;

		if(eccsteps > 1){
			pos += eccsize;
			if (mtd->writesize > 512){
				chip->cmdfunc(mtd, NAND_CMD_RNDOUT, pos, -1);
			}else{
				chip->cmdfunc(mtd, NAND_CMD_READ0, pos, page);
			}
		}
	}

	writel(1, nfc->regs + rBCH_INT);
	writel(readl(nfc->regs + rBCH_CR) & ~BCH_CR_BCH_ENABLE, nfc->regs + rBCH_CR);

	if (oob_required){
		oob_pos = mtd->writesize;
		if (mtd->writesize > 512){
			chip->cmdfunc(mtd, NAND_CMD_RNDOUT, oob_pos, -1);
		}else{
			chip->cmdfunc(mtd, NAND_CMD_READ0, oob_pos, page);
		}

		chip->read_buf(mtd, chip->oob_poi, mtd->oobsize);
	}

	return 0;
}

/**
 * ark_nand_write_page_syndrome - hardware ecc syndrom based page write
 * @mtd:        mtd info structure
 * @chip:       nand chip info structure
 * @buf:        data buffer
 *
 * The hw generator calculates the error syndrome automatically. Therefor
 * we need a special oob layout and handling.
 */
static int ark_nand_write_page_syndrome(struct mtd_info *mtd,
						struct nand_chip *chip, const uint8_t *buf,
						int oob_required, int page)
{
	struct mtd_oob_region oobregion;
	int i, eccsize = chip->ecc.size;
	int eccbytes = chip->ecc.bytes;
	int eccsteps = chip->ecc.steps;
	const uint8_t *p = buf;
	uint8_t *oob = chip->oob_poi;
	int status;

	//printk("ark_nand_write_page_syndrome\n");
	chip->cmdfunc(mtd, NAND_CMD_SEQIN, 0x00, page);

	mtd_ooblayout_ecc(mtd, 0, &oobregion);
	oob += oobregion.offset;

	for (i = 0; eccsteps; eccsteps--, i += eccbytes, p += eccsize) {
		chip->ecc.hwctl(mtd, NAND_ECC_WRITE);
		chip->write_buf(mtd, p, eccsize);
		chip->ecc.calculate(mtd, p, oob);
		oob += eccbytes;
	}

	chip->write_buf(mtd, chip->oob_poi, mtd->oobsize);

	chip->cmdfunc(mtd, NAND_CMD_PAGEPROG, -1, -1);

	status = chip->waitfunc(mtd, chip);
	if (status & NAND_STATUS_FAIL)
		return -EIO;

	return 0;
}

/*
 * Real on-flash format for this chip (kernel/rootfs/bootloader-type
 * partitions), confirmed on real hardware via U-Boot nandoobcheck/dump.oob
 * (see docs/HANDOFF_nand_ecc_uboot_vs_kernel.md): 1024-byte ECC step (2
 * segments per 2048-byte page), 13-byte/7-bit BCH strength, ECC bytes
 * starting at OOB offset 3. Neither the vendor's original driver (which
 * used 1024-byte steps but 23-byte/13-bit BCH) nor generic
 * nand_ooblayout_lp_ops (which places ECC at the end of OOB) match this.
 */
static int ark_nand_ooblayout_ecc(struct mtd_info *mtd, int section,
				   struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);

	if (section >= chip->ecc.steps)
		return -ERANGE;

	oobregion->offset = 3 + (section * chip->ecc.bytes);
	oobregion->length = chip->ecc.bytes;

	return 0;
}

static int ark_nand_ooblayout_free(struct mtd_info *mtd, int section,
				    struct mtd_oob_region *oobregion)
{
	if (section)
		return -ERANGE;

	oobregion->offset = 32;
	oobregion->length = 32;

	return 0;
}

static const struct mtd_ooblayout_ops ark_nand_ooblayout_2seg13b_ops = {
	.ecc = ark_nand_ooblayout_ecc,
	.free = ark_nand_ooblayout_free,
};

static int ark_nand_hw_syndrome_ecc_ctrl_init(struct mtd_info *mtd, struct nand_ecc_ctrl *ecc)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct ark_nfc *nfc = to_ark_nfc(chip);
	u32 val;

	ecc->calculate = ark_nand_calculate_ecc;
    ecc->hwctl = ark_nand_enable_hwecc;
    ecc->correct = ark_nand_correct_data;
	ecc->read_page = ark_nand_read_page_syndrome;
	ecc->write_page = ark_nand_write_page_syndrome;

	if (mtd->oobsize == 64) {
		ecc->size = 1024;
		ecc->bytes = BIT_7_ECC_BYTE;
		ecc->strength = 7;
		val = BCH_CR_SECTOR_MODE | BCH_CR_SECTOR_LENGTH | BCH_BIT_SEL(BCH_7BIT_SEL) | BCH_CR_BCH_ENABLE;
		writel(val, nfc->regs + rBCH_CR);
		mtd_set_ooblayout(mtd, &ark_nand_ooblayout_2seg13b_ops);
	} else if(mtd->oobsize >= 128) {
		ecc->size = 512;
		ecc->bytes = BIT_24_ECC_BYTE;
		ecc->strength = 24;
		val = BCH_CR_SECTOR_MODE | BCH_CR_SECTOR_LENGTH | BCH_BIT_SEL(BCH_24BIT_SEL) | BCH_CR_BCH_ENABLE;
		writel(val, nfc->regs + rBCH_CR);
		mtd_set_ooblayout(mtd, &nand_ooblayout_lp_ops);
	}

	return 0;
}

static int ark_nand_ecc_init(struct mtd_info *mtd, struct nand_ecc_ctrl *ecc)
{
	int ret;

	switch (ecc->mode) {
	case NAND_ECC_HW_SYNDROME:
		ret = ark_nand_hw_syndrome_ecc_ctrl_init(mtd, ecc);
		if (ret)
			return ret;
		break;
	case NAND_ECC_HW:
	case NAND_ECC_NONE:
	case NAND_ECC_SOFT:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void ark_nfc_select_chip(struct mtd_info *mtd, int chipnr)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct ark_nfc *nfc = to_ark_nfc(chip);

	unsigned int nand_cr;

	//printk("ark_nand_select_chip\n");

	if(chipnr == -1){
		chip->cmd_ctrl(mtd, NAND_CMD_NONE, 0 | NAND_CTRL_CHANGE);
		return;
	}

	if(chipnr < nfc->max_chips){
		nand_cr = readl(nfc->regs + rNAND_CR);
		nand_cr &= ~(0x3<<23);
		nand_cr |= (chipnr<<23);
		writel(nand_cr, nfc->regs + rNAND_CR);
		nfc->chip_num = chipnr;
	}

	return;

}

static void ark_nfc_cmd_ctrl(struct mtd_info *mtd, int cmd, unsigned int ctrl)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct ark_nfc *nfc = to_ark_nfc(chip);

	//printk("ark_nand_hwcontrol\n");

	/*
	 * Point the IO_ADDR to DATA and ADDRESS registers instead
	 * of chip address
	 */
	switch (ctrl) {
	case NAND_CTRL_CHANGE | NAND_CTRL_CLE:
		//printk("CLE\n");
		chip->IO_ADDR_W = (void  __iomem *)(nfc->regs + rNAND_CLE_WR);
		break;

	case NAND_CTRL_CHANGE | NAND_CTRL_ALE:
		//printk("ALE\n");
		chip->IO_ADDR_W = (void  __iomem *)(nfc->regs + rNAND_ALE_WR);
		break;

	case NAND_CTRL_CHANGE:
		/* Must be a 32-bit write at ARK1680 NFC */
		//printk("CHANGE\n");
		chip->IO_ADDR_W = (void  __iomem *)(nfc->regs + rNAND_DATA);
		break;
	}

	if(ctrl & NAND_NCE){
		;	/* TODO: Chip Enable activate */
	}else{
		;	/* TODO: Chip Enable de-activate */
	}

	if (cmd != NAND_CMD_NONE){
		//printk("cmd: 0x%x\n",cmd);
		writeb(cmd, chip->IO_ADDR_W);
	}

	return;
}

static int ark_nfc_dev_ready(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct ark_nfc *nfc = to_ark_nfc(chip);

	//return readl(rBCH_NAND_STATUS) & (1 << (18 + nand_info->chip_num));
	return readl(nfc->regs + rBCH_NAND_STATUS) & (1 << (6 + nfc->chip_num));
}

static void ark_nfc_read_buf(struct mtd_info *mtd, uint8_t *buf, int len)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct ark_nfc *nfc = to_ark_nfc(chip);

	int i;
	unsigned int* tmp_buf32;
	uint8_t* tmp_buf8;
	unsigned int unaligned_flag = 0;
	unsigned int excess_len;
	unsigned int addr_aligned;
	unsigned int read_addr = rNAND_DATA;

	union{
		unsigned int word;
		unsigned char byte[4];
	} tmp;

	//printk("ark_nand_read_buf\n");

	addr_aligned = ((unsigned int)buf)%4;

	if(addr_aligned){ // Address not aligned to 32 bit word
		unaligned_flag |= ARK_NAND_ADDR_UNALIGNED;
	}

	excess_len = len%4;
	if(excess_len){ // length not aligned to 32 bit word
		unaligned_flag |= ARK_NAND_LEN_UNALIGNED;
	}

#ifndef USE_DATA_INTERFACE
	if (len >= chip->ecc.size && !nfc->raw_rw)
		read_addr = rNAND_RX_FIFO;
#endif

	switch(unaligned_flag){
	case 0:
		tmp_buf32 = (unsigned int*)buf;
		for (i = 0; i < len >> 2; i++){
			*tmp_buf32++ = readl(nfc->regs + read_addr);
		}
		break;
	case ARK_NAND_ADDR_UNALIGNED:
		tmp_buf8 = buf;
		for (i = 0; i < len >> 2; i++){
			tmp.word = readl(nfc->regs + read_addr);
			*tmp_buf8++ = tmp.byte[0];
			*tmp_buf8++ = tmp.byte[1];
			*tmp_buf8++ = tmp.byte[2];
			*tmp_buf8++ = tmp.byte[3];
		}
		break;

	case ARK_NAND_LEN_UNALIGNED:
		tmp_buf32 = (unsigned int*)buf;
		for (i = 0; i < len >> 2; i++){
			*tmp_buf32++ = readl(nfc->regs + read_addr);
		}

		/* read the excess byte */
		tmp_buf8 = (uint8_t*)tmp_buf32;
		tmp.word = readl(nfc->regs + read_addr);
		for(i = 0; i < excess_len; i++){
			*tmp_buf8++ = tmp.byte[i];
		}
		break;

	case ARK_NAND_ADDR_UNALIGNED | ARK_NAND_LEN_UNALIGNED:
		tmp_buf8 = buf;
		for (i = 0; i < len >> 2; i++){
			tmp.word = readl(nfc->regs + read_addr);
			*tmp_buf8++ = tmp.byte[0];
			*tmp_buf8++ = tmp.byte[1];
			*tmp_buf8++ = tmp.byte[2];
			*tmp_buf8++ = tmp.byte[3];
		}

		/* read the excess byte */
		tmp.word = readl(nfc->regs + read_addr);
		for(i = 0; i < excess_len; i++){
			*tmp_buf8++ = tmp.byte[i];
		}
		break;
	}

	return;
}


static void ark_nfc_write_buf(struct mtd_info *mtd, const uint8_t *buf, int len)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct ark_nfc *nfc = to_ark_nfc(chip);

	int i;
	unsigned int* tmp_buf32;
	uint8_t* tmp_buf8;
	unsigned int unaligned_flag = 0;
	unsigned int excess_len;
	unsigned int addr_aligned;
	unsigned int write_addr = rNAND_DATA;

	union{
		unsigned int word;
		unsigned char byte[4];
	} tmp;

	//printk("ark_nand_write_buf\n");

	tmp.word = 0xFFFFFFFF;

	addr_aligned = (unsigned int)buf % 4;
	if(addr_aligned){ // Address not aligned to 32 bit word
		unaligned_flag |= ARK_NAND_ADDR_UNALIGNED;
	}

	excess_len = len % 0x4;
	if(excess_len){ // length not aligned to 32 bit word
		unaligned_flag |= ARK_NAND_LEN_UNALIGNED;
	}

#ifndef USE_DATA_INTERFACE
	if (len >= chip->ecc.size && !nfc->raw_rw)
		write_addr = rNAND_TX_FIFO;
#endif


	switch(unaligned_flag){
	case 0:
        tmp_buf32 = (unsigned int*)buf;
        for (i = 0; i < len >> 2; i++){
        	writel(*tmp_buf32++, nfc->regs + write_addr);
        }
        break;

    case ARK_NAND_ADDR_UNALIGNED:
        tmp_buf8 = (uint8_t*)buf;
        for (i = 0; i < len >> 2; i++){
	        tmp.byte[0] = *tmp_buf8++;
	        tmp.byte[1] = *tmp_buf8++;
	        tmp.byte[2] = *tmp_buf8++;
	        tmp.byte[3] = *tmp_buf8++;
	        writel(tmp.word, nfc->regs + write_addr);
        }
        break;

    case ARK_NAND_LEN_UNALIGNED:
        tmp_buf32 = (unsigned int*)buf;
        for (i = 0; i < len >> 2; i++){
        	writel(*tmp_buf32++, nfc->regs + write_addr);
        }
        /* write the excess byte */
        tmp_buf8 = (uint8_t*)tmp_buf32;
        tmp.word = 0xFFFFFFFF;
        for(i = 0; i < excess_len; i++){
        	*tmp_buf8++ = tmp.byte[i];
        }
        writel(tmp.word, nfc->regs + write_addr);
        break;

    case ARK_NAND_ADDR_UNALIGNED | ARK_NAND_LEN_UNALIGNED:
        tmp_buf8 = (uint8_t*)buf;
        for (i = 0; i < len >> 2; i++){
	        tmp.byte[0] = *tmp_buf8++;
	        tmp.byte[1] = *tmp_buf8++;
	        tmp.byte[2] = *tmp_buf8++;
	        tmp.byte[3] = *tmp_buf8++;
	        writel(tmp.word, nfc->regs + write_addr);
        }
        tmp.word = 0xFFFFFFFF;
        for(i = 0; i < excess_len; i++){
        	*tmp_buf8++ = tmp.byte[i];
        }
        writel(tmp.word, nfc->regs + write_addr);
        break;
    }

	return;
}

static int ark_nand_chip_init(struct device *dev, struct ark_nfc *nfc)
{
	struct mtd_info *mtd;
	struct nand_chip *nand;
	int ret;

	nand = &nfc->nand;

	nand->chip_delay = 100;
	nand->controller = &nfc->controller;
	/*
	 * Set the ECC mode to the default value in case nothing is specified
	 * in the DT.
	 */
	nand_set_flash_node(nand, dev->of_node);

	nand->IO_ADDR_R = nfc->regs + rNAND_STATUS_RD;
	nand->IO_ADDR_W = nfc->regs + rNAND_DATA;

	nand->select_chip = ark_nfc_select_chip;
	nand->dev_ready = ark_nfc_dev_ready;
	nand->cmd_ctrl = ark_nfc_cmd_ctrl;
	nand->read_buf = ark_nfc_read_buf;
	nand->write_buf = ark_nfc_write_buf;

	mtd = nand_to_mtd(nand);
	mtd->name = "ark-nand";
	mtd->dev.parent = dev;

	ret = nand_scan(nand, ARK_NAND_MAX_CHIPS);
	if (ret)
		return ret;

	ret = mtd_device_register(mtd, NULL, 0);
	if (ret) {
		dev_err(dev, "failed to register mtd device: %d\n", ret);
		nand_release(nand);
		return ret;
	}

	return 0;
}

static void ark_nand_chips_cleanup(struct ark_nfc *nfc)
{
	nand_release(&nfc->nand);
}

static int ark_nand_attach_chip(struct nand_chip *nand)
{
	struct mtd_info *mtd = nand_to_mtd(nand);
	struct ark_nfc *nfc = container_of(nand, struct ark_nfc, nand);
	int ret;

	if (nand->bbt_options & NAND_BBT_USE_FLASH)
		nand->bbt_options |= NAND_BBT_NO_OOB;

	nand->options |= NAND_NO_SUBPAGE_WRITE;

	ret = ark_nand_ecc_init(mtd, &nand->ecc);
	if (ret) {
		dev_err(nfc->dev, "ECC init failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct nand_controller_ops ark_nand_controller_ops = {
	.attach_chip = ark_nand_attach_chip,
};

static int ark_nfc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ark_nfc *nfc;
	struct device_node *np = dev->of_node;
	int irq;
	int ret;
	u32 val;

	nfc = devm_kzalloc(dev, sizeof(*nfc), GFP_KERNEL);
	if (!nfc)
		return -ENOMEM;

	nfc->dev = dev;
	nand_controller_init(&nfc->controller);
	nfc->controller.ops = &ark_nand_controller_ops;

	nfc->raw_rw = 0;
	nfc->chip_num = 0;
	nfc->max_chips = 1;
	if (!of_property_read_u32(np, "max-chips", &val)) {
		nfc->max_chips = val;
	}

	nfc->regs = of_iomap(np, 0);
	if (IS_ERR(nfc->regs))
		return PTR_ERR(nfc->regs);

	irq = of_irq_get(np, 0);
	if (irq < 0) {
		dev_err(dev, "failed to retrieve irq\n");
		return irq;
	}

	ret = devm_request_irq(dev, irq, ark_nfc_interrupt,
			       0, "ark-nand", nfc);
	if (ret) {
		dev_err(dev, "failed to request irq %d.\n", irq);
		return ret;
	}

	platform_set_drvdata(pdev, nfc);

	ret = ark_nand_chip_init(dev, nfc);
	if (ret) {
		dev_err(dev, "failed to init nand chips\n");
		return ret;
	}

	return 0;
}

static int ark_nfc_remove(struct platform_device *pdev)
{
	struct ark_nfc *nfc = platform_get_drvdata(pdev);

	ark_nand_chips_cleanup(nfc);

	return 0;
}

static const struct of_device_id ark_nfc_ids[] = {
	{ .compatible = "arkmicro,ark-nand" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ark_nfc_ids);

static struct platform_driver ark_nfc_driver = {
	.driver = {
		.name = "ark_nand",
		.of_match_table = ark_nfc_ids,
	},
	.probe = ark_nfc_probe,
	.remove = ark_nfc_remove,
};
module_platform_driver(ark_nfc_driver);

MODULE_AUTHOR("Sim");
MODULE_DESCRIPTION("Arkmicro NAND Flash Controller driver");
MODULE_LICENSE("GPL v2");
