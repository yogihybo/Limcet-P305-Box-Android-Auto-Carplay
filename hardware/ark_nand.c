/*
 * (C) Copyright 2006 DENX Software Engineering
 * (C) Copyright 2012 Ho Ka Leung, ASTRI, kalho@astri.org
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <common.h>
#include <linux/err.h>
#include <nand.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand_ecc.h>
#include <asm/arch/ark-nand.h>
#include <asm/io.h>
#include <errno.h>

#define BIT_7_ECC_BYTE	13
#define BIT_13_ECC_BYTE	23
#define BIT_24_ECC_BYTE	42
#define BIT_30_ECC_BYTE 53
#define BIT_48_ECC_BYTE	84

#define EC809_NAND_ADDR_UNALIGNED	0x1
#define EC809_NAND_LEN_UNALIGNED		0x2

//#define USE_DATA_INTERFACE 1

//#define DEBUG
#if 0
static struct nand_ecclayout nand_hw_eccoob_16 = { /* small page 512 byte with 16 byte oob  */
    .eccbytes = BIT_7_ECC_BYTE,
	.eccpos = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    .oobfree = {}
};

static struct nand_ecclayout nand_hw_eccoob_64 = { /* large page 2k with 64 byte oob */
    .eccbytes = BIT_7_ECC_BYTE,
    .eccpos = { 3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
	   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
	},
    .oobfree = { {32, 32} }
};
#endif

static struct nand_ecclayout nand_hw_eccoob_bootstrap = { /* large page 2k with 64 byte oob */
    .eccbytes = BIT_7_ECC_BYTE,
    .eccpos = { 3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 
	   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
	   30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43,
	   44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54	   	
	},
    .oobfree = { {56, 8} }
};

static struct nand_ecclayout nand_hw_eccoob_64 = { /* large page 2k with 64 byte oob */
    .eccbytes = BIT_13_ECC_BYTE,
    .eccpos = {18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
    	34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
    	50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63
	},
    .oobfree = { {2, 16} }
};

//1024--24
static struct nand_ecclayout nand_hw_eccoob_128 = { /* large page 2k with 64 byte oob */
    .eccbytes = BIT_24_ECC_BYTE,
    .eccpos = {44, 45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58, 59,
    	 60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,
		 76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,
		 92,  93,  94,  95,  96,  97,  98,  99, 100, 101, 102, 103, 104, 105, 106, 107,
		108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123,
		124, 125, 126, 127
	},
    .oobfree = { {2, 42} }
};

//1024--24
static struct nand_ecclayout nand_hw_eccoob_256 = {
    .eccbytes = BIT_24_ECC_BYTE,
    .eccpos = {88,  89,
		90,  91,  92,  93,  94,  95,  96,  97,  98,  99,
		100, 101, 102, 103, 104, 105,106, 107, 108, 109,
		110, 111, 112, 113, 114, 115,116, 117, 118, 119,
		120, 121, 122, 123, 124, 125,126, 127, 128, 129,
		130, 131, 132, 133, 134, 135,136, 137, 138, 139,
		140, 141, 142, 143, 144, 145,146, 147, 148, 149,
		150, 151, 152, 153, 154, 155,156, 157, 158, 159,
		160, 161, 162, 163, 164, 165,166, 167, 168, 169,
		170, 171, 172, 173, 174, 175,176, 177, 178, 179,
		180, 181, 182, 183, 184, 185,186, 187, 188, 189,
		190, 191, 192, 193, 194, 195,196, 197, 198, 199,
		200, 201, 202, 203, 204, 205,206, 207, 208, 209,
		210, 211, 212, 213, 214, 215,216, 217, 218, 219,
		220, 221, 222, 223, 224, 225,226, 227, 228, 229,
		230, 231, 232, 233, 234, 235,236, 237, 238, 239,
		240, 241, 242, 243, 244, 245,246, 247, 248, 249,
		250, 251, 252, 253, 254, 255,
	},
    .oobfree = { {2, 86} }
};



/*static uint8_t bbt_pattern[] = {'B', 'b', 't', '0' };
static uint8_t mirror_pattern[] = {'1', 't', 'b', 'B' };

static struct nand_bbt_descr Ark_OOB128_bbt_main_descr = {
	.options = NAND_BBT_LASTBLOCK | NAND_BBT_CREATE | NAND_BBT_WRITE
		| NAND_BBT_2BIT | NAND_BBT_VERSION | NAND_BBT_PERCHIP,
	.offs =	96,
	.len = 4,
	.veroffs = 100,
	.maxblocks = 4,
	.pattern = bbt_pattern
};

static struct nand_bbt_descr Ark_OOB128_bbt_mirror_descr = {
	.options = NAND_BBT_LASTBLOCK | NAND_BBT_CREATE | NAND_BBT_WRITE
		| NAND_BBT_2BIT | NAND_BBT_VERSION | NAND_BBT_PERCHIP,
	.offs =	96,
	.len = 4,
	.veroffs = 100,
	.maxblocks = 4,
	.pattern = mirror_pattern
};

static struct nand_bbt_descr Ark_OOB64_bbt_main_descr = {
	.options = NAND_BBT_LASTBLOCK | NAND_BBT_CREATE | NAND_BBT_WRITE
		| NAND_BBT_2BIT | NAND_BBT_VERSION | NAND_BBT_PERCHIP,
	.offs = 55,
	.len = 4,
	.veroffs = 59,
	.maxblocks = 4,
	.pattern = bbt_pattern
};

static struct nand_bbt_descr Ark_OOB64_bbt_mirror_descr = {
	.options = NAND_BBT_LASTBLOCK | NAND_BBT_CREATE | NAND_BBT_WRITE
		| NAND_BBT_2BIT | NAND_BBT_VERSION | NAND_BBT_PERCHIP,
	.offs =55,
	.len = 4,
	.veroffs = 59,
	.maxblocks = 4,
	.pattern = mirror_pattern
};*/



