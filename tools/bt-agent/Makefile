CROSS_COMPILE ?= $(HOME)/Downloads/linux-arkmicro/buildroot/output/host/bin/arm-buildroot-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

BR_STAGING ?= $(HOME)/Downloads/linux-arkmicro/buildroot/output/staging
STAGE_DIR ?= $(BR_STAGING)
ALSA_DIR ?= $(BR_STAGING)/usr
NSS_STUB ?= ../nss-stub/nss_stub.o

CFLAGS ?= -O2 \
	-Isrc \
	-Isbc \
	-I$(STAGE_DIR)/usr/include/dbus-1.0 \
	-I$(STAGE_DIR)/usr/lib/dbus-1.0/include \
	-I$(ALSA_DIR)/include

LDFLAGS ?= \
	-L$(STAGE_DIR)/usr/lib \
	-L$(ALSA_DIR)/lib \
	-ldbus-1 -lasound -lpthread -lm

TARGET = bt-agent
SRCS = src/main.c sbc/sbc.c sbc/sbc_primitives.c sbc/sbc_primitives_armv6.c sbc/sbc_primitives_iwmmxt.c sbc/sbc_primitives_neon.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET)
	$(STRIP) $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
