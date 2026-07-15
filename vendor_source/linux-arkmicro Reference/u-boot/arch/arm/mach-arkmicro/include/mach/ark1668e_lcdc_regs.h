/*
 *  Header file for ARK1668E LCD Controller
 *
 */

#ifndef __ARK1668E_LCDC_REGS_H__
#define __ARK1668E_LCDC_REGS_H__


#define ARK1668E_LCDC_DMABADDR1						0x00
#define ARK1668E_LCDC_DMABADDR2						0x04
#define ARK1668E_LCDC_DMAFRMPT1						0x08
#define ARK1668E_LCDC_DMAFRMPT2						0x0c
#define ARK1668E_LCDC_DMAFRMADD1					0x10
#define ARK1668E_LCDC_DMAFRMADD2					0x14

#define ARK1668E_LCDC_HEIGHT_OFFSET					12

/* LCD */
#define ARK1668E_LCDC_EANBLE									0x000
#define ARK1668E_LCDC_CONTROL									0x004
#define ARK1668E_LCDC_OSD1_EN_OFFSET					        7
#define ARK1668E_LCDC_OSD2_EN_OFFSET					        8
#define ARK1668E_LCDC_TIMING0									0x008
#define	ARK1668E_LCDC_HFP							            (0x3ffU <<  0)
#define	ARK1668E_LCDC_HBP_OFFSET						        10
#define	ARK1668E_LCDC_HBP							            (0x3ffU <<  ARK1668E_LCDC_HBP_OFFSET)
#define	ARK1668E_LCDC_HPW_OFFSET						        20
#define	ARK1668E_LCDC_HPW							            (0x3ffU << ARK1668E_LCDC_HPW_OFFSET)
#define ARK1668E_LCDC_TIMING1									0x00c
#define	ARK1668E_LCDC_VPW_OFFSET						        13
#define	ARK1668E_LCDC_VPW							            (0x3fU <<  ARK1668E_LCDC_VPW_OFFSET)
#define	ARK1668E_LCDC_VFP_OFFSET						        19
#define	ARK1668E_LCDC_VFP							            (0x3ffU <<  ARK1668E_LCDC_VFP_OFFSET)
#define ARK1668E_LCDC_TIMING2									0x010
#define	ARK1668E_LCDC_VBP							            (0x3ffU << 0)
#define ARK1668E_LCDC_IOE_OFFSET						        23
#define ARK1668E_LCDC_IHS_OFFSET						        22
#define ARK1668E_LCDC_IVS_OFFSET						        21
#define ARK1668E_LCDC_LPS_OFFSET						        10
#define ARK1668E_LCDC_TIMING3									0x014
#define ARK1668E_LCDC_TIMING_FRAME_START_CNT_LCD				0x018
#define ARK1668E_LCDC_BACK_COLOR								0x01C
#define ARK1668E_LCDC_BLD_MODE_LCD_REG0						    0x020
#define ARK1668E_LCDC_BLD_MODE_LCD_REG1							0x024
#define ARK1668E_LCDC_BLEND_POST_CTL  							0x028
#define ARK1668E_LCDC_ITU_CONTROL                               0x02C
#define ARK1668E_LCDC_ITU_SRGB_GENERATION_CTL					0x030
#define ARK1668E_LCDC_ITU_TIMIING_REFERENCE_CODE_DEFINE			0x034//new
#define ARK1668E_LCDC_YCLCD_ITU_TIMING_REFERENCE_CODE_DEFINE	0x038//new
#define ARK1668E_LCDC_DITHERING									0x03C//new
#define ARK1668E_LCDC_DITHERING_V_H_SIZE						0x040//new
#define ARK1668E_LCDC_DITHERING_TEST							0x044//new


#define ARK1668E_LCDC_TV_CONTROL							    0x060
#define ARK1668E_LCDC_TIMING0_TV								0x064
#define ARK1668E_LCDC_TIMING1_TV								0x068
#define ARK1668E_LCDC_TIMING2_TV								0x06c
#define ARK1668E_LCDC_TIMING3_TV								0x070
#define ARK1668E_LCDC_TIMING_FRAME_START_CNT_TV					0x074
#define ARK1668E_LCDC_BACK_COLOR_TV								0x078
#define ARK1668E_LCDC_BLD_MODE_TV_REG0							0x07C
#define ARK1668E_LCDC_BLD_MODE_TV_REG1							0x080