struct ark_nand {
	struct mtd_info		mtd;
	struct nand_chip	*nand;

	int 		max_chip;
	int			chip_num;
	int			raw_rw;
};

static struct ark_nand	ark_nand_info = {0};

static void ark_nand_select_chip(struct mtd_info *mtd, int chipnr)
{
	struct nand_chip *chip = mtd->priv;
	struct ark_nand *nand_info = chip->priv;

	unsigned int nand_cr;

	debug("ark_nand_select_chip\n");

	if(chipnr == -1){
		chip->cmd_ctrl(mtd, NAND_CMD_NONE, 0 | NAND_CTRL_CHANGE);
		return;
	}

	if(chipnr < nand_info->max_chip){
		nand_cr = readl(rNAND_CR);
		nand_cr &= ~(0x3<<23);
		nand_cr |= (chipnr<<23);

		writel(nand_cr, rNAND_CR);

		nand_info->chip_num = chipnr;
	}

	return;

}

static void ark_nand_hwcontrol(struct mtd_info *mtd, int cmd, unsigned int ctrl)
{
	struct nand_chip *chip = mtd->priv;

	debug("ark_nand_hwcontrol\n");
	/*
     * Point the IO_ADDR to DATA and ADDRESS registers instead
     * of chip address
     */
    switch (ctrl) {
    case NAND_CTRL_CHANGE | NAND_CTRL_CLE:
		debug("NAND_CTRL_CLE\n");
        chip->IO_ADDR_W = (void  __iomem *)rNAND_CLE_WR;
        break;
    case NAND_CTRL_CHANGE | NAND_CTRL_ALE:
		debug("NAND_CTRL_ALE\n");
        chip->IO_ADDR_W = (void  __iomem *)rNAND_ALE_WR;
        break;
    case NAND_CTRL_CHANGE:
		debug("NAND_CTRL_DATA\n");
        chip->IO_ADDR_W = (void  __iomem *)rNAND_DATA; /* Must be a 32-bit read at ARK1680 NFC */
        break;
    }

	if(ctrl & NAND_NCE){
		;	/* TODO: Chip Enable activate */
	}else{
		;	/* TODO: Chip Enable de-activate */
	}

/*#ifndef USE_DATA_INTERFACE
	if ((cmd == NAND_CMD_READ0 || cmd == NAND_CMD_SEQIN) && (ctrl & NAND_CLE)) {
		writel(1, rBCH_NAND_STATUS);
	}
#endif*/

	if (cmd != NAND_CMD_NONE){
		debug("cmd 0x%X\n",cmd);
        writeb(cmd, chip->IO_ADDR_W);
	}
}

/**
 * ark_nand_read_buf - read chip data into buffer
 * @mtd:        MTD device structure
 * @buf:        buffer to store date
 * @len:        number of bytes to read
 *
 * Default read function for 8bit buswith
 */
 static int rcount = 0;
