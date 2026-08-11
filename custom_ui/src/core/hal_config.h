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
// config files). Search order -- first path that exists wins, parsed
// in full (no merging across paths, unlike ConfigStore's live+factory
// layering, since there's no "factory default that shouldn't be
// touched" concept here):
//   1. /data/custom_ui/hal.conf   -- writable userdata partition, a
//      real device deployment's actual edit target; survives a
//      firmware-image reflash the way /etc wouldn't.
//   2. /etc/custom_ui/hal.conf    -- shipped default, tracked in this
//      repo at firmware_overlay/etc/custom_ui/hal.conf.
// If neither exists (e.g. a dev host build, or a firmware image that
// hasn't deployed the overlay file yet), every getter below falls
// back to the same literal values this project had hardcoded before
// -- non-fatal, matching every other optional-file pattern in this
// codebase.
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

    std::string bluetooth_daemon_path_ = "/usr/bin/blueware";
    std::string bluetooth_properties_path_ = "/etc/blueware-bw121.properties";
    std::string bluetooth_serial_port_ = "/dev/bw_serial";
    std::string bluetooth_log_path_ = "/tmp/blueware.log";

    std::string wifi_ap_script_ = "/etc/wifi_ap.sh";
    std::string wifi_ap_address_ = "192.168.43.1";
    std::string wifi_ap_ssid_ = "carplay_wifi";
    std::string wifi_ap_password_ = "88888888";
    int wifi_ap_security_mode_ = 8;
    std::uint16_t wifi_session_port_ = 5277;
};

// Process-lifetime singleton, lazily loaded on first call -- same
// pattern as core::default_store(). Both custom_ui and
// androidauto-sidecar link this file and get their own independent
// instance/load (separate processes), which is fine: the file is only
// ever read, never written by this app.
HalConfig & hal_config();

}  // namespace core
