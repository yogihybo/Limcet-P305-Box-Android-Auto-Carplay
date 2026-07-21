#!/bin/sh
rm -rf crcdata.bin
touch crcdata.bin
echo "crc32app u-boot"
./crc32app u-boot.img
echo "crc32app fdt"
./crc32app *.dtb
echo "crc32app zImage"
./crc32app zImage
echo "crc32app rootfs"
./crc32app rootfs*
echo "crc32app bootanimation"
./crc32app bootanimation
echo "crc32app ubootspl"
./crc32app ubootspl.bin
echo "crc32app bootlogo"
./crc32app bootlogo
