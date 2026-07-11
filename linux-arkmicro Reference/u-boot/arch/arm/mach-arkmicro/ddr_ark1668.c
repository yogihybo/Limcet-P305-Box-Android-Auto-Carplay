#include <common.h>

#define DDRIII_BASE		0xE0A00000

/* DDRII_Controller */
#define rDDR_CCR				*((volatile unsigned int  *) (DDRIII_BASE + 0x00))
#define rDDR_DCR				*((volatile unsigned int  *) (DDRIII_BASE + 0x04))
#define rDDR_IOCR				*((volatile unsigned int  *) (DDRIII_BASE + 0x08))
#define rDDR_CSR				*((volatile unsigned int  *) (DDRIII_BASE + 0x0C))
#define rDDR_DRR				*((volatile unsigned int  *) (DDRIII_BASE + 0x10))
#define rDDR_TPR0				*((volatile unsigned int  *) (DDRIII_BASE + 0x14))
#define rDDR_TPR1				*((volatile unsigned int  *) (DDRIII_BASE + 0x18))
#define rDDR_TPR2				*((volatile unsigned int  *) (DDRIII_BASE + 0x1C))
#define rDDR_DLLCR				*((volatile unsigned int  *) (DDRIII_BASE + 0x20))
#define rDDR_DLLCR0				*((volatile unsigned int  *) (DDRIII_BASE + 0x24))
#define rDDR_DLLCR1				*((volatile unsigned int  *) (DDRIII_BASE + 0x28))
#define rDDR_DLLCR2				*((volatile unsigned int  *) (DDRIII_BASE + 0x2C))
#define rDDR_DLLCR3				*((volatile unsigned int  *) (DDRIII_BASE + 0x30))
#define rDDR_DLLCR4				*((volatile unsigned int  *) (DDRIII_BASE + 0x34))
#define rDDR_DLLCR5				*((volatile unsigned int  *) (DDRIII_BASE + 0x38))
#define rDDR_DLLCR6				*((volatile unsigned int  *) (DDRIII_BASE + 0x3C))
#define rDDR_DLLCR7				*((volatile unsigned int  *) (DDRIII_BASE + 0x40))
#define rDDR_DLLCR8				*((volatile unsigned int  *) (DDRIII_BASE + 0x44))
#define rDDR_DLLCR9				*((volatile unsigned int  *) (DDRIII_BASE + 0x48))
#define rDDR_RSLR0				*((volatile unsigned int  *) (DDRIII_BASE + 0x4C))
#define rDDR_RSLR1				*((volatile unsigned int  *) (DDRIII_BASE + 0x50))
#define rDDR_RSLR2				*((volatile unsigned int  *) (DDRIII_BASE + 0x54))
#define rDDR_RSLR3				*((volatile unsigned int  *) (DDRIII_BASE + 0x58))
#define rDDR_RDGR0				*((volatile unsigned int  *) (DDRIII_BASE + 0x5C))
#define rDDR_RDGR1				*((volatile unsigned int  *) (DDRIII_BASE + 0x60))
#define rDDR_RDGR2				*((volatile unsigned int  *) (DDRIII_BASE + 0x64))
#define rDDR_RDGR3				*((volatile unsigned int  *) (DDRIII_BASE + 0x68))
#define rDDR_DQTR0				*((volatile unsigned int  *) (DDRIII_BASE + 0x6C))
#define rDDR_DQTR1				*((volatile unsigned int  *) (DDRIII_BASE + 0x70))
#define rDDR_DQTR2				*((volatile unsigned int  *) (DDRIII_BASE + 0x74))
#define rDDR_DQTR3				*((volatile unsigned int  *) (DDRIII_BASE + 0x78))
#define rDDR_DQTR4				*((volatile unsigned int  *) (DDRIII_BASE + 0x7C))
#define rDDR_DQTR5				*((volatile unsigned int  *) (DDRIII_BASE + 0x80))
#define rDDR_DQTR6				*((volatile unsigned int  *) (DDRIII_BASE + 0x84))
#define rDDR_DQTR7				*((volatile unsigned int  *) (DDRIII_BASE + 0x88))
#define rDDR_DQTR8				*((volatile unsigned int  *) (DDRIII_BASE + 0x8C))
#define rDDR_DQSTR				*((volatile unsigned int  *) (DDRIII_BASE + 0x90))
#define rDDR_DQSBTR				*((volatile unsigned int  *) (DDRIII_BASE + 0x94))
#define rDDR_ODTCR				*((volatile unsigned int  *) (DDRIII_BASE + 0x98))
#define rDDR_DTR0				*((volatile unsigned int  *) (DDRIII_BASE + 0x9C))
#define rDDR_DTR1				*((volatile unsigned int  *) (DDRIII_BASE + 0xA0))
#define rDDR_DTAR				*((volatile unsigned int  *) (DDRIII_BASE + 0xA4))
#define rDDR_ZQCR0				*((volatile unsigned int  *) (DDRIII_BASE + 0xA8))
#define rDDR_ZQCR1				*((volatile unsigned int  *) (DDRIII_BASE + 0xAC))
#define rDDR_ZQCR2				*((volatile unsigned int  *) (DDRIII_BASE + 0xB0))
#define rDDR_ZQSR				*((volatile unsigned int  *) (DDRIII_BASE + 0xB4))
#define rDDR_TPR3				*((volatile unsigned int  *) (DDRIII_BASE + 0xB8))
#define rDDR_ALPMR				*((volatile unsigned int  *) (DDRIII_BASE + 0xBC))
#define rDDR_MR					*((volatile unsigned int  *) (DDRIII_BASE + 0x1F0))
#define rDDR_EMR				*((volatile unsigned int  *) (DDRIII_BASE + 0x1F4))
#define rDDR_EMR2				*((volatile unsigned int  *) (DDRIII_BASE + 0x1F8))
#define rDDR_EMR3				*((volatile unsigned int  *) (DDRIII_BASE + 0x1FC))

