// Settings config store -- the live settings layer. On-disk format
// (flat [Section]/key=value) originally cross-checked against a real
// captured stock userdata dump (firmware_source/mtd7_userdata/msncfg/
// Setting.config) for compatibility during early development:
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
// 2026-08-12 REVISED (three times now): this used to also read the
// stock MSN ini seed files (`/msnprofile/FactoryConfig.ini` /
// `MsnProductInfo.ini`) as a first-boot fallback, then -- once that
// was removed -- still lived at the stock app's own live path
// (`/data/msncfg/Setting.config`), reading/writing the SAME file the
// original MsnCoreApp binary owns. Per explicit request, this app now
// has NO link to msncfg at all, in either direction: its live layer
// lives at its own path (see default_store() below), entirely separate
// from anything stock. custom_ui ships its OWN bundled default-seed
// file (tracked source: custom_ui/etc/default_settings.conf, same
// simple flat [Section]/key=value format -- see hal_config.h's
// hal.conf for the established sibling pattern this follows: staged
// into build/ by the Makefile, resolved at runtime via
// resolve_default_seed_path()'s exe-dir -> /data -> /etc/custom_ui
// search order, first match wins).
//
// 2026-08-15: dropped the /data/custom_ui/ subfolder -- per explicit
// request, a flat file directly under /data (the app's own writable
// userdata mount) is enough, no dedicated subdirectory needed. Also
// removes the whole class of "a path component exists but isn't a
// directory" failure mkdir_parents() had to defend against for the
// subfolder case (see that function's own comment).
//
// Load order:
//   1. This app's own live layer (default_store()'s live_path) --
//      wins over the bundled seed on every boot once anything has
//      ever been saved.
//   2. custom_ui's bundled default_settings.conf -- consulted only for
//      keys NOT already present in the live layer (first-boot /
//      not-yet-provisioned case).
//
// load() promotes every resolved key -- whether it came from the live
// layer or the bundled seed -- into the live layer in memory, and
// default_store() saves once right after loading. The effect: the
// live layer becomes the sole, self-contained source of truth for
// this app from the moment it first runs, not just for keys the user
// happens to touch, and the bundled seed file is never consulted again
// after that one bootstrap pass. Never writes the bundled seed itself
// -- that's the shipped default, not something this app mutates.
//
// Sections: the stock dump this format was cross-checked against only
// had [General], but the bundled seed also carries [BlueTooth]
// (DeviceName etc, per SETTINGS_REFERENCE.md) -- some SKUs place
// General-section keys before any header at all, so keys are stored
// flat as "Section/Key" (default section "General" for
// headerless/unsectioned lines) to parse either layout the same way.
#pragma once

#include <map>
#include <string>

namespace core {

class ConfigStore {
public:
    // live_path is the userdata layer this store reads AND writes.
    ConfigStore(std::string live_path);

    // Loads the live layer, then the bundled default seed (see class
    // comment for precedence and search order), then promotes every
    // resolved key into the live layer in memory so a subsequent
    // save() makes the live file fully self-contained. Safe to call if
    // either/both are missing (e.g. running this UI on a dev host) --
    // missing files are silently skipped, get_*() falls back to
    // caller-supplied defaults in that case.
    void load();

    // Writes the live layer only (never the bundled seed) back to
    // live_path, creating its parent directory if it doesn't exist yet
    // (a not-yet-provisioned device won't have it). Returns false on
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

    // exe-dir/default_settings.conf -> /data/default_settings.conf
    // -> /etc/custom_ui/default_settings.conf, first match wins -- same
    // search order and reasoning as hal_config.cpp's HalConfig::HalConfig().
    static std::string resolve_default_seed_path();

    std::string live_path_;
    std::map<std::string, Entry> values_;
};

// Process-wide store against this app's OWN live path,
// /data/settings.conf -- deliberately separate from stock's
// /data/msncfg/Setting.config, see this file's top comment. Lazily
// load()ed on first call, and saved once immediately after -- see
// load()'s comment: this makes the live file self-contained from this
// app's very first run, decoupled from the bundled default seed from
// then on. Settings/Bluetooth screens should use this rather than
// constructing their own ConfigStore, so every screen sees (and edits)
// the same in-memory state.
ConfigStore & default_store();

}  // namespace core
