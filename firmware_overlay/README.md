# firmware_overlay/prado/

Ready-to-copy, already-patched files for the Prado reconstructed rootfs
(`firmware_source/prado_reconstructed/mtd6_rootfs/rootfs/`). Every file
here mirrors its path in the rootfs (e.g. `etc/rc.d/rcS` here →
`/etc/rc.d/rcS` on the device). `build_bootable_sdcard.sh` rsyncs the
main rootfs onto p2, then rsyncs this directory on top — whatever's here
wins, unconditionally, for every build.

This replaces the old approach of applying `python3`/regex patches to a
copy of the rootfs at build time (`patch_rcs()`,
`patch_rootfs_for_new_kernel()` in the pre-2026-07-17
`build_bootable_sdcard.sh`, archived at
`archive/build_bootable_sdcard.sh.pre-overlay`). That approach had
accumulated real bugs that went unnoticed for a while because the
patch functions printed unconditional "success" messages regardless of
whether their regex actually matched anything:

- The touchscreen `insmod`→`modprobe` fix's regex expected
  `/lib/modules/3.4.0/firmware_source/kernel/drivers/...` — an
  erroneous `firmware_source/` segment that never appeared in the real
  file, so this "fix" silently never applied, in every build, the
  entire time it existed. Same bug, same wrong path, in the WiFi
  module fallback's `.ko` search paths. Both fixed for real here (see
  `etc/rc.d/rcS`, `etc/wifi_ap.sh`).
- Several patches applied to a rootfs whose *committed source* had
  already picked up some of the same edits directly in a separate
  session (`mkdir -p /media`/`/var`, the SSH host-key `chmod`, the
  `sshd -f` config-path fix were already baked into
  `firmware_source/.../etc/rc.d/rcS` before this migration) — the
  build-time patch functions weren't idempotent against that, so
  re-running them duplicated `mkdir -p /var` and the SSH `chmod` line
  on every build.

Reviewing this as actual file diffs (`git diff`) instead of reading
Python string-replacement logic against an unknown starting state is
the whole point of this directory — that's what let both of the above
surface immediately.

## What's here and why

