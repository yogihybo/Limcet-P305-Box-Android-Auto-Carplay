# Atomic OpenOCD Flashing Script for Live STM32F105 Vehicle Module
gdb_port disabled
tcl_port disabled
telnet_port disabled

init
reset halt

echo "=================================================================="
echo " LIVE VEHICLE MCU ATOMIC REFLASH & VERIFICATION"
echo " Target: STM32F105RBT6 Companion MCU"
echo " Image:  combined_factory_boot_factory_app_64k.bin (64 KiB)"
echo "=================================================================="

# 1. Unlock RDP Level 1 (triggers silicon hardware mass erase)
echo "\n--- Step 1: Unlocking Readout Protection (RDP) ---"
stm32f1x unlock 0

# 2. Reset and re-initialize bus matrix
reset init

# 3. Program full 64K combined image (Factory Bootloader + Reconstructed Factory App)
echo "\n--- Step 2: Writing 64K Combined Image ---"
flash write_image erase hardware/MCU/live_dumps/combined_factory_boot_factory_app_64k.bin 0x08000000 bin

# 4. Verify memory byte-for-byte
echo "\n--- Step 3: Verifying Silicon Memory ---"
verify_image hardware/MCU/live_dumps/combined_factory_boot_factory_app_64k.bin 0x08000000 bin

# 5. Release into free-run mode so PB14 (SoC reset) goes HIGH within 15ms
echo "\n--- Step 4: Reset & Release into Production Run Mode ---"
reset run
echo "\nFlash complete. MCU is executing and ArkMicro SoC reset is released."
exit
