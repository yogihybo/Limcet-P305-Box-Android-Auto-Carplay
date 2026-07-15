#include <common.h>
#define DDR_256MX16   1
#define DDR_128MX16   0
//#define DDR_WINBOND
#define DDR3_REG_BASE  0xE9100000
#define AHB_SYS_BASE   0xE4900000
#define rDDR_MCCR   	    *((volatile unsigned int *)(DDR3_REG_BASE + 0x00))
#define rDDR_MCSR   	    *((volatile unsigned int *)(DDR3_REG_BASE + 0x04))
#define rDDR_MRSVR0   	  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x08))
#define rDDR_MRSVR1   	  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x0c))

#define rDDR_EXRANKR   	  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x10))
#define rDDR_TMPR0  	    *((volatile unsigned int *)(DDR3_REG_BASE + 0x14))
#define rDDR_TMPR1   	    *((volatile unsigned int *)(DDR3_REG_BASE + 0x18))
#define rDDR_TMPR2     	  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x1c))

#define rDDR_PHYCR0   	  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x20))
#define rDDR_PHYRDTR  	  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x24))
#define rDDR_COMPBLKCR    	*((volatile unsigned int *)(DDR3_REG_BASE + 0x28))
#define rDDR_AODCR  	    *((volatile unsigned int *)(DDR3_REG_BASE + 0x2c))

#define rDDR_CHARBRA      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x30))
#define rDDR_CHGNTRA      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x34))
#define rDDR_CHGNTRB      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x38))
#define rDDR_PHYWRTMR     	*((volatile unsigned int *)(DDR3_REG_BASE + 0x3c))

#define rDDR_FLUSHCR      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x40))
#define rDDR_FLUSHSR      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x44))
#define rDDR_SPLITCR      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x48))
#define rDDR_UPDCR        	*((volatile unsigned int *)(DDR3_REG_BASE + 0x4c))

#define rDDR_REVR         	*((volatile unsigned int *)(DDR3_REG_BASE + 0x50))
#define rDDR_FEATR1       	*((volatile unsigned int *)(DDR3_REG_BASE + 0x54))
#define rDDR_FEATR2       	*((volatile unsigned int *)(DDR3_REG_BASE + 0x58))
#define rDDR_UDEFR        	*((volatile unsigned int *)(DDR3_REG_BASE + 0x5c))

#define rDDR_WLEVELCR     	*((volatile unsigned int *)(DDR3_REG_BASE + 0x60))
#define rDDR_WLEVELBHR    	*((volatile unsigned int *)(DDR3_REG_BASE + 0x64))
#define rDDR_WLEVELBLR    	*((volatile unsigned int *)(DDR3_REG_BASE + 0x68))
#define rDDR_PHYMISCR1    	*((volatile unsigned int *)(DDR3_REG_BASE + 0x6c))

#define rDDR_RLEVELCR     	*((volatile unsigned int *)(DDR3_REG_BASE + 0x70))
#define rDDR_MSDLYCR      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x74))
#define rDDR_WRDLLCR      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x78))
#define rDDR_TRAFMR       	*((volatile unsigned int *)(DDR3_REG_BASE + 0x7c))

#define rDDR_CMDCNTR0     	*((volatile unsigned int *)(DDR3_REG_BASE + 0x80))
#define rDDR_CMDCNTR1     	*((volatile unsigned int *)(DDR3_REG_BASE + 0x84))
#define rDDR_CMDCNTR2     	*((volatile unsigned int *)(DDR3_REG_BASE + 0x88))
#define rDDR_CMDCNTR3     	*((volatile unsigned int *)(DDR3_REG_BASE + 0x8c))

#define rDDR_CMDCNTR4     	*((volatile unsigned int *)(DDR3_REG_BASE + 0x90))
#define rDDR_CMDCNTR5     	*((volatile unsigned int *)(DDR3_REG_BASE + 0x94))
#define rDDR_CMDCNTR6     	*((volatile unsigned int *)(DDR3_REG_BASE + 0x98))
#define rDDR_CMDCNTR7     	*((volatile unsigned int *)(DDR3_REG_BASE + 0x9c))

