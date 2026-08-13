# CarSyncTech Toyota — MCU Update Analysis

Review of the CarSyncTech CarPlay-box MCU firmware update packages for Toyota
models, and the reverse-engineering of their update-container format. These are a
different vendor/product from the Prado's Limcet box but sit on the **same
Arkmicro ARK1680 SoC platform**, which is why they share the USB
`auto_upgrade.txt` update trigger. See [../../hardware/MCU/MCU_FIRMWARE_REVIEW.md](../../hardware/MCU/MCU_FIRMWARE_REVIEW.md)
for the Prado MCU / trigger mechanism this builds on.

---

## 1. Files in this folder

| File | Size | What it is |
|------|------|-----------|
| `CST-Highlander-Fix-2024.zip` | 32 KB | MCU update package (Highlander) |
| `CarSyncTech-PrevTacoma-ScreenFix.zip` | 32 KB | MCU update package (Tacoma, previous) |
| `CarSyncTech-ScreenFix-Update-Tacoma.zip` | 32 KB | MCU update package (Tacoma, newer) |
| `CSTech-202511-IP17.zip` | 82 MB | **Full ARK1680 SoC firmware** (Box-C235 board) — *not* an MCU file |
| `Tacoma_Instructions_V3-082024.pdf` | 8 MB | End-user install instructions |
| `decrypted/` | — | Unwrapped inner firmware images produced by this analysis (§6) |

`CSTech-202511-IP17.zip` is a complete SoC update (Nboot/Stepldr/uboot/`rootfs.img`/
`UpConfig`/`update`/`zImage`/msn_factory_configs) for a **Box-C235** board — the
same Arkmicro platform family as the Prado's Limcet Box-P306. The three ~32 KB zips
are the MCU firmware packages and the subject of this review.

---

## 2. Package layout & relationships

Each MCU zip contains exactly the Prado-style pair: a trigger flag + a payload.

| Package | Trigger | Payload (`toyotaCarplayFW2.0.bin`) | Payload MD5 |
|---------|---------|-----------------------------------|-------------|
| `CST-Highlander-Fix-2024` | `auto_upgrade.txt` (0 B) | 52,752 B | `1c9dd813…d21f` |
| `CarSyncTech-PrevTacoma-ScreenFix` | `auto_upgrade.txt` (0 B) | 52,752 B | `1c9dd813…d21f` |
| `CarSyncTech-ScreenFix-Update-Tacoma` | `auto_upgrade.txt` (`1`, 1 B) | 52,324 B | `9d080e05…06be` |

- **Highlander == Prev-Tacoma**: byte-identical payloads — one firmware ships for both models.
- **Tacoma "Update"** is a different, newer build (428 B smaller).
- `auto_upgrade.txt` is empty or just `1` — confirming (as proven for the Prado) that
  **content is irrelevant; presence is the trigger**. Same `<mountpoint>/auto_upgrade.txt`
  USB-root mechanism.

Note the payload is named `toyotaCarplayFW2.0.bin`, not the Prado's `can_app.bin`
— this box uses the encrypted-container adapter (`MCUAdapter_Box_Encryption`),
not `MCUAdapter_BoxP300`.

---

## 3. Update-container format (`toyotaCarplayFW2.0.bin`)

Unlike the Prado's `can_app.bin` (a raw STM32 image), this is a wrapped +
obfuscated container:

```
offset 0x00  magic   BE EF E9 91
offset 0x04  text header (plaintext, CRLF-terminated key: value lines):
               bin: stm32fw.bin
               carname: 0BootLoader
               mcu: DEV_MCU_ANT
               customer: Common
               subcustomer: Common
               firmware: Unknown
offset 0x78  body = raw stm32fw.bin  XOR 0x37     (header is NOT encrypted)
```

The body is the raw STM32 firmware with **every byte XORed with `0x37`**. Within
the raw firmware, the actual Cortex-M vector table sits at body **+0x188**
(SP `0x20004b88`, reset `0x08006101`) — i.e. there is a ~392-byte binary
sub-record between the text header and the firmware's vector table.

### The "encryption" is not in the Prado firmware
The decryptor is **not** recoverable from the Prado's `libMcuCenter.so`: a search
of the entire Prado rootfs finds neither the `BE EF E9 91` magic nor the header
field names (`carname`, `subcustomer`, `stm32fw`). The `MCUAdapter_Box_Encryption`
class in the Prado's copy is a thin stub (overrides only `getPortName` /
`getPortSettings` / `onInited`). The real container decryptor lives in the
**Box-C235's own `rootfs.img`** (inside `CSTech-202511-IP17.zip`). The key below
was therefore recovered by cryptanalysis, not by disassembling that code.

---

## 4. How the key was recovered (process)

1. **Header vs body split.** File starts with printable text (magic + `bin:` /
   `carname:` / …). Only the bytes after `firmware: Unknown\r\n` (offset 0x78) are
   high-entropy → only the body is transformed.
