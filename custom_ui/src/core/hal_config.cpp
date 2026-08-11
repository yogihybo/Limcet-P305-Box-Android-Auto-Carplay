#include "core/hal_config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

#include <unistd.h>

namespace core {

namespace {

std::string trim(const std::string & s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Directory containing the running binary, via /proc/self/exe -- same
// technique as androidauto_client.cpp's trySpawnSidecar(). Empty
// string if it can't be resolved (e.g. /proc unavailable) rather than
// throwing/asserting -- this is a "nice to have" search path, not a
// required one.
std::string executable_dir() {
    char exePath[512];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len <= 0) return "";
    exePath[len] = '\0';

    std::string dir(exePath);
    auto slash = dir.find_last_of('/');
    if (slash == std::string::npos) return "";
    dir.resize(slash);
    return dir;
}

}  // namespace

HalConfig::HalConfig() {
    // First path that exists wins -- see class comment, no merging
    // across paths (unlike ConfigStore's live+factory layering).
    std::string exe_dir = executable_dir();
    if (!exe_dir.empty() && load_file(exe_dir + "/hal.conf")) {
        std::printf("core::HalConfig: loaded %s/hal.conf\n", exe_dir.c_str());
        return;
    }
    if (load_file("/data/custom_ui/hal.conf")) {
        std::printf("core::HalConfig: loaded /data/custom_ui/hal.conf\n");
        return;
    }
    if (load_file("/etc/custom_ui/hal.conf")) {
        std::printf("core::HalConfig: loaded /etc/custom_ui/hal.conf\n");
        return;
    }
    std::printf("core::HalConfig: no config file found, using built-in defaults\n");
}

void HalConfig::apply_line(const std::string & section, const std::string & key,
                            const std::string & value) {
    if (section == "Bluetooth") {
        if (key == "DaemonPath") bluetooth_daemon_path_ = value;
        else if (key == "PropertiesPath") bluetooth_properties_path_ = value;
        else if (key == "SerialPort") bluetooth_serial_port_ = value;
        else if (key == "LogPath") bluetooth_log_path_ = value;
    } else if (section == "WiFi") {
        if (key == "ApScript") wifi_ap_script_ = value;
        else if (key == "ApAddress") wifi_ap_address_ = value;
        else if (key == "ApSsid") wifi_ap_ssid_ = value;
        else if (key == "ApPassword") wifi_ap_password_ = value;
        else if (key == "ApSecurityMode") wifi_ap_security_mode_ = std::atoi(value.c_str());
        else if (key == "SessionPort") {
            wifi_session_port_ = static_cast<std::uint16_t>(std::atoi(value.c_str()));
        }
    }
    // Unknown section/key: silently ignored, same "forward compatible"
    // behavior as ConfigStore -- a config file with extra keys for a
    // newer version of this code shouldn't fail to parse on an older
    // binary.
}

bool HalConfig::load_file(const std::string & path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return false;
    }

    std::string section;
    std::string line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') {
            continue;
        }
        if (t.front() == '[' && t.back() == ']') {
            section = t.substr(1, t.size() - 2);
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            continue;  // malformed line, skip rather than fail the whole file
        }
        apply_line(section, trim(t.substr(0, eq)), trim(t.substr(eq + 1)));
    }
    return true;
}

HalConfig & hal_config() {
    static HalConfig config;
    return config;
}

}  // namespace core
