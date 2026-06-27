# Prado Firmware Reconstruction

Reconstructed firmware for a Toyota Prado head unit running on the **Limcet Box P306** (ARK1680 SoC).

The Prado unit uses Holden firmware as its base but requires hardware-specific overrides for the display panel, product identity, and U-Boot environment. This repository tracks those overrides and the reconstruction process.

## Hardware

| Item | Value |
|------|-------|
| SoC | ARK1680 (ARM Cortex-A5) |
| OS | Linux 3.4.0 / BusyBox |
| Bootloader | U-Boot 2012.10 |
| Product ID | Limcet-P306 |
| Resource | Box-P301 |
| Display | 800×480 RGB888 |
| Sound | None (SoundType=0) |
| MCU type | 6 |
| BT module | Feasycom (BlueToothType=6) |

## Repository Structure

```
bootloaders/       Nboot.bin, Stepldr.bin, uboot.bin, bootlogo, reversingtrack
kernel/            zImage (from Holden base — identical kernel_size to Prado dump)
rootfs/            rootfs.ubifs (from Prado live NAND dump)
userdata/          userdata.img (base; live state documented in docs/)
display/
  arkdata_prado.ini          Prado panel config (from MTD4 live dump)
  arkdata_holden.ini         Holden standard reference
  arkdata_holden_0324.ini    Holden March 2024 update reference
msn_factory_configs/
  FactoryConfig.ini          Prado identity + Holden firmware settings
  MsnProductInfo.ini         Hardware identity (Limcet-P306)
env/
  uboot-env.txt              Reconstructed env (bootdelay=9, 106m/6m layout)
  mtd3_env_prado_dump.bin    Raw env from live device (gitignored)
docs/
  SOURCES.md                 Where each file came from and why
  PARTITION_LAYOUT.md        NAND offsets, sizes, flash commands
UpConfig                     Triggers full reflash on boot (from Holden base)
update                       Partition write order script
```

## Key Differences vs Holden Base Firmware

| Item | Holden | Prado |
|------|--------|-------|
| ProductId | Ksmart_DSP | **Limcet-P306** |
| ResourceName | Box-C211 | **Box-P301** |
| McuType | 16 | **6** |
| SoundType | 4 (DSP) | **0** |
| Panel timing (CLKDIV1) | 10 | **11** |
| Panel VBP/HBP | 3/20 | **29/32** |
| Touch keys | 5 | **none** |
| bootdelay | 0 | **9** |
| BT device name | Ksmart | **Limcet Box** |
| BT pair code | 0000 | **8362** |
| Vehicle branding | HOLDEN | **TOYOTA** |

## SSH Access

SSH is enabled in the reconstructed rootfs and starts automatically on boot.

| Item | Value |
|------|-------|
| Binary | `/usr/bin/sshd` (OpenSSH 4.6p1) |
| Config | `/etc/ssh/sshd_config` |
| Host keys | `/etc/ssh/ssh_host_rsa_key` (RSA 2048), `/etc/ssh/ssh_host_dsa_key` |
| Login | `root` with existing password hash from `/etc/shadow` |

**To connect:**

```sh
ssh root@192.168.7.1
```

## USB Networking

The ARK1680 USB gadget stack is configured to use CDC-NCM (`g_ncm.ko`), which creates a `usb0` network interface when connected to a host PC.

| Item | Value |
|------|-------|
| Device IP | `192.168.7.1` |
| Subnet | `255.255.255.0` |
| Set PC address to | `192.168.7.2` (static) |

**Platform notes:**
- **macOS / Linux** — CDC-NCM supported natively; interface appears automatically
- **Windows** — may require the CDC-NCM host driver from Windows Update

`g_zero.ko` has been removed from `rootfs/etc/all.sh` — it was overriding the NCM gadget registration and breaking both USB host mode and the network interface.

## Flash Method

The device uses the `UpConfig` mechanism: placing `UpConfig` in the root of a FAT32 SD card causes U-Boot to run `arkupdate` on boot, which reads the `update` script and flashes each partition in sequence.

See [`docs/PARTITION_LAYOUT.md`](docs/PARTITION_LAYOUT.md) for offsets and individual flash commands.

## Sources

See [`docs/SOURCES.md`](docs/SOURCES.md) for full provenance of each file.