void ark_nand_read_buf(struct mtd_info *mtd, uint8_t *buf, int len)
{
        int i;
	unsigned int* tmp_buf32;
	uint8_t* tmp_buf8;
	unsigned int unaligned_flag = 0;
	unsigned int excess_len;
	unsigned int addr_aligned;
	unsigned int read_addr = rNAND_DATA;
	struct nand_chip *chip = (struct nand_chip *)mtd->priv;
	struct ark_nand *nand_info = chip->priv;

	union{
        unsigned int word;
        unsigned char byte[4];
    } tmp;

	debug("ark_nand_read_buf\n");
	addr_aligned = ((unsigned int)buf)%4;
	if(addr_aligned){ // Address not aligned to 32 bit word
		debug("EC809_NAND_ADDR_UNALIGNED\n");
		unaligned_flag |= EC809_NAND_ADDR_UNALIGNED;
	}

	excess_len = len % 0x4;
	if(excess_len){ // length not aligned to 32 bit word
		debug("EC809_NAND_LEN_UNALIGNED\n");
		unaligned_flag |= EC809_NAND_LEN_UNALIGNED;
	}

#ifndef USE_DATA_INTERFACE
	if (len >= chip->ecc.size && !nand_info->raw_rw)
		read_addr = rNAND_RX_FIFO;
#endif

	switch(unaligned_flag){
	case 0:
		tmp_buf32 = (unsigned int*)buf;
        for (i = 0; i < len >> 2; i++){
        	*tmp_buf32++ = readl(read_addr);
			debug("0. tmp_buf32 0x%08X\n",*(tmp_buf32-1));

			if(rcount)
				printf("%d  %d  0x%08X\n",len, i, *(tmp_buf32-1));
        }
		break;

	case 1:
		tmp_buf8 = buf;
		for (i = 0; i < len >> 2; i++){
            tmp.word = readl(read_addr);
			debug("0. tmp.word 0x%08X\n",tmp.word);
			*tmp_buf8++ = tmp.byte[0];
			*tmp_buf8++ = tmp.byte[1];
			*tmp_buf8++ = tmp.byte[2];
			*tmp_buf8++ = tmp.byte[3];
        }
		break;

	case 2:
		tmp_buf32 = (unsigned int*)buf;
		for (i = 0; i < len >> 2; i++){
        	*tmp_buf32++ = readl(read_addr);
			debug("1. tmp_buf32 0x%08X\n",*(tmp_buf32-1));
		}

		/* read the excess byte */
		tmp_buf8 = (uint8_t*)tmp_buf32;
		tmp.word = readl(read_addr);
		debug("1. tmp.word 0x%08X\n",tmp.word);
		for(i = 0; i < excess_len; i++){
			*tmp_buf8++ = tmp.byte[i];
		}

		break;

	case 3:
		tmp_buf8 = buf;
        for (i = 0; i < len >> 2; i++){
            tmp.word = readl(read_addr);
			debug("2. tmp.word 0x%08X\n",tmp.word);
                *tmp_buf8++ = tmp.byte[0];
                *tmp_buf8++ = tmp.byte[1];
                *tmp_buf8++ = tmp.byte[2];
                *tmp_buf8++ = tmp.byte[3];
        }
		tmp.word = readl(read_addr);
		debug("3. tmp.word 0x%08X\n",tmp.word);
        for(i = 0; i < excess_len; i++){
            *tmp_buf8++ = tmp.byte[i];
        }
		break;
	}

}

/**
 * ark_nand_write_buf - write buffer to chip
 * @mtd:        MTD device structure
 * @buf:        data buffer
 * @len:        number of bytes to write
 *
 * Default write function for 8bit buswith
 */
void ark_nand_write_buf(struct mtd_info *mtd, const uint8_t *buf, int len)
{
    int i;
    unsigned int* tmp_buf32;
    uint8_t* tmp_buf8;
    unsigned int unaligned_flag = 0;
    unsigned int excess_len;
	unsigned int addr_aligned;
	unsigned int write_addr = rNAND_DATA;
	struct nand_chip *chip = (struct nand_chip *)mtd->priv;
	struct ark_nand *nand_info = chip->priv;

    union{
        unsigned int word;
        unsigned char byte[4];
    } tmp;

	debug("ark_nand_write_buf\n");

	tmp.word = 0xFFFFFFFF;
	addr_aligned = (unsigned int)buf % 4;
    if(addr_aligned){ // Address not aligned to 32 bit word
    	unaligned_flag |= EC809_NAND_ADDR_UNALIGNED;
    }

    excess_len = len % 0x4;
    if(excess_len){ // length not aligned to 32 bit word
    	unaligned_flag |= EC809_NAND_LEN_UNALIGNED;
    }

#ifndef USE_DATA_INTERFACE
	if (len >= chip->ecc.size && !nand_info->raw_rw)
		write_addr = rNAND_TX_FIFO;
#endif

    switch(unaligned_flag){
    case 0:
		tmp_buf32 = (unsigned int*)buf;
        for (i = 0; i < len >> 2; i++){
			debug("0 rNAND_DATA = 0x%08X\n",*tmp_buf32);
            writel(*tmp_buf32++, write_addr);
		}
		break;

    case 1:
        tmp_buf8 = (uint8_t*)buf;
        for (i = 0; i < len >> 2; i++){
			tmp.byte[0] = *tmp_buf8++;
			tmp.byte[1] = *tmp_buf8++;
			tmp.byte[2] = *tmp_buf8++;
			tmp.byte[3] = *tmp_buf8++;
			debug("1 rNAND_DATA = 0x%08X\n",tmp.word);
			writel(tmp.word, write_addr);
        }
        break;

    case 2:
     	tmp_buf32 = (unsigned int*)buf;
        for (i = 0; i < len >> 2; i++){
			debug("2 rNAND_DATA = 0x%08X\n",*tmp_buf32);
			writel(*tmp_buf32++, write_addr);
		}
        /* write the excess byte */
        tmp_buf8 = (uint8_t*)tmp_buf32;
		tmp.word = 0xFFFFFFFF;
        for(i = 0; i < excess_len; i++){
        	*tmp_buf8++ = tmp.byte[i];
        }
		debug("2.1 rNAND_DATA = 0x%08X\n",tmp.word);
		writel(tmp.word, write_addr);
        break;
	case 3:
        tmp_buf8 = (uint8_t*)buf;
        for (i = 0; i < len >> 2; i++){
			tmp.byte[0] = *tmp_buf8++;
            tmp.byte[1] = *tmp_buf8++;
            tmp.byte[2] = *tmp_buf8++;
            tmp.byte[3] = *tmp_buf8++;
			debug("3 rNAND_DATA = 0x%08X\n",tmp.word);
            writel(tmp.word, write_addr);
        }
		tmp.word = 0xFFFFFFFF;
        for(i = 0; i < excess_len; i++){
        	*tmp_buf8++ = tmp.byte[i];
        }
		debug("3.1 rNAND_DATA = 0x%08X\n",tmp.word);
		writel(tmp.word, write_addr);
        break;
    }
}

