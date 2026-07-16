# archive/

Superseded scripts, kept for reference rather than deleted outright.

## `build_bootable_sdcard.sh.pre-overlay`

The version of `build_bootable_sdcard.sh` before the 2026-07-17 overlay
migration. Applied rootfs patches (rcS, profile, wifi_ap.sh, inittab,
libGAL.so) via `python3`/regex transforms against a copy of the rootfs
at build time, rather than shipping already-patched files. Also
supported the CarSyncTech CSTech-202511-IP17 rootfs/userdata as an
alternative to the Prado reconstructed one (`--cstech-rootfs`,
`--cstech-userdata`), and an initramfs boot path (`--initramfs`) that
had been confirmed non-functional (the dumped stock kernel doesn't
support it) since before this migration.

See `firmware_overlay/prado/README.md` for what changed and why —
several of this version's patch functions had accumulated real,
previously-unnoticed bugs (regexes that never matched due to a stray
path segment, non-idempotent insertions that duplicated lines on
rebuild) that motivated the migration in the first place.

Kept executable and otherwise unmodified. Not maintained going forward
— CSTech support in particular was dropped in the rewrite and would
need to be re-added from this version if it's needed again.