#define rDDR_AHBRPRER1    	*((volatile unsigned int *)(DDR3_REG_BASE + 0xa0))
#define rDDR_AHBRPRER2    	*((volatile unsigned int *)(DDR3_REG_BASE + 0xa4))
#define rDDR_INITWCR1     	*((volatile unsigned int *)(DDR3_REG_BASE + 0xa8))
#define rDDR_INITWCR2     	*((volatile unsigned int *)(DDR3_REG_BASE + 0xac))

#define rDDR_QOSCR        	*((volatile unsigned int *)(DDR3_REG_BASE + 0xb0))
#define rDDR_QOSCNTRA     	*((volatile unsigned int *)(DDR3_REG_BASE + 0xb4))
#define rDDR_QOSCNTRB     	*((volatile unsigned int *)(DDR3_REG_BASE + 0xb8))
#define rDDR_QOSCNTRC     	*((volatile unsigned int *)(DDR3_REG_BASE + 0xbc))

#define rDDR_QOSCNTRD     	*((volatile unsigned int *)(DDR3_REG_BASE + 0xc0))
#define rDDR_CHARBRB      	*((volatile unsigned int *)(DDR3_REG_BASE + 0xc4))
#define rDDR_CHGNTRC      	*((volatile unsigned int *)(DDR3_REG_BASE + 0xc8))
#define rDDR_CHGNTRD      	*((volatile unsigned int *)(DDR3_REG_BASE + 0xcc))

#define rDDR_REARBDISR  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x12c))

#define rDDR_PHYRDTFR  	  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x130))
#define rDDR_PHYMISCR2  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x134))
#define rDDR_EFIFOCR  	  	*((volatile unsigned int *)(DDR3_REG_BASE + 0x138))

#define rDDR_RB0DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x160))
#define rDDR_RB1DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x164))
#define rDDR_RB2DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x168))
#define rDDR_RB3DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x16c))

#define rDDR_RB4DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x170))
#define rDDR_RB5DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x174))
#define rDDR_RB6DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x178))
#define rDDR_RB7DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x17c))

#define rDDR_WB0DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x180))
#define rDDR_WB1DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x184))
#define rDDR_WB2DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x188))
#define rDDR_WB3DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x18c))

#define rDDR_WB4DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x190))
#define rDDR_WB5DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x194))
#define rDDR_WB6DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x198))
#define rDDR_WB7DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x19c))

#define rDDR_WDMDSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x1a0))
#define rDDR_RB8DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x1a4))
#define rDDR_WB8DSKW      	*((volatile unsigned int *)(DDR3_REG_BASE + 0x1a8))
#define rDDR_B8_PHYCR     	*((volatile unsigned int *)(DDR3_REG_BASE + 0x1ac))


#define SYS_SOFT_RST_N_A   *((volatile unsigned int *)(AHB_SYS_BASE + 0x074))
#define SYS_SOFT_RST_N_B   *((volatile unsigned int *)(AHB_SYS_BASE + 0x078))
#define DDR_CFG_0   	   *((volatile unsigned int *)(AHB_SYS_BASE + 0x210))
#define DDR_CFG_1   	   *((volatile unsigned int *)(AHB_SYS_BASE + 0x214))

#define DDR3_1600

#ifdef  DDR3_1600

	#define GDS         4
	#define MSDLY       0x22