#define rDDR_HPCR0				*((volatile unsigned int  *) (DDRIII_BASE + 0x200))
#define rDDR_HPCR1				*((volatile unsigned int  *) (DDRIII_BASE + 0x204))
#define rDDR_HPCR2				*((volatile unsigned int  *) (DDRIII_BASE + 0x208))
#define rDDR_HPCR3				*((volatile unsigned int  *) (DDRIII_BASE + 0x20C))
#define rDDR_HPCR4				*((volatile unsigned int  *) (DDRIII_BASE + 0x210))
#define rDDR_HPCR5				*((volatile unsigned int  *) (DDRIII_BASE + 0x214))
#define rDDR_HPCR6				*((volatile unsigned int  *) (DDRIII_BASE + 0x218))
#define rDDR_HPCR7				*((volatile unsigned int  *) (DDRIII_BASE + 0x21C))
#define rDDR_HPCR8				*((volatile unsigned int  *) (DDRIII_BASE + 0x220))
#define rDDR_HPCR9				*((volatile unsigned int  *) (DDRIII_BASE + 0x224))
#define rDDR_HPCR10				*((volatile unsigned int  *) (DDRIII_BASE + 0x228))
#define rDDR_HPCR11				*((volatile unsigned int  *) (DDRIII_BASE + 0x22C))
#define rDDR_HPCR12				*((volatile unsigned int  *) (DDRIII_BASE + 0x230))
#define rDDR_HPCR13				*((volatile unsigned int  *) (DDRIII_BASE + 0x234))
#define rDDR_HPCR14				*((volatile unsigned int  *) (DDRIII_BASE + 0x238))
#define rDDR_HPCR15				*((volatile unsigned int  *) (DDRIII_BASE + 0x23C))
#define rDDR_HPCR16				*((volatile unsigned int  *) (DDRIII_BASE + 0x240))
#define rDDR_HPCR17				*((volatile unsigned int  *) (DDRIII_BASE + 0x244))
#define rDDR_HPCR18				*((volatile unsigned int  *) (DDRIII_BASE + 0x248))
#define rDDR_HPCR19				*((volatile unsigned int  *) (DDRIII_BASE + 0x24C))
#define rDDR_HPCR20				*((volatile unsigned int  *) (DDRIII_BASE + 0x250))
#define rDDR_HPCR21				*((volatile unsigned int  *) (DDRIII_BASE + 0x254))
#define rDDR_HPCR22				*((volatile unsigned int  *) (DDRIII_BASE + 0x258))
#define rDDR_HPCR23				*((volatile unsigned int  *) (DDRIII_BASE + 0x25C))
#define rDDR_HPCR24				*((volatile unsigned int  *) (DDRIII_BASE + 0x260))
#define rDDR_HPCR25				*((volatile unsigned int  *) (DDRIII_BASE + 0x264))
#define rDDR_HPCR26				*((volatile unsigned int  *) (DDRIII_BASE + 0x268))
#define rDDR_HPCR27				*((volatile unsigned int  *) (DDRIII_BASE + 0x26C))
#define rDDR_HPCR28				*((volatile unsigned int  *) (DDRIII_BASE + 0x270))
#define rDDR_HPCR29				*((volatile unsigned int  *) (DDRIII_BASE + 0x274))
#define rDDR_HPCR30				*((volatile unsigned int  *) (DDRIII_BASE + 0x278))
#define rDDR_HPCR31				*((volatile unsigned int  *) (DDRIII_BASE + 0x27C))
#define rDDR_PQCR0				*((volatile unsigned int  *) (DDRIII_BASE + 0x280))
#define rDDR_PQCR1				*((volatile unsigned int  *) (DDRIII_BASE + 0x284))
#define rDDR_PQCR2				*((volatile unsigned int  *) (DDRIII_BASE + 0x288))
#define rDDR_PQCR3				*((volatile unsigned int  *) (DDRIII_BASE + 0x28C))
#define rDDR_PQCR4				*((volatile unsigned int  *) (DDRIII_BASE + 0x290))
#define rDDR_PQCR5				*((volatile unsigned int  *) (DDRIII_BASE + 0x294))
#define rDDR_PQCR6				*((volatile unsigned int  *) (DDRIII_BASE + 0x298))
#define rDDR_PQCR7				*((volatile unsigned int  *) (DDRIII_BASE + 0x29C))
#define rDDR_MMGCR				*((volatile unsigned int  *) (DDRIII_BASE + 0x2A0))

