# MCU Firmware Review — `can_app.bin` (STM32F105RBT6)

Analysis of the vehicle-side I/O co-processor firmware for the Limcet Box P306.
The MCU is an **STM32F105RBT6** (ARM Cortex-M3, connectivity line, 128 KB flash /
64 KB SRAM) that handles CAN bus, steering-wheel / panel keys, reverse and
ACC-IGN signals, and drives the Feasycom BT module. It talks to the ARK1668 SoC
over `/dev/ttyHS0`.

Cross-references: [../docs/MCU_ADAPTERS.md](../docs/MCU_ADAPTERS.md),
[../docs/CANBUS.md](../docs/CANBUS.md), [../docs/SECURITY_REVIEW.md](../docs/SECURITY_REVIEW.md),
`../Prado firmware dump/mtd6_rootfs/usr/lib/libMcuCenter.so` (SoC-side driver).

---

## 1. Files in this directory

| File | Size | Notes |
|------|------|-------|
| `can_app.bin` | 31,996 B | The MCU **application** image (raw, links at 0x08004000). |
| `auto_upgrade.txt` | 0 B | Empty sentinel/flag file — an update-package trigger, not config. |

`can_app.bin` is the *application half only*. It is effectively the payload of
the vendor's `McuAppUpdate.img` (see §5); a resident bootloader that is **not**
in this repo lives below it in flash.

---

## 2. Identity & memory map

Confirmed by parsing the vector table and by pointer-range analysis of the image.

| Property | Value |
|---|---|
| Core | ARM Cortex-M3 (STM32F105RB, connectivity line) |
| Image size | 31,996 B (~31 KB of 128 KB flash) |
| **Link/load address** | **0x08004000** |
| Initial SP | 0x20002DC0 |
| Reset handler | 0x08004239 (Thumb) |
| Flash pointers observed | 0x08004000 … 0x0800F04F |
| SRAM pointers observed | 0x2000xxxx (data/bss/stack) |

**Two-stage layout.** Everything links at 0x08004000 with a clean, empty 16 KB
gap below it. That gap (0x08000000–0x08004000) holds a separate **resident
bootloader**, which is why the application starts at 0x08004000. That bootloader
is what performs firmware updates (§5). It never leaves the chip and is **not in
this repo** — the repo only contains Cortex-A SoC boot binaries
(Nboot/Stepldr/u-boot/ARKSDLDR), none of which are the STM32 bootloader.

Reset handler is a textbook `SystemInit(); main();`:
```
0x08004238  ldr r0,[pc]; blx r0     ; SystemInit / lib init
0x0800423c  ldr r0,[pc]; bx  r0     ; -> main()
```

---

## 3. Interrupt handlers & peripherals

The 64-entry IRQ table points almost everything at a common dummy trap
(`0x08004253`). The live handlers:

| IRQ (STM32F105) | Handler | Role |
|---|---|---|
| CAN1_RX0 | 0x08007064 | Fills a 15-slot × 20-byte software ring buffer from CAN FIFO0 |
| USART2 | 0x08007298 | Framed byte protocol (RX state machine + TX ring) |
| USART3 | 0x0800755c | Framed byte protocol |
| UART4  | 0x08007780 | Framed byte protocol |
| UART5  | 0x08007a9c | Framed byte protocol |
| USART1 | — | `bx lr`, unused |

Peripheral register references in the image (peripheral base → hit count):
GPIOB ×22, GPIOC ×13, GPIOA ×12, RCC ×11, **IWDG ×6** (independent watchdog is
enabled), **bxCAN1 ×5 / bxCAN2 ×4**, USART2/3 + UART4/5, FLASH ×3, GPIOD ×2, AFIO, PWR.

Only **CAN1** raises RX interrupts; CAN2 registers are touched but no CAN2 IRQ
vector is populated (filter/master use or polled).

### 3.1 UART framing protocol
The USART handlers run a **stateful binary packet parser** — a state byte in
SRAM (`0x2000005a`) walks cases 0/1/2/3 = header → length → payload → checksum,
with a TX ring buffer drained on TXE. This is the head-unit ↔ MCU protocol. One
UART is the SoC link (`/dev/ttyHS0`); another drives the Feasycom BT module.
`/dev/ttyHS0` on the SoC side is a raw PL011 UART (see
`../linux-arkmicro Reference/.../mcu_serial.c`); all framing is in software above it.

