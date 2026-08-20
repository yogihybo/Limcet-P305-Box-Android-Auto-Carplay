// Editable HAL configuration -- Bluetooth (blueware daemon) and WiFi
// AP (wireless Android Auto/CarPlay handoff) settings that used to be
// hardcoded string/int constants scattered across hal/bluetooth.cpp
// and androidauto/wireless_session_manager.cpp. Moved here so a real
// device deployment can retune them (a different blueware properties
// variant, a different AP SSID/password, etc.) without rebuilding.
//
// NOT the same thing as core/config_store.h's ConfigStore -- that
// class specifically mirrors the vendor's own live/factory
// Setting.config layering (docs/SETTINGS_REFERENCE.md) and is meant
// for user-facing device settings (brightness, volume, language).
// This file is purely internal plumbing (daemon paths, a hardcoded AP
// password) that this project's own code needs, with no vendor-format
// counterpart to mirror -- reusing ConfigStore for it would conflate
// two genuinely different concerns and drag in its live/factory-seed
// precedence logic for no reason.
//
// File format: flat INI, `[Section]` headers, `Key=Value` lines,
// `#`-prefixed comment lines, same style as the vendor .ini files
// ConfigStore already parses (familiar to anyone editing this repo's
// config files). Tracked source: custom_ui/etc/hal.conf -- the
// Makefile stages a copy into build/hal.conf on every `make ui`/
// `make androidauto-sidecar`, right next to the compiled binaries, so
// scp'ing build/ to the device (this project's actual fast-iteration
// workflow, see scripts/run_on_device.sh -- custom_ui isn't wired into
// a real firmware_overlay image build yet) carries the config along
// automatically with no on-device setup.
//
// Search order -- first path that exists wins, parsed in full (no
// merging across paths, unlike ConfigStore's live+factory layering,
// since there's no "factory default that shouldn't be touched" concept
// here):
//   1. <directory containing the running binary>/hal.conf -- resolved
//      via /proc/self/exe, same technique as
//      androidauto_client.cpp's trySpawnSidecar(). The build/hal.conf
//      staging above lands here once scp'd alongside the binary.
//   2. /data/hal.conf -- writable userdata partition, the real edit
//      target on a properly deployed device; survives a firmware-image
//      reflash the way /etc wouldn't. (2026-08-15: dropped the
//      /data/custom_ui/ subfolder -- a flat file directly under /data
//      is enough, no dedicated subdirectory needed.)
//   3. /etc/custom_ui/hal.conf -- would be a real firmware image's
//      shipped default. Nothing deploys a file there today (see
//      above), kept as a search path for when custom_ui does get
//      wired into a real image build.
// If none exist (e.g. a dev host build with no config file around at
// all), every getter below falls back to the same literal values this
// project had hardcoded before -- non-fatal, matching every other
// optional-file pattern in this codebase.
#pragma once

#include <cstdint>
#include <string>

namespace core {

class HalConfig {
public:
    // Loads on construction (see class comment for the search order).
    // Safe to construct even if no config file exists anywhere --
    // every getter has a real fallback.
    HalConfig();

    // ---- Bluetooth (see hal/bluetooth.h/.cpp) --------------------------
    const std::string & bluetooth_daemon_path() const { return bluetooth_daemon_path_; }
    const std::string & bluetooth_properties_path() const { return bluetooth_properties_path_; }
    const std::string & bluetooth_serial_port() const { return bluetooth_serial_port_; }
    const std::string & bluetooth_log_path() const { return bluetooth_log_path_; }

    // ---- WiFi AP (see androidauto/wireless_session_manager.h/.cpp) -----
    const std::string & wifi_ap_script() const { return wifi_ap_script_; }
    const std::string & wifi_ap_address() const { return wifi_ap_address_; }
    const std::string & wifi_ap_ssid() const { return wifi_ap_ssid_; }
    const std::string & wifi_ap_password() const { return wifi_ap_password_; }
    int wifi_ap_security_mode() const { return wifi_ap_security_mode_; }
    std::uint16_t wifi_session_port() const { return wifi_session_port_; }

private:
    void apply_line(const std::string & section, const std::string & key,
                     const std::string & value);
    bool load_file(const std::string & path);

    std::string bluetooth_daemon_path_ = "/usr/libexec/bluetooth/bluetoothd";
    std::string bluetooth_properties_path_ = "/etc/bluetooth/main.conf";
    std::string bluetooth_serial_port_ = "/dev/ttyHS1";
    std::string bluetooth_log_path_ = "/dev/null";

    std::string wifi_ap_script_ = "/etc/wifi_ap.sh";
    std::string wifi_ap_address_ = "192.168.43.1";
    std::string wifi_ap_ssid_ = "custom_ui_wifi";
    std::string wifi_ap_password_ = "88888888";
    // 2026-08-20: was 8 (WPA2_ENTERPRISE per aap_protobuf's own
    // WifiSecurityMode enum) -- the raw value from an earlier captured
    // packet, passed through "to match known-good bytes rather than
    // guess" per bw_aap_client.h's own respondToInfoRequest() comment,
    // which already flagged this exact discrepancy and named 5 as the
    // fallback to try. Real hardware test (2026-08-20): phone shows AA
    // as connecting (RFCOMM handshake completes) but the WiFi link
    // never actually comes up -- consistent with the phone attempting
    // enterprise 802.1x auth against what hostapd-custom_ui.conf (the
    // real AP config: wpa=2, wpa_key_mgmt=WPA-PSK) actually serves,
    // ordinary WPA2-Personal. 5 = WPA2_PERSONAL, matching the real AP.
    int wifi_ap_security_mode_ = 5;
    // 2026-08-12: briefly changed to 5288 (an unconfirmed guess) after
    // 5277 got ECONNREFUSED with this device connecting OUT to the
    // phone -- reverted back to 5277 once a real confirmed-working
    // reference implementation (github.com/mossyhub/openautolink)
    // showed the actual bug was direction, not port: Google's real WPP
    // has the HEAD UNIT as the TCP server on 5277, phone dials in. See
    // wireless_session_manager.h for the full story. Still overridable
    // via hal.conf's SessionPort without a rebuild.
    std::uint16_t wifi_session_port_ = 5277;
};

// Process-lifetime singleton, lazily loaded on first call -- same
// pattern as core::default_store(). Both custom_ui and
// androidauto-sidecar link this file and get their own independent
// instance/load (separate processes), which is fine: the file is only
// ever read, never written by this app.
HalConfig & hal_config();

}  // namespace core
