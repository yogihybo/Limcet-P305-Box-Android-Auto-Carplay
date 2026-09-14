# OpenOCD TCL Script for Rigorous HIL Validation of Factory Firmware
gdb_port disabled
tcl_port disabled
telnet_port disabled

init

echo "============================================================================"
echo " RIGOROUS HARDWARE-IN-THE-LOOP SILICON TEST REPORT"
echo " Board: STM32F105RBT6 Companion MCU"
echo " Firm:  Authentic Factory Bootloader + 100% Reconstructed Factory App (64K)"
echo "============================================================================"

# --- Phase 1: Reset to Bootloader Entry ---
echo "\n--- PHASE 1: Cold Boot & Bootloader Entry ---"
reset halt
set boot_pc [reg pc]
set boot_msp [reg msp]
echo "Bootloader Reset PC: $boot_pc"
echo "Bootloader Initial MSP: $boot_msp"

# --- Phase 2: Execution Handoff ---
echo "\n--- PHASE 2: Bootloader Execution & Handshake ---"
resume
sleep 500
halt

set app_pc [reg pc]
set app_msp [reg msp]
set app_xpsr [reg xPSR]
set hs [mrw 0x20004000]

echo "Live Execution PC: $app_pc"
echo "Application MSP: $app_msp"
echo "Application xPSR: $app_xpsr"
echo "Bootloader Handshake (*0x20004000): [format 0x%08X $hs]"

# --- Phase 3: Core Health & Fault Registers ---
echo "\n--- PHASE 3: System Health & Vector Table Relocation ---"
set vtor [mrw 0xE000ED08]
set cfsr [mrw 0xE000ED28]
set hfsr [mrw 0xE000ED2C]
echo "SCB->VTOR: [format 0x%08X $vtor]"
echo "CFSR: [format 0x%08X $cfsr]"
echo "HFSR: [format 0x%08X $hfsr]"

# --- Phase 4: Clock & Power Trees ---
echo "\n--- PHASE 4: Clock & Power System ---"
set rcc_cr [mrw 0x40021000]
set rcc_cfgr [mrw 0x40021004]
echo "RCC->CR:   [format 0x%08X $rcc_cr]"
echo "RCC->CFGR: [format 0x%08X $rcc_cfgr]"

# --- Phase 5: Hardware Peripherals ---
echo "\n--- PHASE 5: Peripheral Status ---"
set gpiob_odr [mrw 0x40010C0C]
set can1_msr  [mrw 0x40006404]
set can1_btr  [mrw 0x4000641C]
set can1_ier  [mrw 0x40006414]
set u2_cr1    [mrw 0x4000440C]
set u2_brr    [mrw 0x40004408]
set iser0     [mrw 0xE000E100]
set iser1     [mrw 0xE000E104]

echo "GPIOB->ODR (ARK1668 Reset Line PB14): [format 0x%08X $gpiob_odr]"
echo "CAN1->MSR:  [format 0x%08X $can1_msr]"
echo "CAN1->BTR:  [format 0x%08X $can1_btr]"
echo "CAN1->IER:  [format 0x%08X $can1_ier]"
echo "USART2->CR1: [format 0x%08X $u2_cr1]"
echo "USART2->BRR: [format 0x%08X $u2_brr]"
echo "NVIC->ISER0: [format 0x%08X $iser0]"
echo "NVIC->ISER1: [format 0x%08X $iser1]"

# --- Phase 6: Dynamic Cooperative Scheduler (13 Tasks) ---
echo "\n--- PHASE 6: Dynamic Task Scheduler Verification (13 Tasks) ---"
set t0_str ""
for {set i 0} {$i < 13} {incr i} {
    set addr [expr {0x200000CC + $i * 4}]
    append t0_str [format "T%02d:0x%08X " $i [mrw $addr]]
}
echo "Task Table T=0: $t0_str"

resume
sleep 1500
halt

set t1_str ""
for {set i 0} {$i < 13} {incr i} {
    set addr [expr {0x200000CC + $i * 4}]
    append t1_str [format "T%02d:0x%08X " $i [mrw $addr]]
}
echo "Task Table T=1.5s: $t1_str"

# --- Phase 7: Sustained Stress Free-Run (5.0s) ---
echo "\n--- PHASE 7: Sustained Free-Run Stability Stress Test (5.0s) ---"
resume
sleep 5000
halt

set stress_pc [reg pc]
set stress_msp [reg msp]
set stress_cfsr [mrw 0xE000ED28]
set stress_hfsr [mrw 0xE000ED2C]
set stress_vtor [mrw 0xE000ED08]

echo "After 5s Free-Run PC: $stress_pc"
echo "After 5s Free-Run MSP: $stress_msp"
echo "After 5s Free-Run CFSR: [format 0x%08X $stress_cfsr]"
echo "After 5s Free-Run HFSR: [format 0x%08X $stress_hfsr]"
echo "After 5s Free-Run VTOR: [format 0x%08X $stress_vtor]"

# Resume free run for hardware operational readiness
resume
echo "\n=== COMPLETE: Target left free-running in active state ==="
shutdown
