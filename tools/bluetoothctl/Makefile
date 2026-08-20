CROSS_COMPILE ?= arm-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

STAGE_DIR ?= $(HOME)/Downloads/bt-cross/stage
NSS_STUB ?= ../nss-stub/nss_stub.o

CFLAGS ?= -static -O2 \
	-Isrc \
	-Isrc/src \
	-Isrc/lib \
	-Isrc/client \
	-I$(STAGE_DIR)/usr/include \
	-I$(STAGE_DIR)/usr/include/glib-2.0 \
	-I$(STAGE_DIR)/usr/lib/glib-2.0/include \
	-I$(STAGE_DIR)/usr/include/dbus-1.0 \
	-I$(STAGE_DIR)/usr/lib/dbus-1.0/include \
	-DVERSION=\"5.66\"

LDFLAGS ?= -static -no-pie \
	-L$(STAGE_DIR)/usr/lib \
	-lreadline -ltinfow -ldbus-1 -lglib-2.0 -lpcre2-8 -lm -lpthread \
	-Wl,--wrap=getpwnam,--wrap=getpwuid,--wrap=getpwnam_r,--wrap=getpwuid_r,--wrap=getpwent,--wrap=setpwent,--wrap=endpwent,--wrap=getgrgid,--wrap=getgrgid_r,--wrap=getgrouplist,--wrap=dlopen,--wrap=dlerror,--wrap=dlsym,--wrap=dlclose,--wrap=getaddrinfo

SRCS = \
	src/client/main.c \
	src/client/display.c \
	src/client/agent.c \
	src/client/gatt.c \
	src/client/advertising.c \
	src/client/adv_monitor.c \
	src/client/admin.c \
	src/client/player.c \
	src/gdbus/object.c \
	src/gdbus/watch.c \
	src/gdbus/mainloop.c \
	src/gdbus/polkit.c \
	src/gdbus/client.c \
	src/src/shared/shell.c \
	src/src/shared/util.c \
	src/src/shared/io-glib.c \
	src/src/shared/timeout-glib.c \
	src/src/shared/mainloop-glib.c \
	src/src/shared/mainloop-notify.c \
	src/src/shared/queue.c \
	src/src/shared/log.c \
	src/lib/bluetooth.c \
	src/lib/hci.c

TARGET = bluetoothctl

all: $(NSS_STUB) $(TARGET)

$(NSS_STUB):
	$(CC) -c -O2 -o $(NSS_STUB) ../nss-stub/nss_stub.c

$(TARGET): $(SRCS) $(NSS_STUB)
	$(CC) $(CFLAGS) $(SRCS) $(NSS_STUB) $(LDFLAGS) -o $(TARGET)
	$(STRIP) $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