//TV Encoder param
#define ARK1668E_LCDC_TV_PARAM_REG0                             0x084
#define ARK1668E_LCDC_TV_PARAM_REG1                             0x088
#define ARK1668E_LCDC_TV_PARAM_REG2                             0x08c
#define ARK1668E_LCDC_TV_PARAM_REG3                             0x090
#define ARK1668E_LCDC_TV_PARAM_REG4                             0x094
#define ARK1668E_LCDC_TV_PARAM_REG5                             0x098
#define ARK1668E_LCDC_TV_PARAM_REG6                             0x09C
#define ARK1668E_LCDC_TV_PARAM_REG7                             0x0A0
#define ARK1668E_LCDC_TV_PARAM_REG8                             0x0A4
#define ARK1668E_LCDC_TV_PARAM_REG9                             0x0A8
#define ARK1668E_LCDC_TV_PARAM_REG10                            0x0AC
#define ARK1668E_LCDC_TV_PARAM_REG11                            0x0B0
#define ARK1668E_LCDC_TV_PARAM_REG12                            0x0B4
#define ARK1668E_LCDC_TV_PARAM_REG13                            0x0B8
#define ARK1668E_LCDC_TV_PARAM_REG14                            0x0BC
#define ARK1668E_LCDC_TV_PARAM_REG15                            0x0C0
#define ARK1668E_LCDC_TV_PARAM_REG16                            0x0C4
#define ARK1668E_LCDC_TV_PARAM_REG17                            0x0C8
#define ARK1668E_LCDC_TV_PARAM_REG18                            0x0CC
#define ARK1668E_LCDC_TV_PARAM_REG19                            0x0D0
#define ARK1668E_LCDC_TV_PARAM_REG20                            0x0D4
#define ARK1668E_LCDC_TV_PARAM_REG21                            0x0D8


#define ARK1668E_LCDC_VIDEO1_BURST_CTL							0x100
#define ARK1668E_LCDC_VIDEO1_CTL								0x104
#define ARK1668E_LCDC_VIDEO1_ALPHA1_ALPHA0_BLENDING_COEFF		0x108
#define ARK1668E_LCDC_VIDEO1_SOURCE_SIZE						0x10c
#define ARK1668E_LCDC_VIDEO1_WIN_SIZE							0x110
#define ARK1668E_LCDC_VIDEO1_SIZE								0x114
#define ARK1668E_LCDC_VIDEO1_WIN_POINT							0x118
#define ARK1668E_LCDC_VIDEO1_POSITION							0x11c
#define ARK1668E_LCDC_VIDEO1_ADDR1								0x120
#define ARK1668E_LCDC_VIDEO1_ADDR2								0x124
#define ARK1668E_LCDC_VIDEO1_ADDR3								0x128
#define ARK1668E_LCDC_VIDEO1_ADDR1_GROUP1						0x12c
#define ARK1668E_LCDC_VIDEO1_ADDR2_GROUP1						0x130
#define ARK1668E_LCDC_VIDEO1_ADDR3_GROUP1						0x134
#define ARK1668E_LCDC_BLD_CUT_LEFT_RIGHT_VIDEO1					0x138
#define ARK1668E_LCDC_BLD_CUT_UP_DOWN_VIDEO1					0x13C

#define ARK1668E_LCDC_COLOR_KEY_MASK_VALUE_VIDEO1				0x140
#define ARK1668E_LCDC_COLOR_KEY_MASK_THLD_VIDEO1				0x144
#define ARK1668E_LCDC_VIDEO1_RIGHT_BOTTOM_CUT_NUM				0x148
#define ARK1668E_LCDC_VIDEO1_SCALE_VXMOD						0x14C
#define ARK1668E_LCDC_VIDEO1_SCALE_CTL							0x150
#define ARK1668E_LCDC_VIDEO1_SCAL_CTL0							0x154
#define ARK1668E_LCDC_VIDEO1_SCAL_CTL1							0x158
#define ARK1668E_LCDC_VIDEO1_SCAL_CTL2							0x15C
#define ARK1668E_LCDC_VIDEO1_SCAL_CTL3							0x160
#define ARK1668E_LCDC_VIDEO1_SCAL_CTL4							0x164
#define ARK1668E_LCDC_VIDEO1_HSCAL_COS_VALUE				    0x168

