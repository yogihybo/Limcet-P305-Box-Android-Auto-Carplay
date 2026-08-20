CROSS_COMPILE ?= arm-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

STAGE_DIR ?= $(HOME)/Downloads/bt-cross/stage
NSS_STUB ?= ../nss-stub/nss_stub.o

CFLAGS ?= -static -O2 \
	-Isrc \
	-I$(STAGE_DIR)/usr/include/dbus-1.0 \
	-I$(STAGE_DIR)/usr/lib/dbus-1.0/include

LDFLAGS ?= -static -no-pie \
	-L$(STAGE_DIR)/usr/lib \
	-ldbus-1 -lpthread \
	-Wl,--wrap=getpwnam,--wrap=getpwuid,--wrap=getpwnam_r,--wrap=getpwuid_r,--wrap=getpwent,--wrap=setpwent,--wrap=endpwent,--wrap=getgrgid,--wrap=getgrgid_r,--wrap=getgrouplist,--wrap=dlopen,--wrap=dlerror,--wrap=dlsym,--wrap=dlclose,--wrap=getaddrinfo

TARGET = bt-agent

all: $(NSS_STUB) $(TARGET)

$(NSS_STUB):
	$(CC) -c -O2 -o $(NSS_STUB) ../nss-stub/nss_stub.c

$(TARGET): src/main.c $(NSS_STUB)
	$(CC) $(CFLAGS) src/main.c $(NSS_STUB) $(LDFLAGS) -o $(TARGET)
	$(STRIP) $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
