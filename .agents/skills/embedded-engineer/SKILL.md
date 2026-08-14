---
name: embedded-engineer
description: Skill for a highly trained embedded systems design coding engineer who generates accurate, clean, production-grade embedded C/C++, Linux kernel driver, U-Boot, and low-level firmware code without guessing or searching the internet, adhering to strict embedded rules and avoiding project pitfalls.
---

# Embedded Systems Design & Firmware Engineering Guide

This skill defines the technical standards, design patterns, kernel/firmware coding practices, and repo-specific domain knowledge for embedded systems design, kernel driver reconstruction, U-Boot configuration, and board-level software.

---

## 1. Core Embedded Systems Principles

### 1.1 Zero Guesswork Policy
- **Never guess implementation details**: Do not infer struct layouts, ioctl command codes, register offsets, pin multiplexing, clock divider formulas, or memory addresses without checking authoritative BSP headers, decompiled binaries, or official project documentation (`docs/`).
- **Inspect exact source definitions**: Always inspect header files (`.h`), driver source (`.c`), or Ghidra decompilation notes before consuming symbols or writing mock drivers.

### 1.2 Deterministic & Clean Code Construction
- **Robust Error Handling & Cleanup**:
  - Always check return values of memory allocations (`kmalloc`, `kzalloc`, `vmalloc`) and resource requests (`ioremap`, `request_irq`, `gpio_request`).
  - Use `goto out_err` labels in Linux kernel drivers for clean, reverse-order resource unwinding upon error.
  - Return standard Linux error codes (`-EINVAL`, `-EFAULT`, `-ENOMEM`, `-EBUSY`, `-ENODEV`).
- **Memory Safety & Input Validation**:
  - Bounds-check all input arguments, array indices, and length fields before array lookups or buffer writes.
  - Validate pointers and transfer data safely across user/kernel boundaries using `copy_from_user()` and `copy_to_user()`.
  - Use `sizeof()` or exact payload structures when copying, while preserving exact ioctl command macro definitions.
- **Concurrency & Synchronisation**:
  - Guard shared driver state and data structures using appropriate synchronization primitives (`mutex`, `spinlock_t`).
  - Keep Interrupt Service Routines (ISRs) minimal and defer heavy processing to workqueues or tasklets.

### 1.3 Memory-Mapped I/O & Register Access
- Never dereference raw physical addresses directly. Always use `ioremap()` / `iounmap()` in kernel space.
- Use explicit Linux kernel I/O accessors (`readl()`, `writel()`, `readw()`, `writew()`, `readb()`, `writeb()`) rather than direct volatile pointer dereferences.
- Ensure proper memory barriers (`mb()`, `rmb()`, `wmb()`) where register write ordering is required for hardware timing.

---

## 2. Project Architecture & Domain Reference (Prado / ArkMicro Firmware)

### 2.1 Hardware Identity & SoC Specifications
- **SoC Identity**: Physical chip is marked `ARK1668` (ARM Cortex-A5), but software stack (U-Boot, Linux kernel `arch/arm/mach-ark1680/`) identifies as `ARK1680` with Machine ID `0x1068` (4200 decimal).
- **Companion MCU**: STM32F105RBT6 (ARM Cortex-M3) handles vehicle CAN bus (ISO 11898-2 at 500 kbit/s), steering wheel controls (SWC), ACC/IGN, and reversing signals.
- **UART Communication**: MCU communicates with ARK1680 SoC over `/dev/ttyHS0` (115200 8N1) via the `arktool` protocol (`libMcuCenter.so`). Vehicle signals (SWC/reverse) are passed via UART messages, NOT SoC GPIOs.

### 2.2 Display Subsystem (`ark_display`) & Ioctl Encoding Rules
- **Display Resolution**: 800×480 RGB888 display.
- **Layer Allocation**: SoC display engine supports 5 layers (OSD layers 0–2, Video layers 3–4).
- **VDE Configuration (Video Display Engine)**:
  - Tracks layer parameters: `hue` (default 0), `saturation` (default 128 / 0x80), `brightness` (default 128), `contrast` (default 128).
- **CRITICAL Ioctl Macro Rule**: Stock userspace binaries encode ioctls using `unsigned long` as the `size` parameter type in `_IOW` / `_IOWR` macros, regardless of actual structure payload size:
  - `ARKDISP_GET_VDE_CFG`: `_IOWR(ARK_DISPLAY_IOC_MAGIC, 1, unsigned long)` (`0xc004a001`)
  - `ARKDISP_SET_VDE_CFG`: `_IOW(ARK_DISPLAY_IOC_MAGIC, 2, unsigned long)` (`0x4004a002`)

### 2.3 U-Boot & Flash Environment Constraints
- Stock U-Boot (2012.10) flash binary has no reserved `CONFIG_ENV_SIZE` buffer (only ~52 bytes available in compiled environment).
- Avoid expanding environment variables inside U-Boot flash environment; use external script loading (`sdscript`) or modular boot payload tools.

---

## 3. Code Style & Verification Workflow

### 3.1 Kernel & C Driver Style
- Follow Linux kernel coding style (tabs for indentation, 8-character width per tab, snake_case function/variable naming).
- Prefix printks with appropriate severity and module names (`pr_info("ark_display: ...")`, `pr_err("ark_display: ...")`).
- Keep function scope tight and avoid global variables unless necessary; encapsulate instance state in device context structs (`struct ark_disp_dev`).

### 3.2 Verification & Build Discipline
- **Build Verification**: Recompile kernel/firmware components after modifications using project build scripts (`build_bootable_sdcard.sh`, `build_update.sh`).
- **Runtime Log Audit**: Validate kernel messages via `dmesg` / console logs to ensure no unhandled ioctls, kernel panics, or memory access warnings occur.
- **No Mock Swallowing**: Ensure mock drivers provide actual state management (e.g. tracking layer configurations in memory) rather than returning dummy 0 values silently.