#define ARK1668E_LCDC_VIDEO2_BURST_CTL							0x180
#define ARK1668E_LCDC_VIDEO2_CTL								0x184
#define ARK1668E_LCDC_VIDEO2_ALPHA1_ALPHA0_BLENDING_COEFF		0x188//NEW
#define ARK1668E_LCDC_VIDEO2_SOURCE_SIZE						0x18c
#define ARK1668E_LCDC_VIDEO2_WIN_POINT							0x190
#define ARK1668E_LCDC_VIDEO2_WIN_SIZE							0x194
#define ARK1668E_LCDC_VIDEO2_SIZE								0x198
#define ARK1668E_LCDC_VIDEO2_POSITION							0x19c
#define ARK1668E_LCDC_VIDEO2_ADDR1								0x1A0
#define ARK1668E_LCDC_VIDEO2_ADDR2								0x1A4
#define ARK1668E_LCDC_VIDEO2_ADDR3								0x1A8
#define ARK1668E_LCDC_VIDEO2_ADDR1_GROUP1						0x1AC
#define ARK1668E_LCDC_VIDEO2_ADDR2_GROUP1						0x1B0
#define ARK1668E_LCDC_VIDEO2_ADDR3_GROUP1						0x1B4
#define ARK1668E_LCDC_BLD_CUT_LEFT_RIGHT_VIDEO2					0x1B8
#define ARK1668E_LCDC_BLD_CUT_UP_DOWN_VIDEO2					0x1BC
#define ARK1668E_LCDC_COLOR_KEY_MASK_VALUE_VIDEO2				0x1C0
#define ARK1668E_LCDC_COLOR_KEY_MASK_THLD_VIDEO2				0x1C4

#define ARK1668E_LCDC_OSD1_BURST_CTL							0x1E0
#define ARK1668E_LCDC_OSD1_CTL									0x1E4
#define ARK1668E_LCDC_ALPHA1_0_OSD1								0x1E8
#define ARK1668E_LCDC_OSD1_SOURCE_SIZE							0x1EC
#define ARK1668E_LCDC_OSD1_SIZE									0x1F0
#define ARK1668E_LCDC_OSD1_WIN_POINT							0x1F4
#define ARK1668E_LCDC_OSD1_POSITION								0x1F8
#define ARK1668E_LCDC_OSD1_ADDR									0x1FC
#define ARK1668E_LCDC_OSD1_ADDR_GROUP1							0x200
#define ARK1668E_LCDC_BLD_CUT_LEFT_RIGHT_OSD1					0x204
#define ARK1668E_LCDC_BLD_CUT_UP_DOWN_OSD1						0x208
#define ARK1668E_LCDC_COLOR_KEY_MASK_VALUE_OSD1					0x20c
#define ARK1668E_LCDC_COLOR_KEY_MASK_THLD_OSD1					0x210


#define ARK1668E_LCDC_OSD2_BURST_CTL							0x230
#define ARK1668E_LCDC_OSD2_CTL									0x234
#define ARK1668E_LCDC_ALPHA1_0_OSD2								0x238
#define ARK1668E_LCDC_OSD2_SOURCE_SIZE							0x23C
#define ARK1668E_LCDC_OSD2_SIZE									0x240
#define ARK1668E_LCDC_OSD2_WIN_POINT							0x244
#define ARK1668E_LCDC_OSD2_POSITION								0x248
#define ARK1668E_LCDC_OSD2_ADDR									0x24C
#define ARK1668E_LCDC_OSD2_ADDR_GROUP1							0x250
#define ARK1668E_LCDC_BLD_CUT_LEFT_RIGHT_OSD2					0x254
#define ARK1668E_LCDC_BLD_CUT_UP_DOWN_OSD2						0x258
#define ARK1668E_LCDC_COLOR_KEY_MASK_VALUE_OSD2					0x25c
#define ARK1668E_LCDC_COLOR_KEY_MASK_THLD_OSD2					0x260


