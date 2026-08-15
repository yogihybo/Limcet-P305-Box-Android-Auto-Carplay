#include "core/config_store.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <sys/stat.h>
#include <unistd.h>

namespace core {

namespace {

std::string trim(const std::string & s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Recursively creates the parent directory chain for `path` (e.g.
// /data/custom_ui/settings.conf -> mkdir /data, mkdir /data/custom_ui).
// A not-yet-provisioned device may not have /data/custom_ui/ yet.
//
// 2026-08-15: self-heals the case where a path component already
// exists but is a plain FILE, not a directory -- observed on real
// hardware as save() failing with ENOTDIR ("Not a directory") on
// every single boot, permanently, with no recovery (mkdir()'s error
// was ignored outright, on the assumption the only failure mode was
// EEXIST-as-directory). Every component this function ever creates is
// exclusively this app's own path space (/data/custom_ui/...), never
// shared with stock or another process, so removing a stray file here
// and replacing it with the directory it was always supposed to be is
// safe.
void mkdir_parents(const std::string & path) {
    size_t pos = path.find('/', 1);
    while (pos != std::string::npos) {
        std::string dir = path.substr(0, pos);
        if (mkdir(dir.c_str(), 0755) != 0 && errno == EEXIST) {
            struct stat st{};
            if (stat(dir.c_str(), &st) == 0 && !S_ISDIR(st.st_mode)) {
                std::fprintf(stderr,
                             "core::ConfigStore: %s exists but isn't a directory -- removing and recreating\n",
                             dir.c_str());
                unlink(dir.c_str());
                mkdir(dir.c_str(), 0755);  // best-effort; save()'s own open() will report if this still failed
            }
        }
        pos = path.find('/', pos + 1);
    }
}

// Directory containing the running binary, via /proc/self/exe -- same
// technique as hal_config.cpp's executable_dir() (this class deliberately
// doesn't depend on core::hal_config() -- keeps this a self-contained
// settings-only module).
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

bool file_exists(const std::string & path) {
    std::ifstream f(path);
    return f.is_open();
}

}  // namespace

ConfigStore::ConfigStore(std::string live_path) : live_path_(std::move(live_path)) {}

std::string ConfigStore::make_map_key(const std::string & section, const std::string & key) {
    return section + "/" + key;
}

void ConfigStore::parse_file(const std::string & path, bool is_live_layer) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return;  // missing file is normal (see class comment), not an error
    }

    std::string section = "General";  // default per SETTINGS_REFERENCE.md's headerless-SKU note
    std::string line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') {
            continue;  // blank or comment (leading '#' = "fall back to firmware default")
        }
        if (t.front() == '[' && t.back() == ']') {
            section = t.substr(1, t.size() - 2);
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = trim(t.substr(0, eq));
        std::string value = trim(t.substr(eq + 1));
        std::string map_key = make_map_key(section, key);

        // Live layer entries always win and are always overwritable;
        // the bundled seed only fills gaps the live layer hasn't
        // already claimed.
        auto it = values_.find(map_key);
        if (it != values_.end() && it->second.from_live) {
            continue;
        }
        if (it != values_.end() && !is_live_layer) {
            continue;  // already seeded
        }
        values_[map_key] = Entry{value, is_live_layer};
    }
}

std::string ConfigStore::resolve_default_seed_path() {
    constexpr const char * kFilename = "default_settings.conf";
    std::string exe_dir = executable_dir();
    if (!exe_dir.empty() && file_exists(exe_dir + "/" + kFilename)) {
        return exe_dir + "/" + kFilename;
    }
    std::string data_path = std::string("/data/custom_ui/") + kFilename;
    if (file_exists(data_path)) {
        return data_path;
    }
    std::string etc_path = std::string("/etc/custom_ui/") + kFilename;
    if (file_exists(etc_path)) {
        return etc_path;
    }
    return "";  // none found -- load()'s parse_file() no-ops on an empty/missing path
}

void ConfigStore::load() {
    values_.clear();
    parse_file(live_path_, /*is_live_layer=*/true);
    parse_file(resolve_default_seed_path(), /*is_live_layer=*/false);

    // Decouple from the bundled seed file from here on -- promote
    // every key resolved above (whether it came from the live layer
    // or the seed) into the live layer in memory. See this class's
    // header comment: the next save() then makes live_path_ a full,
    // self-contained copy of the resolved config, not just the subset
    // the user has explicitly touched.
    for (auto & [map_key, entry] : values_) {
        entry.from_live = true;
    }
}

bool ConfigStore::save() {
    mkdir_parents(live_path_);
    std::ofstream f(live_path_, std::ios::trunc);
    if (!f.is_open()) {
        std::fprintf(stderr, "core::ConfigStore::save: failed to open %s (%s)\n",
                     live_path_.c_str(), std::strerror(errno));
        return false;
    }

    // Only ever writes keys that are (now) part of the live layer --
    // after load()'s promotion step (see its comment) that's every key
    // this app knows about, so this ends up as a full self-contained
    // copy, not just a handful of user-touched overrides.
    std::string current_section;
    for (auto & [map_key, entry] : values_) {
        if (!entry.from_live) continue;
        size_t slash = map_key.find('/');
        std::string section = map_key.substr(0, slash);
        std::string key = map_key.substr(slash + 1);
        if (section != current_section) {
            f << "[" << section << "]\n";
            current_section = section;
        }
        f << key << "=" << entry.value << "\n";
    }
    return true;
}

int ConfigStore::get_int(const std::string & key, int default_value,
                          const std::string & section) const {
    auto it = values_.find(make_map_key(section, key));
    if (it == values_.end()) return default_value;
    try {
        return std::stoi(it->second.value);
    } catch (...) {
        return default_value;
    }
}

bool ConfigStore::get_bool(const std::string & key, bool default_value,
                            const std::string & section) const {
    auto it = values_.find(make_map_key(section, key));
    if (it == values_.end()) return default_value;
    std::string v = it->second.value;
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    if (v == "true" || v == "1") return true;
    if (v == "false" || v == "0") return false;
    return default_value;
}

std::string ConfigStore::get_string(const std::string & key, const std::string & default_value,
                                     const std::string & section) const {
    auto it = values_.find(make_map_key(section, key));
    if (it == values_.end()) return default_value;
    return it->second.value;
}

void ConfigStore::set_int(const std::string & key, int value, const std::string & section) {
    values_[make_map_key(section, key)] = Entry{std::to_string(value), true};
}

void ConfigStore::set_bool(const std::string & key, bool value, const std::string & section) {
    values_[make_map_key(section, key)] = Entry{value ? "true" : "false", true};
}

void ConfigStore::set_string(const std::string & key, const std::string & value,
                              const std::string & section) {
    values_[make_map_key(section, key)] = Entry{value, true};
}

ConfigStore & default_store() {
    static ConfigStore store("/data/custom_ui/settings.conf");
    static bool loaded = false;
    if (!loaded) {
        store.load();
        // Persist immediately -- see load()'s comment: this makes the
        // live file self-contained from this app's very first run
        // rather than waiting for the user to touch a setting.
        // Best-effort/non-fatal, same as every other optional-write
        // pattern in this codebase (e.g. a dev host build has nowhere
        // real to write this).
        store.save();
        loaded = true;
    }
    return store;
}

}  // namespace core
