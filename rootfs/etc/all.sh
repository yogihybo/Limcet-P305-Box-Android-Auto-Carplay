#! /bin/sh

mount -t ramfs -n none /media
/sbin/mdev -s

#H264 and it565 driver, needed by media and carpaly etc.
cd /etc
./memalloc_load.sh
./driver_load.sh
./itu656_load.sh

#audio
#insmod  /lib/modules/3.4.0/kernel/drivers/ark/audio/ark-cs4334-codec.ko
#insmod  /lib/modules/3.4.0/kernel/drivers/ark/audio/ark-sddac-codec.ko
#insmod  /lib/modules/3.4.0/kernel/drivers/ark/audio/snd-soc-ark-i2s.ko
#insmod  /lib/modules/3.4.0/kernel/drivers/ark/audio/snd-soc-ark-pcm-dma.ko
#insmod  /lib/modules/3.4.0/kernel/drivers/ark/audio/snd-soc-ark-sddac.ko

#usb
insmod /lib/modules/3.4.0/kernel/drivers/usb/musb/musb_hdrc.ko
insmod /lib/modules/3.4.0/kernel/drivers/usb/musb/ark1680_musb.ko
insmod /lib/modules/3.4.0/kernel/drivers/usb/gadget/g_ncm.ko
insmod /lib/modules/3.4.0/kernel/drivers/usb/gadget/g_zero.ko

/etc/switchotg.sh

#sd
insmod  /lib/modules/3.4.0/kernel/drivers/ark/sdmmc/ark_dw_mmc.ko

#amixer cset numid=1,iface=MIXER,name='Left Playback Volume' 118
#amixer cset numid=2,iface=MIXER,name='Right Playback Volume' 118

#2d
#insmod  /lib/modules/3.4.0/galcore.ko registerMemBase=0xE0F00000 irqLine=32 contiguousSize=0x400000 physSize=0x80000000 powerManagement=0

#ulimit -c unlimited
