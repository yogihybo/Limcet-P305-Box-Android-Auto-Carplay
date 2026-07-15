# Extracted Kernel — CSTech-202511-IP17

Source: `CarSyncTech Toyota/CSTech-202511-IP17/zImage` (3,255,848 bytes)

## Kernel version string

```
Linux version 3.4.0 (root@build-server) (gcc version 4.9.4 (Buildroot 2018.08-rc1-00026-gaeef2a9) ) #396 Fri Nov 21 10:29:08 CST 2025
```

- **Version:** Linux 3.4.0, build `#396`
- **Build date:** Fri Nov 21 2025, 10:29:08 CST
- **Toolchain:** gcc 4.9.4 (Buildroot 2018.08-rc1-00026-gaeef2a9)

## Extraction method

The zImage is a self-extracting ARM boot image (decompressor stub + LZO-compressed
piggyback payload).

1. Located the lzop magic (`89 4C 5A 4F 00 0D 0A 1A 0A`) in the file. It occurs
   twice:
   - **offset 0x1914 (6420)** — false positive; this is inside the decompressor's
     embedded LZO error-string table (`"invalid header"` etc.), not a real header.
   - **offset 0x1a68 (6760)** — the real lzop stream start.
2. Extracted bytes from offset 6760 to end of file.
3. Decompressed with `lzop -d` (warns "ignoring trailing garbage" — expected, since
   trailing bytes belong to the appended DTB/padding, not the lzop stream).

Result: `vmlinux` (6,384,484 bytes, raw uncompressed kernel image).

## Files in this folder

- `vmlinux` — decompressed kernel image
- `KERNEL_INFO.md` — this file

## Built-in driver/filesystem support (SD card & USB storage)

No `CONFIG_MODULES`/`.ko` support was found — no loadable kernel modules exist;
everything relevant is statically compiled into `vmlinux`. Checked via `strings`
on the decompressed image (driver names, error/log strings, sysfs module
parameter names such as `foo.bar_param`, which only exist for code actually
linked in).

**MMC/SD:**
- `dw_mmc` — Synopsys DesignWare MMC host controller driver (compiled in)
- `mmcblk` — MMC/SD block device layer (compiled in)
- Full `mmc_core` stack present (`mmc_add_host`, `mmc_attach_sdio`,
  `mmc_erase`, high-speed/SDR200 modes, card detect GPIO, etc.)
- → **SD card support: present and built in.**

**USB:**
- `musb-ark1680` — Mentor Graphics MUSB controller driver for the ARK1680/ARK1668
  SoC (compiled in). This is the actual USB host/OTG controller driver for this
  platform (no generic ehci/ohci/xhci/dwc2 host driver strings found — the SoC
  uses MUSB instead).
- `usb-storage` / `usb_storage` — USB mass-storage class driver (compiled in)
- `usbcore`, `scsi_mod`, `sd_mod`/`sd_probe` (SCSI disk layer used by
  usb-storage) all present
- USB gadget (device-mode) support also present (`usb_gadget_*`), so OTG
  device mode is possible too
- → **USB storage support: present and built in** (host mode via MUSB + usb-storage + SCSI disk layer).

**Filesystems available for mounting SD/USB media:**
- FAT/VFAT (`FAT-fs`, `vfat_rename`, `do_msdos_rename`) — needed for typical
  SD card / USB stick FAT32 formatting
- EXT4, EXT3-style journaling errors present, NTFS (read support), JFFS2,
  UBIFS (for the onboard NAND), partition-table parsing (msdos partitions)

**Bottom line:** the kernel already has everything needed to mount an SD card
or USB flash drive (FAT-formatted) out of the box — MMC host driver, USB host
driver (MUSB) + mass-storage class driver, SCSI disk layer, and FAT filesystem
support are all statically built into this `vmlinux`. Nothing needs to be
added at the kernel-config level; if SD/USB doesn't work in practice, the
issue is more likely device-tree pinmux/wiring or userspace (mount points,
udev/mdev rules) rather than missing kernel driver support.
