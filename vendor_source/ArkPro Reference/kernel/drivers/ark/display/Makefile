ifeq ($(CONFIG_ARK_DISP_HW_EMULATION),y)
EXTRA_CFLAGS += -DARK_DISP_USE_HW_EMULATION
endif
ifeq ($(CONFIG_ARK_DISP_DEBUG),y)
EXTRA_CFLAGS += -DARK_DISP_DBG
endif
ifeq ($(CONFIG_ARK_DISP_OUTPUT_EMULATION),y)
EXTRA_CFLAGS += -DARK_DISP_USE_OUTPUT_EMULATION
endif

arkdisplay-objs :=                  \
		ark_display_drv.o   \
		ark_display_core.o  \
		ark_display_osd.o   \
		ark_display_video.o \
		ark_display_lcd.o   \
		ark_display_tvenc.o \
		ark_display_fb.o    \
                fb_ioctl.o


	#ark_display_v4l2.o  

obj-$(CONFIG_ARK_DISP) += arkdisplay.o