2. **Entropy ≈ 6.48 bits/byte** → not real crypto (AES would be ~8.0); consistent
   with a light per-byte transform over ARM firmware.
3. **Period test.** Max-symbol frequency per residue class for periods 1–64 stays
   flat at the global rate (~0.175 for periods 16/32/48/64) → **no periodicity;
   it's a period-1 (position-independent) per-byte transform.**
4. **Known-plaintext from word statistics.** The most common 4-byte aligned cipher
   word is `37 37 37 37` (794×) = encrypted `00 00 00 00` (zero padding) →
   **S(0x00) = 0x37**. A run of a repeated cipher word `20 56 37 3f` (×8, a vector
   table's default-handler slot) decodes to a `0x0800xxxx` flash address →
   **S(0x08) = 0x3f**.
5. **Both constraints fit XOR 0x37**: `0x00 ^ 0x37 = 0x37`, `0x08 ^ 0x37 = 0x3f`.
6. **Validation.** Applying `XOR 0x37` to the whole body yields:
   - `70 47 70 47…` = repeated Thumb `bx lr` (the `pGpG` ASCII pattern),
   - a valid Cortex-M vector table (SP `0x20004b88`, reset `0x08006101`),
   - and clean multi-word plaintext strings (`Toyota Crown-Carplay`, `Aug 28 2021`,
     `#####ERROR: BITMAP_COUNT[%d] != COUNT_OF(SpiosdTABLE)[%d] #####`).
   A 60-character string decoding with zero garbage proves the key is exactly
   `0x37` at every position (no position-dependent component).

### Decrypt recipe
```python
d   = open("toyotaCarplayFW2.0.bin","rb").read()
he  = d.index(b"firmware: Unknown"); he = d.index(b"\n", he) + 1   # end of text header (0x78)
fw  = bytes(b ^ 0x37 for b in d[he:])                              # raw stm32fw.bin
open("stm32fw.bin","wb").write(fw)
```

---

## 5. What the inner firmware is

An **STM32 SPI-OSD display-controller** application — *not* a CAN decoder like the
Prado's `DCn32-VOLVO can_app.bin`.

| Property | Value |
|---|---|
| Product string | `Toyota Crown-Carplay` |
| Build date | `Aug 28 2021` |
| Role | SPI on-screen-display / bitmap overlay (`SpiosdTABLE`, `BITMAP_COUNT`, `Failed to write device 0x%02X`) |
| Size (decrypted) | 52,632 B (prev/highlander) / 52,204 B (tacoma update) |
| Vector table | body +0x188 — SP `0x20004b88`, reset `0x08006101` |

This is why the packages are branded "**ScreenFix**": the MCU firmware drives the
display overlay. The earlier guess that it shared the Prado's DCn32 codebase was
**wrong** — the matching `pGpG` fragments are just generic Thumb `bx lr` (`0x4770`),
not shared code. Different application; likely the same vendor toolchain.

**prev vs update diff:** same product string and same `Aug 28 2021` build date;
the "Update" differs only in trivial code fragments and is 428 B smaller — a minor
patch, not a new firmware version.

---

## 6. Deliverables

Decrypted, container-stripped STM32 images (open directly as firmware) in
[`decrypted/`](decrypted):

| File | Size | Source packages |
|------|------|-----------------|
| `stm32fw_TacomaPrev-Highlander_Aug2021.bin` | 52,632 B | Highlander-Fix-2024 + PrevTacoma-ScreenFix (identical) |
| `stm32fw_TacomaUpdate_Aug2021.bin` | 52,204 B | ScreenFix-Update-Tacoma |

---

## 7. Security notes

- **`XOR 0x37` is obfuscation, not encryption** — trivially reversible, no key,
  no authentication of the payload.
- Combined with the unauthenticated `auto_upgrade.txt` USB-root auto-flash trigger
  (proven in [../../hardware/MCU/MCU_FIRMWARE_REVIEW.md](../../hardware/MCU/MCU_FIRMWARE_REVIEW.md) §5),
  anyone can craft or modify one of these update packages. Same exposure class as
  the Prado MCU update path.

---

## 8. Cross-reference to the Prado MCU

| | Prado (Limcet P306) | CarSyncTech (Tacoma/Highlander) |
|---|---|---|
| SoC platform | ARK1680 (Box-P306) | ARK1680 (Box-C235) |
| USB trigger | `auto_upgrade.txt` | `auto_upgrade.txt` (same) |
| Payload filename | `can_app.bin` | `toyotaCarplayFW2.0.bin` |
| Payload format | raw STM32 image | `BEEFE991` container + `XOR 0x37` body |
| Inner MCU app | `DCn32-VOLVO` CAN decoder | `Toyota Crown-Carplay` SPI-OSD controller |
| SoC adapter | `MCUAdapter_BoxP300` | `MCUAdapter_Box_Encryption` |
