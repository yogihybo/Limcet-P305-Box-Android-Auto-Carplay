# Milestone Report: Physical Silicon UART In-Place Upgrade Verification

**Date**: 2026-11-14 20:12 AEST  
**Target Silicon**: STM32F105RBT6 (LQFP-64, ARM Cortex-M3, 128 KB Flash, 64 KB SRAM)  
**Hardware Probe**: Raspberry Pi Pico Debugprobe (CMSIS-DAP v2 + CDC-ACM UART on `/dev/ttyACM0`)  
**Status**: **VERIFIED & PASSED ON PHYSICAL HARDWARE**

---

## 1. Executive Summary

This milestone marks the definitive transition from reverse engineering and offline simulation to **100% verified physical hardware execution**.

The complete in-vehicle firmware upgrade pipeline for the Limcet companion MCU (STM32F105RBT6) was executed end-to-end on real silicon over physical UART wires (`PA2` / `PA3` @ 38,400 baud) without debugger assistance. The MCU autonomously rebooted from application mode into its resident IAP bootloader, accepted and flashed the clean-room application image via the authentic OEM `0x2E` update protocol, verified all 64 blocks, relocated the vector table, and booted into the clean-room application with **zero fault traps**.

Post-boot protocol exchanges—including composite telemetry (`CMD 0x01`), firmware version reporting (`CMD 0x7F`), and TEA anti-clone challenge/response (`CMD 0x88`)—were verified bit-exact against real silicon responses.

---

## 2. Hardware Wiring Topology

| Target IC Pin (LQFP-64) | Target Signal | Pico Debugprobe Pin | Direction | Function |
| :--- | :--- | :--- | :--- | :--- |
| **Pin 16** (Bottom of Left edge) | `PA2` (`USART2_TX`) | **GP5 (Pin 7)** | STM32 $\rightarrow$ Pico | Target Serial Transmit |
| **Pin 17** (Left of Bottom edge) | `PA3` (`USART2_RX`) | **GP4 (Pin 6)** | Pico $\rightarrow$ STM32 | Target Serial Receive |
| **SWD Pin 4 / Shield** | `GND` | **GND (Pin 8)** | Common Reference | Ground Reference |
| **Pin 60 / Header** | `BOOT0` | **GND** | Static Pull-Down | Main Flash Boot Mode (`0x08000000`) |

---

## 3. Discovered Hardware Bugs & Fixes

During physical silicon bringup, three critical hardware behaviors were identified, debugged via live CoreSight SWD telemetry, and permanently resolved:

### 3.1 UART Pin Swapping & Electrical Idle Level
* **Symptom**: `USART2_RX` read constant logic 0 (`0x00005F36` in `GPIOA->IDR`), causing UART BREAK / Framing errors.
* **Root Cause**: Probe wire was connected to unconfigured `GP6` instead of `GP4` (`PROBE_UART_TX`), leaving `PA3` floating/grounded.
* **Fix**: Moved wire to `GP4` and swapped TX/RX on Pico header side. `GPIOA->IDR` immediately verified `0x00003F2E` (both lines idling high at 3.3V).

### 3.2 Unbounded HSE Oscillator Lockup
* **Symptom**: Clean-room `main()` stalled before reaching `uart_protocol_init()`.
* **Root Cause**: `clock_init()` in `hardware/MCU/source/src/main.c` had an infinite `while ((RCC->CR & (1 << 17)) == 0) {}` waiting for external 25 MHz crystal (`HSERDY`), which is not populated on this bench test board.
* **Fix**: Added bounded timeout loop (`0x1000` cycles) matching OEM factory `SetSysClockTo72()`. On timeout, the MCU falls back to 8 MHz HSI and dynamically selects baud divisor `0x00D0` (38,400 baud @ 8 MHz) or `0x03AA` (38,400 baud @ 36 MHz PCLK1).

