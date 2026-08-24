#include "aap_wifi_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>

#include <pb_encode.h>
#include <pb_decode.h>

#include "aap_protobuf/aaw/WifiStartRequest.pb.h"
#include "aap_protobuf/aaw/WifiStartResponse.pb.h"
#include "aap_protobuf/aaw/WifiInfoRequest.pb.h"
#include "aap_protobuf/aaw/WifiInfoResponse.pb.h"
#include "aap_protobuf/aaw/WifiConnectionStatus.pb.h"

#define WPP_MSG_START_REQUEST     1
#define WPP_MSG_INFO_REQUEST      2
#define WPP_MSG_INFO_RESPONSE     3
#define WPP_MSG_VERSION_REQUEST   4
#define WPP_MSG_VERSION_RESPONSE  5
#define WPP_MSG_CONNECT_STATUS    6
#define WPP_MSG_START_RESPONSE    7

static const unsigned char kWifiVersionRequestPayload[] = {
    0x08, 0x01, 0x10, 0x00, 0x18, 0x00, 0x20, 0xbc, 0x28
};

static bool read_fully(int fd, void *buf, size_t len) {
    uint8_t *cursor = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, cursor + got, len - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n == 0) return false; /* EOF */
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool send_wpp_frame(int fd, uint16_t type, const uint8_t *payload, size_t payload_len) {
    uint8_t header[4];
    uint16_t len_be = htons((uint16_t)payload_len);
    uint16_t type_be = htons(type);
    memcpy(&header[0], &len_be, 2);
    memcpy(&header[2], &type_be, 2);

    if (write(fd, header, 4) != 4) return false;
    if (payload && payload_len > 0) {
        if (write(fd, payload, payload_len) != (ssize_t)payload_len) return false;
    }
    return true;
}

static bool recv_wpp_frame(int fd, uint16_t *out_type, uint8_t *out_payload, size_t *out_len, size_t max_len, int timeout_sec) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (ret <= 0) return false;

    uint8_t header[4];
    if (!read_fully(fd, header, 4)) return false;

    uint16_t len_be, type_be;
    memcpy(&len_be, &header[0], 2);
    memcpy(&type_be, &header[2], 2);
    uint16_t length = ntohs(len_be);
    *out_type = ntohs(type_be);

    if (length > max_len) return false;
    if (length > 0 && !read_fully(fd, out_payload, length)) return false;

    *out_len = length;
    return true;
}

bool aap_wifi_ensure_ap_up(void) {
    if (system("pidof hostapd >/dev/null 2>&1") == 0) {
        if (system("pidof udhcpd >/dev/null 2>&1") != 0) {
            system("ifconfig wlan0 192.168.43.1 netmask 255.255.255.0 2>/dev/null");
            system("mkdir -p /var/lib/misc; touch /data/udhcpd.leases; udhcpd /etc/udhcpd.conf >/dev/null 2>&1 &");
            usleep(200000);
        }
        return true;
    }
    int rc = system("/etc/wifi_ap.sh");
    return (rc == 0);
}

bool aap_wifi_get_bssid(char *out_bssid, size_t max_len) {
    if (!out_bssid || max_len < 18) return false;

    FILE *f = popen("hostapd_cli -i wlan0 status 2>/dev/null", "r");
    if (f) {
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) {
            if (strncmp(buf, "bssid[0]=", 9) == 0) {
                char *nl = strpbrk(buf + 9, "\r\n");
                if (nl) *nl = '\0';
                strncpy(out_bssid, buf + 9, max_len - 1);
                pclose(f);
                return true;
            }
        }
        pclose(f);
    }

    FILE *sf = fopen("/sys/class/net/wlan0/address", "r");
    if (sf) {
        if (fgets(out_bssid, (int)max_len, sf)) {
            char *nl = strpbrk(out_bssid, "\r\n");
            if (nl) *nl = '\0';
            fclose(sf);
            return true;
        }
        fclose(sf);
    }

    return false;
}

