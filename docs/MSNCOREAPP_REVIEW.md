# MsnCoreApp / Associated Libraries — Security Review

Follow-on to [`SECURITY_REVIEW.md`](SECURITY_REVIEW.md)'s "not yet done" item: a protocol/binary-level
look at `MsnCoreApp` and its linked libraries (`libMcuCenter.so`, `libBlueTooth.so`, `libCanBus.so`) for
network-exposed listeners or command-injection-style bugs, as opposed to just the SSH credential path.

**Method:** `MsnCoreApp` is a non-stripped ELF with full DWARF/symtab (confirmed in
`docs/SOC_ARK1668_CROSSREF.md` §7 already) — 5040 `.symtab` entries. No `objdump`/`nm`/`readelf`
available in this environment, so used `pyelftools` (symbol table, relocations, PLT resolution) and
`capstone` (ARM disassembly) directly via Python instead. Both pulled from PyPI for this session.

## Finding: unauthenticated arbitrary root-filesystem write via any inserted USB drive or SD card

**Severity: high — this is a more directly usable "unlock" mechanism than the SSH credentials in
`SECURITY_REVIEW.md`.** No network access, no password, no serial console, no U-Boot interrupt needed
— just physical insertion of removable media.

### The mechanism, traced precisely

`DiskDeviceWatcher::mountDiskPartition(DiskDeviceType, QString, QString&)` — the function that runs
automatically whenever the device auto-mounts a newly inserted disk partition (USB drive or SD card;
this is the same hotplug-driven auto-mount flow already documented for the SD-boot/update workflow
elsewhere in this project) — contains this exact sequence, confirmed via disassembly at
`0x428d4`–`0x42950` in the live device's `MsnCoreApp` binary:

```
LockRebootSystem()                                              ; @0x428d4, prevents reboot mid-operation
cmd = "mount -o remount,rw / && cp -rf %1msn_autocopy/* /"       ; @0x428d8, loaded from .rodata
cmd = cmd.arg(mountPath)                                         ; @0x42908, QString::arg — %1 substituted
                                                                  ;   with the mount path, NO shell escaping
localBytes = cmd.toLocal8Bit()                                   ; @0x42944
system(localBytes.data())                                        ; @0x42950 — confirmed direct PLT call
```

Immediately before this (`0x42824`–`0x428d0`), the function builds `<mountPath> + "msn_autocopy"` as a
`QFileInfo` and checks `.exists()` — **the copy only fires if a folder named `msn_autocopy` is present
at the root of the inserted media**, gating the mechanism, but not authenticating it in any way.

### What this means practically

Format any USB drive or SD card, create a folder named `msn_autocopy` at its root, put any files in it
(a replacement `/usr/bin/sshd`, a modified `/etc/passwd` or `/etc/shadow`, a cron job, an init script, an
SSH authorized_keys file, anything), insert it into the device. On normal auto-mount — which happens
automatically for any inserted media, no user interaction — the device will:

1. Remount its own root filesystem read-write (`mount -o remount,rw /`)
2. Recursively copy every file from `<media>/msn_autocopy/` onto `/`, overwriting anything at matching
   paths, as root (the whole userspace runs as root on this device already)

No PIN, no password, no confirmation dialog (the confirmation string `"Find the factory configuration
file, do you upgrade it?"` seen nearby in `.rodata` belongs to a *different*, separate code path —
the `msn_factory_configs/` upgrade flow this project's own Holden→Prado conversion process already
relies on — not this one). `gAutoCopyDlg`/`gAutoCopyTimerCount` globals exist but nothing in the traced
call path shows a user-confirmation gate before the `system()` call fires.

This is very likely an intentional factory/dealer-service feature (push a patch to the device without a
full firmware reflash) rather than a deliberately malicious backdoor, but it is completely
unauthenticated as shipped.

### Secondary note: not shell-escaped

`mountPath` is substituted into the command string via plain `QString::arg()`, not sanitized for shell
metacharacters, before reaching `system()`. The primary exploitation path above doesn't need this (just
naming the folder correctly is already sufficient), but if the mount path is ever derived from anything
attacker-influenceable (e.g. a volume label rather than a fixed `/media/sdX/`-style device path — not
confirmed either way here), this would be a second, independent command-injection route into the same
`system()` call. Not verified further in this pass.

### Not yet done

- Didn't trace exactly how `mountPath` itself is constructed (fixed device-path pattern vs.
  volume-label-influenced) — relevant to the secondary injection note above.
- Checked `libMcuCenter.so`, `libBlueTooth.so`, `libCanBus.so` for direct `bl` calls to their own
  imported `system` PLT stub — found none in a straightforward scan of each `.text` section. All three
  do import `system` (confirmed in their `.dynsym`), so it's called from somewhere, likely via an
  indirect call pattern this pass didn't specifically hunt for. Left as an open item rather than
  claiming these three are clean.
- No raw TCP/UDP listener (`QTcpServer`, `QUdpSocket`, direct `socket()`/`bind()`/`listen()`) found in
  any of the four binaries beyond Qt/Qt-Embedded internals (`QSocketNotifier`, `QWSServer` — the
  Qt/Embedded windowing server, not network-facing) and a netlink socket in `DiskDeviceWatcher` used for
  kernel hotplug (`uevent`) messages — not attacker-reachable over any network interface. So the
  network-facing-daemon angle raised in `SECURITY_REVIEW.md` §4 appears to be a dead end: this
  binary's remote exposure is via SSH (already covered there), not a custom protocol listener of its
  own. Didn't do the same pass against every other rootfs daemon (`AudioService`, `SettingService`,
  etc.) — those are internal D-Bus session-bus services, same threat-model caveat as always with D-Bus
  (local IPC, not network-reachable unless something bridges it to TCP, which nothing here appears to).

### Tooling note

`pyelftools` and `capstone` were installed via `pip install` for this session (not committed anywhere,
not a repo dependency) since no `binutils`/disassembler was available in this environment. Anyone
reproducing this: `pip install pyelftools capstone`, then resolve PLT stubs from `.rel.plt` +
`.dynsym` manually (ARM `.plt`: 20-byte PLT0 header, then one 12-byte stub per relocation, in
relocation order) since there's no higher-level tooling shortcut for that mapping in `pyelftools` itself.