/**
 * ark_nand_verify_buf - Verify chip data against buffer
 * @mtd:        MTD device structure
 * @buf:        buffer containing the data to compare
 * @len:        number of bytes to compare
 *
 * Default verify function for 8bit buswith
 */
/*static int ark_nand_verify_buf(struct mtd_info *mtd, const uint8_t *buf, int len)
{
    int i,j;
    const uint8_t* tmp_buf8;
    unsigned int excess_len;

    union{
        unsigned int word;
        unsigned char byte[4];
    } tmp;

	debug("ark_nand_verify_buf\n");

	tmp_buf8 = buf;
    excess_len = len % 0x4;

	for (i = 0; i < len >> 2; i++){
		tmp.word = readl(rNAND_DATA);
		for (j = 0; j < 4; j++){
			if(tmp.byte[j] != *tmp_buf8++)
				return -EFAULT;
		}
	}

	if(excess_len){
		tmp.word = readl(rNAND_DATA);
		for(i = 0; i < excess_len; i++){
			if(tmp.byte[i] != *tmp_buf8++)
				return -EFAULT;
		}
	}

	return 0;
}*/

static void ark_nand_enable_hwecc(struct mtd_info *mtd, int mode)
{
	debug("ark_nand_enable_hwecc\n");
#if 0
	switch(mode){
		case NAND_ECC_READ:
			writel(readl(rBCH_CR) | BCH_CR_SECTOR_MODE,
                               rBCH_CR);
			writel(readl(rBCH_CR) | BCH_CR_DECODER_RESET,
                               rBCH_CR);
			writel(readl(rBCH_CR) & ~BCH_CR_DECODER_RESET,
                               rBCH_CR);
			writel(readl(rBCH_CR) | BCH_CR_BCH_ENABLE,
                               rBCH_CR);
			printf("\r\n>>>>>NAND_ECC_READ rBCH_CR 0x%08X\n",readl(rBCH_CR));
			break;

		case NAND_ECC_WRITE:
			writel(readl(rBCH_CR) | BCH_CR_SECTOR_MODE,
                               rBCH_CR);
                        writel(readl(rBCH_CR) | BCH_CR_ENCODER_RESET,
                               rBCH_CR);
                        writel(readl(rBCH_CR) & ~BCH_CR_ENCODER_RESET,
                               rBCH_CR);
			writel(readl(rBCH_CR) | (BCH_CR_SOFT_ECC_ENABLE|BCH_CR_BCH_ENABLE),
                               rBCH_CR);
			printf("\r\n>>>>>sNAND_ECC_WRITE rBCH_CR 0x%08X\n",readl(rBCH_CR));
			break;
#else
	switch(mode){
		case NAND_ECC_READ:
			writel(readl(rBCH_CR) | BCH_CR_SECTOR_MODE,
                               rBCH_CR);
			writel(readl(rBCH_CR) | BCH_CR_DECODER_RESET,
                               rBCH_CR);
			writel(readl(rBCH_CR) & ~BCH_CR_DECODER_RESET,
                               rBCH_CR);
			writel(readl(rBCH_CR) | (BCH_CR_SOFT_ECC_ENABLE|BCH_CR_BCH_ENABLE),
                               rBCH_CR);
			debug("\r\n>>>>>NAND_ECC_READ rBCH_CR 0x%08X\n",readl(rBCH_CR));
#ifndef USE_DATA_INTERFACE
			writel(1, rNAND_RDBLK_START);
#endif
			break;

		case NAND_ECC_WRITE:
			writel(readl(rBCH_CR) | BCH_CR_SECTOR_MODE,
                               rBCH_CR);
            writel(readl(rBCH_CR) | BCH_CR_ENCODER_RESET,
                               rBCH_CR);
            writel(readl(rBCH_CR) & ~BCH_CR_ENCODER_RESET,
                               rBCH_CR);
			writel(readl(rBCH_CR) | (BCH_CR_SOFT_ECC_ENABLE|BCH_CR_BCH_ENABLE),
                               rBCH_CR);
			debug("\r\n>>>>>sNAND_ECC_WRITE rBCH_CR 0x%08X\n",readl(rBCH_CR));
#ifndef USE_DATA_INTERFACE
			writel(1, rNAND_WRBLK_START);
#endif
			break;


#endif

		case NAND_ECC_READSYN:
#ifndef USE_DATA_INTERFACE
            //wait for data decode end while useing block process
            while(!(readl(rBCH_INT) & NAND_INT_DECODE_END));
			debug("NAND_ECC_READSYN rBCH_INT 0x%08X\n",readl(rBCH_INT));
            writel(1, rBCH_INT);
#endif
			debug("\r\n>>>>NAND_ECC_READSYN rBCH_CR 0x%08X\n",readl(rBCH_CR));
			break;

	}
}

#if 0  //512---7bit

#else //1024---7bit
static int ark_nand_calculate_ecc(struct mtd_info *mtd, const u_char *dat,
                                    u_char *ecc_code)
{
	struct nand_chip *chip = mtd->priv;
	unsigned int bch_encode_no = 0;
	int i;

	union{
		unsigned int word;
		unsigned char byte[4];
	} tmp;

	debug("ark_nand_calculate_ecc\n");
#ifndef USE_DATA_INTERFACE
	//wait for data encode end with block process
    while(!(readl(rBCH_INT) & NAND_INT_ENCODE_END));
	debug("rBCH_INT 0x%08X\n",readl(rBCH_INT));
    writel(1, rBCH_INT);
#endif

	writel(readl(rBCH_CR) & ~BCH_CR_BCH_ENABLE,
               rBCH_CR);  //disable BCH for write ECC Code

#if 1
	debug("rBCH_NAND_STATUS 0x%08X : 0x%x\n", rBCH_NAND_STATUS, readl(rBCH_NAND_STATUS));
#ifdef USE_DATA_INTERFACE
        while((readl(rBCH_NAND_STATUS) & 0x3F) != 0 ); //wait for fsm to idle state
#else
        while((readl(rBCH_NAND_STATUS) >> 14) & 0x0F); //wait for all data in fifo to be transfered over.
        while((readl(rBCH_NAND_STATUS) & 0x3F)  != 0 );//wait for fsm to idle state
#endif
        debug("wait rBCH_NAND_STATUS done\n");
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
		tmp.word = readl(EX_BCH_ENCODE_RESULT_ADDR + i*4);
		*ecc_code++ = tmp.byte[0];
		*ecc_code++ = tmp.byte[1];
		*ecc_code++ = tmp.byte[2];
		*ecc_code++ = tmp.byte[3];
		debug("EX_BCH_ENCODE_RESULT_ADDR %d: 0x%08X\n", i, tmp.word);
	}

	return 0;
}

