# Live Vehicle MCU Flashing, Recovery & SoC Reset Protocol

**Target Hardware**: STM32F105RBT6 (LQFP-64) Companion MCU in Toyota Prado 150 Head Unit  
**Host SoC**: ArkMicro ARK1668 / ARK1680 (Linux Application Processor)  
**Deliverable Image**: `hardware/MCU/live_dumps/combined_factory_boot_factory_app_64k.bin`  
**Image SHA-256**: `b385f87cbb8a183add3507c45fb6006cbad3798cbd00805338f15cc50704c161`  
**Date**: September 2026  

---

## 1. Executive Summary & Confidence Assessment

The companion MCU firmware has been 100% reconstructed and validated end-to-end on live STM32F105 silicon. A rigorous 19-phase Hardware-In-The-Loop (HIL) test suite (`tools/test_factory_app_hil.py`) confirmed:
1. **Bootloader Handoff**: `*0x20004000 = 0x20141003` authentication handshake PASS; `SCB->VTOR = 0x08003000` PASS; zero faults (`CFSR=0`, `HFSR=0`).
2. **SoC Hardware Reset (`PB14`)**: Both bootloader (`0x080017A6`) and application (`0x08006E68`) reliably assert `PB14` HIGH within 15 ms of reset, releasing the ArkMicro SoC from hardware reset.
3. **CAN Gateway**: Ring buffer (`0x20000270`, 20-byte SPL `CanRxMsg` slots) consumes and decodes Toyota Prado steering angle (`0x025`) and clearance sonar radar (`0x396`).
4. **Audio DSP Settling Delay**: Gated by hardware strap (`PC11` & `PA10` = `SoundType 3`) and 4000-tick settling delay; failsafe gracefully handles missing/unpopulated IC over `I2C1` (`PB6`/`PB7`) with zero CPU faults.
5. **Power Management**: Active keep-alive maintains `POWER_STATE_RUN` (`0x03`); idle drops cleanly to low-power `POWER_STATE_SLEEP` (`0x05`); wake events restore run state cleanly.

---

## 2. The Critical SoC Reset Line Dependency (`PB14`)

On this head unit, the ArkMicro Linux SoC's active-low hardware reset line (`RESET_N`) is wired directly to **STM32 Companion MCU pin `PB14`**.

### 2.1 Firmware Disassembly Proof
* **Bootloader (`0x080017A6`)**:
  ```arm
  0x080017A6: mov.w r1, #0x4000       ; PB14 (Pin 14)
  0x080017AA: ldr   r0, [pc, #0xe4]   ; 0x40010C00 (GPIOB_BASE)
  0x080017AC: bl    GPIO_SetBits      ; PB14 = HIGH -> Releases SoC from reset
  ```
* **Application (`0x08006E68`)**:
  ```arm
  0x08006E68: push  {r4, lr}
  0x08006E6A: mov   r4, r0
  0x08006E6C: cbz   r4, #0x8006e7a    ; If r4 == 0 (sleep/standby):
  0x08006E6E: mov.w r1, #0x4000       ; If r4 != 0 (active run):
  0x08006E72: ldr   r0, [pc, #0x14c]  ; GPIOB_BASE
  0x08006E74: bl    GPIO_SetBits      ; PB14 = HIGH (SoC Active)
  ...
  0x08006E7A: mov.w r1, #0x4000
  0x08006E7E: ldr   r0, [pc, #0x140]  ; GPIOB_BASE
  0x08006E80: bl    GPIO_ResetBits    ; PB14 = LOW  (Hold SoC in Reset)
  ```

### 2.2 Live Device RDP1 Halt Behavior
On the production vehicle unit, Readout Protection Level 1 (RDP1) is active. When an SWD probe establishes a connection:
* The Cortex-M3 core is immediately halted by silicon hardware security at reset before code runs.
* Because the CPU is halted at reset, `PB14` remains in floating input mode (pulled down by the PCB).
* **The ArkMicro SoC is held in hardware reset and the LCD screen remains completely black during probe connection.** This is expected behavior and does not indicate a bricked device.

---

## 3. SWD Pin Remap Disassembly Proof

An audit of the entire combined 64K firmware proved that **SWD is NEVER disabled**:

In both `0x08006BD2` and `0x08006CF2`:
```arm
0x08006BD0: ldr  r0, =0x00300200   ; GPIO_Remap_SWJ_JTAGDisable
0x08006BD2: bl   GPIO_PinRemapConfig
```
* `0x00300200` (`GPIO_Remap_SWJ_JTAGDisable`): Disables 5-wire JTAG pins (`PA15`, `PB3`, `PB4`) to free them for GPIO use, while **explicitly maintaining Serial Wire Debug (SWD) on `PA13` (SWDIO) and `PA14` (SWCLK)**.
* `GPIO_Remap_SWJ_Disable` (`0x00300400`) is completely absent from the binary. SWD cannot be locked out by firmware.

---

## 4. Pre-Flash Live Verification (Halted Peripheral Test)

Before unlocking or flashing, verify that `PB14` directly controls the SoC on the live unit without modifying flash memory:

```tcl
# Connect via OpenOCD (target halts due to RDP1)
openocd -f tools/pico_stm32.cfg -c "init; halt; mww 0x40021018 0x00000008; mww 0x40010C04 0x44244444; mww 0x40010C10 0x00004000; exit"
```
1. `mww 0x40021018 0x00000008`: Enables `RCC->APB2ENR` GPIOB clock.
2. `mww 0x40010C04 0x44244444`: Configures `PB14` as General Purpose Push-Pull Output (2 MHz).
3. `mww 0x40010C10 0x00004000`: Drives `PB14` HIGH via `GPIOB->BSRR`.
* **Visual Result**: The ArkMicro SoC boots immediately, displaying the boot logo on the LCD screen, even with the MCU halted.

To clamp back into reset:
```tcl
openocd -f tools/pico_stm32.cfg -c "init; halt; mww 0x40010C14 0x00004000; exit"
```

---

## 5. Safe Flashing Procedure

The flashing operation is executed atomically via [`tools/flash_live_safe.tcl`](tools/flash_live_safe.tcl) in **~3.5 seconds**:

```bash
openocd -f tools/pico_stm32.cfg -f tools/flash_live_safe.tcl
```

### Script Execution Sequence:
1. `init; reset halt`: Halts core at vector entry.
2. `stm32f1x unlock 0`: Unlocks Option Byte RDP (Level 1 $\to$ Level 0), triggering hardware mass erase.
3. `reset init`: Re-initializes clock and memory controllers.
4. `flash write_image erase ... 0x08000000 bin`: Programs full 64K combined image (`0x08000000`–`0x0800FFFF`).
5. `verify_image ... 0x08000000 bin`: Verifies byte-for-byte against silicon.
6. `reset run; exit`: Releases MCU into free-run. At $t = 15\,\text{ms}$, bootloader asserts `PB14` HIGH, and SoC boots normally.

---

## 6. Recovery Pathways Matrix

| Failure Scenario | Physical State | Recovery Pathway |
| :--- | :--- | :--- |
| **Probe Wire Disconnect Mid-Flash** | Flash is blank/partial; `PB14` low; screen black. | Re-attach SWD probe wires. Re-run `tools/flash_live_safe.tcl`. Hardware DAP remains accessible regardless of flash corruption. |
| **Flashing Verified but Application Traps** | MCU resets continuously via watchdog. | Halt via SWD (`openocd -f tools/pico_stm32.cfg -c "init; halt; reg pc; echo [mrw 0xE000ED28]; exit"`). Inspect `CFSR` fault register. Flash fallback [`combined_cleanroom_boot_factory_app_64k.bin`](../hardware/MCU/live_dumps/combined_cleanroom_boot_factory_app_64k.bin). |
| **SoC Does Not Boot Post-Flash** | MCU runs, but screen remains dark. | Run `openocd -f tools/pico_stm32.cfg -c "init; halt; echo [mrw 0x40010C0C]; resume; exit"`. Verify bit 14 is `1`. If not, force `PB14` HIGH via `BSRR` (`0x40010C10 = 0x4000`). |
| **SWD Port Unresponsive (Worst Case)** | Hardware lockup / invalid Option Bytes. | Pull `BOOT0` (pin 60) HIGH to 3.3V and pulse `NRST` low. MCU enters internal ST factory mask ROM (`0x1FFFF000`). SWD connects instantly; re-flash via OpenOCD. |
