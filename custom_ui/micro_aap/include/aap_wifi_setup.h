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