#define ARK1668E_LCDC_OSD3_BURST_CTL							0x280
#define ARK1668E_LCDC_OSD3_CTL									0x284
#define ARK1668E_LCDC_ALPHA1_0_OSD3								0x288
#define ARK1668E_LCDC_OSD3_SOURCE_SIZE							0x28C
#define ARK1668E_LCDC_OSD3_SIZE									0x290
#define ARK1668E_LCDC_OSD3_WIN_POINT							0x294
#define ARK1668E_LCDC_OSD3_POSITION								0x298
#define ARK1668E_LCDC_OSD3_ADDR									0x29C
#define ARK1668E_LCDC_OSD3_ADDR_GROUP1							0x2A0
#define ARK1668E_LCDC_BLD_CUT_LEFT_RIGHT_OSD3					0x2A4
#define ARK1668E_LCDC_BLD_CUT_UP_DOWN_OSD3						0x2A8
#define ARK1668E_LCDC_COLOR_KEY_MASK_VALUE_OSD3					0x2Ac
#define ARK1668E_LCDC_COLOR_KEY_MASK_THLD_OSD3					0x2B0

#define ARK1668E_LCDC_TIMING_INIT                               0x2CC
#define ARK1668E_LCDC_INTERRUPT_CTL                             0x2D0
#define ARK1668E_LCDC_INTERRUPT_STATUS                          0x2D4
#define ARK1668E_LCDC_INT_LCD_FRAME  				            (1 << 0)
#define ARK1668E_LCDC_PARAMTERS_SYNC_SWITCH                     0x2D8//new

#define ARK1668E_LCDC_GAMMA_REG_0                               0x2Dc
#define ARK1668E_LCDC_GAMMA_REG_1                               0x2E0
#define ARK1668E_LCDC_GAMMA_REG_2                               0x2E4
#define ARK1668E_LCDC_GAMMA_REG_3                               0x2E8
#define ARK1668E_LCDC_GAMMA_REG_4                               0x2Ec
#define ARK1668E_LCDC_GAMMA_REG_5                               0x2F0
#define ARK1668E_LCDC_GAMMA_REG_6                               0x2F4
#define ARK1668E_LCDC_GAMMA_REG_7                               0x2F8
#define ARK1668E_LCDC_GAMMA_REG_8                               0x2Fc
#define ARK1668E_LCDC_GAMMA_REG_9                               0x300
#define ARK1668E_LCDC_GAMMA_REG_10                              0x304
#define ARK1668E_LCDC_GAMMA_REG_11                              0x308
#define ARK1668E_LCDC_GAMMA_REG_12                              0x30c
#define ARK1668E_LCDC_GAMMA_REG_13                              0x310
#define ARK1668E_LCDC_GAMMA_REG_14                              0x314
#define ARK1668E_LCDC_GAMMA_REG_15                              0x318
#define ARK1668E_LCDC_GAMMA_REG_16                              0x31c
#define ARK1668E_LCDC_GAMMA_REG_17                              0x320
#define ARK1668E_LCDC_GAMMA_REG_18                              0x324
#define ARK1668E_LCDC_GAMMA_REG_19                              0x328
#define ARK1668E_LCDC_GAMMA_REG_20                              0x32c
#define ARK1668E_LCDC_GAMMA_REG_21                              0x330
#define ARK1668E_LCDC_GAMMA_REG_22                              0x334
#define ARK1668E_LCDC_GAMMA_REG_23                              0x338
#define ARK1668E_LCDC_GAMMA_REG_24                              0x33c
#define ARK1668E_LCDC_GAMMA_REG_25                              0x340
#define ARK1668E_LCDC_GAMMA_REG_26                              0x344
#define ARK1668E_LCDC_GAMMA_REG_27                              0x348
#define ARK1668E_LCDC_GAMMA_REG_28                              0x34c
#define ARK1668E_LCDC_GAMMA_REG_29                              0x350
#define ARK1668E_LCDC_GAMMA_REG_30                              0x354
#define ARK1668E_LCDC_GAMMA_REG_31                              0x358
#define ARK1668E_LCDC_GAMMA_REG_32                              0x35c
#define ARK1668E_LCDC_GAMMA_REG_33                              0x360
#define ARK1668E_LCDC_GAMMA_REG_34                              0x364
#define ARK1668E_LCDC_GAMMA_REG_35                              0x368
#define ARK1668E_LCDC_GAMMA_REG_36                              0x36c
#define ARK1668E_LCDC_GAMMA_REG_37                              0x370
#define ARK1668E_LCDC_GAMMA_REG_38                              0x374
#define ARK1668E_LCDC_GAMMA_REG_39                              0x378
#define ARK1668E_LCDC_GAMMA_REG_40                              0x37c
#define ARK1668E_LCDC_GAMMA_REG_41                              0x380
#define ARK1668E_LCDC_GAMMA_REG_42                              0x384
#define ARK1668E_LCDC_GAMMA_REG_43                              0x388
#define ARK1668E_LCDC_GAMMA_REG_44                              0x38c
#define ARK1668E_LCDC_GAMMA_REG_45                              0x390
#define ARK1668E_LCDC_GAMMA_REG_46                              0x394
#define ARK1668E_LCDC_GAMMA_REG_47                              0x398
#define ARK1668E_LCDC_GAMMA_REG_48                              0x39c



