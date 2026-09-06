#ifndef AAP_WIFI_SETUP_H
#define AAP_WIFI_SETUP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start WiFi AP if not running */
bool aap_wifi_ensure_ap_up(void);

/* Tear down the WiFi AP (hostapd + udhcpd + wlan0 down) WITHOUT
 * unloading the WiFi kernel module -- see wifi_ap_down.sh's own
 * comment for why. Called by main.c ~10s after an AA session ends with
 * no new one taking its place, so the phone sees a real WiFi
 * disconnect instead of staying silently associated to a head unit
 * with no active session behind it. */
void aap_wifi_teardown_ap(void);

/* Read wlan0 MAC address / BSSID */
bool aap_wifi_get_bssid(char *out_bssid, size_t max_len);

/* Perform Wireless Projection Protocol (WPP) handshake over Bluetooth RFCOMM */
bool aap_wifi_setup_handshake(int rfcomm_fd, const char *ap_ip, uint16_t ap_port,
                              const char *ssid, const char *password, const char *bssid,
                              int security_mode);

#ifdef __cplusplus
}
#endif

#endif /* AAP_WIFI_SETUP_H */