### 3.2 Bluetooth AT commands (outbound to the Feasycom module)
These strings are **sent by the MCU to the BT chip**, not the STM32's own interface:
```
AT+NAME=DC_AUDIO_BT5.0,1   AT+PIN=0000     AT+LINKCFG=0
AT+LEDCFG=3                AT+AUDROUTE=1/2 AT+UPGRADE
```
Note `AT+UPGRADE` upgrades the **Feasycom BT module**, not the STM32 — unrelated
to MCU reflashing. `AT+PIN=0000` is the weak default legacy pairing PIN.

### 3.3 Version string
```
DCn32-VOLVO-V2.10-20240909
```
"DCn32" is the generic CAN-decoder product line; the compiled-in vehicle profile
is **VOLVO**, dated 2024-09-09. For a Toyota Prado this is the standout concern —
SWC key codes, reverse/ACC-IGN triggers and illumination decoding are
vehicle-specific. A Volvo profile will not correctly decode the Toyota bus
(Prado SWC is CAN ID `0x3C4` at 500 kbit/s per [../docs/CANBUS.md](../docs/CANBUS.md)).
**Verify this is the intended image before flashing it to a Prado.**

### 3.4 The application cannot program flash
Searched the whole image for the STM32 flash-unlock keys `0x45670123` /
`0xCDEF89AB` and the option-byte keys — **none are present.** The FLASH
peripheral base (0x40022000) is referenced only 3× (wait-states / flash-size
reads). All erase/program logic lives in the resident bootloader, not here.

---

## 4. Two-stage architecture (why this matters)

```
0x08000000  ┌──────────────────────────────┐
            │ Resident bootloader (16 KB)  │  ← YMODEM receiver, does the actual
            │  - NOT in this repo          │    flash erase/program
            │  - never leaves the chip     │
0x08004000  ├──────────────────────────────┤
            │ Application = can_app.bin    │  ← this file (CAN/keys/BT, no flash access)
0x0800BCFC  └──────────────────────────────┘
            (…up to 0x08020000 = 128 KB)
```

---

## 5. How the MCU update file is actually flashed

Reconstructed from `libMcuCenter.so` (SoC side, runs under MsnCoreApp on the
ARK1668). **The SoC never writes STM32 flash directly.** It acts as a **YMODEM
sender**; the resident bootloader inside the STM32 (§4) is the YMODEM *receiver*
and is the code that erases and programs the flash.

### 5.1 Update files
| Name | Meaning |
|---|---|
| `McuAppUpdate.img` | Application image (this = `can_app.bin` payload, written to 0x08004000). |
| `McuSubUpdate.img` | Secondary / sub-processor image (optional). |
| `mcuupdate_hud.bin` | HUD-variant image. |
| dir `mcuupdate4/` | Where the SoC looks for MCU update files. |

Config keys (from `MsnProductInfo.ini` / `FactoryConfig.ini`):
`MCUPortName="/dev/ttyHS0"`, `MCUBaudSpeed`, `MCUUpdateName`, `McuType=6`,
`CanType=0`, `DisableSetMCUType=1`.

### 5.2 Flow (symbols/log strings from `libMcuCenter.so`)
1. Update image placed in the firmware/USB dir (`mcuupdate4/`);
   `UpdateDialog::copyUpdateFileToTempDir` copies it to `/tmp/`. Name from `MCUUpdateName`.
2. **Trigger:** `onStartUpdateMCU()` (user action / disk insert / firmware package).
   `tryOpenUpdateFile` → `checkMCUUpdateFile` validate it.
3. `Open MCU Serial Port` (ttyHS0 at `MCUBaudSpeed`).
   `readyToUpdateMCU` / `sendReadyPackage` / `onSendUpdateReadyTimer` send an
   in-band command over the normal MCU protocol that tells the running app to
   **hand off / reset into its resident bootloader** and listen for YMODEM.
4. `sendUpdateFileInfo()` → YMODEM **block 0** (filename + size). Log: `Start send update packaget 0`.
5. `sendUpdateFileData(i)` / `sendYModemDatas(...)` stream 128/1024-byte YMODEM
   blocks, each with **`CRC16_YMODEM`**, waiting for ACK; on NAK it resends
   `gLastUpdateByteArray` (log: `ReSend Last Update Datas:`).
   `UpdateDialog::updatePercent` / `setUpdateFailed` drive the progress UI.
   `gUpdateMasks` / `C UpdateMask:` select which images (App vs Sub) to send.