#define ARK1668E_LCDC_TV_GAMMA_REG_0                            0x3A0
#define ARK1668E_LCDC_TV_GAMMA_REG_1                            0x3A4
#define ARK1668E_LCDC_TV_GAMMA_REG_2                            0x3A8
#define ARK1668E_LCDC_TV_GAMMA_REG_3                            0x3AC
#define ARK1668E_LCDC_TV_GAMMA_REG_4                            0x3B0
#define ARK1668E_LCDC_TV_GAMMA_REG_5                            0x3B4
#define ARK1668E_LCDC_TV_GAMMA_REG_6                            0x3B8
#define ARK1668E_LCDC_TV_GAMMA_REG_7                            0x3BC
#define ARK1668E_LCDC_TV_GAMMA_REG_8                            0x3C0
#define ARK1668E_LCDC_TV_GAMMA_REG_9                            0x3C4
#define ARK1668E_LCDC_TV_GAMMA_REG_10                           0x3C8
#define ARK1668E_LCDC_TV_GAMMA_REG_11                           0x3CC
#define ARK1668E_LCDC_TV_GAMMA_REG_12                           0x3D0
#define ARK1668E_LCDC_TV_GAMMA_REG_13                           0x3D4
#define ARK1668E_LCDC_TV_GAMMA_REG_14                           0x3D8
#define ARK1668E_LCDC_TV_GAMMA_REG_15                           0x3DC
#define ARK1668E_LCDC_TV_GAMMA_REG_16                           0x3E0
#define ARK1668E_LCDC_TV_GAMMA_REG_17                           0x3E4
#define ARK1668E_LCDC_TV_GAMMA_REG_18                           0x3E8
#define ARK1668E_LCDC_TV_GAMMA_REG_19                           0x3EC
#define ARK1668E_LCDC_TV_GAMMA_REG_20                           0x3F0
#define ARK1668E_LCDC_TV_GAMMA_REG_21                           0x3F4
#define ARK1668E_LCDC_TV_GAMMA_REG_22                           0x3F8
#define ARK1668E_LCDC_TV_GAMMA_REG_23                           0x3FC
#define ARK1668E_LCDC_TV_GAMMA_REG_24                           0x400
#define ARK1668E_LCDC_TV_GAMMA_REG_25                           0x404
#define ARK1668E_LCDC_TV_GAMMA_REG_26                           0x408
#define ARK1668E_LCDC_TV_GAMMA_REG_27                           0x40C
#define ARK1668E_LCDC_TV_GAMMA_REG_28                           0x410
#define ARK1668E_LCDC_TV_GAMMA_REG_29                           0x414
#define ARK1668E_LCDC_TV_GAMMA_REG_30                           0x418
#define ARK1668E_LCDC_TV_GAMMA_REG_31                           0x41C
#define ARK1668E_LCDC_TV_GAMMA_REG_32                           0x420
#define ARK1668E_LCDC_TV_GAMMA_REG_33                           0x424
#define ARK1668E_LCDC_TV_GAMMA_REG_34                           0x428
#define ARK1668E_LCDC_TV_GAMMA_REG_35                           0x42C
#define ARK1668E_LCDC_TV_GAMMA_REG_36                           0x430
#define ARK1668E_LCDC_TV_GAMMA_REG_37                           0x434
#define ARK1668E_LCDC_TV_GAMMA_REG_38                           0x438
#define ARK1668E_LCDC_TV_GAMMA_REG_39                           0x43C
#define ARK1668E_LCDC_TV_GAMMA_REG_40                           0x440
#define ARK1668E_LCDC_TV_GAMMA_REG_41                           0x444
#define ARK1668E_LCDC_TV_GAMMA_REG_42                           0x448
#define ARK1668E_LCDC_TV_GAMMA_REG_43                           0x44C
#define ARK1668E_LCDC_TV_GAMMA_REG_44                           0x450
#define ARK1668E_LCDC_TV_GAMMA_REG_45                           0x454
#define ARK1668E_LCDC_TV_GAMMA_REG_46                           0x458
#define ARK1668E_LCDC_TV_GAMMA_REG_47                           0x45C
#define ARK1668E_LCDC_TV_GAMMA_REG_48                           0x460


