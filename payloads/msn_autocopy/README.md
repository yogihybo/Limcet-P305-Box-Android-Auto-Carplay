# msn_autocopy telnetd payload

A USB/SD payload that uses the `msn_autocopy` auto-update mechanism found in `MsnCoreApp`
(disassembly trace: [`docs/UI_AND_APP_ANALYSIS.md`](../../docs/UI_AND_APP_ANALYSIS.md)) to get a root
shell on a **stock, unmodified** Prado head unit — no serial console, no U-Boot interrupt, no
firmware reflash, and no binary transplant.

**Status: confirmed working on real hardware, 2026-07-13** (telnet session established after the
v2 fix below). Supersedes the earlier `msn_autocopy_payload_do not use/` attempt, which transplanted
`sshd` and `rcS` from the *reconstructed* rootfs and never worked — this payload only patches
stock's own `rcS` and uses `busybox telnetd`, which is already compiled into stock's own busybox
binary (confirmed via string search: `strings bin/busybox | grep telnetd`). No foreign binary ever
touches the device, which sidesteps the whole class of shared-library/path mismatch problems that
likely killed the SSH approach.

## What it does

Same trigger mechanism as documented in the main README's
[§10.0 USB Auto-Update](../../README.md#usb-auto-update-msn_autocopy): `DiskDeviceWatcher` auto-mounts
inserted media, and if a folder named `msn_autocopy` exists at its root, runs
`mount -o remount,rw / && cp -rf <mountpath>/msn_autocopy/* /` — no auth, no confirmation.

This payload's `msn_autocopy/etc/rc.d/rcS` is **stock's actual dumped `rcS`**
(`Prado firmware dump/mtd6_rootfs/etc/rc.d/rcS`) with one functional block added, right after
`/sbin/mdev -s` and before anything else runs:

```sh
mkdir -p /dev/pts
mount -t devpts none /dev/pts 2>/tmp/telnetd_devpts.log
busybox telnetd -l /bin/sh > /tmp/telnetd.log 2>&1 &
echo "telnetd pid check:" > /tmp/telnetd_status.log
sleep 2 >> /tmp/telnetd_status.log 2>&1
ps | grep telnetd >> /tmp/telnetd_status.log 2>&1
netstat -ln 2>/dev/null | grep :23 >> /tmp/telnetd_status.log 2>&1

(
  sleep 20
  for mp in /media/sdisk/*/ /media/*/; do
    if [ -d "$mp" ] && [ -w "$mp" ]; then
      cp /tmp/telnetd.log "$mp/telnetd.log" 2>/dev/null
      cp /tmp/telnetd_devpts.log "$mp/telnetd_devpts.log" 2>/dev/null
      cp /tmp/telnetd_status.log "$mp/telnetd_status.log" 2>/dev/null
      ifconfig > "$mp/ifconfig.log" 2>&1
      echo done > "$mp/diag_complete.log"
    fi
  done
) &
```

`busybox telnetd -l /bin/sh` starts a **passwordless root telnet listener on port 23** — `-l /bin/sh`
tells it to spawn `/bin/sh` directly per connection instead of running `/bin/login`, so there's no
password prompt at all. The rest is diagnostics: it logs `telnetd`'s own stdout/stderr (needed
because the first attempt failed silently — see "Debugging history" below), checks the process is
actually running and the port is actually bound, and after 20s (enough time for `DiskDeviceWatcher`
to auto-mount the same USB stick this payload shipped on, confirmed from a live boot log to happen
around 14-15s in) copies all of that back onto the USB drive itself. This means you can diagnose a
failed attempt just by pulling the USB stick and reading the log files on a PC — no working
shell/telnet needed to retrieve them.

## Why telnetd instead of getting the console shell to respond

Stock's `/etc/inittab` has `::respawn:-/bin/sh` — a root shell respawning on the console with no
`getty`/login wrapper — which looked promising (`/bin/sh` invoked this way often doesn't print a
visible prompt, so it could plausibly be silently interactive). **Tested directly and it doesn't
respond to typed input** — something else is intercepting/blocking the console (not yet
root-caused). `MsnCoreApp -qws&` in `/etc/profile` is backgrounded (not exec'd), so the shell
process should still be alive and attached, but whatever's actually happening on that tty, blind
typing produces no effect. `telnetd` over the existing `carplay_wifi` AP routes around this
entirely rather than depending on understanding it.

## Deployment

1. Format a USB drive FAT32.
2. At the drive's root, create a folder named exactly `msn_autocopy`.
3. Copy this folder's contents into it, so the final layout is:
   ```
   <usb root>/msn_autocopy/etc/rc.d/rcS
   ```
4. Confirm `rcS` is executable (`chmod +x`) — FAT32 doesn't store Unix permission bits, so verify
   after copying; some tools/OSes may not preserve exec intent across a FAT32 write. (Not
   confirmed to have caused a failure in practice, but worth checking — see below.)
5. Boot the unit into stock firmware (`bootstock` from the custom U-Boot, or just power it on
   normally if it's still on stock NAND).
6. Insert the USB drive. The copy fires silently — no visible confirmation either way.
7. Reboot the unit (the modified `rcS` only takes effect on the *next* boot — the running system
   doesn't re-read `inittab`/`rcS` live).
8. Connect to WiFi `carplay_wifi`, password `88888888`.
9. `telnet <device-ip> 23` — should land directly at a root shell prompt, no login.

**Windows telnet client:** not enabled by default. Either:
```
dism /online /Enable-Feature /FeatureName:TelnetClient
```
(elevated Command Prompt, one-time), then `telnet <ip> 23`, or use PuTTY in Telnet mode.

## Debugging history (why v1 didn't work, why v2 does)

- **v1** — added `busybox telnetd -l /bin/sh &` with no output redirection, no `/dev/pts` mount.
  Boot log showed `rcS` ran fine (reached the touch-driver `insmod` lines normally), WiFi/DHCP
  worked (a client got a lease), but `telnet` connection was refused — nothing listening on 23.
- **Cause (most likely):** `busybox telnetd` needs `/dev/pts` mounted to allocate a pty per
  session; without it, `telnetd` most likely exits immediately at startup. Since its output wasn't
  captured, there was no way to confirm this from the boot log alone.
- **v2** — added the `mount -t devpts` line and redirected all diagnostic output as shown above.
  Confirmed working after this fix.

## Source of truth

`etc/rc.d/rcS` in this payload is derived from
`Prado firmware dump/mtd6_rootfs/etc/rc.d/rcS` — the actual dumped stock file, not the
reconstructed rootfs's version — with only the block above added. Diff against the dump to verify
no other changes crept in before redeploying an updated version of this payload.

## Cross-references

- [`docs/UI_AND_APP_ANALYSIS.md`](../../docs/UI_AND_APP_ANALYSIS.md) — disassembly trace of the
  `msn_autocopy` mechanism itself.
- [`docs/SECURITY_REVIEW.md`](../../docs/SECURITY_REVIEW.md) — broader credential/access-path review.
- [`README.md` §10.0](../../README.md#usb-auto-update-msn_autocopy) — main project documentation for
  device access methods generally.
- `msn_autocopy_payload_do not use/` — the earlier, non-working SSH-transplant attempt, kept for
  reference on what not to do (source rootfs mismatch).