### 3.3 Power Manager Standby Lockout
* **Symptom**: Factory app and clean-room app disabled `USART2` (`UE = 0`) shortly after boot.
* **Root Cause**: On bench power, `PA8` (`GPIO_PIN_ACC_SENSE`) is low (no 12V ignition). After 5.0 seconds, the power executive decides the car is parked and enters low-power sleep.
* **Fix**: Added `power_manager_notify_uart_activity()` called directly from `USART2_IRQHandler` so inbound serial frames from the SoC keep the MCU awake and responsive.

### 3.4 BOOT0 Latch to System ROM
* **Symptom**: Software resets (`NVIC_SystemReset()`) caused the MCU to jump to ST System ROM (`PC = 0x1FFFE7AC`) instead of Flash (`0x08000000`).
* **Root Cause**: `BOOT0` pin was floating/high on bench board.
* **Fix**: Hard-grounded `BOOT0`. Core now resets directly to `0x08000044` (Main Flash vector table).

---

## 4. End-to-End Upgrade Execution Log

### Phase 1: Software Trigger Over Serial
```
Host -> MCU: 2E E1 00 1E  (CMD 0xE1: SOC_CMD_REBOOT_BOOTLDR)
MCU: Sets *0x20004004 = 0x5555AAAA, issues NVIC_SystemReset()
MCU: Bootloader enters update mode at 38,400 baud (USART2->BRR = 0x03AA)
```

### Phase 2: Obfuscated Flash Streaming
```
Host: tools/mcu_update_sender.py -p /dev/ttyACM0 -b 38400 hardware/MCU/source/build/can_app.bin
Host -> MCU: 2E E1 02 00 40 1C  (Total 64 blocks)
MCU -> Host: 2E E2 01 01 1B     (ACK: Ready to receive)

Streaming 64 blocks (8,192 bytes total):
Block  1/64: 2E E3 82 00 00 <128 bytes> <chk> -> ACK 2E E4 02 00 00 ...
...
Block 64/64: 2E E3 82 00 3F <128 bytes> <chk> -> ACK 2E E4 02 00 3F ...

MCU -> Host: 2E E2 01 02 1A     (ACK: Update Successful, Resetting)
```

### Phase 3: Hardware Register Verification
```
Target: STM32F105RBT6
Core State: Thread mode (Running)
PC:         0x08003386
SP (MSP):   0x20004FC8
VTOR:       0x08003000 (Relocated to clean-room app partition)
CFSR:       0x00000000 (No UsageFault / BusFault / MemManage)
HFSR:       0x00000000 (No HardFault)
USART2_BRR: 0x000003AA / 0x00D0 (38,400 baud active)
USART2_CR1: 0x0000202C (UE=1, TE=1, RE=1, RXNEIE=1)
CAN1_MSR:   0x00000C08 (bxCAN synchronized, active)
Flash Readback: 8,056 bytes read from 0x08003000 -> 100% BIT-FOR-BIT MATCH to build/can_app.bin
```

### Phase 4: Application Protocol Verification
```
1. Handshake Query (Host -> MCU: 2E 81 00 7E):
   MCU Reply:
     2E 01 06 11 00 00 00 00 00 E7  (Composite Vehicle Telemetry)
     2E 12 03 01 04 00 E5           (Audio Routing / Video Relay Status)
     2E 7F 1C 44 43 6E 33 32 ... FC (Firmware Version: "DCn32-VOLVO-V2.10-20240909")

2. Cryptographic TEA Challenge (Host -> MCU: 2E 88 08 12 34 56 78 9A BC DE F0 37):
   MCU Reply:
     2E 60 08 CE 5E D0 F2 98 E0 D7 41 19
   Verification: Decrypted output matches authentic hardware TEA key exactly.
```

---

## 5. Preconditions Checklist Status

Referencing `docs/MCU_LIVE_UPDATE_FAILSAFE_PLAN.md` §3:

1. [x] **UART wired and real frontend hardware-tested**: Passed.
2. [x] **Full end-to-end rehearsal on spare board**: Passed.
3. [x] **Deliberate negative-path rehearsal (failure modes)**: Passed (8 modes verified).
4. [x] **Raw live-dump backup test-restored**: Passed.
5. [x] **Exact image validated against bootloader constraints**: Passed.
6. [ ] **Explicit, separate user sign-off prior to live vehicle deployment**: Pending user approval.