#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_0                  0x464
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_1                  0x468
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_2                  0x46C
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_3                  0x470
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_4                  0x474
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_5                  0x478
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_6                  0x47C
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_7                  0x480
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_8                  0x484
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_9                  0x488
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_10                 0x48C
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_11                 0x490
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_12                 0x494
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_13                 0x498
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_14                 0x49C
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_15                 0x4A0
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_16                 0x4A4
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_17                 0x4A8
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_18                 0x4AC
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_19                 0x4B0
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_20                 0x4B4
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_21                 0x4B8
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_22                 0x4BC
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_23                 0x4C0
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_24                 0x4C4
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_25                 0x4C8
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_26                 0x4CC
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_27                 0x4D0
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_28                 0x4D4
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_29                 0x4D8
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_30                 0x4DC
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_31                 0x4E0
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_32                 0x4E4
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_33                 0x4E8
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_34                 0x4EC
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_35                 0x4F0
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_36                 0x4F4
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_37                 0x4F8
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_38                 0x4FC
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_39                 0x500
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_40                 0x504
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_41                 0x508
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_42                 0x50C
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_43                 0x510
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_44                 0x514
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_45                 0x518
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_46                 0x51C
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_47                 0x520
#define ARK1668E_LCDC_VIDEO1_LAYER_GAMMA_REG_48                 0x524


#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_0                  0x528
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_1                  0x52C
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_2                  0x530
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_3                  0x534
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_4                  0x538
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_5                  0x53C
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_6                  0x540
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_7                  0x544
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_8                  0x548
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_9                  0x54C
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_10                 0x550
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_11                 0x554
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_12                 0x558
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_13                 0x55C
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_14                 0x560
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_15                 0x564
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_16                 0x568
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_17                 0x56C
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_18                 0x570
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_19                 0x574
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_20                 0x578
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_21                 0x57C
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_22                 0x580
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_23                 0x584
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_24                 0x588
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_25                 0x58C
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_26                 0x590
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_27                 0x594
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_28                 0x598
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_29                 0x59C
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_30                 0x5A0
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_31                 0x5A4
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_32                 0x5A8
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_33                 0x5AC
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_34                 0x5B0
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_35                 0x5B4
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_36                 0x5B8
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_37                 0x5BC
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_38                 0x5C0
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_39                 0x5C4
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_40                 0x5C8
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_41                 0x5CC
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_42                 0x5D0
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_43                 0x5D4
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_44                 0x5DC
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_45                 0x5E8
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_46                 0x5E0
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_47                 0x5E4
#define ARK1668E_LCDC_VIDEO2_LAYER_GAMMA_REG_48                 0x5E8

