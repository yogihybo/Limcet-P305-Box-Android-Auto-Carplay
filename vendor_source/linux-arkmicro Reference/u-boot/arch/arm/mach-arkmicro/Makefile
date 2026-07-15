# SPDX-License-Identifier: GPL-2.0+

ifneq ($(CONFIG_SPL_BUILD),)
obj-$(CONFIG_TARGET_ARK1668) += spl_ark1668.o ddr_ark1668.o
obj-$(CONFIG_TARGET_ARK1668_TYW_ZKSW) += spl_ark1668.o ddr_ark1668.o
obj-$(CONFIG_TARGET_TYW_DASHBOARD) += spl_ark1668.o ddr_ark1668.o
obj-$(CONFIG_TARGET_XINSHIYU) += spl_ark1668.o ddr_ark1668.o
obj-$(CONFIG_TARGET_ARK1668_FT) += spl_ark1668.o ddr_ark1668.o
obj-$(CONFIG_TARGET_ARK1668_AOFAN) += spl_ark1668_aofan.o ddr_ark1668_aofan.o
obj-$(CONFIG_TARGET_ARK1668_DONGLE_SIM) += spl_ark1668_dongle_sim.o ddr_ark1668_dongle_sim.o
obj-$(CONFIG_TARGET_ARK1668E_FPGA) += spl_ark1668e_fpga.o ddr_ark1668e_fpga.o
obj-$(CONFIG_TARGET_ARK1668E_DEVB) += spl_ark1668e.o ddr_ark1668e.o
obj-$(CONFIG_TARGET_ARK1668E_DEVB_DASHBOARD) += spl_ark1668e.o ddr_ark1668e.o
obj-$(CONFIG_TARGET_ARK1668E_DEVB_EMMC) += spl_ark1668e.o ddr_ark1668e.o
obj-y += spl.o
endif

obj-$(CONFIG_CPU_V7A)		+= armv7/
