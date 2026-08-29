# Limcet STM32F105 Companion MCU Source Code

Standalone bare-metal C firmware codebase for the **STM32F105RBT6** companion MCU used in the Limcet Box P305 / P306 CarPlay / Android Auto head unit platform.

---

## 1. Directory Structure

```
hardware/MCU/source/
├── Makefile                     # Build system (generates .bin, .hex, .elf, .lst)
├── stm32f105_app.ld             # Linker script (links at application base 0x08004000)
├── README.md                    # This documentation
├── include/
│   ├── stm32f105.h              # Peripheral register maps (RCC, GPIO, AFIO, bxCAN, USART)
│   ├── can_driver.h             # bxCAN1 driver & circular RX ring buffer definitions
│   ├── uart_protocol.h          # SoC UART communication framing & command definitions
│   ├── vehicle_profiles.h       # Vehicle CAN message dispatch tables & button mapping
│   └── tea_crypto.h             # CMD 0x88 TEA cipher (real algorithm + real recovered key)
└── src/
    ├── startup_stm32f105.c      # Vector table & Reset_Handler
    ├── main.c                   # System clocks, watchdog, GPIO init, and main loop
    ├── can_driver.c             # bxCAN1 driver implementation & ISR
    ├── uart_protocol.c          # USART2 driver (/dev/ttyHS0 @ 38400) & SoC protocol engine
    ├── vehicle_profiles.c       # Toyota Prado 150 CAN decoding implementation
    └── tea_crypto.c             # CMD 0x88 TEA cipher implementation
```

## Status: real, disassembly-verified findings, not just clean-room guesses

This source started as a plausible-looking clean-room reimplementation
inherited from an earlier, largely unverified handoff document. As of
2026-08-29 a long session of direct disassembly against the real,
unprotected reference binary (`hardware/MCU/can_app.bin`, RDP-locked on
the actual chip but freely readable as a file) replaced most of the
guesswork with confirmed real behavior -- and found and fixed several
real errors along the way (a wrong SoC-reset pin, fabricated CAN IDs, a
misattributed GPIO relay). **`docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md` is
the authoritative, continuously-updated record of every finding** --
read it before trusting any specific address/pin/command claim in this
source's own comments, which may lag behind the latest correction.

Headline real findings, each with much more detail in that doc:
- **`GPIOB Pin 14`**, not `PC13`, is the real ArkMicro SoC hardware-reset
  line -- confirmed both by disassembly and by this project's own earlier
  live-hardware SWD finding.
- **`GPIOC Pin 13`/`Pin 2`** are a real, hardware-confirmed combined
  audio+video OEM-bypass relay pair (your own stock head unit's screen
  and speakers going dark when SWD halts the MCU is direct, independent
  confirmation of this, not just static analysis).
- **`CMD 0x88`'s TEA cipher key was fully recovered** (not guessed) by
  tracing the real firmware's own `.data` init table to its flash source
  bytes -- real algorithm, real key, real (weak, ~32-bit effective)
  entropy, all in `tea_crypto.h`/`.c`.
- **`CMD 0x84`, `0x87`, `0x85`, `0xFF`** are all now implemented against
  their real disassembled handlers, including a genuinely surprising one:
  `CMD 0x84` ("Audio Route") sends real `AT+AUDROUTE=1`/`=2` over a
  second UART (`USART3`, real pins PB10/PB11) and drives the exact same
  GPIOC13/PC2 relay `CMD 0xA0 id=0x11` does.
- **The full real `CMD 0xA0` settings list was cross-referenced against
  the actual stock vendor app** (`MCUAdapter_BoxP300` in
  `usr/lib/libMcuCenter.so`) -- every populated item index matched to its
  real display label and its real MCU-side effect. `id=0x11`'s real
  vendor-assigned meaning is **"Microphone,"** not camera-related as
  earlier guessed.
- The Toyota Prado 150 CAN IDs below (`0x3C4`/`0x025`/`0x127`) were
  **never actually disassembly-derived** -- they don't appear in any of
  this project's three real reference firmware binaries' own CAN
  dispatch tables. Real, confirmed values exist (see "Customizing
  Vehicle CAN Codes" below) but are themselves only as trustworthy as the
  real firmware's own built-in vehicle-profile guess, not an independent
  Prado-specific capture -- `docs/MCU_CAN_BUS_SWD_SNIFFING_PLAN.md` and
  `tools/can-sniffer/` exist to get a real one.

---

## 2. Building the Firmware

To compile the binary:

```bash
cd hardware/MCU/source
make
```

The build produces:
- `build/can_app.bin`: Raw flash binary linked at `0x08004000`.
- `build/can_app.elf`: ELF binary with symbols.
- `build/can_app.hex`: Intel HEX file.
- `build/can_app.lst`: Complete assembly disassembly listing.

---

## 3. Customizing Vehicle CAN Codes

To edit or add CAN bus frames (e.g. steering wheel controls, reverse gear, illumination):

1. Open `src/vehicle_profiles.c`.
2. Edit or add handlers:
   ```c
   static void handle_toyota_prado_swc(const CanFrame *f) {
       // Decode button bitmasks from f->data[0..7]
       // Forward key events to SoC:
       uart_send_key_event(KEYCODE_VOL_UP, true);
   }
   ```
3. Update the dispatch table (`g_mode1_table`, `g_mode2_table`, etc.) --
   **the real, current values below are the disassembly-confirmed ones**
   (`Mode 1` is the real firmware's own "Default/Primary Profile"; see
   `vehicle_profiles.h` for full derivation and honesty caveats -- these
   are still not an independent Prado-specific CAN capture, just the real
   firmware's own built-in guess):
   ```c
   static const CanDispatchEntry g_mode1_table[] = {
       { TOYOTA_PRADO_CAN_SWC,    handle_toyota_prado_swc },     /* 0x105 */
       { TOYOTA_PRADO_CAN_STATUS, handle_toyota_prado_status },  /* 0x28A */
       { TOYOTA_PRADO_CAN_GEAR,   handle_toyota_prado_reverse }, /* 0x185 */
       ...
   };
   ```
4. Run `make`.

---

## 4. Packaging & USB Flashing

To package the compiled firmware into a ready-to-flash USB update:

```bash
./tools/mcu_builder/build_mcu_update.sh -i hardware/MCU/source/build/can_app.bin --direct
```

This creates:
- `tools/mcu_builder/output/usb_root/auto_upgrade.txt` (0-byte trigger flag)
- `tools/mcu_builder/output/usb_root/can_app.bin` (Firmware payload)

Copy both files directly onto the root of a FAT32-formatted USB flash drive and plug it into the device to auto-flash.
