#!/bin/sh
# Start the WiFi access point for wireless Android Auto/CarPlay
# projection (WirelessSessionManager's own AP, see
# custom_ui/src/androidauto/wireless_session_manager.cpp) -- also
# reachable for SSH management, but that's a side effect, not what
# this SSID/config is actually for.
#
# 2026-08-16: this used to share /etc/hostapd/hostapd.conf and the
# SSID "carplay_wifi" with MsnCoreApp's own real AA/CarPlay AP -- per
# explicit request, deliberately separated so the two never share
# state or risk phone-side cached-credential mixups between two
# different real APs advertising identical SSID/password: own config
# file (hostapd-custom_ui.conf) and own SSID (custom_ui_wifi) now.
# MsnCoreApp's own AP keeps working entirely on its own, untouched by
# anything in this file.
# SSID: custom_ui_wifi  Password: 88888888  IP: 192.168.43.1

# hostapd needs /dev/urandom for WPA key generation
[ -e /dev/random.orig ] || mv /dev/random /dev/random.orig
ln -sf /dev/urandom /dev/random

# Tune socket buffers (required for stable AP operation)
echo 2097152 > /proc/sys/net/core/rmem_default
echo 2097152 > /proc/sys/net/core/rmem_max
echo 1048576 > /proc/sys/net/core/wmem_default
echo 1048576 > /proc/sys/net/core/wmem_max

# Load WiFi kernel module.
# MsnCoreApp copies the detected chip's driver to /tmp/wlan.ko at runtime.
# At early boot we probe /lib/modules directly — SDIO combo chips first
# (RTL8821CS / RTL8822CS are most likely for Feasycom BT+WiFi modules).
# rtw_country_code=US, same fix as hostapd.sh/rcS (2026-08-04, see
# firmware_overlay/README.md): without a valid HW/SW channel plan, the
# rtl8811cu driver's own regulatory fallback (RTW_CHPLAN_WORLDWIDE)
# excludes 5GHz UNII-1 channels entirely, including channel 36 -- which
# this file's own hostapd-custom_ui.conf now uses (2026-08-16, switched
# from 2.4GHz to match real stock/MsnCoreApp band). rcS's own module
# load already applies this fix at boot, but if this script ends up
# reloading the module fresh (e.g. /tmp/wlan.ko populated by
# MsnCoreApp after boot) without repeating it here too, that reload
# would silently lose the regulatory fix and break channel 36 again --
# same failure class already root-caused once for the hostapd.sh path.
if [ -f /tmp/wlan.ko ]; then
    insmod /tmp/wlan.ko rtw_country_code=US
else
    # Try modprobe first -- uses modules.dep for 4.19.192
    # rtw_drv_log_level=2 (_DRV_ERR_, lowered from 3/_DRV_WARNING_
    # 2026-07-31 -- WARNING level in this vendor driver is still
    # routinely noisy) quiets routine driver-internal chatter on the
    # three variants that support the param; rtl8822cs/rtl8821cu have
    # no source in this tree (always fail to "module not found"
    # regardless) so there's nothing to quiet there.
    modprobe rtl8821cs rtw_drv_log_level=2 rtw_country_code=US 2>/dev/null || \
    modprobe rtl8822cs rtw_country_code=US 2>/dev/null || \
    modprobe rtl8189fs rtw_drv_log_level=2 rtw_country_code=US 2>/dev/null || \
    modprobe rtl8821cu rtw_country_code=US 2>/dev/null || \
    modprobe rtl8811cu rtw_drv_log_level=2 rtw_country_code=US 2>/dev/null || {
        # Fallback: direct .ko path using running kernel version
        KVER=$(uname -r)
        for ko in \
            /lib/modules/${KVER}/kernel/drivers/net/wireless/realtek/rtl8821cs/rtl8821cs.ko \
            /lib/modules/${KVER}/kernel/drivers/net/wireless/realtek/rtlwifi/rtl8192cu/rtl8192cu.ko \
            /lib/modules/${KVER}/kernel/drivers/net/wireless/realtek/rtl8811cu/rtl8811cu.ko \
            /lib/modules/${KVER}/kernel/drivers/net/wireless/realtek/rtl8xxxu/rtl8xxxu.ko; do
            [ -f "$ko" ] && insmod "$ko" && break
        done
    }
fi

# The onboard WiFi module can take a long time to finish USB enumeration
# on this hardware -- the musb controller's own OTG recovery/retry cycle
# (see [musb] messages elsewhere in rcS/dmesg) has been observed taking
# 15-20+ seconds in real boot logs, well past a fixed `sleep 1`. Poll for
# wlan0 to actually exist instead of assuming it's ready.
i=0
while [ ! -e /sys/class/net/wlan0 ] && [ $i -lt 30 ]; do
    sleep 1
    i=$((i + 1))
done

ifconfig wlan0 up || exit 1
ifconfig wlan0 192.168.43.1 netmask 255.255.255.0
echo 1 > /proc/sys/net/ipv6/conf/wlan0/disable_ipv6

mkdir -p /var/lib/misc
touch /data/udhcpd.leases
udhcpd /etc/udhcpd.conf &

hostapd -B /etc/hostapd/hostapd-custom_ui.conf
