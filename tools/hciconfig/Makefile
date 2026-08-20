CROSS_COMPILE ?= arm-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip
CFLAGS ?= -static -O2 -Isrc -DVERSION=\"5.66\" -DSTORAGEDIR=\"/var/lib/bluetooth\"

SRCS = \
	src/hciconfig.c \
	src/lib/bluetooth.c \
	src/lib/hci.c \
	src/src/textfile.c \
	src/src/shared/util.c

TARGET = hciconfig

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)
	$(STRIP) $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
