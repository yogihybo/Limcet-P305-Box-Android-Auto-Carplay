# Atomic OpenOCD Flashing Script for Live STM32F105 Vehicle Module
#
# DO NOT invoke this file directly. Run it via tools/flash_live_vehicle.sh,
# which performs the pre-flight image checks and the operator confirmation
# prompts. This script re-checks its own preconditions and refuses to run
# if it was not launched through that wrapper.
gdb_port disabled
tcl_port disabled
telnet_port disabled

set IMAGE "hardware/MCU/live_dumps/combined_factory_boot_factory_app_64k.bin"
set EXPECTED_SHA256 "b385f87cbb8a183add3507c45fb6006cbad3798cbd00805338f15cc50704c161"
set EXPECTED_SIZE 65536

proc abort_flash {msg} {
    echo "\n=================================================================="
    echo " ABORTED"
    echo " $msg"
    echo "=================================================================="
    error $msg
}

# --- Guard 1: refuse to run outside tools/flash_live_vehicle.sh ---
if {![info exists ::env(FLASH_LIVE_GUARD_FILE)]} {
    abort_flash "This script must be run via tools/flash_live_vehicle.sh, not invoked directly."
}
if {![file exists $::env(FLASH_LIVE_GUARD_FILE)]} {
    abort_flash "Confirmation guard file missing -- wrapper did not confirm. Re-run tools/flash_live_vehicle.sh."
}

# --- Guard 2: re-verify the image on disk, independent of the wrapper ---
if {![file exists $IMAGE]} {
    abort_flash "Image not found: $IMAGE"
}
set actual_size [file size $IMAGE]
if {$actual_size != $EXPECTED_SIZE} {
    abort_flash "Image is $actual_size bytes, expected exactly $EXPECTED_SIZE."
}
set actual_sha256 [lindex [split [exec sha256sum $IMAGE]] 0]
if {$actual_sha256 ne $EXPECTED_SHA256} {
    abort_flash "Image SHA-256 mismatch.\n  expected: $EXPECTED_SHA256\n  actual:   $actual_sha256"
}

init
reset halt

echo "=================================================================="
echo " LIVE VEHICLE MCU ATOMIC REFLASH & VERIFICATION"
echo " Target: STM32F105RBT6 Companion MCU"
echo " Image:  $IMAGE"
echo " SHA256: $actual_sha256 (verified)"
echo "=================================================================="

# --- Guard 3: confirm this is really an STM32F1 before touching flash ---
set idcode_line [capture "flash banks"]
if {![string match "*stm32f1x*" $idcode_line]} {
    abort_flash "Target does not report as stm32f1x (got: $idcode_line). Wrong probe/target config?"
}

# 1. Unlock RDP Level 1 (triggers silicon hardware mass erase)
echo "\n--- Step 1: Unlocking Readout Protection (RDP) ---"
if {[catch {stm32f1x unlock 0} err]} {
    abort_flash "RDP unlock failed: $err\nMass erase may be incomplete -- see recovery doc section 6 before retrying."
}

# 2. Reset and re-initialize bus matrix
reset init

# 3. Program full 64K combined image (Factory Bootloader + Reconstructed Factory App)
echo "\n--- Step 2: Writing 64K Combined Image ---"
if {[catch {flash write_image erase $IMAGE 0x08000000 bin} err]} {
    abort_flash "Flash write failed: $err\nDevice flash is now blank/partial. Do not power-cycle or disconnect the\nprobe -- re-run tools/flash_live_vehicle.sh to retry while still connected.\nSee docs/LIVE_VEHICLE_FLASH_AND_RECOVERY_PROTOCOL.md section 6."
}

# 4. Verify memory byte-for-byte -- MUST pass before the target is ever
# allowed to run, since a partial/incorrect image left running could
# assert PB14 in an undefined way.
echo "\n--- Step 3: Verifying Silicon Memory ---"
if {[catch {verify_image $IMAGE 0x08000000 bin} err]} {
    abort_flash "Post-write verification FAILED: $err\nTarget is left HALTED (not released to run) so it cannot execute a bad\nimage. Re-run tools/flash_live_vehicle.sh to reflash before disconnecting."
}

# 5. Release into free-run mode so PB14 (SoC reset) goes HIGH within 15ms
echo "\n--- Step 4: Reset & Release into Production Run Mode ---"
reset run
echo "\nFlash complete and verified. MCU is executing and ArkMicro SoC reset is released."
exit
