#include "core/config_store.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <sys/stat.h>

namespace core {

namespace {

std::string trim(const std::string & s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Recursively creates the parent directory chain for `path` (e.g.
// /data/msncfg/Setting.config -> mkdir /data, mkdir /data/msncfg).
// A not-yet-provisioned device may not have /data/msncfg/ yet.
void mkdir_parents(const std::string & path) {
    size_t pos = path.find('/', 1);
    while (pos != std::string::npos) {
        std::string dir = path.substr(0, pos);
        mkdir(dir.c_str(), 0755);  // ignore errors -- may already exist
        pos = path.find('/', pos + 1);
    }
}

}  // namespace

ConfigStore::ConfigStore(std::string live_path, std::string factory_config_ini_path,
                          std::string product_info_ini_path)
    : live_path_(std::move(live_path)),
      factory_config_ini_path_(std::move(factory_config_ini_path)),
      product_info_ini_path_(std::move(product_info_ini_path)) {}

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
        // ini-seed entries only fill gaps the live layer hasn't
        // already claimed (first file loaded for a given key wins
        // among the ini seeds, live layer loaded first overall).
        auto it = values_.find(map_key);
        if (it != values_.end() && it->second.from_live) {
            continue;
        }
        if (it != values_.end() && !is_live_layer) {
            continue;  // an earlier ini already seeded this key
        }
        values_[map_key] = Entry{value, is_live_layer};
    }
}

void ConfigStore::load() {
    values_.clear();
    parse_file(live_path_, /*is_live_layer=*/true);
    parse_file(factory_config_ini_path_, /*is_live_layer=*/false);
    parse_file(product_info_ini_path_, /*is_live_layer=*/false);
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
    // matches the real device's own Setting.config, which per the
    // real dump only ever carries a handful of live-overridable keys,
    // not a full copy of every ini field.
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
    static ConfigStore store("/data/msncfg/Setting.config", "/msnprofile/FactoryConfig.ini",
                              "/msnprofile/MsnProductInfo.ini");
    static bool loaded = false;
    if (!loaded) {
        store.load();
        loaded = true;
    }
    return store;
}

}  // namespace core
