ifeq ($(CONFIG_ARK_PWM_HW_EMULATION),y)
EXTRA_CFLAGS += -DARK_PWM_USE_HW_EMULATION
endif
ifeq ($(CONFIG_ARK_PWM_DEBUG),y)
EXTRA_CFLAGS += -DARK_PWM_DBG
endif

arkpwm-objs :=                  \
		ark_pwm.o 

obj-$(CONFIG_ARK_PWM) += arkpwm.o
