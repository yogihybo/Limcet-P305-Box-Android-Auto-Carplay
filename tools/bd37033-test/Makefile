CROSS_COMPILE ?= /home/osboxes/Downloads/linux-arkmicro/buildroot/output/host/bin/arm-buildroot-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Wextra -O2

TARGET = bd37033-test
SRC = bd37033-test.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)

.PHONY: all clean
