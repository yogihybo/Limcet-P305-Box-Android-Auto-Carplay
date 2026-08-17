# ==============================================================================
# Makefile for STM32F105 Companion MCU Firmware (can_app.bin)
# ==============================================================================

# Toolchain detection
CC      := $(shell which arm-none-eabi-gcc 2>/dev/null || which arm-linux-gnueabihf-gcc)
OBJCOPY := $(shell which arm-none-eabi-objcopy 2>/dev/null || which arm-linux-gnueabihf-objcopy || which objcopy)
OBJDUMP := $(shell which arm-none-eabi-objdump 2>/dev/null || which arm-linux-gnueabihf-objdump || which objdump)
SIZE    := $(shell which arm-none-eabi-size 2>/dev/null || which arm-linux-gnueabihf-size || which size)

# Project Name
TARGET   = can_app
BUILD_DIR = build

# Source Files
C_SOURCES = \
    src/startup_stm32f105.c \
    src/main.c \
    src/can_driver.c \
    src/uart_protocol.c \
    src/vehicle_profiles.c

# Includes
INCLUDES = -Iinclude

# Compiler Flags
MCU_FLAGS = -mcpu=cortex-m3 -mthumb -mfloat-abi=soft
CFLAGS    = $(MCU_FLAGS) $(INCLUDES) -Os -Wall -Wextra -ffreestanding -nostdlib -fdata-sections -ffunction-sections -fno-builtin
LDFLAGS   = $(MCU_FLAGS) -Tstm32f105_app.ld -nostdlib -static -Wl,--build-id=none -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(TARGET).map

# Object Files
OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))

all: $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).lst
	@echo "======================================================================"
	@echo " Build Completed Successfully: $(BUILD_DIR)/$(TARGET).bin"
	@echo "======================================================================"
	@$(SIZE) $(BUILD_DIR)/$(TARGET).elf

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	@echo "  CC    $<"
	@$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS)
	@echo "  LD    $@"
	@$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	@echo "  BIN   $@"
	@$(OBJCOPY) -O binary -S $< $@

$(BUILD_DIR)/$(TARGET).hex: $(BUILD_DIR)/$(TARGET).elf
	@echo "  HEX   $@"
	@$(OBJCOPY) -O ihex $< $@

$(BUILD_DIR)/$(TARGET).lst: $(BUILD_DIR)/$(TARGET).elf
	@echo "  DISAS $@"
	@$(OBJDUMP) -d -S $< > $@

$(BUILD_DIR):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
