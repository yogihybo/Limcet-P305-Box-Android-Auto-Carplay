CROSS_COMPILE ?= arm-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

STAGE_DIR ?= $(HOME)/Downloads/bt-cross/stage
ALSA_DIR ?= $(HOME)/build-deps/alsa-arm-install-1.2.4
NSS_STUB ?= ../nss-stub/nss_stub.o

CFLAGS ?= -static -O2 \
	-Isrc \
	-Isbc \
	-I$(STAGE_DIR)/usr/include/dbus-1.0 \
	-I$(STAGE_DIR)/usr/lib/dbus-1.0/include \
	-I$(ALSA_DIR)/include

LDFLAGS ?= -static -no-pie \
	-L$(STAGE_DIR)/usr/lib \
	-L$(ALSA_DIR)/lib \
	-ldbus-1 -lasound -lpthread -lm \
	-Wl,--wrap=getpwnam,--wrap=getpwuid,--wrap=getpwnam_r,--wrap=getpwuid_r,--wrap=getpwent,--wrap=setpwent,--wrap=endpwent,--wrap=getgrgid,--wrap=getgrgid_r,--wrap=getgrouplist,--wrap=dlopen,--wrap=dlerror,--wrap=dlsym,--wrap=dlclose,--wrap=getaddrinfo

TARGET = bt-agent
SRCS = src/main.c sbc/sbc.c sbc/sbc_primitives.c sbc/sbc_primitives_armv6.c sbc/sbc_primitives_iwmmxt.c sbc/sbc_primitives_neon.c

all: $(NSS_STUB) $(TARGET)

$(NSS_STUB):
	$(CC) -c -O2 -o $(NSS_STUB) ../nss-stub/nss_stub.c

$(TARGET): $(SRCS) $(NSS_STUB)
	$(CC) $(CFLAGS) $(SRCS) $(NSS_STUB) $(LDFLAGS) -o $(TARGET)
	$(STRIP) $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
