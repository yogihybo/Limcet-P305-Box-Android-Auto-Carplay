# Self-contained SD boot — experimental, untested on hardware

**Status: statically verified, not yet tested on real hardware.** Do not rely
on this without testing on your own unit first — the fallback (removing the
SD card) is safe (see below), but this hasn't booted a real device yet.

## What this is

A way to auto-boot kernel + rootfs from SD card with **zero NAND writes of
any kind** — not even to spare/placeholder space. Everything lives on the SD
card:

| File | Goes where | Purpose |
|------|-----------|---------|
| `uboot_selfcontained.bin` | SD p1, renamed to `UBOOT.BIN` | Patched U-Boot — Stepldr loads this in preference to NAND (existing, already-documented mechanism) |
| `s` | SD p1, same name | Boot script — the real boot logic (`setenv bootargs`, `fatload zImage`, `bootz`) |
| `zImage` | SD p1 | Kernel (from `Prado firmware reconstructed/mtd5_kernel/zImage`) |
| — | SD p2 (ext4) | Rootfs |

Remove the SD card and the device boots completely normally — nothing on
NAND was ever touched by this.

## How it works

`uboot_selfcontained.bin`'s compiled-in env was patched to a single, minimal
key that fits the ~52-byte safe capacity of a raw/Holden-derived `uboot.bin`
(see `docs/UBOOT_SDBOOT_INVESTIGATION.md` for why the full `sdboot` preset
doesn't fit):

```
bootcmd=fatload mmc 0:1 1000000 s;source 1000000
```

This just loads the file named `s` from SD p1 and executes it via U-Boot's
`source` command — the actual boot logic lives in that file, not in the
tiny compiled-in buffer, so it can be as long as needed. `s` contains:

```
setenv bootargs console=ttyS0,115200n8 console=tty0 mem=180M root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw
fatload mmc 0:1 1000000 zImage
bootz 1000000
```

(source: `env/sdboot_script.txt`)

The NAND-offset redirect (`--patch-nand-offset`, the same mechanism already
used and proven safe elsewhere in this repo) is also applied, so this
compiled-in env is actually consulted instead of the real NAND env.

Generated with:
```bash
python patch_uboot.py -i "Prado firmware reconstructed/mtd1-mtd2_uboot/uboot.bin" \
  -o experimental_sdboot/uboot_selfcontained.bin \
  --mode sdscript --replace-env --patch-nand-offset
```

## What's been verified (static analysis only)

- **Env patch applied correctly**: `--dump-env` shows exactly the one
  `bootcmd` key, `bootdelay`/`baudrate` cleanly dropped.
- **NAND-offset redirect applied**: `--find-nand-offset` reports 0 remaining
  valid candidates (all three `MOV #0x120000` instructions redirected).
- **No corruption**: full byte-diff against the source `uboot.bin` shows
  exactly 6 differing regions (2–23 bytes each, ~38 bytes total for the env
  change + 6 bytes for the 3 NAND-offset instructions) — nowhere near the
  ~4000-byte corruption this project hit previously (see
  `corrupted/README.md`). `set_default_env`, `env_import`, and `saveenv` —
  the command-table strings that got wiped in the corrupted files — are all
  still present and intact in this binary.

## What's NOT verified — needs real hardware

- **Whether `source` accepts a plain-text script file directly**, or
  requires the `mkimage -T script`-wrapped format (image header + CRC).
  This is the one real unknown. `env/sdboot_script.txt` / `s` is currently
  plain text — try that first (cheaper to test), and if `source` rejects it,
  regenerate as a wrapped image:
  ```bash
  mkimage -A arm -T script -C none -n "SD boot script" -d env/sdboot_script.txt s
  ```
  (requires `u-boot-tools`, same package already used elsewhere in this repo).
- Whether U-Boot actually boots correctly end-to-end from this point
  onward — the kernel/rootfs loading itself uses the same `fatload`/`bootz`
  pattern already confirmed working in the README's manual SD/USB boot
  commands, so this part is lower-risk, but the whole chain hasn't been
  run together on a real device.

## Testing steps

1. Format an SD card: FAT32 partition (p1), ext4 partition (p2) with rootfs.
2. Copy `uboot_selfcontained.bin` to SD p1 as `UBOOT.BIN`.
3. Copy `s` to SD p1 (as-is, plain text).
4. Copy `Prado firmware reconstructed/mtd5_kernel/zImage` to SD p1.
5. Insert SD card, power on. Watch serial console.
6. If it hangs or errors at `source`, rebuild `s` with `mkimage` (see above)
   and retry.
7. Report back what happened (serial log) so this README and the
   investigation doc can be updated with the real result.