| File | What changed vs. `firmware_source` | Why |
|---|---|---|
| `etc/rc.d/rcS` | Touchscreen `insmod`→`modprobe` (fixed path), userdata mount rewritten to try ext4 p3 first (SD/USB boot) with NAND UBI/yaffs2 fallback | 4.19.192 kernel compat; SD/USB boot support — see `docs/AUDIO_SUBSYSTEM_INVESTIGATION.md`/`docs/UBOOT_SDBOOT_INVESTIGATION.md` |
| `etc/profile` | `QWS_DISPLAY` switched from `directfb` to `LinuxFB`; old `insmod .../galcore.ko` (3.4.0 path) commented out — `galcore` is now `modprobe`d from `rcS` instead | Avoids the GPU-driver-version-mismatch crash class documented in `docs/ARK1680_TS_REVERSE_ENGINEERING.md`; avoids loading galcore twice via two different paths |
| `etc/inittab` | `getty` on `ttyS0` replaced with a direct login shell | This busybox build has no `login` applet — `getty` alone can't provide a shell |
| `etc/wifi_ap.sh` | 3.4.0 `wlan_rtl*.ko` hardcoded paths replaced with `modprobe` + a real-kernel-version fallback path | 4.19.192 kernel compat |
| `etc/switchotg.sh` | Fixed sysfs paths (`/sys/devices/platform/musb-ark1680.N/...` never existed on this kernel — real path is `e0100000.usb`/`e0400000.usb`, this kernel's actual DT-derived device names); added a root-filesystem safety check before switching usb0 | `linux-arkmicro` `83ab185e6` changed usb0's DTS `dr_mode` from `otg` to `host` (eliminates several seconds of ID-pin negotiation retries at every boot — see that commit). This script is what restores OTG/gadget capability afterward via the driver's runtime-switchable sysfs `mode` attribute, but only when root isn't actively mounted from that same port (`root=/dev/sda*`) — switching usb0 away from host mode while it's serving as the live root filesystem would yank root out from under the running system, a real hazard given `bootusb` is used constantly for on-device testing |
| `etc/wifi_client.sh`, `etc/wifi_client.conf` | Not stock content at all — join a local WiFi network at boot instead of hosting the CarPlay AP, for remote SSH access during development | Never existed in any vendor dump; `rcS` here starts `wifi_ap.sh` by default and has this commented out as the alternative (2026-07-18: moved here from `firmware_source`'s base rootfs — it was never referenced by that copy of `rcS`, only this overlay's) |
| `etc/ssh/sshd_config`, `etc/ssh/ssh_host_{rsa,dsa}_key[.pub]`, `usr/bin/sshd` | Not stock content at all — SSH server + host keys for on-device debug access during development | Never existed in any vendor dump (`git log` traces these to "Add SSH and USB networking to rootfs", not an extraction). 2026-07-18: moved here from `firmware_source`'s base rootfs, where they'd been living by accident since the rootfs was originally built — they belong here with the rest of the dev-only additions, not baked into the base image. `rcS` here `chmod 600`s the host keys and starts `sshd -f /etc/ssh/sshd_config` |
| `usr/lib/libGAL.so` | **2026-07-20:** replaced with the `fb`-backend `libGAL.so` from NXP's official `imx-gpu-viv-6.2.4.p1.8-aarch32.bin` release (real download from `https://www.nxp.com/lgfiles/NMG/MAD/YOCTO/` — the Yocto recipe's `fsl-eula=true` is only a local build-config flag, not real server-side auth; the installer has a built-in `--auto-accept` option). Supersedes the pre-2026-07-20 `libGAL.fb.so`-content swap (see git history for this path) | Matched-version-pair fix for the DirectFB/`galcore` ABI-mismatch investigation (see `docs/DEVICE_TEST_CHECKLIST_2026-07-18.md` §1b) — pairs with the `6.2.4.p1.8` `galcore.ko` already built for 4.19.192, so both sides of the raw ioctl struct agree, sidestepping the struct-mismatch crash entirely. **Not yet hardware-tested.** The prior fix (swapping in `libGAL.fb.so`'s content) addressed a *different* problem — the original file's corrupted `.dynamic` section — and is superseded, not obsoleted; if this new file causes new problems, the old one is still valid for that original corruption issue and can be restored from git history. |
| `usr/bin/sshd`, `usr/bin/start_msn_linuxfb`, `usr/bin/start_msn_directfb` | The only `usr/bin/*` entries left in this overlay — everything else that used to be manually copied here (diagnostic tools/scripts) was removed 2026-07-26 | **2026-07-26 reversal:** the 2026-07-17 change this row used to describe (manual copies, "kept in sync manually") had already drifted stale by the time it was reverted — several tools built in later sessions (`fb-scan`, `lcdc-regdump`, `mem-dump`, `pin-force`, `pinmux-watch`, `gpio-i2c-probe`, `i2c-gpio-bruteforce`, `i2c-read-raw`) were never added here, and `mcu-handshake` carried a real bugfix in `tools/` that this copy didn't. `build_bootable_sdcard.sh` now has an `install_diag_tools()` step (runs right after `apply_overlay`, so it wins over any leftover duplicate) that copies every compiled binary/script/data file from each `tools/*/` directory into `/usr/bin` automatically on every build — see that script for the exact copy logic and exclusions (source/doc/build artifacts, debug builds). No more manual sync step, no more drift. |
| `usr/bin/start_msn_linuxfb`, `usr/bin/start_msn_directfb` | New — two copies of `start_msn` with a fixed `QWS_DISPLAY` backend each (`linuxfb` and `directfb` respectively), so switching backends for testing doesn't require hand-editing `start_msn`'s `QWS_DISPLAY` block each time | 2026-07-20, added during the DirectFB/`galcore` matched-version-pair investigation (`docs/DEVICE_TEST_CHECKLIST_2026-07-18.md` §1b) — `start_msn` itself is unchanged and still defaults to `linuxfb` |
| `usr/bin/lcd-overlay-watch.sh` | New — polls all 5 LCDC layer enable-bits (`ARK1668_LCDC_CONTROL`), the back-color fill, and each layer's frame address, printing only on change | 2026-07-20, added to diagnose the cross-backend red-overlay-flash bug after the `ARKFB_HIDE_WINDOW` ioctl-number fix (`linux-arkmicro` `bf91e9e21`) didn't resolve it — see `docs/DEVICE_TEST_CHECKLIST_2026-07-18.md` §1b |
| `etc/rc.d/rcS` (banner) | The "=== Diagnostic tools ===" banner listing all of the above, appended to the end of the file | Previously generated at build time by `append_diag_banner()`, one `echo` per tool actually installed — now static since every tool above is always present |
| `msnprofile/arkdata.ini`, `msnprofile/arkdata/arkdata_0.ini` | `RgbMode=5` (`RGB`) set for `[DISPLAY_INTERFACE]` | Overrides rootfs `RgbMode=0` (`BGR`) default so `MsnCoreApp` sets the hardware VDE color matrix to straight `RGB` pass-through mode, matching `bootlogo.raw` and physical panel PCB wiring without mutating `firmware_source/` |
| `etc/profile` (dmesg alias) | `alias dmesg='/usr/bin/dmesg --color=always'` | BusyBox's own `dmesg` applet (earlier in `$PATH`) lacks `--color` — see `tools/dmesg/README.md`. No `-T` (this device has no RTC/NTP, so ctime dates are meaningless — the default `[seconds.microseconds]` since-boot format is what's actually useful) or `-x` (facility:level text column — color alone already distinguishes severity). Previously appended conditionally at build time only if `dmesg` was installed; now unconditional since `usr/bin/dmesg` above is always present |
| `usr/share/dbus-1/services/com.arkmicro.auto.service` | `Exec=` wrapped in `sh -c "exec ... >>/var/log/sink.log 2>&1"` instead of the stock bare `Exec=/usr/bin/sink` | 2026-07-26 — `sink` (the wireless Android Auto daemon) is D-Bus-activated, so its stdout/stderr previously went nowhere capturable; needed to debug a "phone associates+completes WPA handshake then leaves after ~11s, video never starts" failure where the on-console `MsnCoreApp`/`ArkDbus` log alone wasn't enough. `rcS` here also gained `mkdir -p /var/log` (never created anywhere before) |
| `bin/busybox` + `busybox-applets.manifest` | Rebuilt from real busybox 1.30.1 source (`defconfig`, same cross-toolchain as the rest of this project) instead of the stock 1.25.0 binary, whose original build config is unknown/unrecoverable | 2026-07-27 — needed `ipcs`/`ipcrm` for live debugging (clearing a stuck SysV shared-memory flag, see `docs/DEVICE_TEST_CHECKLIST_2026-07-18.md`'s Android Auto black-screen investigation) and neither exists in the stock build. `defconfig` enables ~390 applets essentially for free, so took the opportunity to ship the full set rather than cherry-picking just these two. **The applet symlinks themselves are NOT stored as real symlinks in this directory** — this repo's working tree sits on a VirtualBox shared folder (`vboxsf`), which cannot create symlinks at all. `busybox-applets.manifest` (plain text, `path target` per line) is materialized into real symlinks on the properly-mounted rootfs image by `build_bootable_sdcard.sh`'s `install_busybox_applets()` step instead. Two applet names are deliberately excluded from the manifest (`dmesg`, `less`) because this project already ships better standalone replacements for both (`tools/dmesg`, real GNU `less`) at the same effective `$PATH` position — a busybox-provided symlink for either would silently shadow the real tool, reintroducing a bug already fixed once (see the `dmesg` alias row below). **Hardware-confirmed working, 2026-07-27** (tested via SD card) — this also replaces `/sbin/init` (busybox's own `init` applet, matching this rootfs's existing `/etc/inittab` format), the highest-stakes single entry in the manifest; boots and runs correctly. |
| `msnprofile/FactoryConfig.ini` | `Language=4097` (English) instead of the live-captured device's `Language=4098` (简体中文/Chinese Simplified); base `firmware_source` rootfs ships this key commented out entirely | 2026-07-26 — `4097`, not `4096`, is the real English value, confirmed by disassembling `libMsnCommons.so`'s `GetLanguageValueList()`/`GetLanguageNameList()` (the earlier `docs/SETTINGS_REFERENCE.md` guess of `4096` was wrong); set explicitly rather than left commented since an unset key's real fallback behavior isn't confirmed |

## What's still applied by the build script, not this overlay

A few things stay as small, focused, genuinely-conditional build-time
insertions rather than baked-in overlay content, because they're
opt-in/opt-out per build rather than "always wanted":

- **MTD partition redirect** (`redirect_mtd_data` toggle) — inserts
  `/dev/mtdN` → `/nanddata/*` symlinks into `rcS` after `mdev -s`, only
  if enabled.
- **MsnCoreApp autolaunch** (`disable_msncoreapp_autolaunch` toggle) —
  `firmware_source`'s `etc/profile` already ships with
  `MsnCoreApp -qws&` commented out (matches this toggle's default-ON
  state); the build script only needs to act when the toggle is
  explicitly turned OFF (re-enables the line).
- **telnetd** (`install_telnetd` toggle, OFF by default) — inserts an
  unauthenticated root telnetd into `rcS`. Deliberately opt-in only,
  never baked into a shipped default.

Diagnostic tools themselves are **not** a toggle anymore (see the table
above) — they're copied automatically from `tools/*/` by
`install_diag_tools()` in `build_bootable_sdcard.sh` on every build.
There's no per-tool skip mechanism; to exclude one, either remove it
from `tools/` or add it to that function's exclusion `case` pattern.

`fix_usb_port0_otg_race` was dropped entirely during this migration —
it had been a pure no-op (all its actual commands commented out,
inserting nothing but a documentation comment into `rcS`) since the
kernel-level fix in `musb_ark.c` was confirmed sufficient on its own.

## Regenerating

There's no regeneration script — these are hand-maintained, reviewed
files. If `firmware_source`'s baseline `rcS`/`profile`/etc. changes
upstream (a fresh NAND dump, for instance), diff the new baseline
against `archive/build_bootable_sdcard.sh.pre-overlay`'s patch logic
(or against this README's table) to work out what still needs
reapplying, and edit the files here directly.
