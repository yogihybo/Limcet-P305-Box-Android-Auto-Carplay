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
│   └── vehicle_profiles.h       # Vehicle CAN message dispatch tables & button mapping
└── src/
    ├── startup_stm32f105.c      # Vector table & Reset_Handler
    ├── main.c                   # System clocks, watchdog, and main loop
    ├── can_driver.c             # bxCAN1 driver implementation & ISR
    ├── uart_protocol.c          # USART2 driver (/dev/ttyHS0 @ 38400) & SoC protocol engine
    └── vehicle_profiles.c       # Toyota Prado 150 CAN decoding implementation
```

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
3. Update the dispatch table (`g_mode1_table`, `g_mode2_table`, etc.):
   ```c
   static const CanDispatchEntry g_mode1_table[] = {
       { TOYOTA_PRADO_CAN_SWC,    handle_toyota_prado_swc },     /* 0x3C4 */
       { TOYOTA_PRADO_CAN_STATUS, handle_toyota_prado_status },  /* 0x025 */
       { TOYOTA_PRADO_CAN_GEAR,   handle_toyota_prado_reverse }, /* 0x127 */
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