6. STM32 bootloader erases the app region and programs each block into
   0x08004000+. EOT → log `End Update Mcu!`.
7. `RequestRebootSystem` / the MCU resets into the new application.

### 5.3 Consequences
- **Write-only push.** YMODEM upload provides **no read-back** path. There is no
  documented way to read/dump MCU flash back over `/dev/ttyHS0`.
- **Integrity only, no authenticity.** Transfer is protected by CRC16-YMODEM
  (corruption detection) — there is **no signature / authentication** of the
  image (matches [../docs/SECURITY_REVIEW.md](../docs/SECURITY_REVIEW.md): "no firmware
  update signature/checksum verification"). Anyone with root on the SoC can drop
  a `McuAppUpdate.img` and trigger a flash of arbitrary MCU firmware.
- The resident bootloader is required for this to work and is **not recoverable
  from the SoC** — it never transits `ttyHS0`.

---

## 6. Dumping the existing MCU flash

There is **no read-back in the normal software stack**: the running app has no
memory-read command and cannot touch flash (§3.4), and the SoC updater is a
write-only YMODEM push (§5.3). Options, best-first:

**Route 1 — You may already have it (do first).** The factory image is likely a
file already on the device (`mcuupdate4/McuAppUpdate.img`, `msnmcu_update.bin`,
etc.). This `can_app.bin` almost certainly came from such a file. Grab it and
compare its version string against `DCn32-VOLVO-…`. Caveat: it is the *update*
image the vendor shipped, which may differ from what is physically on the chip.

**Route 2 — STM32 factory ROM bootloader (AN3155) over UART** — the only
*documented* serial read. Blockers, in order:
- **BOOT0 = 1 at reset** (hardware pin). If the board doesn't route BOOT0 to a
  SoC-controllable GPIO (it usually doesn't), you cannot enter it from software
  over `ttyHS0`; you need physical access to strap BOOT0 + reset.
- **`ttyHS0` must land on a bootloader-capable UART** (F105 ROM: USART1/USART2 or
  CAN/DFU per AN2606). The app link looks like USART2 (0x40004400), which the ROM
  supports — confirm the physical pin mapping.
- **Read-out protection (RDP).** If option-byte RDP ≥ level 1, Read Memory
  returns NACK and any bypass triggers mass-erase. RDP state is not in this image;
  check it first. If set, serial dump is impossible without wiping.
- If all clear: `stm32flash -r dump.bin /dev/ttyHS0` from the SoC (cross-compile
  `stm32flash` for the ARK's ARMv7 Linux, or script AN3155 at even parity).

**Route 3 — SWD (most reliable; not via `ttyHS0`).** ST-Link/J-Link on
SWDIO/SWCLK, then `st-flash read dump.bin 0x08000000 0x20000` reads the full
128 KB — **including the 16 KB resident bootloader this file lacks**, which is
what you actually want for a complete reconstruction. Blocked only by RDP.

**Recommended:** (1) harvest `mcuupdate4/` off the device and diff versions;
(2) live-capture `cat /dev/ttyHS0 | hexdump -C` while pressing SWC keys to learn
framing/baud and confirm which STM32 UART it is; (3) before any serial attempt,
determine RDP + BOOT0 accessibility; otherwise use SWD for a guaranteed complete dump.

---

## 7. Summary of notable findings
- Structurally valid, uncorrupted STM32F105 CAN-gateway app with watchdog enabled.
- **Volvo** vehicle profile (`DCn32-VOLVO-V2.10-20240909`) — likely wrong for a Prado; verify before flashing.
- Application has **no flash-programming code**; a separate resident bootloader (0x08000000–0x08004000, not in repo) does updates.
- MCU updates are a **YMODEM push** (`McuAppUpdate.img`) from the SoC over `/dev/ttyHS0` — CRC-protected but **unauthenticated**.
- **No serial read-back** exists; dumping the live chip needs the ROM bootloader (BOOT0 + no RDP) or SWD.
- BT module uses default PIN `0000`.
