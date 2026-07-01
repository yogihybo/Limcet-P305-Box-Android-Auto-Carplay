# NAND Partition Layout — Prado

Confirmed from live device `printenv mtdparts`:

```
mtdparts=mtdparts=ark1680-nand:128k(S-Loader),512k(U-boot),512k(U-boot_back),
256K(U-boot-Env),256K(arkdata),4m(kernel),106m(rootfs),6m(userdata),
512K(bootlogo),3m(bootanimation),3m(reversingtrack),256K(Unicode)
```

| # | Name | Offset | Size | End | File |
|---|------|--------|------|-----|------|
| 0 | S-Loader | 0x000000 | 128K | 0x020000 | `bootloaders/Nboot.bin` |
| 1 | U-Boot | 0x020000 | 512K | 0x0A0000 | `bootloaders/uboot.bin` |
| 2 | U-boot_back | 0x0A0000 | 512K | 0x120000 | `bootloaders/uboot.bin` (copy) |
| 3 | U-boot-Env | 0x120000 | 256K | 0x160000 | `env/uboot-env.bin` (built from `env/uboot-env.txt` via `mkenvimage`) |
| 4 | arkdata | 0x160000 | 256K | 0x1A0000 | `display/arkdata_prado.ini` |
| 5 | kernel | 0x1A0000 | 4M | 0x5A0000 | `kernel/zImage` |
| 6 | rootfs | 0x5A0000 | 106M | 0x6FA0000 | `rootfs/rootfs.ubifs` |
| 7 | userdata | 0x6FA0000 | 6M | 0x75A0000 | `userdata/userdata.img` |
| 8 | bootlogo | 0x75A0000 | 512K | 0x7620000 | `bootloaders/bootlogo` |
| 9 | bootanimation | 0x7620000 | 3M | 0x7920000 | *(not in pkg)* |
| 10 | reversingtrack | 0x7920000 | 3M | 0x7C20000 | `bootloaders/reversingtrack` |
| 11 | Unicode | 0x7C20000 | 256K | 0x7C60000 | *(not in pkg)* |

## Flash commands (from U-Boot)

```
# Erase and write each partition
nand erase 0x000000 0x20000;  nand write 0x1000000 0x000000 0x20000    # S-Loader
nand erase 0x020000 0x80000;  nand write 0x1000000 0x020000 0x80000    # U-Boot
nand erase 0x0A0000 0x80000;  nand write 0x1000000 0x0A0000 0x80000    # U-boot_back
nand erase 0x120000 0x40000;  nand write 0x1000000 0x120000 0x40000    # U-boot-Env
nand erase 0x160000 0x40000;  nand write 0x1000000 0x160000 0x40000    # arkdata
nand erase 0x1A0000 0x400000; nand write 0x1000000 0x1A0000 0x400000   # kernel
nand erase 0x5A0000 0x6A00000 # rootfs (flashed via UpConfig/update mechanism)
nand erase 0x6FA0000 0x600000 # userdata wipe
```

## userdata wipe only
```
nand erase 0x6FA0000 0x600000
```
Device reinitialises /data from defaults on next boot.
