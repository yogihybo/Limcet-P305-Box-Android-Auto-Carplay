# Micro-AAP standalone Makefile
# Zero Boost, Zero Google Protobuf, Zero Abseil dependencies.

BUILDROOT_OUTPUT_DIR ?= $(HOME)/Downloads/linux-arkmicro/buildroot/output
BUILDROOT_STAGING    ?= $(BUILDROOT_OUTPUT_DIR)/staging
CROSS_COMPILE        ?= $(BUILDROOT_OUTPUT_DIR)/host/bin/arm-buildroot-linux-gnueabihf-

CC      := $(CROSS_COMPILE)gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -O2 -s \
           -Iinclude \
           -Iproto_gen \
           -I../third_party/nanopb \
           -I$(BUILDROOT_STAGING)/usr/include

LDFLAGS := -s -L$(BUILDROOT_STAGING)/usr/lib
LIBS    := -lssl -lcrypto -lasound -lpthread -ldl -lrt

BUILD_DIR := build

SRCS_CORE  := src/aap_wire.c src/aap_cryptor.c src/aap_audio.c src/aap_video.c src/aap_session.c src/main.c
SRCS_NANOPB := ../third_party/nanopb/pb_common.c ../third_party/nanopb/pb_encode.c ../third_party/nanopb/pb_decode.c
SRCS_PROTO := $(shell find proto_gen -name "*.pb.c")

OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS_CORE)) \
        $(patsubst ../third_party/nanopb/%.c,$(BUILD_DIR)/nanopb/%.o,$(SRCS_NANOPB)) \
        $(patsubst proto_gen/%.pb.c,$(BUILD_DIR)/proto/%.pb.o,$(SRCS_PROTO))

TARGET := $(BUILD_DIR)/micro-androidauto-sidecar

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Built $(TARGET)"
	@ls -lh $(TARGET)
	@size $(TARGET)

$(BUILD_DIR)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/nanopb/%.o: ../third_party/nanopb/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/proto/%.pb.o: proto_gen/%.pb.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR)