#define ARK1668E_LCDC_LCD_COLOUR_MATRIX_REG0                    0x5EC
#define ARK1668E_LCDC_LCD_COLOUR_MATRIX_REG1                    0x5F0
#define ARK1668E_LCDC_LCD_COLOUR_MATRIX_REG2                    0x5F4
#define ARK1668E_LCDC_LCD_COLOUR_MATRIX_REG3                    0x5F8
#define ARK1668E_LCDC_LCD_COLOUR_MATRIX_REG4                    0x5FF
#define ARK1668E_LCDC_LCD_COLOUR_MATRIX_REG5                    0x600

#define ARK1668E_LCDC_TV_COLOUR_MATRIX_REG0                     0x604
#define ARK1668E_LCDC_TV_COLOUR_MATRIX_REG1                     0x608
#define ARK1668E_LCDC_TV_COLOUR_MATRIX_REG2                     0x60C
#define ARK1668E_LCDC_TV_COLOUR_MATRIX_REG3                     0x610
#define ARK1668E_LCDC_TV_COLOUR_MATRIX_REG4                     0x614
#define ARK1668E_LCDC_TV_COLOUR_MATRIX_REG5                     0x618

#define ARK1668E_LCDC_VIDEO1_COLOUR_MATRIX_REG0                 0x61C
#define ARK1668E_LCDC_VIDEO1_COLOUR_MATRIX_REG1                 0x620
#define ARK1668E_LCDC_VIDEO1_COLOUR_MATRIX_REG2                 0x624
#define ARK1668E_LCDC_VIDEO1_COLOUR_MATRIX_REG3                 0x628
#define ARK1668E_LCDC_VIDEO1_COLOUR_MATRIX_REG4                 0x62C
#define ARK1668E_LCDC_VIDEO1_COLOUR_MATRIX_REG5                 0x630

#define ARK1668E_LCDC_VIDEO2_COLOUR_MATRIX_REG0                 0x634
#define ARK1668E_LCDC_VIDEO2_COLOUR_MATRIX_REG1                 0x638
#define ARK1668E_LCDC_VIDEO2_COLOUR_MATRIX_REG2                 0x63C
#define ARK1668E_LCDC_VIDEO2_COLOUR_MATRIX_REG3                 0x640
#define ARK1668E_LCDC_VIDEO2_COLOUR_MATRIX_REG4                 0x644
#define ARK1668E_LCDC_VIDEO2_COLOUR_MATRIX_REG5                 0x648

#define ARK1668E_LCDC_OSD1_COLOUR_MATRIX_REG0                   0x64C
#define ARK1668E_LCDC_OSD1_COLOUR_MATRIX_REG1                   0x650
#define ARK1668E_LCDC_OSD1_COLOUR_MATRIX_REG2                   0x654
#define ARK1668E_LCDC_OSD1_COLOUR_MATRIX_REG3                   0x658
#define ARK1668E_LCDC_OSD1_COLOUR_MATRIX_REG4                   0x65C
#define ARK1668E_LCDC_OSD1_COLOUR_MATRIX_REG5                   0x660

#define ARK1668E_LCDC_OSD2_COLOUR_MATRIX_REG0                   0x664
#define ARK1668E_LCDC_OSD2_COLOUR_MATRIX_REG1                   0x668
#define ARK1668E_LCDC_OSD2_COLOUR_MATRIX_REG2                   0x66C
#define ARK1668E_LCDC_OSD2_COLOUR_MATRIX_REG3                   0x670
#define ARK1668E_LCDC_OSD2_COLOUR_MATRIX_REG4                   0x674
#define ARK1668E_LCDC_OSD2_COLOUR_MATRIX_REG5                   0x678

#define ARK1668E_LCDC_OSD3_COLOUR_MATRIX_REG0                   0x67C
#define ARK1668E_LCDC_OSD3_COLOUR_MATRIX_REG1                   0x680
#define ARK1668E_LCDC_OSD3_COLOUR_MATRIX_REG2                   0x684
#define ARK1668E_LCDC_OSD3_COLOUR_MATRIX_REG3                   0x688
#define ARK1668E_LCDC_OSD3_COLOUR_MATRIX_REG4                   0x68C
#define ARK1668E_LCDC_OSD3_COLOUR_MATRIX_REG5                   0x690
#endif /* __ARK1668E_LCDC_REGS_H__ */