static int ark_nand_correct_data(struct mtd_info *mtd, u_char *dat,
                                   u_char *read_ecc, u_char *calc_ecc)
{
	struct nand_chip *chip = mtd->priv;

	unsigned int bch_decode_no = 0;
	unsigned char bch_decode_status;
	unsigned int err_byte_addr[2];
    unsigned int err_bit_addr[2];
	int i = 0, ret = 0;

	debug("ark_nand_correct_data\n");

	switch(chip->ecc.bytes){
    case BIT_7_ECC_BYTE:
        bch_decode_no = 4; /* no. of loops to correct 2 bits per loop   */
        break;
    case BIT_13_ECC_BYTE:
        bch_decode_no = 7;
        break;
    case BIT_24_ECC_BYTE:
        bch_decode_no = 12;
        break;
    case BIT_30_ECC_BYTE:
        bch_decode_no = 15;
        break;
    case BIT_48_ECC_BYTE:
        bch_decode_no = 24;
        break;
	}

	//wait for ecc data read end
	while(readl(rEX_BCH_DECODE_STATUS)&0x01);

#ifdef DEBUG
	debug("ECC code:");

	for(i = 0; i<chip->ecc.bytes; i++)
		debug("0x%X ",*read_ecc++);

	debug("\n");
#endif


/*
	printf("\r\nECC code:");

	for(i = 0; i<chip->ecc.bytes; i++)
		printf("0x%x ",*read_ecc++);

	temp=readl(rEX_BCH_DECODE_STATUS);
	printf("\n!! status:0x%x\n",temp);

	temp=readl(rBCH_CR);
	printf("\n!!rBCH_CR status:0x%x\n",temp);
*/


	bch_decode_status = (unsigned char)(readl(rEX_BCH_DECODE_STATUS) & 0x7);

	if(bch_decode_status & 0x2)
	{    //All Data is right

		debug("\nAll data are correct\n");
		//continue;
	}
	else if(bch_decode_status & 0x4)
	{   //err more than 8 bit
		writel(readl(rBCH_CR) & ~BCH_CR_BCH_ENABLE,
                       rBCH_CR);
		printf("\n!!Read Data err more than 8 bit and Group = %d status:0x%x\n",i,bch_decode_status);
		return -EBADMSG;
	}
	else
	{
		for(i=0; i<bch_decode_no; i++){
			unsigned int val;
			val = readl(EX_BCH_DECODE_RESULT_ADDR+4*i);

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
	debug("Bit error no.: %d\n",ret);
    return ret;
}



#endif

/**
 * ark_nand_read_page_raw_syndrome - [Intern] read raw page data without ecc
 * @mtd:        mtd info structure
 * @chip:       nand chip info structure
 * @buf:        buffer to store read data
 * @page:       page number to read
 *
 * We need a special oob layout and handling even when OOB isn't used.
 */
static int ark_nand_read_page_raw_syndrome(struct mtd_info *mtd,
                                             struct nand_chip *chip,
                                             uint8_t *buf, int oob_required, int page)
{
	struct ark_nand *nand_info = chip->priv;

	debug("ark_nand_read_page_raw_syndrome\n");
	debug("page %d, mtd->writesize %d\n",page, mtd->writesize);
	nand_info->raw_rw = 1;
    chip->read_buf(mtd, buf, mtd->writesize);
    chip->read_buf(mtd, chip->oob_poi, mtd->oobsize);
	nand_info->raw_rw = 0;

    return 0;
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
	int i, eccsize = chip->ecc.size;
	int eccbytes = chip->ecc.bytes;
	int eccsteps = chip->ecc.steps;
	uint8_t *p = buf;
	uint8_t *oob = chip->oob_poi;
	int oob_pos = 0, pos = 0;

	debug("ark_nand_read_page_syndrome\n");
	oob_pos += mtd->writesize;

    if(chip->ecc.prepad){
        oob_pos += chip->ecc.prepad;
		oob += chip->ecc.prepad;
	}

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

		if (stat < 0) {
			mtd->ecc_stats.failed++;
		} else {
			mtd->ecc_stats.corrected += stat;
		}

		if(eccsteps > 1)
		{
			pos += eccsize;
			if (mtd->writesize > 512)
			{
	        	chip->cmdfunc(mtd, NAND_CMD_RNDOUT, pos, -1);
	        }
			else
			{
	        	chip->cmdfunc(mtd, NAND_CMD_READ0, pos, page);
	        }
		}
    }

	writel(1,rBCH_INT);
    writel(readl(rBCH_CR) & ~BCH_CR_BCH_ENABLE, rBCH_CR);

	/* the whole oob area if there is prepad or postpad area */
    if (chip->ecc.prepad){
		oob_pos = mtd->writesize;
        if (mtd->writesize > 512){
        	chip->cmdfunc(mtd, NAND_CMD_RNDOUT, oob_pos, -1);
        }else{
        	chip->cmdfunc(mtd, NAND_CMD_READ0, oob_pos, page);
        }
        chip->read_buf(mtd, chip->oob_poi, chip->ecc.prepad);
	}

	if (chip->ecc.postpad){
		oob_pos = mtd->writesize + chip->ecc.prepad + (chip->ecc.steps * chip->ecc.bytes);
	    if (mtd->writesize > 512){
	    	chip->cmdfunc(mtd, NAND_CMD_RNDOUT, oob_pos, -1);
	    }else{
	    	chip->cmdfunc(mtd, NAND_CMD_READ0, oob_pos, page);
	    }
	    chip->read_buf(mtd, (chip->oob_poi + chip->ecc.prepad + (chip->ecc.steps * chip->ecc.bytes)),
		chip->ecc.postpad);
 	}

	return 0;
}

/**
 * ark_nand_read_oob_syndrome - OOB data read function
 * @mtd:        mtd info structure
 * @chip:       nand chip info structure
 * @page:       page number to read
 * @sndcmd:     flag whether to issue read command or not
 */
static int ark_nand_read_oob_syndrome(struct mtd_info *mtd, struct nand_chip *chip,
                             int page)
{
	debug("ark_nand_read_oob_syndrome\n");
    chip->cmdfunc(mtd, NAND_CMD_READOOB, 0, page);
    chip->read_buf(mtd, chip->oob_poi, mtd->oobsize);

    return 0;
}


/**
 * ark_nand_write_oob_syndrome - OOB data write function for HW ECC
 *                           with syndrome - only for large page flash !
 * @mtd:        mtd info structure
 * @chip:       nand chip info structure
 * @page:       page number to write
 */
static int ark_nand_write_oob_syndrome(struct mtd_info *mtd,
                                   struct nand_chip *chip, int page)
{
    int status = 0;
    const uint8_t *buf = chip->oob_poi;
    int length = mtd->oobsize;
	debug("ark_nand_write_oob_syndrome\n");

    chip->cmdfunc(mtd, NAND_CMD_SEQIN, mtd->writesize, page);
    chip->write_buf(mtd, buf, length);
    /* Send command to program the OOB data */
    chip->cmdfunc(mtd, NAND_CMD_PAGEPROG, -1, -1);

    status = chip->waitfunc(mtd, chip);

    return status & NAND_STATUS_FAIL ? -EIO : 0;
}



/**
 * ark_nand_write_page_raw_syndrome - raw page write function
 * @mtd:        mtd info structure
 * @chip:       nand chip info structure
 * @buf:        data buffer
 *
 * We need a special oob layout and handling even when ECC isn't checked.
 */
static int ark_nand_write_page_raw_syndrome(struct mtd_info *mtd,
                                        struct nand_chip *chip,
                                        const uint8_t *buf,
                                        int oob_required, int page)
{
	struct ark_nand *nand_info = chip->priv;

	debug("ark_nand_write_page_raw_syndrome\n");
	nand_info->raw_rw = 1;
	chip->write_buf(mtd, buf, mtd->writesize);
    chip->write_buf(mtd, chip->oob_poi, mtd->oobsize);
	nand_info->raw_rw = 0;

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
    int i, eccsize = chip->ecc.size;
    int eccbytes = chip->ecc.bytes;
    int eccsteps = chip->ecc.steps;
    const uint8_t *p = buf;
    uint8_t *oob = chip->oob_poi;

	debug("ark_nand_write_page_syndrome\n");
	debug("eccsize %d, eccbytes %d, eccsteps %d\n", eccsize, eccbytes, eccsteps);
	debug("rBCH_NAND_STATUS 0x%x\n", readl(rBCH_NAND_STATUS));

	if(chip->ecc.prepad){
		oob += chip->ecc.prepad;
	}

    for (i = 0; eccsteps; eccsteps--, i += eccbytes, p += eccsize) {
        chip->ecc.hwctl(mtd, NAND_ECC_WRITE);
        chip->write_buf(mtd, p, eccsize);
        chip->ecc.calculate(mtd, p, oob);
		oob += eccbytes;
    }

	debug("ark_nand: write ecc\n");
	chip->write_buf(mtd, chip->oob_poi, mtd->oobsize);

	return 0;
}

static int ark_nand_hw_init(struct nand_chip *chip)
{
	unsigned int val;
	debug("ark_nand_hw_init\n");

	writel(0, rNAND_CR);
	val =0;

#ifdef CONFIG_ARK1668EFAMILY
	val =(NAND_PAGE_TYPE(0) )|NAND_SEL_CHIP(0)| NAND_PRO_WRITE|NAND_CE_ENABLE |
		NAND_WR_SETPASS |NAND_RD_TRP_NUM(4) |NAND_RD_TREH_NUM(3)|
		NAND_WR_HOLD_NUM(3)|NAND_WR_WP_NUM(3)|NAND_WR_SET_NUM(4);
#else
	val =(NAND_PAGE_TYPE(0) )|NAND_SEL_CHIP(0)| NAND_PRO_WRITE|NAND_CE_ENABLE |
		NAND_WR_SETPASS |NAND_RD_TRP_NUM(2) |NAND_RD_TREH_NUM(1)|
		NAND_WR_HOLD_NUM(3)|NAND_WR_WP_NUM(3)|NAND_WR_SET_NUM(4);
#endif
    
	/* val =(NAND_PAGE_TYPE(0) )|NAND_SEL_CHIP(0)| NAND_PRO_WRITE|NAND_CE_ENABLE |
		NAND_WR_SETPASS |NAND_RD_TRP_NUM(2) |NAND_RD_TREH_NUM(2)|
		NAND_WR_HOLD_NUM(2)|NAND_WR_WP_NUM(2)|NAND_WR_SET_NUM(2); */

	writel(val,rNAND_CR);

	return 0;
}

static int ark_dev_ready(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd->priv;
	struct ark_nand *nand_info = chip->priv;

	//return readl(rBCH_NAND_STATUS) & (1 << (18 + nand_info->chip_num));
	return readl(rBCH_NAND_STATUS) & (1 << (6 + nand_info->chip_num));
}

static int ark_hwecc_nand_init_param(struct nand_chip *chip, struct mtd_info *mtd)
{
	u32 val;

	chip->ecc.mode = NAND_ECC_HW_SYNDROME;
	chip->ecc.calculate = ark_nand_calculate_ecc;
    chip->ecc.hwctl = ark_nand_enable_hwecc;
    chip->ecc.correct = ark_nand_correct_data;
	chip->ecc.read_page = ark_nand_read_page_syndrome;
	chip->ecc.read_page_raw = ark_nand_read_page_raw_syndrome;
	chip->ecc.read_oob = ark_nand_read_oob_syndrome;
	chip->ecc.write_page = ark_nand_write_page_syndrome;
	chip->ecc.write_page_raw = ark_nand_write_page_raw_syndrome;
	chip->ecc.write_oob = ark_nand_write_oob_syndrome;
	chip->ecc.size = 512;

	if (mtd->oobsize == 64) {
		chip->ecc.bytes = BIT_7_ECC_BYTE;
		chip->ecc.strength = 7;
		chip->ecc.layout = &nand_hw_eccoob_bootstrap;
		chip->ecc.prepad = nand_hw_eccoob_bootstrap.eccpos[0];
		chip->ecc.postpad = nand_hw_eccoob_bootstrap.oobfree[0].offset;
		val = readl(rBCH_CR);
		val &= ~((1 << 7) | (0x7 << 4));
		val |= (1 << 8) | (1 << 0);
		writel(val, rBCH_CR);
	} else if(mtd->oobsize >= 128 && mtd->oobsize < 256) {
		chip->ecc.bytes = BIT_24_ECC_BYTE; // should be 13 bytes but the ECC encoder register is in 32 bit word
		chip->ecc.strength = 24;
		chip->ecc.layout = &nand_hw_eccoob_128;
		chip->ecc.prepad = nand_hw_eccoob_128.eccpos[0];
		chip->ecc.postpad = nand_hw_eccoob_128.oobfree[0].offset;
		val = readl(rBCH_CR);
		val &= ~(0x7 << 4);
		val |= (1 << 8) | (1 << 7) | (2 << 4) | (1 << 0);
		writel(val, rBCH_CR);
	} else if(mtd->oobsize >= 256) {
		chip->ecc.bytes = BIT_24_ECC_BYTE;
		chip->ecc.strength = 24;
		chip->ecc.layout = &nand_hw_eccoob_256;
		chip->ecc.prepad = nand_hw_eccoob_256.eccpos[0];
		chip->ecc.postpad = nand_hw_eccoob_256.oobfree[0].offset;
		val = readl(rBCH_CR);
		val &= ~(0x7 << 4);
		val |= (1 << 8) | (1 << 7) | (2 << 4) | (1 << 0);
		writel(val, rBCH_CR);
	 }
	return 0;
}

static int ark_nand_init(struct nand_chip *chip, int devnum)
{
	struct mtd_info *mtd;
	struct ark_nand *nand_info = &ark_nand_info;
	int ret;

	ark_nand_hw_init(chip);

	debug("board_nand_init\n");

	mtd = nand_to_mtd(chip);
	mtd->priv = chip;
	chip->priv = nand_info;

	nand_info->max_chip = 4;
	nand_info->chip_num = 0;

	/* 5 us command delay time */
    chip->chip_delay = 100;

	chip->IO_ADDR_R = (void __iomem *)rNAND_STATUS_RD;
	chip->IO_ADDR_W = (void __iomem *)rNAND_DATA;

#ifdef CONFIG_SYS_NAND_USE_FLASH_BBT
    chip->bbt_options |= NAND_BBT_USE_FLASH | NAND_BBT_NO_OOB;
#endif
	chip->options |= NAND_NO_SUBPAGE_WRITE;//refer kernel
	chip->cmd_ctrl = ark_nand_hwcontrol;
    chip->select_chip = ark_nand_select_chip;
    chip->write_buf = ark_nand_write_buf;
    chip->read_buf = ark_nand_read_buf;
	chip->dev_ready = ark_dev_ready;
#if 0 /* TODO: it needs to be fixed due to the 32 bit width of the data read write register */
        nand->verify_buf = ark_nand_verify_buf;
#endif

	ret = nand_scan_ident(mtd, CONFIG_SYS_NAND_MAX_CHIPS, NULL);
	if (ret)
		return ret;

	ret = ark_hwecc_nand_init_param(chip, mtd);
	if (ret)
		return ret;

	ret = nand_scan_tail(mtd);
	if (!ret)
		nand_register(devnum, mtd);

	return ret;
}


static struct nand_chip nand_chip[CONFIG_SYS_MAX_NAND_DEVICE];

void board_nand_init(void)
{
	struct nand_chip *nand = &nand_chip[0];

	if (ark_nand_init(nand, 0))
		puts("ARK NAND init failed\n");
}
static int do_switchecc(cmd_tbl_t *cmdtp, int flag, int argc,
		       char * const argv[])
{
	struct nand_chip *chip = &nand_chip[0];
	struct mtd_info *mtd = nand_to_mtd(chip);
	ulong bootecc;
	unsigned int val;
	
	if (argc >= 2) {
		bootecc = simple_strtoul(argv[1], NULL, 16);
		if (bootecc) {
			chip->ecc.size = 512;
			chip->ecc.steps = mtd->writesize / chip->ecc.size;
			chip->ecc.bytes = BIT_7_ECC_BYTE; // should be 13 bytes but the ECC encoder register is in 32 bit word
			chip->ecc.strength = 7;
			chip->ecc.layout = &nand_hw_eccoob_bootstrap;
			chip->ecc.prepad = nand_hw_eccoob_bootstrap.eccpos[0];
			chip->ecc.postpad = nand_hw_eccoob_bootstrap.oobfree[0].offset;
			val = readl(rBCH_CR);
			val &= ~((1 << 7) | (0x7 << 4));
			val |= (1 << 8) | (1 << 0);
			writel(val, rBCH_CR);
		} else {
			chip->ecc.size = 1024;
			chip->ecc.steps = mtd->writesize / chip->ecc.size;
			if (mtd->oobsize == 64) {
				chip->ecc.bytes = BIT_13_ECC_BYTE; // should be 13 bytes but the ECC encoder register is in 32 bit word
				chip->ecc.strength = 13;
				chip->ecc.layout = &nand_hw_eccoob_64;
				chip->ecc.prepad = nand_hw_eccoob_64.eccpos[0];
				chip->ecc.postpad = nand_hw_eccoob_64.oobfree[0].offset;
				val = readl(rBCH_CR);
				val &= ~(0x7 << 4);
				val |= (1 << 8) | (1 << 7) | (1 << 4) | (1 << 0);
				writel(val, rBCH_CR);
			} else if(mtd->oobsize >= 128 && mtd->oobsize <256) {
				chip->ecc.bytes = BIT_24_ECC_BYTE; // should be 13 bytes but the ECC encoder register is in 32 bit word
				chip->ecc.strength = 24;
				chip->ecc.layout = &nand_hw_eccoob_128;
				chip->ecc.prepad = nand_hw_eccoob_128.eccpos[0];
				chip->ecc.postpad = nand_hw_eccoob_128.oobfree[0].offset;
				val = readl(rBCH_CR);
				val &= ~(0x7 << 4);
				val |= (1 << 8) | (1 << 7) | (2 << 4) | (1 << 0);
				writel(val, rBCH_CR);
			} else if(mtd->oobsize >= 256) {
				chip->ecc.bytes = BIT_24_ECC_BYTE;
				chip->ecc.strength = 24;
				chip->ecc.layout = &nand_hw_eccoob_256;
				chip->ecc.prepad = nand_hw_eccoob_256.eccpos[0];
				chip->ecc.postpad = nand_hw_eccoob_256.oobfree[0].offset;
				val = readl(rBCH_CR);
				val &= ~(0x7 << 4);
				val |= (1 << 8) | (1 << 7) | (2 << 4) | (1 << 0);
				writel(val, rBCH_CR);
			}
		}
	}

	return 0;
}

U_BOOT_CMD(switchecc, 4, 1, do_switchecc,
	"switch or restore nand ecc to/from bootstrap setting",
	"1:to bootstrap ecc; 0:restore to normal ecc"
);