/*
  MR0:
	[1:0]   BL                // 00:8; 01:4/8; 10:4; 11:RES
	[3]     READ Burst Type   // 0: Sequential; 1:Interleaved
  [6:4,2] CL                //0000:RES; 0010:5; 1110:11; 0001:12; 0011:13; 0101:14;
  [8]     DLL RST           //0: NO;  1:YES
  [11:9]  WR                //000:16; 001:5; 010:6; 011:7; 100:8; 101:10; 110:12; 111:14;
  [12]    PD                //0: DLL OFF    1: DLL ON

  MR1:
	[0]     DLL Enable                //0 en  1 dis
  [5,1]   Output Drive St rength    //00: 40  01:34
  [4:3]   Additive Latency (AL)     //00: dis 01:CL-1 02:CL-2 11:RES
  [9,6,2] RTT,nom                   //000:DIS  001:60 010:120 011:40 100:20 101:30
  [7]     Write Levelization        //0: dis    1: enable
  [11]    TDQS                      //0: dis    1: enable
  [12]    Q Off                     //0: enable 1: dis

  MR2:
	[2:0]   PASR                      //000: default
  [5:3]   CWL                       //000: (tCK.AVG ≥ 2.5 ns; 001: 2.5 ns > tCK.AVG ≥ 1.875 ns
                                      010: 1.875 ns > tCK.AVG ≥ 1.5 ns 011: 1.5 ns > tCK.AVG ≥ 1.25 ns
                                      100: 1.25 ns > tCK.AVG ≥ 1.07 ns 101: 1.07 ns > tCK.AVG ≥ 0.935ns
  [4:3]   Additive Latency (AL)     //00: dis 01:CL-1 02:CL-2 11:RES
  [9,6,2] RTT,nom                   //000:DIS  001:60 010:120 011:40 100:20 101:30
  [7]     Write Levelization        //0: dis    1: enable
  [11]    TDQS                      //0: dis    1: enable
  [12]    Q Off                     //0: enable 1: dis

*/
#if DDR_256MX16
	#define TRFC        0x68  // 104 //(unsigned int)((260*DDR_MCLK)/1000)
#endif
#if DDR_128MX16
	#define TRFC        0x40  // 64 (unsigned int)((160*DDR_MCLK)/1000)
#endif

	#define TFAW        0x10  // 16 // (unsigned int)((35*DDR_MCLK)/1000)
	#define TRC         0x14  // 20 (unsigned int)((50*DDR_MCLK)/1000)
	#define TRAS        0xe   //(unsigned int)((35*DDR_MCLK)/1000)

	#define TWTR        5
	#define TRTP        4     //(unsigned int)((8*DDR_MCLK)/1000)
	#define TWR         9     //(unsigned int)((15*2*DDR_MCLK)/1000)
	#define TMOD        12    //(unsigned int)((15*2*DDR_MCLK)/1000)
	#define TMRD        4
	#define TRP         6      //(unsigned int)((15*DDR_MCLK)/1000)
	#define TRRD        1      //(unsigned int)((7.5*DDR_MCLK)/1000)
	#define TRCD        6      //(unsigned int)((15*DDR_MCLK)/1000)

	#define TREFI       0x32 //0x64
	#define TXSR        0x20
	#define TR2w        0
	#define TR2R        0
	#define TW22        0
	#define TW2R        0
	#define mr0         0x1d70
	#define mr1         0x4
	#define mr2         0x258
	#define tphy_wrlat  3
	#define tphy_wrdata 1
	#define trddata_en  4
	#define tphy_rdlat  0
	#define twl         8  //TWL=CWL
	#define trl         11 //TRL=CL

#endif



void ApbWriteFun(unsigned int addr, unsigned int data)
{
	* (volatile unsigned int *) addr = data;
}


/*
#define MAGIC_DATA	0x55aaccee
int DDRTraining(void)
{
	int i;
	int min = -1, max = -1;
	int gds = -1;
	int	gdsnum = 0;
	unsigned int val;

	for (i = 0; i < 8; i++) {
//		 set GDS
		val = ApbReadFun(0xE9100000);
		val &= ~7;
		val |= i;
		ApbWriteFun(0xE9100000, val);

//		 check ddr data r/w ok
		udelay(10);
		ApbWriteFun(0x40000000, MAGIC_DATA);
		val = ApbReadFun(0x40000000);
		if (val == MAGIC_DATA) {
			if (min < 0)
				min = i;
			max = i;
		}
		if (val != MAGIC_DATA || i == 7) {
			if (min >=0 && max >= 0) {
				val = max - min + 1;
				if (val > gdsnum) {
					gdsnum = val;
					gds = min + max / 2;
				}
			}
			min = -1;
			max = -1;
		}
	}

	if (gds < 0) {
		SendUartString("DDR training fail!\r\n");
		return -1;
	}

//	 set GDS
	val = ApbReadFun(0xE9100000);
	val &= ~7;
	val |= gds;
	ApbWriteFun(0xE9100000, val);
#if 0
//  set read-leveling by hardware
	val = ApbReadFun(0xE9100070);
	val &= ~3;
	ApbWriteFun(0xE9100070, val);

//	 set write-leveling by hardware
	val = ApbReadFun(0xE9100060);
	val &= ~(3 << 16);
	ApbWriteFun(0xE9100060, val);
#endif
	udelay(10);
	return 0;
}
*/


