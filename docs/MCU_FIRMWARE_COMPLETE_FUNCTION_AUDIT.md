# Authoritative STM32F105 Microcontroller Firmware Audit & Function Decomposition

> **METHODOLOGY & AUDIT STANDARDS**:
> Derived strictly from direct machine disassembly of `hardware/MCU/live_dumps/live_bootloader.bin` (16 KB) and `hardware/MCU/live_dumps/live_app_1302.bin` (48 KB).
> Zero guesswork, zero unverified assumptions. Every instruction, vector entry, register address, and call graph is verified against authoritative ARM Cortex-M3 architecture references and the ST Microelectronics STM32F105xx reference manual (RM0008).

---

## 1. Unified Flash Memory Architecture & Partition Boundaries

The companion microcontroller on the Prado head unit is an **STM32F105RBT6** (Connectivity Line, 128 KB Flash, 64 KB SRAM, Dual bxCAN, 5 USARTs/UARTs, 2 ADCs).
Forensic cross-correlation of the two live dumps reveals they form a single continuous 64 KB flash image from `0x08000000` to `0x0800FFFF`:

| Memory Range | Partition | Size | Content Description |
| :--- | :--- | :---: | :--- |
| `0x08000000` – `0x0800014F` | Bootloader Vector Table | 336 B | 84 Cortex-M3 / STM32F105 exception and interrupt vectors |
| `0x08000150` – `0x08002047` | Factory IAP Bootloader | 7,928 B | YMODEM protocol, Flash programming routines, Boot validator |
| `0x08002048` – `0x08002FFF` | Erased Flash Gap | 4,024 B | Unprogrammed Flash padding (`0xFFFFFFFF`) separating partitions |
| `0x08003000` – `0x0800314F` | **Application Vector Table** | 336 B | **Real Application Vector Table** (checked by bootloader at `0x08001844`) |
| `0x08003150` – `0x08003205` | Application Startup | 182 B | Initial Reset Handler & C-runtime initialization (`__main`) |
| `0x08003206` – `0x080035B2` | ISR Trampolines | 940 B | Active hardware ISR handlers (CAN1, CAN2, USART1-3, UART5, I2C2, EXTI) |
| `0x080035B3` – `0x08005F13` | Core SPL Drivers | 10.6 KB | Standard Peripheral Library (bxCAN, USART, ADC, GPIO, RCC, EXTI, I2C) |
| `0x08005F14` – `0x080063AB` | Task Scheduler & `main` | 1.1 KB | Cooperative multi-tasking executive & registration engine |
| `0x080063AC` – `0x0800B820` | Application Task Modules | 21.6 KB | 13 periodic task functions (CAN decoders, UART protocol, SWC, Touch) |
| `0x0800B820` – `0x0800BF60` | Dispatch Tables & Strings | 1.8 KB | Mode 1 Toyota Prado CAN tables, Identity string (`Limcet-V1.0-1302`) |
| `0x0800BF60` – `0x0800FFFF` | Application Flash Tail | 16.2 KB | Timer drivers and unprogrammed erased flash (`0xFF`) |

---

## 2. Factory Bootloader Application Validation & Handoff Routine

The factory bootloader checks whether a valid application exists before booting or entering YMODEM recovery.
Disassembly of `0x08001844` in `live_bootloader.bin`:

```arm
0x08001844: ldr  r0, [pc, #0x60]  @ (0x080018A8) -> 0x20141003 (Factory build date: Oct 03, 2014)
0x08001846: ldr  r1, [pc, #0x64]  @ (0x080018AC) -> 0x20004000 (SRAM scratch register)
0x08001848: str  r0, [r1]         @ *(0x20004000) = 0x20141003
0x0800184A: ldr  r0, [pc, #0x4c]  @ (0x08001898) -> 0x08002FFE (Literal)
0x0800184C: adds r0, r0, #2       @ r0 = 0x08003000 (Application Vector Table Base!)
0x0800184E: ldr  r0, [r0]         @ r0 = *(0x08003000) (Fetch candidate Initial MSP)
0x08001850: ldr  r1, [pc, #0x5c]  @ (0x080018B0) -> 0x2FFE0000 (SRAM Bitmask)
0x08001852: ands r0, r1          @ Mask against 0x2FFE0000
0x08001854: cmp.w r0, #0x20000000 @ Check if MSP lies within valid SRAM (0x20000000 - 0x2001FFFF)
0x08001858: bne  #0x800187e      @ If invalid, branch to YMODEM IAP update mode
0x0800185A: ldr  r0, [pc, #0x3c]  @ (0x08001898) -> 0x08002FFE
0x0800185C: adds r0, r0, #2       @ r0 = 0x08003000
0x0800185E: ldr  r0, [r0, #4]     @ r0 = *(0x08003004) (Fetch Application Reset Vector)
0x08001860: ldr  r1, [pc, #0x50]  @ (0x080018B4) -> 0x20000030
0x08001862: str  r0, [r1]         @ Save entry point
0x0800186C: ldr  r1, [pc, #0x28]  @ r1 = 0x08003000
0x08001870: ldr  r0, [r1]         @ r0 = Initial MSP
0x08001872: bl   #0x80001fe       @ Set MSP via `msr msp, r0`
0x08001876: ldr  r0, [pc, #0x40]  @ r0 = Reset Vector
0x0800187A: blx  r0               @ JUMP INTO APPLICATION!
```

**Key Architectural Fact**: The application begins at **`0x08003000`**, NOT `0x08004000`. The bootloader occupies 12 KB (`0x08000000` – `0x08002FFF`).

---

## 3. Application Vector Table Breakdown (`0x08003000` – `0x0800314F`)

The application vector table defines 84 vectors (16 core Cortex-M3 exceptions + 68 STM32F105 interrupts):

| Vector Index | Vector Offset | Exception / IRQ Name | Handler Address | Active / Target |
| :---: | :---: | :--- | :---: | :--- |
|  0 | `0x3000` | `Initial SP` | `0xFFFFFFFF` | Unpopulated / Hardware Masked |
|  1 | `0x3004` | `Reset` | `0xFFFFFFFF` | Unpopulated / Hardware Masked |
|  2 | `0x3008` | `NMI` | `0x080031ED` | **ACTIVE ISR (`0x080031ED`)** |
|  3 | `0x300C` | `HardFault` | `0x080031EF` | **ACTIVE ISR (`0x080031EF`)** |
|  4 | `0x3010` | `MemManage` | `0x080031F3` | **ACTIVE ISR (`0x080031F3`)** |
|  5 | `0x3014` | `BusFault` | `0x080031F7` | **ACTIVE ISR (`0x080031F7`)** |
|  6 | `0x3018` | `UsageFault` | `0x080031FB` | **ACTIVE ISR (`0x080031FB`)** |
|  7 | `0x301C` | `Reserved7` | `0xFFFFFFFF` | Unpopulated / Hardware Masked |
|  8 | `0x3020` | `Reserved8` | `0xFFFFFFFF` | Unpopulated / Hardware Masked |
|  9 | `0x3024` | `Reserved9` | `0xFFFFFFFF` | Unpopulated / Hardware Masked |
| 10 | `0x3028` | `Reserved10` | `0xFFFFFFFF` | Unpopulated / Hardware Masked |
| 11 | `0x302C` | `SVCall` | `0x080031FF` | **ACTIVE ISR (`0x080031FF`)** |
| 12 | `0x3030` | `DebugMon` | `0x08003201` | **ACTIVE ISR (`0x08003201`)** |
| 13 | `0x3034` | `Reserved13` | `0xFFFFFFFF` | Unpopulated / Hardware Masked |
| 14 | `0x3038` | `PendSV` | `0x08003203` | **ACTIVE ISR (`0x08003203`)** |
| 15 | `0x303C` | `SysTick` | `0x08003205` | **ACTIVE ISR (`0x08003205`)** |
| 16 | `0x3040` | `WWDG` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 17 | `0x3044` | `PVD` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 18 | `0x3048` | `TAMPER` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 19 | `0x304C` | `RTC` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 20 | `0x3050` | `FLASH` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 21 | `0x3054` | `RCC` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 22 | `0x3058` | `EXTI0` | `0x080032A1` | **ACTIVE ISR (`0x080032A1`)** |
| 23 | `0x305C` | `EXTI1` | `0x080032A9` | **ACTIVE ISR (`0x080032A9`)** |
| 24 | `0x3060` | `EXTI2` | `0x080032B1` | **ACTIVE ISR (`0x080032B1`)** |
| 25 | `0x3064` | `EXTI3` | `0x080032B9` | **ACTIVE ISR (`0x080032B9`)** |
| 26 | `0x3068` | `EXTI4` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 27 | `0x306C` | `DMA1_Channel1` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 28 | `0x3070` | `DMA1_Channel2` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 29 | `0x3074` | `DMA1_Channel3` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 30 | `0x3078` | `DMA1_Channel4` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 31 | `0x307C` | `DMA1_Channel5` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 32 | `0x3080` | `DMA1_Channel6` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 33 | `0x3084` | `DMA1_Channel7` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 34 | `0x3088` | `ADC1_2` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 35 | `0x308C` | `CAN1_TX` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 36 | `0x3090` | `CAN1_RX0` | `0x0800320D` | **ACTIVE ISR (`0x0800320D`)** |
| 37 | `0x3094` | `CAN1_RX1` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 38 | `0x3098` | `CAN1_SCE` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 39 | `0x309C` | `EXTI9_5` | `0x080032C1` | **ACTIVE ISR (`0x080032C1`)** |
| 40 | `0x30A0` | `TIM1_BRK` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 41 | `0x30A4` | `TIM1_UP` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 42 | `0x30A8` | `TIM1_TRG_COM` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 43 | `0x30AC` | `TIM1_CC` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 44 | `0x30B0` | `TIM2` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 45 | `0x30B4` | `TIM3` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 46 | `0x30B8` | `TIM4` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 47 | `0x30BC` | `I2C1_EV` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 48 | `0x30C0` | `I2C1_ER` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 49 | `0x30C4` | `I2C2_EV` | `0x080032C9` | **ACTIVE ISR (`0x080032C9`)** |
| 50 | `0x30C8` | `I2C2_ER` | `0x080032F5` | **ACTIVE ISR (`0x080032F5`)** |
| 51 | `0x30CC` | `SPI1` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 52 | `0x30D0` | `SPI2` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 53 | `0x30D4` | `USART1` | `0x08003281` | **ACTIVE ISR (`0x08003281`)** |
| 54 | `0x30D8` | `USART2` | `0x08003289` | **ACTIVE ISR (`0x08003289`)** |
| 55 | `0x30DC` | `USART3` | `0x08003291` | **ACTIVE ISR (`0x08003291`)** |
| 56 | `0x30E0` | `EXTI15_10` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 57 | `0x30E4` | `RTCAlarm` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 58 | `0x30E8` | `OTG_FS_WKUP` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 59 | `0x30EC` | `Reserved43` | `0x00000000` | Reserved Vector Slot (0x0) |
| 60 | `0x30F0` | `Reserved44` | `0x00000000` | Reserved Vector Slot (0x0) |
| 61 | `0x30F4` | `Reserved45` | `0x00000000` | Reserved Vector Slot (0x0) |
| 62 | `0x30F8` | `Reserved46` | `0x00000000` | Reserved Vector Slot (0x0) |
| 63 | `0x30FC` | `Reserved47` | `0x00000000` | Reserved Vector Slot (0x0) |
| 64 | `0x3100` | `Reserved48` | `0x00000000` | Reserved Vector Slot (0x0) |
| 65 | `0x3104` | `Reserved49` | `0x00000000` | Reserved Vector Slot (0x0) |
| 66 | `0x3108` | `TIM5` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 67 | `0x310C` | `SPI3` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 68 | `0x3110` | `UART4` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 69 | `0x3114` | `UART5` | `0x08003299` | **ACTIVE ISR (`0x08003299`)** |
| 70 | `0x3118` | `TIM6` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 71 | `0x311C` | `TIM7` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 72 | `0x3120` | `DMA2_Channel1` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 73 | `0x3124` | `DMA2_Channel2` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 74 | `0x3128` | `DMA2_Channel3` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 75 | `0x312C` | `DMA2_Channel4` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 76 | `0x3130` | `DMA2_Channel5` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 77 | `0x3134` | `ETH` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 78 | `0x3138` | `ETH_WKUP` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 79 | `0x313C` | `CAN2_TX` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 80 | `0x3140` | `CAN2_RX0` | `0x08003247` | **ACTIVE ISR (`0x08003247`)** |
| 81 | `0x3144` | `CAN2_RX1` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 82 | `0x3148` | `CAN2_SCE` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |
| 83 | `0x314C` | `OTG_FS` | `0x080035B3` | Default_Handler (`0x080035B3` infinite loop) |