#define rSYS_DDR_STATUS			*((volatile unsigned int *)(0xe4900180))

int ddr3_data_training(int ba)
{
	rDDR_DTAR = (0x80 <<12 ) | (ba<<28);
	rDDR_CCR |=  (1<<30) ;
	udelay(500);
	if(rSYS_DDR_STATUS & (1 << 1)) {
		printf("training one\n");
		rDDR_CSR &= ~(1<<20);
		return -1;
	} else {
		printf("training ok\n");
		return 0;
	}
}

void ddr3_sdramc_init(void)
{
	rDDR_DLLCR = 0x00707000;
	udelay(200);
	rDDR_DRR = 0x7<<24|27800<<8|0x64<<0;

	//1G bit
	//rDDR_DRR = 0x7<<24|18000<<8|0x32<<0;
	rDDR_CCR = 0x20004;
	rDDR_DLLCR0 |= (1<<5)|(1<<11)|(0x0<<14); //phase detect forward adjust middle back adjust middle phase 0x01 means 72 0x00 means 00
	rDDR_DLLCR1 |= (1<<5)|(1<<11)|(0x0<<14);//phase detect forward adjust middle back adjust middle phase 0x01 means 72 0x00 means 00
	//      0:90   1:72  2:54  3:36  4:108  8:126 c:144
	rDDR_DLLCR9 |= (1<<11); //cmd line dll back adjust middle

	// for 6 layer phase 54 middle
	rDDR_DQTR0  =0x55555555; //add 1 step
	rDDR_DQTR1  =0x55555555; //add 1 step
	rDDR_DQSTR  =0x12;  //sub 1 step
	rDDR_DQSBTR =0x12;  //sub 1 step
	rDDR_MR = (0x5<<9)|(0x1<<8)|(0x2<<4);   //MR0   BURST=8 CL=6 WR=10  bit4 CL= VALUE+4
	rDDR_EMR = (0x1<<6)|(0x3<<1);           //MR1  AL=0 CWL=6
	rDDR_EMR2 = (0x1<<9)|(0<<3);            //MR2 CWL=6 BIT3  BIT3 CWL = (VALUE-5)
	rDDR_ZQCR0 =0x1<<31;
	udelay(8000);

	rDDR_IOCR =(0x3<<30)|(0xf<<7)|(0x0<<3)| (3<<0); //  enable odt  6pcb
	rDDR_TPR0 = 0;
	rDDR_TPR0 |= (2<<0)|(5<<2)|(5<<5)|(5<<8)|(5<<12)|(18<<16)|(5<<21)|(24<<25);
	rDDR_TPR1 = 0;
	rDDR_TPR1 |= (0x0<<2)|(25<<3)|(2<<12)|(2<<14);
	rDDR_TPR2 = 0;
	rDDR_TPR2 |= (200<<0)|(15<<10)|(6<<15);
	rDDR_TPR3 = 0;
	rDDR_TPR3 |= (2<<0)|(6<<3)|(5<<7)|(10<<11);  //BURST LENGTH 8

	//2G
	rDDR_DCR = (0x1<<24)|(0x1<<14)|(0x1<<7)|(0x5<<4)|(0x2<<2)|(0x1<<0);

	//1G bit
	//rDDR_DCR = 0x1<<24|(0x0<<14)|0x1<<7|0x4<<4|0x2<<2|0x1;

	printf("ARK169 2Gb 20180412-6-5-10_330_430_nboot 0x%x\n", rDDR_IOCR);
}
