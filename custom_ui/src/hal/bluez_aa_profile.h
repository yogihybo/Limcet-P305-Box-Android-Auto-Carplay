#pragma once

#include <cstdint>
#include <string>

namespace hal {

// BlueZ D-Bus server for the Android Auto wireless RFCOMM profile --
// SDP/profile advertisement for the well-known Android Auto Wireless
// service UUID (4de17a00-52cb-11e6-bdf4-0800200c9a66, confirmed real --
// see custom_ui/docs/BLUETOOTH_RECONNECT_HANDOFF.md and hal/bluetooth.h)
// via org.bluez.ProfileManager1.RegisterProfile(), plus the blocking
// wait for BlueZ to hand back a connected RFCOMM socket fd once a phone
// actually dials in.
//
// 2026-08-20: relocated here from custom_ui/src/androidauto/
// (androidauto::BluezClient) -- moving ALL Bluetooth connectivity into
// custom_ui, alongside hal/bluetooth.cpp's adapter/pairing/A2DP
// ownership, instead of splitting it across two processes (custom_ui
// running bt-agent's Agent1+A2DP, androidauto-sidecar independently
// running its own separate BlueZ connection just for this one profile).
// See hal/bluetooth.cpp's aa_profile_server_loop() for the caller: it
// registers this profile once at boot and hands each connected fd to
// androidauto-sidecar over their existing local socket (see
// hal/androidauto_client.h's sendConnectFd()) via SCM_RIGHTS, rather
// than the sidecar owning any BlueZ/D-Bus knowledge itself.
//
// register_agent()/the Agent1 registration this class used to also own
// is GONE -- bt-agent (spawned by hal::bluetooth::ensure_bluetooth_daemon_running(),
// same process now) already registers the one NoInputNoOutput default
// agent this device needs; having a second agent re-register itself
// here on top of that was pure redundant D-Bus churn (found and
// removed from the old sidecar-owned call site earlier this session,
// this move finishes the job by not bringing it back at all).
//
// Deliberately raw libdbus, not GDBus/sd-bus -- this project doesn't
// link glib anywhere, and sd-bus isn't available on this device's
// glibc/musl-less toolchain. See tools/bluetoothd-test/README.md for
// why libdbus itself had to be cross-compiled from source (no armhf
// apt/multiarch on the build machine) -- same vendored dbus, staged at
// ~/build-deps/dbus-arm-install, now linked into custom_ui itself
// (Makefile's UI_TARGET) rather than only androidauto-sidecar.
//
// Synchronous/blocking by design -- runs on its own dedicated
// background thread (hal::bluetooth's aa_profile_server_loop()), not
// the LVGL main loop.
class BluezAaProfile {
public:
    BluezAaProfile();
    ~BluezAaProfile();

    BluezAaProfile(const BluezAaProfile &) = delete;
    BluezAaProfile & operator=(const BluezAaProfile &) = delete;

    // Connects to the system bus (DBUS_SYSTEM_BUS_ADDRESS if set,
    // otherwise libdbus's own compiled-in default -- see
    // tools/bluetoothd-test/bt-daemon-probe.sh's own comment for why
    // this device's real dbus-daemon resolves to the doubled
    // /var/run/run/dbus/system_bus_socket path; this project's own
    // bluetoothd/dbus-daemon pair, brought up by
    // hal::ensure_bluetooth_daemon_running(), uses that same real
    // path). Returns false on failure (logs why).
    bool connect();

    // Registers a profile for the Android Auto Wireless service UUID
    // via org.bluez.ProfileManager1.RegisterProfile() -- BlueZ
    // allocates the RFCOMM channel itself and builds/advertises the
    // real SDP record (this is the standard, correct way a custom
    // RFCOMM profile is discoverable; not the same mechanism
    // accept_rfcomm_connection()'s own manual bind()/listen() assumed,
    // which is why that function is superseded, not extended, by this
    // class -- see bluetooth_rfcomm_server.h's own "KNOWN GAP" comment
    // this closes). Returns false on any D-Bus failure.
    bool register_profile();

    // Blocks (bounded by timeoutSeconds) pumping the D-Bus connection
    // until bluetoothd calls our Profile1.NewConnection() -- i.e. a
    // phone actually connected to the registered channel -- and
    // returns the connected RFCOMM socket fd BlueZ hands us (via
    // D-Bus's own UNIX_FD passing). Caller owns the fd afterward (same
    // ownership contract as accept_rfcomm_connection() it replaces).
    // Returns -1 on timeout or any D-Bus failure -- NOT necessarily an
    // error worth logging loudly, since this is called in a loop by
    // aa_profile_server_loop() and a timeout just means "no phone
    // dialed in yet, keep waiting."
    int wait_for_connection(int timeoutSeconds);

    void close();

    // Public only so bluez_aa_profile.cpp's free-function D-Bus message
    // handler (profile_message_handler -- a plain C-style callback,
    // can't be a private member since libdbus takes a function
    // pointer) can name and use this incomplete type as its user_data;
    // the real definition still lives entirely in the .cpp, so this
    // reveals nothing to actual callers of this class.
    struct Impl;

private:
    Impl * impl_;
};

}  // namespace hal
