# Extracted Rootfs — CSTech-202511-IP17

Source: `CarSyncTech Toyota/CSTech-202511-IP17/rootfs.img` (100,007,936 bytes, UBI image v1)

## UBI image layout

- Min I/O: 2048, LEB size: 126976, PEB size: 131072
- Total blocks: 763 (2 layout, 761 data)
- Single volume: `rootfs` (dynamic, vol ID 0, autoresize), PEB range 2-762

## Extraction method

Extracted with `ubi_reader` (`ubireader_extract_files`), installed to a user-local
Python environment via `pip install --user --break-system-packages ubi_reader`
(no root/kernel MTD device required — pure userspace UBI/UBIFS parsing).

Extraction was run to a local (non-vboxfs) directory first: this repo checkout lives
on a VirtualBox shared folder (`vboxsf`), which does not support symlinks, and the
rootfs contains 1133 symlinks (busybox applet links, Qt versioned `.so` links, etc.).
Extracting directly onto vboxfs silently drops every symlink.

## Contents

- `rootfs.tar.gz` — full extracted rootfs tree (965 regular files + 1133 symlinks,
  158 MB uncompressed), rooted at `rootfs/` (i.e. `rootfs/bin`, `rootfs/etc`, ...).
- Top-level dirs: `bin data dev etc lib linuxrc media mnt msnprofile opt proc root
  sbin sys tmp usr var`
- `linuxrc` -> `bin/busybox`, `sbin/init` -> `../bin/busybox` (standard BusyBox
  single-binary init rootfs)
- `usr/local/Qt4.7.4/` — Qt 4.7.4 libs, confirms Qt-based UI stack

To inspect: `tar xzf rootfs.tar.gz` into a local (non-vboxfs) directory to preserve
symlinks.
