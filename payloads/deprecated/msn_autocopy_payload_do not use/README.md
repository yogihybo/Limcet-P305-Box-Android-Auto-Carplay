# msn_autocopy SSH payload - DO NOT USE AS IT WILL LOCK UP DEVICE ON REBOOT AND REQUIRE HARD RECOVERY
#USE TELNET PAYLOAD

A USB/SD payload that exploits the `msn_autocopy` auto-update mechanism discovered in `MsnCoreApp`
to install and autostart `sshd` on a **stock, unmodified** Prado head unit without any serial
console access, U-Boot interrupt, or firmware reflash.

See [`docs/MSNCOREAPP_REVIEW.md`](../docs/MSNCOREAPP_REVIEW.md) for the full mechanism analysis.

---

## What it does

When inserted into the head unit, this payload causes the device to:

1. Remount its root filesystem read-write (`mount -o remount,rw /`)
2. Copy everything under `msn_autocopy/` onto `/`, overwriting:
   - `/usr/bin/sshd` — OpenSSH 4.6p1 ARM binary
   - `/etc/rc.d/rcS` — startup script, with `sshd` autostart appended
   - `/etc/ssh/sshd_config` — `PermitRootLogin yes`, `PasswordAuthentication yes`
   - `/etc/ssh/ssh_host_rsa_key` + `.pub` — RSA 2048 host key
   - `/etc/ssh/ssh_host_dsa_key` + `.pub` — DSA host key

No user interaction is required beyond inserting the media.

---

## Payload layout

```
msn_autocopy/
  usr/bin/sshd                  ← OpenSSH 4.6p1, ARM, from reconstructed rootfs
  etc/rc.d/rcS                  ← stock rcS + mkdir -p /var/run/sshd + /usr/bin/sshd &
  etc/ssh/sshd_config           ← PermitRootLogin yes, PasswordAuthentication yes, Port 22
  etc/ssh/ssh_host_rsa_key
  etc/ssh/ssh_host_rsa_key.pub
  etc/ssh/ssh_host_dsa_key
  etc/ssh/ssh_host_dsa_key.pub
```

---

## How to deploy

1. Format a USB drive or SD card as FAT32.
2. Copy the `msn_autocopy/` folder to the **root** of the drive (the trigger is the folder name).
3. Insert the drive into the head unit while it is running.
4. `MsnCoreApp` auto-mounts all inserted media — the copy fires automatically within a few
   seconds. No dialog, no confirmation.
5. Eject the drive. The files are now live on the rootfs.
6. **Reboot the head unit.** The modified `rcS` runs on startup, which starts `sshd`.

After reboot, SSH is available:

```
ssh root@192.168.7.1    # over USB-NCM (cable connected)
```

Password: `123456` (stock factory default — see [`docs/SECURITY_REVIEW.md`](../docs/SECURITY_REVIEW.md) §1).

> The WiFi AP (`carplay_wifi`, password `88888888`) is also started by the same modified `rcS`,
> so SSH is reachable over WiFi at whatever DHCP address the device assigns itself if you prefer
> wireless access.

---

## Hardening after first login

Once you have a root shell, lock down the credentials before leaving the device connected to
anything:

```sh
# Install your SSH public key
mkdir -p /root/.ssh
echo 'ssh-ed25519 AAAA... yourkey' >> /root/.ssh/authorized_keys

# Disable password auth
sed -i 's/^PasswordAuthentication yes/PasswordAuthentication no/' /etc/ssh/sshd_config

# Kill and restart sshd to pick up the new config
kill $(cat /var/run/sshd.pid 2>/dev/null) 2>/dev/null
mkdir -p /var/run/sshd && /usr/bin/sshd &
```

The rootfs is normally mounted read-only. The `msn_autocopy` mechanism (or a manual
`mount -o remount,rw /` from a root shell) is required before writing to it.

---

## Source files

All files in `msn_autocopy/` are copied directly from:

```
Prado firmware reconstructed/mtd6_rootfs/rootfs/
```

The `sshd_config` and host keys are the same ones shipped in the reconstructed firmware image.
The `rcS` is the reconstructed version (not the stock dump) — it already has the sshd lines;
no further modifications were made for this payload.