//#if 1
unsigned int ddr3_sdramc_init(void)
{
	int dll_frange=0;
//softa
	SYS_SOFT_RST_N_B = 0xfffffffd;
	udelay (1); //

	/*
	dll_frange	ddr data rate(freq*2)
		0	:	[0-400)
		1	:	[400-600)
		2	:	[600-700)
		3	:	[700-800)
		4	:	[800-900)
		5	:	[900-1000)
		6	:	[1000-1200)
		7	:	[1200-1600)
	*/
	dll_frange = 7;
	DDR_CFG_0 = 0x06060860|(dll_frange<<28);
//apb_sys, DDR_CFG_1
	DDR_CFG_1 = 0;
//softa
	SYS_SOFT_RST_N_B = 0xfffffffd;
	udelay (100);
	DDR_CFG_1 = 0xFFe0BFFF;
	udelay (500); // > 50us
	DDR_CFG_1 = 0xFFe8BFFF;
	udelay (10);
	DDR_CFG_1 = 0xFFe8FFFF;
	udelay (500);//> 100us
	DDR_CFG_1 = 0xFFeBFFFF;
	udelay (200);
	DDR_CFG_1 = 0xFFeFFFFF;
	udelay (200);
	DDR_CFG_1 = 0xFFeFEFFF;
	udelay (200);
//apb_sys, DDR_CFG_1
//	DDR_CFG_1 = 0xffefe374;
	DDR_CFG_1 = 0xffefe274;
//softa
	SYS_SOFT_RST_N_B = 0xffffffff;
//wait dll locked
	while(!((rDDR_PHYCR0>>25)&0x1));//byte1 dll locked
	while(!((rDDR_PHYCR0>>24)&0x1));//byte0 dll locked

	udelay (1000);
// ark1668e initial DDR3
//0x00    0x8c0e104
//	rDDR_MCCR   =  1<<27 |	1<<20 | 1<<8 | 4<<0;
	rDDR_MCCR   =  1<<27 |	1<<23 | 2<<21 | 1<<13 | 1<<8 | 4<<0;  //0323
//	rDDR_MCCR   =  1<<27 |	0<<23 | 0<<21 | 1<<20 | 7<<13 | 1<<8 | 0xc<<0;  //0323
//0x08
	rDDR_MRSVR0 =  (mr1<<16)| mr0;
//0x0C
	rDDR_MRSVR1 =   mr2;
//0x04
//	rDDR_MCSR |=   1<<6 | 1<<1 ;
//	while(rDDR_MCSR & (1<<12));

//	rDDR_MCSR |=   1<<7 | 1<<1 ;
//	while(rDDR_MCSR & (1<<13));
//0x10
#if DDR_128MX16
  rDDR_EXRANKR =  5<<4 | 4<<0;  // 128Mx16 2G bit
#endif
#if DDR_256MX16
	rDDR_EXRANKR =  6<<4 | 5<<0;  // 256Mx16 4G bit
#endif
//0x14
	rDDR_TMPR0 = TRAS << 0 | TRC << 8| TFAW << 16| TRFC << 24 ;
//0x18
	rDDR_TMPR1 = TRCD<< 0 | TRRD<<4 | TRP << 8 | TMRD << 12 | TMOD << 16 | TWR << 20 | TRTP <<24 | TWTR <<28  ;
//0x1c
    rDDR_TMPR2 = TREFI << 0 | TXSR<<8 | TR2w << 24 | TR2R << 26 | TW22 << 28 | TW2R << 30;

 //0x20
//    rDDR_PHYCR0  = 1 << 13 | 1 << 11 | 1 << 10 | 1 << 9 | 1 << 8 | 4<<4 | 4<<0;
    rDDR_PHYCR0  = 1 << 13 | 1 << 11 | 1 << 10 | 1 << 9 | 1 << 8 | 2<<4 | 2<<0;

#ifdef DDR_WINBOND
//0x24
    rDDR_PHYRDTR   =  8<<4 | 8<<0;

//0x130
    rDDR_PHYRDTFR  	 =  8<<4 | 8<<0;
#else
//0x24
    rDDR_PHYRDTR   =  9<<4 | 9<<0;

//0x130
    rDDR_PHYRDTFR  	 =  9<<4 | 9<<0;
#endif

 //0x78
    rDDR_WRDLLCR     =  10<<4 | 10<<0;
/*
//0x28
    rDDR_COMPBLKCR =   0x1f<<7 | 0x1f<<1 | 1<<0;

//0x2c
   rDDR_AODCR  = 1<<28 | 0xfff<<16;
*/
//0x30
//    rDDR_CHARBRA =  3<<30 |  0x1e<<24 | 1<<0 ;
    rDDR_CHARBRA =  0xc<<28 | 1<<0 ;
//0x34
    rDDR_CHGNTRA =  0x5<<24 | 0x5<<16 | 0x5<<8 |0xff<<0 ;
//	rDDR_CHGNTRA =  0x5<<24 | 0x2<<16 | 0x10<<8 |0xff<<0 ;

//0x3c
    rDDR_PHYWRTMR  =  tphy_rdlat<<20 | trddata_en<<16  | tphy_wrdata<<4 | tphy_wrlat;

//0x4c
	rDDR_UPDCR = 2<<16 | 0x10<<8 | 5<<0;
//0x60
//      rDDR_WLEVELCR    =  0xff<<16 | 10<<0;
      rDDR_WLEVELCR    =  0xff<<16 | 8<<0;
//0x68
    rDDR_WLEVELBLR	=   0x3<<8 | 0x3<<0;   //test for 0--a test fail

//0x6C
//    rDDR_PHYMISCR1   =  0<<5 | 0<<4 | 0<<1 | 0<<0;
//0x70
    rDDR_RLEVELCR    = 0<<16 | 0<<8 | 0xff<<0;
//0x74
//	rDDR_MSDLYCR     =  2<<4 | 2<<0;   //test for 0--3
	rDDR_MSDLYCR     =  1<<4 | 1<<0;   //test for 0--3





 // test wr 88;
    rDDR_INITWCR1    =  0x30d50<<0;
    rDDR_INITWCR2    =  0x618a0<<0;

//0xc4
    rDDR_CHARBRB    =  0x20<<24 | 3<<16 |  1<<0 ;

//0xc8
    rDDR_CHGNTRC    =  0x5<<24 | 0x5<<16 | 0x1f<<8 |0x5<<0 ;

//0x134
 //  rDDR_PHYMISCR2   =   5<<28 | 5<<24 |7<<20 | 3<<16 | 4<<12 | 4<<8 | 4<<4 | 4<<0 ;


//0x138
    rDDR_EFIFOCR  	 =   0<<4 | 1<<2 | 1<<0 ;
/*
//0x160
    rDDR_RB0DSKW     =  4<<28 | 4<<24 |4<<20 | 4<<16 | 4<<12 | 4<<8 | 4<<4 | 4<<0 ;
//0x164
    rDDR_RB1DSKW     =  4<<28 | 4<<24 |4<<20 | 4<<16 | 4<<12 | 4<<8 | 4<<4 | 4<<0 ;
//0x180
    rDDR_WB0DSKW     =  3<<28 | 3<<24 |3<<20 | 3<<16 | 3<<12 | 3<<8 | 3<<4 | 3<<0 ;
//0x184
    rDDR_WB1DSKW     =  3<<28 | 3<<24 |3<<20 | 3<<16 | 3<<12 | 3<<8 | 3<<4 | 3<<0 ;
//0x1a0
    rDDR_WDMDSKW     =   3<<4 | 3<<0 ;
 */

//0x4
	rDDR_MCSR = 1;
	while(!((rDDR_MCSR>>8)&0x1));
	udelay (1);

	printf("DDR3 256*16_20221130\n");
    return 0;
}