bool aap_wifi_setup_handshake(int rfcomm_fd, const char *ap_ip, uint16_t ap_port,
                              const char *ssid, const char *password, const char *bssid,
                              int security_mode) {
    printf("aap_wifi_setup: starting WPP handshake on rfcomm_fd=%d\n", rfcomm_fd);

    /* 1. Send WIFI_VERSION_REQUEST (type 4) */
    if (!send_wpp_frame(rfcomm_fd, WPP_MSG_VERSION_REQUEST, kWifiVersionRequestPayload, sizeof(kWifiVersionRequestPayload))) {
        fprintf(stderr, "aap_wifi_setup: failed to send WIFI_VERSION_REQUEST\n");
        return false;
    }

    /* 2. Receive WIFI_VERSION_RESPONSE (type 5) */
    uint16_t type = 0;
    uint8_t rx_buf[1024];
    size_t rx_len = 0;
    if (!recv_wpp_frame(rfcomm_fd, &type, rx_buf, &rx_len, sizeof(rx_buf), 10) || type != WPP_MSG_VERSION_RESPONSE) {
        fprintf(stderr, "aap_wifi_setup: expected WIFI_VERSION_RESPONSE (5), got %u\n", type);
        return false;
    }
    printf("aap_wifi_setup: received WIFI_VERSION_RESPONSE\n");

    /* 3. Send WIFI_START_REQUEST (type 1) */
    aap_protobuf_aaw_WifiStartRequest start_req = aap_protobuf_aaw_WifiStartRequest_init_default;
    strncpy(start_req.ip_address, ap_ip ? ap_ip : "192.168.43.1", sizeof(start_req.ip_address) - 1);
    start_req.port = ap_port ? ap_port : 5000;

    uint8_t pb_buf[256];
    pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    pb_encode(&ostream, aap_protobuf_aaw_WifiStartRequest_fields, &start_req);

    if (!send_wpp_frame(rfcomm_fd, WPP_MSG_START_REQUEST, pb_buf, ostream.bytes_written)) {
        fprintf(stderr, "aap_wifi_setup: failed to send WIFI_START_REQUEST\n");
        return false;
    }
    printf("aap_wifi_setup: sent WIFI_START_REQUEST (%s:%u)\n", start_req.ip_address, start_req.port);

    /* 4. Process post-start messages (WIFI_START_RESPONSE, WIFI_INFO_REQUEST, WIFI_CONNECT_STATUS) */
    bool got_start_resp = false;
    bool sent_info_resp = false;

    for (int i = 0; i < 5; i++) {
        if (!recv_wpp_frame(rfcomm_fd, &type, rx_buf, &rx_len, sizeof(rx_buf), 10)) {
            printf("aap_wifi_setup: no more frames on RFCOMM (may be normal if phone is connecting to WiFi)\n");
            break;
        }

        printf("aap_wifi_setup: received WPP frame type=%u (%zu bytes)\n", type, rx_len);

        if (type == WPP_MSG_START_RESPONSE) {
            got_start_resp = true;
            printf("aap_wifi_setup: got WIFI_START_RESPONSE (type 7)\n");
        } else if (type == WPP_MSG_INFO_REQUEST) {
            printf("aap_wifi_setup: got WIFI_INFO_REQUEST (type 2), sending WIFI_INFO_RESPONSE (type 3)\n");
            aap_protobuf_aaw_WifiInfoResponse info_resp = aap_protobuf_aaw_WifiInfoResponse_init_default;
            strncpy(info_resp.ssid, ssid ? ssid : "custom_ui_wifi", sizeof(info_resp.ssid) - 1);
            strncpy(info_resp.password, password ? password : "custom_ui_wifi_pass", sizeof(info_resp.password) - 1);
            strncpy(info_resp.bssid, bssid ? bssid : "", sizeof(info_resp.bssid) - 1);
            info_resp.security_mode = (aap_protobuf_service_wifiprojection_message_WifiSecurityMode)security_mode;

            ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            pb_encode(&ostream, aap_protobuf_aaw_WifiInfoResponse_fields, &info_resp);

            if (!send_wpp_frame(rfcomm_fd, WPP_MSG_INFO_RESPONSE, pb_buf, ostream.bytes_written)) {
                fprintf(stderr, "aap_wifi_setup: failed to send WIFI_INFO_RESPONSE\n");
                return false;
            }
            sent_info_resp = true;
            printf("aap_wifi_setup: sent WIFI_INFO_RESPONSE (ssid=%s, bssid=%s)\n", info_resp.ssid, info_resp.bssid);
        } else if (type == WPP_MSG_CONNECT_STATUS) {
            printf("aap_wifi_setup: got WIFI_CONNECT_STATUS (type 6)\n");
            break;
        }
    }

    printf("aap_wifi_setup: WPP Bluetooth handshake complete! Phone associating to WiFi...\n");
    return true;
}
