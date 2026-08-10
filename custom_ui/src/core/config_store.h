// Settings config store -- mirrors stock's real two-layer model
// documented in docs/SETTINGS_REFERENCE.md and cross-checked against
// a real captured userdata dump
// (firmware_source/mtd7_userdata/msncfg/Setting.config):
//
//   [General]
//   Brightness=128
//   Contrast=128
//   Saturation=64
//   Mute=false
//   Volume=32
//   AutoStartCarLink=true
//   Language=4096
//
// Load order (see project_language_setting_userdata memory +
// docs/ARCHITECTURE.md "Settings / config files"):
//   1. `/data/msncfg/Setting.config` -- the LIVE layer. Once a device
//      has provisioned, this wins over the ini files on every
//      subsequent boot.
//   2. `/msnprofile/FactoryConfig.ini` and `/msnprofile/MsnProductInfo.ini`
//      -- one-time factory seed, only consulted for keys NOT already
//      present in the live layer (first-boot / not-yet-provisioned
//      case).
//
// This store therefore loads all three files, live layer first so its
// keys win, ini files filling gaps -- then save() writes ONLY the live
// layer back out (per Phase 3's checklist: "read/write the LIVE
// settings layer ... not just the static .ini factory defaults").
// Never writes the ini files -- those are the factory-owned seed, not
// something this app should mutate.
//
// Sections: `Setting.config` observed so far only has [General], but
// FactoryConfig.ini has [General]/[BlueTooth]/[Sound]/[Radio], and per
// SETTINGS_REFERENCE.md some SKUs place General-section keys before
// any header at all. Keys are stored flat as "Section/Key" (default
// section "General" for headerless/unsectioned lines) so every real
// on-disk layout parses the same way.
#pragma once

#include <map>
#include <string>

namespace core {

class ConfigStore {
public:
    // live_path is the userdata layer this store reads AND writes.
    // The two ini paths are read-only factory seeds, only consulted
    // for keys not already found in live_path.
    ConfigStore(std::string live_path,
                std::string factory_config_ini_path,
                std::string product_info_ini_path);

    // Loads all three files (see class comment for precedence). Safe
    // to call if any/all paths are missing (e.g. running this UI on a
    // dev host, or a not-yet-provisioned device) -- missing files are
    // silently skipped, get_*() falls back to caller-supplied
    // defaults in that case.
    void load();

    // Writes the live layer only (never the ini seeds) back to
    // live_path, creating /data/msncfg/ if it doesn't exist yet (a
    // not-yet-provisioned device won't have it). Returns false on
    // write failure (logs the reason) -- callers should treat this as
    // non-fatal, same as every other optional-hardware pattern in
    // this codebase, since a dev host build has nowhere real to write.
    bool save();

    // section defaults to "General" -- that covers every key in
    // SETTINGS_REFERENCE.md's tables except the handful explicitly
    // marked [BlueTooth]/[Sound]/[Radio].
    int get_int(const std::string & key, int default_value,
                const std::string & section = "General") const;
    bool get_bool(const std::string & key, bool default_value,
                  const std::string & section = "General") const;
    std::string get_string(const std::string & key, const std::string & default_value,
                            const std::string & section = "General") const;

    // Sets a key in the in-memory live layer. Does NOT write to disk
    // -- call save() explicitly (e.g. once per settings screen "back"
    // action, not on every slider-drag tick).
    void set_int(const std::string & key, int value, const std::string & section = "General");
    void set_bool(const std::string & key, bool value, const std::string & section = "General");
    void set_string(const std::string & key, const std::string & value,
                     const std::string & section = "General");

private:
    struct Entry {
        std::string value;
        bool from_live;  // true if this key came from (or was set into) the live layer
    };

    static std::string make_map_key(const std::string & section, const std::string & key);
    void parse_file(const std::string & path, bool is_live_layer);

    std::string live_path_;
    std::string factory_config_ini_path_;
    std::string product_info_ini_path_;
    std::map<std::string, Entry> values_;
};

// Process-wide store against this device's real, confirmed paths
// (docs/SETTINGS_REFERENCE.md):
//   live:    /data/msncfg/Setting.config
//   factory: /msnprofile/FactoryConfig.ini, /msnprofile/MsnProductInfo.ini
// Lazily load()ed on first call. Settings/Bluetooth screens should use
// this rather than constructing their own ConfigStore, so every screen
// sees (and edits) the same in-memory state.
ConfigStore & default_store();

}  // namespace core
