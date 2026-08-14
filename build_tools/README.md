# build_tools/

Helper scripts and vendor utilities used by the top-level build scripts
(`build_bootable_sdcard.sh`, `build_update.sh`) and referenced directly from
[README §6.0–§8.0](../README.md#60-booting-stock-kernel-from-sd-card-or-usb-non-destructive).

## U-Boot patching

| Script | Purpose |
|--------|---------|
| `patch_uboot_env.py` | The preferred, currently-used technique: **relocates** the stock `uboot.bin`'s tiny (~73-byte) compiled-in environment into unused zero space elsewhere in the image, so a full SD auto-boot command fits without any NAND writes. Also patches the header banner string and can invalidate the NAND env's CRC to force the relocated default to be used. See [README §6.0](../README.md#60-booting-stock-kernel-from-sd-card-or-usb-non-destructive) for what this does and why. |
| `patch_uboot.py` | The older technique: **edits** the compiled-in env in place (space-constrained, ~73 bytes) and can corrupt the NAND env offset to force a CRC failure. Still used by `build_bootable_sdcard.sh` to patch its auto-detected U-Boot source. Superseded by `patch_uboot_env.py` for the full SD auto-boot case, but kept as the underlying patch mechanism the build script calls. |
| `inject_ark_header.py` | Injects the proprietary 96-byte ARK header (magic `0x12345678`) into a freshly source-compiled U-Boot binary — required before `Stepldr` will accept it, since the open-source build doesn't produce this header itself. See [README §7.0](../README.md#70-custom-u-boot-and-kernel). |

## Bootlogo generation

| Script | Purpose |
|--------|---------|
| `convert_bootlogo.py` | Converts a dumped bootlogo JPEG into a raw 32bpp OSD framebuffer U-Boot can `fatload` directly — works around the open-source U-Boot build having no JPEG decoder (the stock `jpeghw` hardware codec path has no released source). |
| `make_touch2_bootlogo.py` | Generates the real Toyota Touch2-styled bootlogo (gradient, Toyota emblem, wordmark) that feeds into `convert_bootlogo.py`. |
| `make_test_bootlogo.py` | Generates a placeholder 800×480 "U-boot loading" test image, for exercising the bootlogo pipeline without needing a real logo asset. |
| `generate_boot_status_logos.py` | Reuses the exact same composition pipeline as `make_touch2_bootlogo.py` + `convert_bootlogo.py` (confirmed byte-identical) to generate the four boot-progress-status bootlogo variants with only the status line text changed. |

## Rootfs tree repair (Windows checkout artifacts)

| Script | Purpose |
|--------|---------|
| `apply_rootfs_perms.sh` | Restores Unix executable bits on a rootfs tree — a Windows checkout materializes every file at mode `0644`, which `mkfs.ubifs`/`rsync -a` would otherwise pack as-is into a broken image (no executable `busybox`, `sshd`, etc.). |
| `restore_rootfs_symlinks.sh` | Recreates the rootfs symlinks a Windows checkout/extraction silently drops (e.g. `/bin/sh` → `busybox`), using `rootfs.symlinks` as the manifest. |
| `rootfs.symlinks` | Plain-text manifest (`link-path<TAB>target`) consumed by `restore_rootfs_symlinks.sh`. |

## Vendor tools (ArkMicro, Windows-only)

Kept for reference/citation — not invoked by any script in this repo.

| Folder | Purpose |
|--------|---------|
| `ark-crc/` | ArkMicro's own CRC32 checksum tool (`crc32app` + `crc.sh`) for firmware image partitions (u-boot, kernel, rootfs, bootanimation, etc.). |
| `ark-create-bootanimation/` | ArkMicro's own Windows tool (`CreateBootanimationImg.exe` + `bootanimation.bat`) for packing a `bootlogo` + `animation/` frame sequence into a single `bootanimation` image file. |
| `ark-reverse-track/` | ArkMicro's own Windows tool (`arktrack-tool.rar`, archived) for building `reversingtrack` overlay files — see [`docs/9.1_PARTITION_LAYOUT.md`](../docs/9.1_PARTITION_LAYOUT.md) for the RSTK format this produces. |

## Other

| Path | Purpose |
|------|---------|
| `directfb-fbdev-fix/` | Two source patches (+ its own detailed `README.md`) rebuilding DirectFB's `fbdev` system module to fix a black-screen bug — see that folder's README and [`docs/historical/DEVICE_TEST_CHECKLIST_2026-07-18.md`](../docs/historical/DEVICE_TEST_CHECKLIST_2026-07-18.md) for the full root-cause trace. |
