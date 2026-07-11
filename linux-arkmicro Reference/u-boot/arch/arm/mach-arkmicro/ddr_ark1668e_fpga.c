#include <common.h>

#define DDR3_REG_BASE  0xE9100000

#define rDDR_MCCR   	*((volatile unsigned int *)(DDR3_REG_BASE + 0x00))
#define rDDR_MCSR   	*((volatile unsigned int *)(DDR3_REG_BASE + 0x04))
#define rDDR_MRSVR0   	*((volatile unsigned int *)(DDR3_REG_BASE + 0x08))


#define rDDR_MRSVR1   	*((volatile unsigned int *)(DDR3_REG_BASE + 0x0c))
#define rDDR_EXRANKR   	*((volatile unsigned int *)(DDR3_REG_BASE + 0x10))
#define rDDR_TMPR0  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x14))


#define rDDR_TMPR1   	*((volatile unsigned int *)(DDR3_REG_BASE + 0x18))
#define rDDR_TMPR2   	*((volatile unsigned int *)(DDR3_REG_BASE + 0x1c))
#define rDDR_PHYCR0   	*((volatile unsigned int *)(DDR3_REG_BASE + 0x20))
#define rDDR_PHYRDTR  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x24))
#define rDDR_PHYWRTMR   *((volatile unsigned int *)(DDR3_REG_BASE + 0x3c))
#define rDDR_MSDLYCR  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x74))

#define rDDR_EFIFOCR  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x138))

/* int ddr3_data_training(int ba)
{
} */

static inline void ApbWriteFun(unsigned int addr, unsigned int data)
{	
	* (volatile unsigned int *) addr = data;
}

void ddr3_sdramc_init(void)
{
	// ark1668e initial DDR3
	ApbWriteFun(0xE9100000, 0x08c0E110 );
	ApbWriteFun(0xE9100008, 0x30230); 
	//ApbWriteFun(0xE9100010, 0x10000045 ); //DDR3 128M
	ApbWriteFun(0xE9100010, 0x10000054 ); 	//DDR3 256M
	ApbWriteFun(0xE9100130, 0x00000000 );
	udelay(0x1000);
	ApbWriteFun(0xE910003C, 0x00010012 );
	ApbWriteFun(0xE910000C, 0x00000010 );
	ApbWriteFun(0xE9100014, 0x00000000 ); //0x240e100b
	ApbWriteFun(0xE9100018, 0x42951424 );
	ApbWriteFun(0xE910001C, 0x00002051 );
	ApbWriteFun(0xE9100020, 0x00001f41 );//0x00000f41
	ApbWriteFun(0xE9100024, 0x00000000 );
	ApbWriteFun(0xE9100028, 0x00000000 );
	ApbWriteFun(0xE910002C, 0x00001000 );
	ApbWriteFun(0xE9100030, 0x96ff00e5 );//0x90000000
	ApbWriteFun(0xE9100034, 0x00000000 );//0x05050505
	ApbWriteFun(0xE9100038, 0x00000000 );//0x05050305
	ApbWriteFun(0xE9100040, 0x0000aa00 );
	ApbWriteFun(0xE9100048, 0x00555500 );
	ApbWriteFun(0xE9100060, 0x00ff0006 );//0x0000000e
	ApbWriteFun(0xE9100064, 0x00000000 );
	ApbWriteFun(0xE9100068, 0x00000000 );
	ApbWriteFun(0xE910006C, 0x00000000 );//DDR PHY misc control udelaydq   0x44444444
	ApbWriteFun(0xE9100070, 0x000000ff );//read by software
	ApbWriteFun(0xE9100074, 0x11111111 );
	ApbWriteFun(0xE9100078, 0x00000000 );
	ApbWriteFun(0xE910007c, 0x00000000 );
	ApbWriteFun(0xE91000a0, 0x81818184 );//0x81818181
	ApbWriteFun(0xE91000a4, 0x00000000 );
	ApbWriteFun(0xE91000a8, 0x00000200 );
	ApbWriteFun(0xE91000ac, 0x00000200 );
	ApbWriteFun(0xE91000b0, 0x00000001 );
	ApbWriteFun(0xE91000b4, 0x03030400 );//0x03030404
	ApbWriteFun(0xE91000b8, 0x01010202 );//0x01010202
	ApbWriteFun(0xE91000c4, 0x10ff0000 );
	ApbWriteFun(0xE9100138, 0x00000005 );       
	ApbWriteFun(0xE9100004, 0x00000001 );//initial operation

	udelay(200);
	rDDR_MCCR = 0x08d0e103;
	udelay(200000);
	rDDR_MSDLYCR = 0x2; 
	udelay(200000);
	rDDR_PHYRDTR = 0x0; 
	udelay(200000);
}
