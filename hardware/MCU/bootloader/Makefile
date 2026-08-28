# Makefile for STM32F105 Resident IAP Bootloader (16 KB)
TARGET   = bootloader
BUILD_DIR = build

# Toolchain
CC       = arm-none-eabi-gcc
OBJCOPY  = arm-none-eabi-objcopy
OBJDUMP  = arm-none-eabi-objdump
SIZE     = arm-none-eabi-size

# Fallback cross-compiler check
ifeq (, $(shell which $(CC) 2>/dev/null))
    CC      = arm-linux-gnueabihf-gcc
    OBJCOPY = arm-linux-gnueabihf-objcopy
    OBJDUMP = arm-linux-gnueabihf-objdump
    SIZE    = arm-linux-gnueabihf-size
endif

# Flags
MCU_FLAGS = -mcpu=cortex-m3 -mthumb -mfloat-abi=soft
INCLUDES  = -Iinclude
CFLAGS    = $(MCU_FLAGS) $(INCLUDES) -Os -Wall -Wextra -ffreestanding -nostdlib -fdata-sections -ffunction-sections -fno-builtin
LDFLAGS   = $(MCU_FLAGS) -Tstm32f105_bootloader.ld -nostdlib -static -Wl,--build-id=none -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(TARGET).map

SRCS = src/startup_stm32f105.c \
       src/main.c \
       src/ymodem.c

OBJS = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SRCS))

all: $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).hex

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJS)
	@echo "  LD    $@"
	@$(CC) $(OBJS) $(LDFLAGS) -o $@

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	@echo "  BIN   $@"
	@$(OBJCOPY) -O binary $< $@
	@echo "  HEX   $(BUILD_DIR)/$(TARGET).hex"
	@$(OBJCOPY) -O ihex $< $(BUILD_DIR)/$(TARGET).hex
	@echo "  DISAS $(BUILD_DIR)/$(TARGET).lst"
	@$(OBJDUMP) -d -S $< > $(BUILD_DIR)/$(TARGET).lst
	@echo "======================================================================"
	@echo " Build Completed Successfully: $@"
	@echo "======================================================================"
	@$(SIZE) $<

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
