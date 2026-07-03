obj-y += cpu.o
obj-y += reset.o
obj-y += clock.o
ifdef CONFIG_ARCH_ARKE
obj-y += timer-arke.o
else
obj-y += timer.o
obj-y += gpio.o
endif
obj-y += mcu_serial.o
obj-y += early_printf.o
obj-y += URBprobar.o
obj-y += ark_common.o
ifdef CONFIG_ARK1668EFAMILY
ifeq ($(CONFIG_SPL_BUILD),)
obj-y += ark1668e_display_cfg.o
obj-y += ark1668e_lcd.o
endif
endif