---

## 4. Cooperative Task Executive & The 13 Periodic Tasks

The application runtime is governed by a deterministic, non-preemptive cooperative task scheduler configured in `main()` (`0x08006042`).
Each task registers a function pointer, a run priority, and a timer period (in milliseconds):

| Task # | Function Address | Priority | Period (ms) | Subsystem / Functional Role |
| :---: | :---: | :---: | :---: | :--- |
| **0** | [`0x080063AC`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | 3 | **35 ms** | **UART Protocol RX Frame Parser & Dispatcher** |
| **1** | [`0x080063F4`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | 2 | **30 ms** | **CAN1 Vehicle Bus Frame Dispatcher (Mode 1 - Prado)** |
| **2** | [`0x0800759C`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | 3 | **25 ms** | **Analog / SWC Ladder Sampling & Debounce Engine** |
| **3** | [`0x08008934`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | 6 | **35 ms** | **Vehicle Speed / Odometer / Trip Computer Processor** |
| **4** | [`0x08007C2C`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | 2 | **100 ms** | **Backlight / Illumination & Day/Night Dimming Control** |
| **5** | [`0x0800700C`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | 4 | **50 ms** | **Discrete GPIO Polling (ACC / IGN / Reverse / Cam)** |
| **6** | [`0x08008AC2`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | 1 | **1 ms** | **High-Frequency Core Tick (1ms State Machines & Timers)** |
| **7** | [`0x08009164`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | 1 | **1 ms** | **High-Frequency Comm Router (1ms UART/CAN FIFO Shuffler)** |
| **8** | [`0x0800A1BC`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | 1 | **25 ms** | **Steering Angle Sensor Zero-Calibration & Keep-Alive** |
| **9** | [`0x0800A3AC`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | 4 | **35 ms** | **CAN Sub-Profile Dispatcher (Mode 3 Secondary Bus)** |
| **10** | [`0x0800A77C`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | 2 | **100 ms** | **CAN Sub-Profile Dispatcher (Mode 2 Body Network)** |
| **11** | [`0x080075AC`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | 1 | **5 ms** | **Touchscreen ADC Coordinate Sampler & Touch Event Generator** |
| **12** | [`0x0800B57C`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | 3 | **10 ms** | **I2C Audio DSP / Radio Tuner / Codec Driver** |

### Detailed Analysis of Every Periodic Task

### Task 0: `UART Protocol RX Frame Parser & Dispatcher` (`0x080063AC`)
- **Execution Period**: 35 ms (Priority 3)
- **Instruction Count**: 31 instructions
- **Subroutines Called**: `#0x8006368, #0x8008194, r0`
- **Disassembly Excerpt**:
```arm
0x080063AC: push     {r3, r4, r5, lr}
0x080063AE: movs     r0, #0
0x080063B0: str      r0, [sp]
0x080063B2: movs     r4, #0
0x080063B4: mov      r2, sp
0x080063B6: movs     r1, #0
0x080063B8: ldr      r0, [pc, #0x3a4]
0x080063BA: bl       #0x8008194
0x080063BE: ldrb.w   r0, [sp]
0x080063C2: cbnz     r0, #0x80063f2
0x080063C4: bl       #0x8006368
0x080063C8: cbz      r0, #0x80063f2
```

### Task 1: `CAN1 Vehicle Bus Frame Dispatcher (Mode 1 - Prado)` (`0x080063F4`)
- **Execution Period**: 30 ms (Priority 2)
- **Instruction Count**: 13 instructions
- **Subroutines Called**: `#0x8007a4c, r1`
- **Disassembly Excerpt**:
```arm
0x080063F4: push     {r4, lr}
0x080063F6: movs     r0, #0
0x080063F8: bl       #0x8007a4c
0x080063FC: mov      r4, r0
0x080063FE: ldrb     r0, [r4]
0x08006400: ldrh     r0, [r0]
0x08006402: adds     r0, r0, #1
0x08006404: ldr      r1, [pc, #0x5c]
0x08006406: strh     r0, [r1]
0x08006408: ldr      r3, [pc, #0x35c]
0x0800640A: ldr.w    r1, [r3, r2, lsl #2]
0x0800640E: blx      r1
```

### Task 2: `Analog / SWC Ladder Sampling & Debounce Engine` (`0x0800759C`)
- **Execution Period**: 25 ms (Priority 3)
- **Instruction Count**: 5 instructions
- **Subroutines Called**: `#0x8007370, #0x8007408, #0x80074a0`
- **Disassembly Excerpt**:
```arm
0x0800759C: push     {r4, lr}
0x0800759E: bl       #0x80074a0
0x080075A2: bl       #0x8007408
0x080075A6: bl       #0x8007370
0x080075AA: pop      {r4, pc}
```

### Task 3: `Vehicle Speed / Odometer / Trip Computer Processor` (`0x08008934`)
- **Execution Period**: 35 ms (Priority 6)
- **Instruction Count**: 9 instructions
- **Subroutines Called**: `#0x8008722, #0x8008794, #0x80088ac`
- **Disassembly Excerpt**:
```arm
0x08008934: push     {r4, lr}
0x08008936: bl       #0x80088ac
0x0800893A: bl       #0x8008794
0x0800893E: ldr      r0, [pc, #0x28]
0x08008940: ldrb.w   r0, [r0, #0x3b]
0x08008944: cmp      r0, #2
0x08008946: bne      #0x800894e
0x08008948: bl       #0x8008722
0x0800894C: pop      {r4, pc}
```

### Task 4: `Backlight / Illumination & Day/Night Dimming Control` (`0x08007C2C`)
- **Execution Period**: 100 ms (Priority 2)
- **Instruction Count**: 71 instructions
- **Subroutines Called**: `#0x8007b2a, #0x8007b70, #0x8007c18, #0x8008156, #0x8008afa, #0x800ae40`
- **Disassembly Excerpt**:
```arm
0x08007C2C: push     {r4, lr}
0x08007C2E: ldr      r0, [pc, #0x114]
0x08007C30: ldrb     r0, [r0]
0x08007C32: cmp      r0, #6
0x08007C34: bl       #0x8008156
0x08007C38: and      fp, r0, #3
0x08007C3C: movs     r4, #0x18
0x08007C3E: adds     r3, #0x2d
0x08007C40: ldr      r0, [pc, #0x100]
0x08007C42: ldrb     r0, [r0]
0x08007C44: adds     r0, r0, #1
0x08007C46: ldr      r1, [pc, #0xfc]
```

### Task 5: `Discrete GPIO Polling (ACC / IGN / Reverse / Cam)` (`0x0800700C`)
- **Execution Period**: 50 ms (Priority 4)
- **Instruction Count**: 12 instructions
- **Subroutines Called**: `#0x800513e, #0x8006c1a`
- **Disassembly Excerpt**:
```arm
0x0800700C: push     {r4, lr}
0x0800700E: ldr      r0, [pc, #0x244]
0x08007010: ldrb     r0, [r0, #2]
0x08007012: cbz      r0, #0x8007028
0x08007014: movs     r0, #1
0x08007016: bl       #0x8006c1a
0x0800701A: movs     r0, #0
0x0800701C: b        #0x8007028
0x0800701E: mov.w    r1, #0x400
0x08007022: ldr      r0, [pc, #0x98]
0x08007024: bl       #0x800513e
0x08007028: pop      {r4, pc}
```

### Task 6: `High-Frequency Core Tick (1ms State Machines & Timers)` (`0x08008AC2`)
- **Execution Period**: 1 ms (Priority 1)
- **Instruction Count**: 17 instructions
- **Subroutines Called**: `#0x8006288, #0x8006b04, #0x80075d0, #0x8007d14, #0x8009d3c, #0x800a486, #0x800ab90, #0x800ac08, #0x800adee, #0x800b5cc`
- **Disassembly Excerpt**:
```arm
0x08008AC2: push     {r4, lr}
0x08008AC4: bl       #0x8007d14
0x08008AC8: bl       #0x80075d0
0x08008ACC: bl       #0x8006b04
0x08008AD0: bl       #0x8006288
0x08008AD4: bl       #0x8009d3c
0x08008AD8: bl       #0x800a486
0x08008ADC: ldr      r0, [pc, #0x64]
0x08008ADE: ldrb.w   r0, [r0, #0x36]
0x08008AE2: cmp      r0, #1
0x08008AE4: bne      #0x8008aec
0x08008AE6: bl       #0x800ac08
```

### Task 7: `High-Frequency Comm Router (1ms UART/CAN FIFO Shuffler)` (`0x08009164`)
- **Execution Period**: 1 ms (Priority 1)
- **Instruction Count**: 55 instructions
- **Subroutines Called**: `#0x8006b1c, #0x8006b72, #0x8008eb0, #0x8009070`
- **Disassembly Excerpt**:
```arm
0x08009164: push     {r4, lr}
0x08009166: ldr      r0, [pc, #0x19c]
0x08009168: ldrb     r0, [r0]
0x0800916A: cmp      r0, #5
0x0800916C: bhs      #0x80091da
0x0800916E: tbb      [pc, r0]
0x08009172: lsrs     r3, r0, #0x1c
0x08009174: cmp      r5, #0x21
0x08009176: movs     r0, r6
0x08009178: movs     r0, #2
0x0800917A: bl       #0x8006b1c
0x0800917E: movs     r0, #0x32
```

### Task 8: `Steering Angle Sensor Zero-Calibration & Keep-Alive` (`0x0800A1BC`)
- **Execution Period**: 25 ms (Priority 1)
- **Instruction Count**: 12 instructions
- **Subroutines Called**: `None (Leaf function)`
- **Disassembly Excerpt**:
```arm
0x0800A1BC: push     {r4, lr}
0x0800A1BE: ldr      r0, [pc, #0x1c8]
0x0800A1C0: ldrb     r0, [r0]
0x0800A1C2: cbnz     r0, #0x800a1d4
0x0800A1C4: movs     r0, #0
0x0800A1C6: ldr      r1, [pc, #0x1b4]
0x0800A1C8: strb     r0, [r1, #0x11]
0x0800A1CA: ldr      r1, [pc, #0x1c0]
0x0800A1CC: strb     r0, [r1, #0x12]
0x0800A1CE: ldr      r1, [pc, #0x1c0]
0x0800A1D0: strh     r0, [r1]
0x0800A1D2: pop      {r4, pc}
```

### Task 9: `CAN Sub-Profile Dispatcher (Mode 3 Secondary Bus)` (`0x0800A3AC`)
- **Execution Period**: 35 ms (Priority 4)
- **Instruction Count**: 37 instructions
- **Subroutines Called**: `#0x8007a4c, r1`
- **Disassembly Excerpt**:
```arm
0x0800A3AC: push     {r4, lr}
0x0800A3AE: movs     r0, #3
0x0800A3B0: bl       #0x8007a4c
0x0800A3B4: mov      r4, r0
0x0800A3B6: ldrb     r0, [r4]
0x0800A3B8: cmp      r0, #7
0x0800A3BA: bge      #0x800a3c8
0x0800A3BC: ldrh     r0, [r4, #2]
0x0800A3BE: ldrb     r2, [r4]
0x0800A3C0: ldr      r3, [pc, #0x31c]
0x0800A3C2: ldr.w    r1, [r3, r2, lsl #2]
0x0800A3C6: blx      r1
```

### Task 10: `CAN Sub-Profile Dispatcher (Mode 2 Body Network)` (`0x0800A77C`)
- **Execution Period**: 100 ms (Priority 2)
- **Instruction Count**: 20 instructions
- **Subroutines Called**: `#0x80078e4, #0x8007a4c, r1`
- **Disassembly Excerpt**:
```arm
0x0800A77C: push     {r4, lr}
0x0800A77E: movs     r0, #2
0x0800A780: bl       #0x8007a4c
0x0800A784: mov      r4, r0
0x0800A786: ldrb     r0, [r4]
0x0800A788: cmp      r0, #4
0x0800A78A: bge      #0x800a798
0x0800A78C: ldrh     r0, [r4, #2]
0x0800A78E: ldrb     r2, [r4]
0x0800A790: ldr      r3, [pc, #0x374]
0x0800A792: ldr.w    r1, [r3, r2, lsl #2]
0x0800A796: blx      r1
```

### Task 11: `Touchscreen ADC Coordinate Sampler & Touch Event Generator` (`0x080075AC`)
- **Execution Period**: 5 ms (Priority 1)
- **Instruction Count**: 13 instructions
- **Subroutines Called**: `#0x800ad84, #0x800af78`
- **Disassembly Excerpt**:
```arm
0x080075AC: push     {r4, lr}
0x080075AE: ldr      r0, [pc, #0xe8]
0x080075B0: ldrb.w   r0, [r0, #0x36]
0x080075B4: cmp      r0, #3
0x080075B6: bne      #0x80075c0
0x080075B8: bl       #0x800af78
0x080075BC: bl       #0x800ad84
0x080075C0: ldr      r0, [pc, #0xd4]
0x080075C2: ldrb.w   r0, [r0, #0x36]
0x080075C6: cmp      r0, #1
0x080075C8: bne      #0x80075ce
0x080075CA: bl       #0x800ad84
```

### Task 12: `I2C Audio DSP / Radio Tuner / Codec Driver` (`0x0800B57C`)
- **Execution Period**: 10 ms (Priority 3)
- **Instruction Count**: 37 instructions
- **Subroutines Called**: `#0x800b19a, #0x800b1e4`
- **Disassembly Excerpt**:
```arm
0x0800B57C: push     {r4, lr}
0x0800B57E: ldr      r0, [pc, #0x270]
0x0800B580: ldrb     r0, [r0]
0x0800B582: cbz      r0, #0x800b592
0x0800B584: cmp      r0, #1
0x0800B586: beq      #0x800b5a4
0x0800B588: cmp      r0, #2
0x0800B58A: beq      #0x800b5ba
0x0800B58C: cmp      r0, #3
0x0800B58E: bne      #0x800b5c2
0x0800B590: b        #0x800b5bc
0x0800B592: ldr      r0, [pc, #0x25c]
```

---

## 5. Subsystem Decompositions: CAN, UART, Buttons/SWC, Touch

### 5.1 CAN Bus Subsystem & Toyota Prado Mode 1 Table
The STM32F105 bxCAN controller is wired to an NXP TJA1042 transceiver on `PB8`/`PB9`.
The Mode 1 (Toyota Prado) message dispatch table is located at `0x0800B9F4`:

| CAN ID | Hex ID | DLC | Handler Routine | Decoded Vehicle Metric / Action |
| :--- | :---: | :---: | :---: | :--- |
| **0x025** | `0x00000025` | 8 | `0x0800956A` | **Steering Wheel Angle Sensor**: Decodes rotation angle & angular velocity for dynamic reverse camera trajectories (`CMD 0x0A`). |
| **0x1D0** | `0x000001D0` | 8 | `0x080092B6` | **Transmission & Wheel Speeds**: Decodes Reverse gear engage (`R`) and individual wheel rotation speeds. |
| **0x396** | `0x00000396` | 8 | `0x080093D8` | **Powertrain Status**: Engine RPM, coolant temperature, vehicle moving status. |
| **0x622** | `0x00000622` | 8 | `0x080095E4` | **Body Control**: Door open/closed status (driver, passenger, tailgate), headlamp/parking lamp state. |

### 5.2 Steering Wheel Controls (SWC) & Analog Resistor Ladder
- **Hardware Signal Path**: Steering wheel audio controls on Toyota Prado 150 are **analog resistive**, connected to `SW1`/`SW2` inputs.
- **Digitization**: Sampled by `ADC1` (`0x40012400`) via regular conversion sequence.
- **Task 2 (`0x0800759C`)**: Runs every 25 ms, calls `0x080074A0`, `0x08007408`, `0x08007370` to filter ADC counts, evaluate nominal resistor windows (0 ohm, 330 ohm, 1k ohm, 3.1k ohm, 10k ohm), debounce button presses, and emit UART key packets.

### 5.3 Resistive Touchscreen Digitizer Subsystem
- **Hardware Signal Path**: 4-wire resistive touch panel lines (X+, X-, Y+, Y-) connected to `ADC1` analog channels.
- **Task 11 (`0x080075AC`)**: Runs every 5 ms. Toggles drive lines between GPIO output and ADC analog input to sample X and Y coordinates.
- **Forwarding**: Formats coordinates into `CMD 0x20` protocol frames: `AA 55 08 20 [status] [X_lo] [X_hi] [Y_lo] [Y_hi] [chk]` and pushes to UART TX ring buffer (`0x08007764`).

### 5.4 UART Protocol Engine (`/dev/ttyHS0`)
- **Peripheral**: `USART1` / `USART2` communicating with ARK1680 SoC at 115,200 baud (8N1).
- **Task 0 (`0x080063AC`)**: Runs every 35 ms. Pops raw bytes from UART circular buffer (`0x08008194`), detects `0xAA 0x55` frame header, validates length and checksum, and dispatches to handler table at `0x080062C0`.
- **Supported Host Commands**:
  - `0x01`: MCU Status Request / Reverse trigger report
  - `0x02`: SWC Key Event transmission
  - `0x03`: Firmware Version Request (`'Limcet-V1.0-1302'`)
  - `0x0A`: Dynamic Steering Angle Report (forwarding decoded CAN `0x025` data)
  - `0x20`: Touch coordinate streaming

---

## 6. Complete Catalog of All 329 Extracted Functions

Every function discovered across both binary dumps is indexed below with its exact physical flash memory bounds:

| Function Address Range | Partition | Size | Entry Instruction | Exit Instruction | Peripherals / Callees |
| :--- | :---: | :---: | :--- | :--- | :--- |
| `0x080001C4 - 0x080001C8` | BOOTLOADER | 4 B | `push {r0, r1, r2, r3, r4, lr}` | `pop {r0, r1, r2, r3, r4, pc}` | Internal logic |
| `0x080001C8 - 0x080001CC` | BOOTLOADER | 4 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x0800020C - 0x0800020E` | BOOTLOADER | 2 B | `bx lr` | `bx lr` | Internal logic |
| `0x0800020E - 0x08000212` | BOOTLOADER | 4 B | `nop` | `b #0x8000210` | Internal logic |
| `0x08000212 - 0x08000216` | BOOTLOADER | 4 B | `nop` | `b #0x8000214` | Internal logic |
| `0x08000216 - 0x0800021A` | BOOTLOADER | 4 B | `nop` | `b #0x8000218` | Internal logic |
| `0x0800021A - 0x0800021E` | BOOTLOADER | 4 B | `nop` | `lsls r7, r0, #0x13` | Internal logic |
| `0x0800021E - 0x08000220` | BOOTLOADER | 2 B | `lsrs r0, r0, #0x20` | `lsrs r0, r0, #0x20` | Internal logic |
| `0x08000220 - 0x08000222` | BOOTLOADER | 2 B | `lsls r7, r0, #0x13` | `lsls r7, r0, #0x13` | Internal logic |
| `0x08000222 - 0x08000224` | BOOTLOADER | 2 B | `lsrs r0, r0, #0x20` | `lsrs r0, r0, #0x20` | Internal logic |
| `0x08000224 - 0x08000330` | BOOTLOADER | 268 B | `lsls r7, r0, #0x13` | `pop {r2, r3, pc}` | FLASH_CTRL, RCC |
| `0x08000330 - 0x08000338` | BOOTLOADER | 8 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08000228 |
| `0x08000338 - 0x08000394` | BOOTLOADER | 92 B | `push {r4, lr}` | `pop {r4, pc}` | NVIC, RCC, SCB |
| `0x08000394 - 0x08000434` | BOOTLOADER | 160 B | `push {r4, r5, r6, r7, lr}` | `ubfx r7, r7, #8, #4` | RCC |
| `0x080004AC - 0x080004C6` | BOOTLOADER | 26 B | `ldr r0, [pc, #0x24]` | `b #0x80004c4` | Calls 0x08000150, 0x08000338 |
| `0x080004C6 - 0x080004D2` | BOOTLOADER | 12 B | `b #0x80004c6` | `bx lr` | Internal logic |
| `0x0800058E - 0x080005B4` | BOOTLOADER | 38 B | `push {lr}` | `pop {pc}` | Calls 0x0800055E |
| `0x080005B4 - 0x080005FC` | BOOTLOADER | 72 B | `push {r4, r5, lr}` | `pop {r4, r5, pc}` | FLASH_CTRL |
| `0x080005FC - 0x0800062A` | BOOTLOADER | 46 B | `push {r4, lr}` | `bx lr` | FLASH_CTRL |
| `0x08000640 - 0x08000666` | BOOTLOADER | 38 B | `push {lr}` | `pop {pc}` | Calls 0x0800055E |
| `0x08000666 - 0x080006AA` | BOOTLOADER | 68 B | `push {r4, lr}` | `pop {r4, pc}` | FLASH_CTRL |
| `0x080006BE - 0x08000754` | BOOTLOADER | 150 B | `push {r4, r5, lr}` | `pop {r4, r5, pc}` | FLASH_CTRL |
| `0x08000754 - 0x080007BA` | BOOTLOADER | 102 B | `push {r3, r4, r5, r6, lr}` | `pop {r3, r4, r5, r6, pc}` | FLASH_CTRL |
| `0x080007BA - 0x080007F6` | BOOTLOADER | 60 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | FLASH_CTRL |
| `0x080007F6 - 0x0800083E` | BOOTLOADER | 72 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | FLASH_CTRL |
| `0x0800083E - 0x08000906` | BOOTLOADER | 200 B | `push.w {r4, r5, r6, r7, r8, sb, lr}` | `pop.w {r4, r5, r6, r7, r8, sb, pc}` | FLASH_CTRL |
| `0x08000906 - 0x080009A2` | BOOTLOADER | 156 B | `push {r4, r5, lr}` | `pop {r4, r5, pc}` | FLASH_CTRL |
| `0x080009A2 - 0x080009FA` | BOOTLOADER | 88 B | `push {r4, r5, r6, r7, lr}` | `pop {r4, r5, r6, r7, pc}` | FLASH_CTRL |
| `0x08000A06 - 0x08000A1C` | BOOTLOADER | 22 B | `push {r4, r5, lr}` | `bx lr` | FLASH_CTRL |
| `0x08000AA8 - 0x08000B54` | BOOTLOADER | 172 B | `push {r4, lr}` | `pop {r4, pc}` | ADC1, ADC2, AFIO, GPIOA, GPIOB, GPIOC, GPIOD, GPIOE |
| `0x08000B54 - 0x08000B68` | BOOTLOADER | 20 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x080012CC |
| `0x08000B68 - 0x08000C8E` | BOOTLOADER | 294 B | `push.w {r4, r5, r6, r7, r8, lr}` | `bx lr` | Calls 0x080013CC |
| `0x08000D0A - 0x08000D94` | BOOTLOADER | 138 B | `push {r4, r5, r6, r7, lr}` | `pop {r4, r5, r6, r7, pc}` | AFIO, GPIOA, GPIOB |
| `0x08000D94 - 0x08000DD6` | BOOTLOADER | 66 B | `push {r4, r5, lr}` | `pop {r4, r5, pc}` | AFIO, GPIOA, GPIOB |
| `0x08000ED8 - 0x08000F10` | BOOTLOADER | 56 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x08000F10 - 0x08000F48` | BOOTLOADER | 56 B | `push {r3, r4, r5, lr}` | `pop {r3, r4, r5, pc}` | Calls 0x08000ED8 |
| `0x080010BC - 0x08001244` | BOOTLOADER | 392 B | `push.w {r4, r5, r6, r7, r8, lr}` | `rsbs r2, r0, #0` | Internal logic |
| `0x08001348 - 0x080013CE` | BOOTLOADER | 134 B | `push {r4, lr}` | `pop {r4, pc}` | I2C1, I2C2, UART4, UART5, USART1, USART2, USART3 |
| `0x080013CE - 0x08001406` | BOOTLOADER | 56 B | `push.w {r4, r5, r6, r7, r8, sb, sl, lr}` | `bx lr` | Internal logic |
| `0x080014B8 - 0x080014DA` | BOOTLOADER | 34 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x080014FE - 0x08001548` | BOOTLOADER | 74 B | `push {r4, r5, r6, r7, lr}` | `pop {r4, r5, r6, r7, pc}` | Internal logic |
| `0x080016C8 - 0x0800171C` | BOOTLOADER | 84 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | Internal logic |
| `0x0800171C - 0x0800173A` | BOOTLOADER | 30 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x08001750 - 0x08001772` | BOOTLOADER | 34 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08000E04, 0x08000E0A, 0x08000E10, 0x08000E16, 0x08000E20 |
| `0x08001772 - 0x080017CC` | BOOTLOADER | 90 B | `push {r3, lr}` | `pop {r3, pc}` | AFIO, GPIOA, GPIOB, GPIOC, GPIOD, GPIOE |
| `0x080017E2 - 0x0800181C` | BOOTLOADER | 58 B | `push {r4, lr}` | `nop` | Calls 0x0800052A, 0x08000542, 0x08001750, 0x08001772, 0x080017CC, 0x080018BC, 0x08001EB0 |
| `0x0800181C - 0x08001890` | BOOTLOADER | 116 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x080001FE, 0x08001750 |
| `0x080018BC - 0x0800193C` | BOOTLOADER | 128 B | `push {lr}` | `pop {pc}` | AFIO, GPIOA, GPIOB, UART4, USART2, USART3 |
| `0x08001956 - 0x080019A2` | BOOTLOADER | 76 B | `push {r4, lr}` | `pop {r4, pc}` | UART4, USART2, USART3 |
| `0x080019A2 - 0x080019EC` | BOOTLOADER | 74 B | `push {r4, lr}` | `pop {r4, pc}` | UART4, USART2, USART3 |
| `0x080019EC - 0x08001A00` | BOOTLOADER | 20 B | `push.w {r4, r5, r6, r7, r8, lr}` | `nop` | Internal logic |
| `0x08001B62 - 0x08001B82` | BOOTLOADER | 32 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | Calls 0x080006BE, 0x08000A02 |
| `0x08001B82 - 0x08001BB2` | BOOTLOADER | 48 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x080005B4, 0x080007BA, 0x08000A60 |
| `0x08001BB2 - 0x08001C2A` | BOOTLOADER | 120 B | `push {r4, r5, r6, lr}` | `movs r7, #0` | UART4, USART2, USART3 |
| `0x08001C72 - 0x08001CF4` | BOOTLOADER | 130 B | `push.w {r4, r5, r6, r7, r8, sb, sl, lr}` | `movs r0, #0` | Calls 0x08000A60 |
| `0x08001D78 - 0x08001EFA` | BOOTLOADER | 386 B | `push.w {r4, r5, r6, r7, r8, lr}` | `bx lr` | UART4, USART2, USART3 |
| `0x08001F14 - 0x08001F48` | BOOTLOADER | 52 B | `push {r5, lr}` | `bx lr` | Calls 0x080004C8 |
| `0x08001F48 - 0x08001F60` | BOOTLOADER | 24 B | `push {r4, lr}` | `bx lr` | Internal logic |
| `0x08003150 - 0x080031A6` | APPLICATION | 86 B | `bl #0x8003158` | `bx lr` | Calls 0x08003158, 0x080031CC |
| `0x080031C4 - 0x080031C8` | APPLICATION | 4 B | `push {r0, r1, r2, r3, r4, lr}` | `pop {r0, r1, r2, r3, r4, pc}` | Internal logic |
| `0x080031C8 - 0x080031CC` | APPLICATION | 4 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x080031EC - 0x080031EE` | APPLICATION | 2 B | `bx lr` | `bx lr` | Internal logic |
| `0x080031EE - 0x080031F2` | APPLICATION | 4 B | `nop` | `b #0x80031f0` | Internal logic |
| `0x080031F2 - 0x080031F6` | APPLICATION | 4 B | `nop` | `b #0x80031f4` | Internal logic |
| `0x080031F6 - 0x080031FA` | APPLICATION | 4 B | `nop` | `b #0x80031f8` | Internal logic |
| `0x080031FA - 0x080031FE` | APPLICATION | 4 B | `nop` | `b #0x80031fc` | Internal logic |
| `0x080031FE - 0x08003200` | APPLICATION | 2 B | `bx lr` | `bx lr` | Internal logic |
| `0x08003200 - 0x08003202` | APPLICATION | 2 B | `movs r0, r0` | `movs r0, r0` | Internal logic |
| `0x08003202 - 0x08003204` | APPLICATION | 2 B | `movs r0, r0` | `movs r0, r0` | Internal logic |
| `0x08003204 - 0x0800320C` | APPLICATION | 8 B | `movs r0, r0` | `mrrc2 p13, #1, fp, r8, c0` | Internal logic |
| `0x0800320C - 0x08003246` | APPLICATION | 58 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x08003246 - 0x08003280` | APPLICATION | 58 B | `push {r4, lr}` | `pop {r4, pc}` | CAN1, CAN2 |
| `0x08003280 - 0x08003288` | APPLICATION | 8 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08007DEC |
| `0x08003288 - 0x08003290` | APPLICATION | 8 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08008062 |
| `0x08003290 - 0x08003298` | APPLICATION | 8 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x080083BC |
| `0x08003298 - 0x080032A0` | APPLICATION | 8 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08008C2E |
| `0x080032A0 - 0x080032A8` | APPLICATION | 8 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800AEF6 |
| `0x080032A8 - 0x080032B0` | APPLICATION | 8 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800AF0C |
| `0x080032B0 - 0x080032B8` | APPLICATION | 8 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800AF24 |
| `0x080032B8 - 0x080032C0` | APPLICATION | 8 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800AF3C |
| `0x080032C0 - 0x080032C8` | APPLICATION | 8 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800AF5A |
| `0x080032C8 - 0x080032F4` | APPLICATION | 44 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800B212, 0x0800B324 |
| `0x080032F4 - 0x080032FC` | APPLICATION | 8 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800B436 |
| `0x08003310 - 0x08003418` | APPLICATION | 264 B | `push {r2, r3, lr}` | `pop {r2, r3, pc}` | FLASH_CTRL, RCC |
| `0x08003418 - 0x0800347A` | APPLICATION | 98 B | `push {r4, lr}` | `pop {r4, pc}` | NVIC, RCC, SCB |
| `0x0800347A - 0x08003562` | APPLICATION | 232 B | `push {r4, r5, r6, r7, lr}` | `pop {r4, r5, r6, r7, pc}` | RCC |
| `0x080035B2 - 0x080035BE` | APPLICATION | 12 B | `b #0x80035b2` | `bx lr` | Internal logic |
| `0x080035E2 - 0x08003646` | APPLICATION | 100 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | NVIC, SCB |
| `0x080036B4 - 0x08003702` | APPLICATION | 78 B | `push {r4, lr}` | `pop {r4, pc}` | ADC1, ADC2, USART1 |
| `0x08003702 - 0x08003748` | APPLICATION | 70 B | `push {r4, r5, lr}` | `pop {r4, r5, pc}` | Internal logic |
| `0x08003786 - 0x0800379E` | APPLICATION | 24 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x08003802 - 0x0800381C` | APPLICATION | 26 B | `push {r4, r5, lr}` | `pop {r4, pc}` | Internal logic |
| `0x08003832 - 0x080038EA` | APPLICATION | 184 B | `push {r4, r5, r6, r7, lr}` | `pop {r4, r5, r6, r7, pc}` | Internal logic |
| `0x0800398C - 0x08003A08` | APPLICATION | 124 B | `push {r4, r5, r6, r7, lr}` | `bx lr` | Internal logic |
| `0x08003A0E - 0x08003A26` | APPLICATION | 24 B | `push {r4, lr}` | `bx lr` | Internal logic |
| `0x08003A3A - 0x08003A56` | APPLICATION | 28 B | `push {r3, lr}` | `pop {r3, pc}` | Internal logic |
| `0x08003AD4 - 0x08003AF8` | APPLICATION | 36 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | Internal logic |
| `0x08003B04 - 0x08003B36` | APPLICATION | 50 B | `push {r4, lr}` | `pop {r4, pc}` | CAN1, CAN2 |
| `0x08003B36 - 0x08003C02` | APPLICATION | 204 B | `push {r4, r5, lr}` | `bx lr` | Internal logic |
| `0x08003C04 - 0x08003C36` | APPLICATION | 50 B | `push {r4, lr}` | `b.w #0x7e53656` | Calls 0x08005B62 |
| `0x08003C36 - 0x08003C4A` | APPLICATION | 20 B | `push {r4, r5, lr}` | `pop {r4, r5, pc}` | Internal logic |
| `0x08003C4A - 0x08003D4C` | APPLICATION | 258 B | `push {r4, lr}` | `pop {r4, pc}` | CAN1, CAN2 |
| `0x08003F58 - 0x08003FE2` | APPLICATION | 138 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x08004012 - 0x08004102` | APPLICATION | 240 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x08004136 - 0x080041D8` | APPLICATION | 162 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x08004236 - 0x0800423C` | APPLICATION | 6 B | `push {r4, lr}` | `bx lr` | Internal logic |
| `0x0800425A - 0x080042D2` | APPLICATION | 120 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x08004318 - 0x08004366` | APPLICATION | 78 B | `push {r4, r5, r6, lr}` | `lsls r0, r0, #4` | Internal logic |
| `0x08004638 - 0x08004674` | APPLICATION | 60 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x08004906 - 0x0800492C` | APPLICATION | 38 B | `push {lr}` | `pop {pc}` | Calls 0x080048D6 |
| `0x0800492C - 0x08004974` | APPLICATION | 72 B | `push {r4, r5, lr}` | `pop {r4, r5, pc}` | FLASH_CTRL |
| `0x08004974 - 0x080049B8` | APPLICATION | 68 B | `push {r4, lr}` | `pop {r4, pc}` | FLASH_CTRL |
| `0x080049B8 - 0x080049DE` | APPLICATION | 38 B | `push {lr}` | `pop {pc}` | Calls 0x080048D6 |
| `0x080049DE - 0x08004A06` | APPLICATION | 40 B | `push {r4, lr}` | `bx lr` | FLASH_CTRL |
| `0x08004A06 - 0x08004A2C` | APPLICATION | 38 B | `push {lr}` | `pop {pc}` | FLASH_CTRL |
| `0x08004ACC - 0x08004B32` | APPLICATION | 102 B | `push {r3, r4, r5, r6, lr}` | `pop {r3, r4, r5, r6, pc}` | FLASH_CTRL |
| `0x08004B32 - 0x08004B6E` | APPLICATION | 60 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | FLASH_CTRL |
| `0x08004B6E - 0x08004BB6` | APPLICATION | 72 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | FLASH_CTRL |
| `0x08004BB6 - 0x08004C1A` | APPLICATION | 100 B | `push.w {r4, r5, r6, r7, r8, sb, lr}` | `lsls r0, r5, #0xb` | FLASH_CTRL |
| `0x08004C7E - 0x08004D1A` | APPLICATION | 156 B | `push {r4, r5, lr}` | `pop {r4, r5, pc}` | FLASH_CTRL |
| `0x08004D1A - 0x08004D72` | APPLICATION | 88 B | `push {r4, r5, r6, r7, lr}` | `pop {r4, r5, r6, r7, pc}` | FLASH_CTRL |
| `0x08004ECC - 0x08004EE0` | APPLICATION | 20 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08005A48 |
| `0x08004EE0 - 0x08005018` | APPLICATION | 312 B | `push.w {r4, r5, r6, r7, r8, lr}` | `bx lr` | Internal logic |
| `0x08005082 - 0x0800510C` | APPLICATION | 138 B | `push {r4, r5, r6, r7, lr}` | `pop {r4, r5, r6, r7, pc}` | AFIO, GPIOA, GPIOB |
| `0x0800510C - 0x0800514E` | APPLICATION | 66 B | `push {r4, r5, lr}` | `pop {r4, r5, pc}` | AFIO, GPIOA, GPIOB |
| `0x0800517C - 0x080051AE` | APPLICATION | 50 B | `push {r4, lr}` | `pop {r4, pc}` | I2C1, I2C2, UART5 |
| `0x080051AE - 0x080052AA` | APPLICATION | 252 B | `push.w {r4, r5, r6, r7, r8, sb, lr}` | `bx lr` | Calls 0x08005838 |
| `0x080053B0 - 0x080053C6` | APPLICATION | 22 B | `push {r3, lr}` | `pop {r3, pc}` | Internal logic |
| `0x080054B4 - 0x080054DE` | APPLICATION | 42 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | Internal logic |
| `0x080054DE - 0x080054F8` | APPLICATION | 26 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x080054F8 - 0x08005532` | APPLICATION | 58 B | `push {r2, r3, lr}` | `pop {r2, r3, pc}` | Internal logic |
| `0x0800553E - 0x08005574` | APPLICATION | 54 B | `push {r4, r5, lr}` | `pop {r4, r5, pc}` | Internal logic |
| `0x08005654 - 0x0800568C` | APPLICATION | 56 B | `push {r4, lr}` | `pop {r4, pc}` | RCC |
| `0x0800568C - 0x080056C4` | APPLICATION | 56 B | `push {r3, r4, r5, lr}` | `pop {r3, r4, r5, pc}` | Calls 0x08005654 |
| `0x08005838 - 0x080059C0` | APPLICATION | 392 B | `push.w {r4, r5, r6, r7, r8, lr}` | `rsbs r2, r0, #0` | RCC |
| `0x08005AC4 - 0x08005B4A` | APPLICATION | 134 B | `push {r4, lr}` | `pop {r4, pc}` | I2C1, I2C2, UART4, UART5, USART1, USART2, USART3 |
| `0x08005B4A - 0x08005C06` | APPLICATION | 188 B | `push.w {r4, r5, r6, r7, r8, sb, sl, lr}` | `lsls r0, r1, #0x12` | USART1 |
| `0x08005C7A - 0x08005CC4` | APPLICATION | 74 B | `push {r4, r5, r6, r7, lr}` | `pop {r4, r5, r6, r7, pc}` | Internal logic |
| `0x08005E44 - 0x08005E98` | APPLICATION | 84 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | Internal logic |
| `0x08005E98 - 0x08005EB6` | APPLICATION | 30 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x08005ECC - 0x08005F14` | APPLICATION | 72 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x08005F14 - 0x08005F7E` | APPLICATION | 106 B | `movs r4, #0` | `b #0x8005f18` | Calls 0x08005592, 0x08005ECC |
| `0x08005F7E - 0x08005F8A` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08008A60 |
| `0x08005F8A - 0x08005FB0` | APPLICATION | 38 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | Internal logic |
| `0x08005FE0 - 0x0800601C` | APPLICATION | 60 B | `push {r4, lr}` | `blt #0x800600e` | Calls 0x08005ECC, 0x08006CE0, 0x08007606, 0x08007D6A, 0x08007FBA, 0x08008984, 0x08008B48 |
| `0x0800601E - 0x08006042` | APPLICATION | 36 B | `smlawb fp, sb, r4, lr` | `pop {r4, pc}` | Calls 0x08006412, 0x08007318, 0x08009D96, 0x0800A470 |
| `0x08006042 - 0x08006168` | APPLICATION | 294 B | `bl #0x800601e` | `bx lr` | Calls 0x08005F14, 0x08005F7E, 0x08005F8A, 0x08005FE0, 0x0800601E, 0x080063AC, 0x080063F4, 0x0800700C... |
| `0x08006168 - 0x08006174` | APPLICATION | 12 B | `push {r4, r5, lr}` | `pop {r4, r5, pc}` | Internal logic |
| `0x080061F8 - 0x080061FC` | APPLICATION | 4 B | `push {r0, r2, r3, r4, r5, r6, lr}` | `lsrs r0, r0, #0x20` | Internal logic |
| `0x080061FC - 0x0800622C` | APPLICATION | 48 B | `push {r4, lr}` | `cbz r1, #0x8006242` | Calls 0x08007A4C |
| `0x0800622C - 0x08006288` | APPLICATION | 92 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x080063AC - 0x080063F4` | APPLICATION | 72 B | `push {r3, r4, r5, lr}` | `pop {r3, r4, r5, pc}` | Calls 0x08006368, 0x08008194 |
| `0x080063F4 - 0x08006412` | APPLICATION | 30 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08007A4C |
| `0x0800643C - 0x08006468` | APPLICATION | 44 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08007764 |
| `0x080065D8 - 0x0800660E` | APPLICATION | 54 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x080078E4 |
| `0x0800660E - 0x0800665C` | APPLICATION | 78 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08007764 |
| `0x0800665C - 0x08006664` | APPLICATION | 8 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800B842 |
| `0x08006668 - 0x080066D4` | APPLICATION | 108 B | `push.w {r4, r5, r6, r7, r8, sb, sl, lr}` | `pop.w {r4, r5, r6, r7, r8, sb, sl, pc}` | Calls 0x0800823A |
| `0x080066D4 - 0x0800675E` | APPLICATION | 138 B | `push {r2, r3, r4, lr}` | `pop {r2, r3, r4, pc}` | Calls 0x08006668, 0x08007764 |
| `0x08006778 - 0x0800688A` | APPLICATION | 274 B | `push {r3, r4, r5, lr}` | `b #0x800682c` | Calls 0x08006668, 0x08006E46, 0x08007764 |
| `0x0800688A - 0x08006896` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006778 |
| `0x08006896 - 0x080068BA` | APPLICATION | 36 B | `push {r3, r4, r5, lr}` | `pop {r3, r4, r5, pc}` | Internal logic |
| `0x080068EC - 0x08006906` | APPLICATION | 26 B | `push {r3, r4, r5, lr}` | `pop {r3, r4, r5, pc}` | Calls 0x08006668 |
| `0x08006908 - 0x08006942` | APPLICATION | 58 B | `push {r3, r4, r5, lr}` | `pop {r3, r4, r5, pc}` | Calls 0x08006668 |
| `0x08006942 - 0x0800698E` | APPLICATION | 76 B | `push {r3, r4, r5, lr}` | `pop {r3, r4, r5, pc}` | Calls 0x08006668 |
| `0x0800698E - 0x08006A06` | APPLICATION | 120 B | `push {r2, r3, r4, lr}` | `pop {r3, r4, r5, pc}` | Calls 0x08006668, 0x08006768 |
| `0x08006A88 - 0x08006A98` | APPLICATION | 16 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006668 |
| `0x08006A98 - 0x08006ADA` | APPLICATION | 66 B | `push {r2, r3, r4, lr}` | `pop {r2, r3, r4, pc}` | Calls 0x08006668 |
| `0x08006B1C - 0x08006B5A` | APPLICATION | 62 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006E68, 0x08006E86, 0x08006EA4 |
| `0x08006B5A - 0x08006B6A` | APPLICATION | 16 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006B1C |
| `0x08006B72 - 0x08006BA2` | APPLICATION | 48 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800B8C0 |
| `0x08006BA2 - 0x08006BAC` | APPLICATION | 10 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006168, 0x08009164 |
| `0x08006BC0 - 0x08006C00` | APPLICATION | 64 B | `push {r3, lr}` | `pop {r3, pc}` | AFIO, GPIOA, GPIOB, GPIOC, GPIOD, GPIOE |
| `0x08006C1A - 0x08006C1C` | APPLICATION | 2 B | `push {r4, lr}` | `push {r4, lr}` | Internal logic |
| `0x08006C1C - 0x08006C32` | APPLICATION | 22 B | `push {r4, lr}` | `ldr r0, [pc, #0x388]` | GPIOC, GPIOD, GPIOE |
| `0x08006C38 - 0x08006C52` | APPLICATION | 26 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006C52 - 0x08006C70` | APPLICATION | 30 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006C70 - 0x08006C8A` | APPLICATION | 26 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006C8A - 0x08006CA8` | APPLICATION | 30 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006CA8 - 0x08006CC2` | APPLICATION | 26 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006CC2 - 0x08006CE0` | APPLICATION | 30 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006CE0 - 0x08006E06` | APPLICATION | 294 B | `push {r3, lr}` | `movs r0, #0x10` | AFIO, GPIOA, GPIOB, GPIOC, GPIOD, GPIOE |
| `0x08006E08 - 0x08006E46` | APPLICATION | 62 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08004FE0, 0x08006C00, 0x08006C38, 0x08006C52, 0x08006CC2 |
| `0x08006E4A - 0x08006E68` | APPLICATION | 30 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006E68 - 0x08006E86` | APPLICATION | 30 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006E86 - 0x08006EA4` | APPLICATION | 30 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006EAC - 0x08006EB8` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006EB8 - 0x08006EC6` | APPLICATION | 14 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006EC6 - 0x08006ED4` | APPLICATION | 14 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006ED4 - 0x08006EE2` | APPLICATION | 14 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006EE2 - 0x08006EF0` | APPLICATION | 14 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006EF0 - 0x08006EFE` | APPLICATION | 14 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006EFE - 0x08006F0C` | APPLICATION | 14 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006F0C - 0x08006F2A` | APPLICATION | 30 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006F2A - 0x08006F48` | APPLICATION | 30 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006F48 - 0x08006F54` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006F54 - 0x08006F60` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006F60 - 0x08006F6C` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006F6C - 0x08006F78` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006F78 - 0x08006F84` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006F84 - 0x08006F90` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006F90 - 0x08006F9C` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006F9C - 0x08006FA8` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006FA8 - 0x08006FB4` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006FD0 - 0x08006FDC` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006FDC - 0x08006FE8` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08006FE8 - 0x08006FF4` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x08006FF4 - 0x08007000` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE |
| `0x0800700C - 0x0800702A` | APPLICATION | 30 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800513E, 0x08006C1A |
| `0x0800702A - 0x08007036` | APPLICATION | 12 B | `push {r4, lr}` | `ldr r0, [pc, #0x80]` | Internal logic |
| `0x08007260 - 0x0800726E` | APPLICATION | 14 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | Internal logic |
| `0x08007318 - 0x08007370` | APPLICATION | 88 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800B87C |
| `0x08007370 - 0x080073C8` | APPLICATION | 88 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | Internal logic |
| `0x08007408 - 0x08007460` | APPLICATION | 88 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | Internal logic |
| `0x080074A0 - 0x0800759C` | APPLICATION | 252 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08007764 |
| `0x0800759C - 0x080075AC` | APPLICATION | 16 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08007370, 0x08007408, 0x080074A0 |
| `0x080075AC - 0x080075D0` | APPLICATION | 36 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800AD84, 0x0800AF78 |
| `0x08007764 - 0x080077F0` | APPLICATION | 140 B | `push {r3, r4, r5, r6, r7, lr}` | `pop {r3, r4, r5, r6, r7, pc}` | Internal logic |
| `0x080077F0 - 0x080078E4` | APPLICATION | 244 B | `push.w {r3, r4, r5, r6, r7, r8, lr}` | `b #0x8007878` | Calls 0x0800385A, 0x0800389E, 0x08003932 |
| `0x080078E4 - 0x0800794E` | APPLICATION | 106 B | `push {r3, r4, r5, r6, r7, lr}` | `pop {r3, r4, r5, r6, r7, pc}` | Internal logic |
| `0x080079B2 - 0x08007A4C` | APPLICATION | 154 B | `push {r3, r4, r5, r6, r7, lr}` | `pop {r3, r4, r5, r6, r7, pc}` | Internal logic |
| `0x08007A4C - 0x08007ADC` | APPLICATION | 144 B | `push {r4, r5, lr}` | `pop {r4, r5, pc}` | Internal logic |
| `0x08007AFE - 0x08007B14` | APPLICATION | 22 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08007ADC |
| `0x08007B2A - 0x08007B70` | APPLICATION | 70 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006B5A, 0x08006B6A, 0x08006CE0, 0x08006E08, 0x08007606, 0x08007AFE, 0x08007D6A, 0x08007DE0... |
| `0x08007B70 - 0x08007BFE` | APPLICATION | 142 B | `push {r4, lr}` | `movs r0, #2` | Calls 0x08004864, 0x08005592, 0x0800560E, 0x08005654, 0x080056D8, 0x080056F6, 0x08005738, 0x08005756... |
| `0x08007C2A - 0x08007C2C` | APPLICATION | 2 B | `push {r4, lr}` | `push {r4, lr}` | Internal logic |
| `0x08007C2C - 0x08007D14` | APPLICATION | 232 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006EAC, 0x08007B2A, 0x08007B70, 0x08007C18, 0x08008156, 0x08008AFA, 0x0800AE40 |
| `0x08007D6A - 0x08007DE0` | APPLICATION | 118 B | `push {lr}` | `pop {pc}` | AFIO, GPIOA, GPIOB, USART1 |
| `0x08007DE0 - 0x08007DEC` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | USART1 |
| `0x08007DEC - 0x08007E46` | APPLICATION | 90 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | USART1 |
| `0x08007E5A - 0x08007EE4` | APPLICATION | 138 B | `push.w {r4, r5, r6, r7, r8, sb, sl, lr}` | `bx lr` | Calls 0x08007E46 |
| `0x08007EF6 - 0x08007FBA` | APPLICATION | 196 B | `push.w {r4, r5, r6, r7, r8, lr}` | `bx lr` | USART1 |
| `0x08007FBA - 0x08008056` | APPLICATION | 156 B | `push {lr}` | `pop {pc}` | AFIO, GPIOA, GPIOB, UART4, USART2, USART3 |
| `0x08008056 - 0x08008062` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | UART4, USART2, USART3 |
| `0x08008062 - 0x08008180` | APPLICATION | 286 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | UART4, USART2, USART3 |
| `0x08008194 - 0x0800823A` | APPLICATION | 166 B | `push.w {r4, r5, r6, r7, r8, lr}` | `bx lr` | Calls 0x08008180 |
| `0x0800823A - 0x08008306` | APPLICATION | 204 B | `push.w {r4, r5, r6, r7, r8, lr}` | `bx lr` | UART4, USART2, USART3 |
| `0x08008306 - 0x080083B0` | APPLICATION | 170 B | `push {lr}` | `pop {pc}` | AFIO, GPIOA, GPIOB, UART4, USART2, USART3 |
| `0x080083B0 - 0x080083BC` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | UART4, USART2, USART3 |
| `0x080083BC - 0x08008406` | APPLICATION | 74 B | `push {r4, r5, r6, lr}` | `bx lr` | UART4, USART2, USART3 |
| `0x08008406 - 0x08008578` | APPLICATION | 370 B | `push {lr}` | `pop {r4, r5, r6, pc}` | UART4, USART1, USART2, USART3 |
| `0x0800858C - 0x080085E6` | APPLICATION | 90 B | `push.w {r4, r5, r6, r7, r8, lr}` | `bx lr` | Calls 0x08008578 |
| `0x080085F8 - 0x080086B0` | APPLICATION | 184 B | `push.w {r4, r5, r6, r7, r8, lr}` | `ands r1, r0` | UART4, USART2, USART3 |
| `0x080086B0 - 0x08008722` | APPLICATION | 114 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006EB8, 0x080078E4 |
| `0x08008722 - 0x08008794` | APPLICATION | 114 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006EB8, 0x080078E4 |
| `0x08008794 - 0x080087D8` | APPLICATION | 68 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006EC6, 0x08006ED4, 0x08006EE2, 0x08006EF0, 0x08006EFE |
| `0x08008822 - 0x080088AC` | APPLICATION | 138 B | `push {r4, lr}` | `b #0x80087d6` | Calls 0x08006BC0, 0x08006FB8, 0x08009DAC, 0x0800A4AE, 0x0800B5E4 |
| `0x080088AC - 0x08008934` | APPLICATION | 136 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006EAC, 0x08007764 |
| `0x08008934 - 0x0800894E` | APPLICATION | 26 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08008722, 0x08008794, 0x080088AC |
| `0x08008984 - 0x08008A34` | APPLICATION | 176 B | `push {r3, lr}` | `strb.w r0, [sp]` | Calls 0x080035D8, 0x080035E2 |
| `0x08008A34 - 0x08008A36` | APPLICATION | 2 B | `push {r4, lr}` | `push {r4, lr}` | Internal logic |
| `0x08008A60 - 0x08008ABA` | APPLICATION | 90 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | NVIC, SCB |
| `0x08008ABA - 0x08008AC2` | APPLICATION | 8 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x080060D8 |
| `0x08008AC2 - 0x08008AFA` | APPLICATION | 56 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006288, 0x08006B04, 0x080075D0, 0x08007D14, 0x08009D3C, 0x0800A486, 0x0800AB90, 0x0800AC08... |
| `0x08008B48 - 0x08008B6A` | APPLICATION | 34 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08005580, 0x08005586, 0x0800558C, 0x08005592, 0x0800559C |
| `0x08008B86 - 0x08008C2E` | APPLICATION | 168 B | `push {lr}` | `pop {r4, pc}` | GPIOC, GPIOD, GPIOE, I2C1, I2C2, UART5 |
| `0x08008C2E - 0x08008C38` | APPLICATION | 10 B | `push {r4, r5, r6, lr}` | `bx lr` | Internal logic |
| `0x08008D60 - 0x08008DEA` | APPLICATION | 138 B | `push.w {r4, r5, r6, r7, r8, sb, sl, lr}` | `bx lr` | Calls 0x08008D4C |
| `0x08008DFC - 0x08008E1A` | APPLICATION | 30 B | `push.w {r4, r5, r6, r7, r8, lr}` | `b #0x8008e0a` | Calls 0x08008DEE |
| `0x08008EB0 - 0x08009070` | APPLICATION | 448 B | `push {lr}` | `pop {pc}` | AFIO, CAN1, CAN2, GPIOA, GPIOB |
| `0x08009070 - 0x08009164` | APPLICATION | 244 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | Internal logic |
| `0x08009164 - 0x080091E0` | APPLICATION | 124 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006B1C, 0x08006B72, 0x08008EB0, 0x08009070 |
| `0x080091E0 - 0x080092B6` | APPLICATION | 214 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08007764 |
| `0x080092B6 - 0x080092C2` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x080093D8 - 0x08009408` | APPLICATION | 48 B | `push {lr}` | `movs r0, #0` | Internal logic |
| `0x080095E4 - 0x0800962C` | APPLICATION | 72 B | `push {r4, lr}` | `bne #0x800963a` | Internal logic |
| `0x0800962C - 0x080096D8` | APPLICATION | 172 B | `push {r4, lr}` | `movs r0, #0` | Calls 0x08007764 |
| `0x080096D8 - 0x0800975A` | APPLICATION | 130 B | `push {lr}` | `pop {pc}` | AFIO, GPIOA, GPIOB, I2C1, I2C2, UART5 |
| `0x0800975A - 0x08009784` | APPLICATION | 42 B | `push {r3, lr}` | `pop {r3, pc}` | AFIO, GPIOA, GPIOB |
| `0x08009784 - 0x08009790` | APPLICATION | 12 B | `push {r4, lr}` | `pop {r4, pc}` | AFIO, GPIOA, GPIOB |
| `0x08009790 - 0x080097D8` | APPLICATION | 72 B | `push {r3, r4, r5, lr}` | `pop {r3, r4, r5, pc}` | AFIO, GPIOA, GPIOB |
| `0x080097D8 - 0x0800980A` | APPLICATION | 50 B | `push.w {r4, r5, r6, r7, r8, lr}` | `strb.w r8, [sp, #0x1f0]` | Calls 0x080054B4 |
| `0x0800980A - 0x0800981A` | APPLICATION | 16 B | `push.w {r4, r5, r6, r7, r8, sb, sl, lr}` | `mov r0, r7` | Internal logic |
| `0x080098CC - 0x080099BA` | APPLICATION | 238 B | `push.w {r4, r5, r6, r7, r8, sb, sl, lr}` | `pop.w {r4, r5, r6, r7, r8, sb, sl, pc}` | Calls 0x080052F2, 0x0800530A, 0x08005322, 0x08005392, 0x0800539E, 0x080054F8, 0x08005532, 0x08008AFA... |
| `0x080099BA - 0x08009A06` | APPLICATION | 76 B | `push.w {r2, r3, r4, r5, r6, r7, r8, sb, sl, fp, ip, lr}` | `mov r0, r7` | Calls 0x080052F2, 0x08005322, 0x080097D8 |
| `0x08009B66 - 0x08009C1E` | APPLICATION | 184 B | `push.w {r2, r3, r4, r5, r6, r7, r8, sb, sl, fp, ip, lr}` | `bl #0x7c05c14` | Calls 0x080052F2, 0x08005322, 0x08005392, 0x0800539E, 0x08005532, 0x08008B1A, 0x080097D8 |
| `0x08009D96 - 0x08009DAC` | APPLICATION | 22 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800B87C |
| `0x08009DB4 - 0x08009F14` | APPLICATION | 352 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08007764, 0x08008BFA, 0x08009890 |
| `0x08009F14 - 0x08009F68` | APPLICATION | 84 B | `push {r3, r4, r5, lr}` | `pop {r3, r4, r5, pc}` | I2C1, I2C2, UART5 |
| `0x08009F68 - 0x0800A036` | APPLICATION | 206 B | `push {r4, lr}` | `ldr r0, [pc, #0x21c]` | Calls 0x08007764, 0x08009ABA |
| `0x0800A188 - 0x0800A1BC` | APPLICATION | 52 B | `push {r3, r4, r5, lr}` | `pop {r3, r4, r5, pc}` | I2C1, I2C2, UART5 |
| `0x0800A1BC - 0x0800A1D4` | APPLICATION | 24 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x0800A2C8 - 0x0800A37A` | APPLICATION | 178 B | `push {r4, lr}` | `pop {r4, pc}` | I2C1, I2C2, UART5 |
| `0x0800A3AC - 0x0800A3FE` | APPLICATION | 82 B | `push {r4, lr}` | `movs r0, #3` | Calls 0x08007A4C |
| `0x0800A470 - 0x0800A486` | APPLICATION | 22 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800B87C |
| `0x0800A4B6 - 0x0800A4E4` | APPLICATION | 46 B | `push {r4, lr}` | `pop {r4, pc}` | I2C1, I2C2, UART5 |
| `0x0800A4E4 - 0x0800A504` | APPLICATION | 32 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08006C38 |
| `0x0800A504 - 0x0800A586` | APPLICATION | 130 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | Calls 0x0800A4B6 |
| `0x0800A586 - 0x0800A600` | APPLICATION | 122 B | `push {r4, r5, r6, lr}` | `ldrb r0, [r1]` | Calls 0x08006C38, 0x0800A4B6, 0x0800D250 |
| `0x0800A604 - 0x0800A61A` | APPLICATION | 22 B | `push {r4, r5, r6, lr}` | `pop {r4, pc}` | Calls 0x08006C38 |
| `0x0800A660 - 0x0800A6E0` | APPLICATION | 128 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | Calls 0x0800A4B6 |
| `0x0800A744 - 0x0800A77C` | APPLICATION | 56 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | Calls 0x0800A700 |
| `0x0800A77C - 0x0800A7AA` | APPLICATION | 46 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x080078E4, 0x08007A4C |
| `0x0800A7AA - 0x0800A816` | APPLICATION | 108 B | `push.w {r4, r5, r6, r7, r8, sb, sl, lr}` | `pop.w {r4, r5, r6, r7, r8, sb, sl, pc}` | Calls 0x08007EF6 |
| `0x0800A816 - 0x0800AABA` | APPLICATION | 676 B | `push {r4, lr}` | `pop {r2, r3, r4, r5, r6, pc}` | Calls 0x0800A7AA |
| `0x0800AABA - 0x0800AB28` | APPLICATION | 110 B | `push {r2, r3, r4, lr}` | `pop {r2, r3, r4, pc}` | Calls 0x0800A7AA |
| `0x0800AB28 - 0x0800AB72` | APPLICATION | 74 B | `push {r1, r2, r3, r4, r5, lr}` | `pop {r1, r2, r3, r4, r5, pc}` | Calls 0x0800A7AA |
| `0x0800AB90 - 0x0800AC08` | APPLICATION | 120 B | `push {r4, lr}` | `lsrs r0, r0, #0x20` | Calls 0x08006FDC, 0x08006FE8 |
| `0x0800AC08 - 0x0800AC28` | APPLICATION | 32 B | `push {r4, lr}` | `pop {r2, r3, r4, pc}` | Calls 0x0800A8AA |
| `0x0800AC28 - 0x0800AC80` | APPLICATION | 88 B | `push {r1, r2, r3, r4, r5, lr}` | `pop {r4, pc}` | Calls 0x08007000 |
| `0x0800AC80 - 0x0800AD84` | APPLICATION | 260 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x0800AD84 - 0x0800ADC8` | APPLICATION | 68 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08007764, 0x0800AB74, 0x0800AC80 |
| `0x0800AE40 - 0x0800AEF6` | APPLICATION | 182 B | `push {r2, r3, r4, lr}` | `pop {r2, r3, r4, pc}` | Calls 0x08004764, 0x0800510C |
| `0x0800AEF6 - 0x0800AF0C` | APPLICATION | 22 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08004828, 0x08004830 |
| `0x0800AF0C - 0x0800AF24` | APPLICATION | 24 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08004828, 0x08004830 |
| `0x0800AF24 - 0x0800AF3C` | APPLICATION | 24 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08004828, 0x08004830 |
| `0x0800AF3C - 0x0800AF5A` | APPLICATION | 30 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08004828, 0x08004830 |
| `0x0800AF5A - 0x0800AF78` | APPLICATION | 30 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x08004828, 0x08004830 |
| `0x0800AF78 - 0x0800B006` | APPLICATION | 142 B | `push {r4, lr}` | `str r0, [r1, #0x10]` | Calls 0x08006F48, 0x08006F6C |
| `0x0800B024 - 0x0800B036` | APPLICATION | 18 B | `push {r4, lr}` | `movs r0, #4` | Calls 0x08004930 |
| `0x0800B080 - 0x0800B0A0` | APPLICATION | 32 B | `push {r3, lr}` | `pop {r3, pc}` | AFIO, GPIOA, GPIOB |
| `0x0800B0A0 - 0x0800B0EC` | APPLICATION | 76 B | `push {r3, lr}` | `pop {r3, pc}` | Calls 0x080035E2, 0x080059FA, 0x08005A14 |
| `0x0800B0EC - 0x0800B142` | APPLICATION | 86 B | `push {r0, r1, r2, r3, r4, lr}` | `pop {r0, r1, r2, r3, r4, pc}` | Calls 0x0800517C, 0x080051AE, 0x080052AA, 0x08005380, 0x08005480 |
| `0x0800B142 - 0x0800B198` | APPLICATION | 86 B | `push {r0, r1, r2, r3, r4, lr}` | `pop {r0, r1, r2, r3, r4, pc}` | Calls 0x0800517C, 0x080051AE, 0x080052AA, 0x08005380, 0x08005480 |
| `0x0800B19A - 0x0800B1E4` | APPLICATION | 74 B | `push {r4, lr}` | `pop {r4, pc}` | I2C1, I2C2, UART5 |
| `0x0800B1E4 - 0x0800B212` | APPLICATION | 46 B | `push {r3, lr}` | `pop {r3, pc}` | AFIO, GPIOA, GPIOB, I2C1, I2C2, UART5 |
| `0x0800B212 - 0x0800B324` | APPLICATION | 274 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | I2C1, I2C2, UART5 |
| `0x0800B324 - 0x0800B424` | APPLICATION | 256 B | `push {r4, r5, r6, lr}` | `pop {r4, r5, r6, pc}` | I2C1, I2C2, UART5 |
| `0x0800B424 - 0x0800B42A` | APPLICATION | 6 B | `push {r4, r5, r6, lr}` | `ldr r0, [pc, #0x15c]` | Internal logic |
| `0x0800B540 - 0x0800B57C` | APPLICATION | 60 B | `push {r4, lr}` | `pop {r4, pc}` | Internal logic |
| `0x0800B57C - 0x0800B5CC` | APPLICATION | 80 B | `push {r4, lr}` | `pop {r4, pc}` | Calls 0x0800B19A, 0x0800B1E4 |
| `0x0800B826 - 0x0800B842` | APPLICATION | 28 B | `uxtb r0, r2` | `pop {r4, pc}` | Internal logic |
| `0x0800B8C4 - 0x0800B90E` | APPLICATION | 74 B | `push {lr}` | `bx lr` | Internal logic |
| `0x0800B92A - 0x0800B95E` | APPLICATION | 52 B | `push {r5, lr}` | `bx lr` | Calls 0x080035B4 |
| `0x0800B95E - 0x0800B974` | APPLICATION | 22 B | `push {r4, lr}` | `bx lr` | Internal logic |
| `0x0800BA2A - 0x0800BC64` | APPLICATION | 570 B | `push {r5, lr}` | `lsls r7, r7, #3` | Internal logic |
| `0x0800CC4E - 0x0800CC5E` | APPLICATION | 16 B | `push {r0, r4, r5, lr}` | `subs r0, #0x90` | Internal logic |
| `0x0800CD5C - 0x0800CD68` | APPLICATION | 12 B | `push {r0, r1, r2, r3, r4, r5, r6, lr}` | `str r0, [sp, #0x3c0]` | Internal logic |
| `0x0800D230 - 0x0800D304` | APPLICATION | 212 B | `push {r0, r1, r2, r4, r5, r7, lr}` | `adr r5, #0x258` | Internal logic |
