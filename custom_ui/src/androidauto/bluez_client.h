#pragma once

#include <cstdint>
#include <string>

namespace androidauto {

// Minimal BlueZ D-Bus client for the Android Auto wireless bootstrap --
// replaces blueware's /dev/bw_aap local proxy (see bw_aap_client.h's
// own header comment for why that existed) with a real kernel
// hci0/BlueZ stack. Owns: pairing (an org.bluez.Agent1 that auto-
// accepts, matching "pairing is phone-initiated" -- hal/bluetooth.h's
// own established finding, this project never drove pairing from the
// head unit side) and SDP/profile advertisement for the well-known
// Android Auto Wireless service UUID
// (4de17a00-52cb-11e6-bdf4-0800200c9a66, confirmed real -- see
// custom_ui/docs/BLUETOOTH_RECONNECT_HANDOFF.md and hal/bluetooth.h)
// via org.bluez.ProfileManager1.RegisterProfile().
//
// Deliberately raw libdbus, not GDBus/sd-bus -- this project doesn't
// link glib anywhere, and sd-bus isn't available on this device's
// glibc/musl-less toolchain. See tools/bluetoothd-test/README.md for
// why libdbus itself had to be cross-compiled from source (no armhf
// apt/multiarch on the build machine) -- same vendored dbus, now
// staged at ~/build-deps/dbus-arm-install for this target to link
// against too.
//
// Synchronous/blocking by design, same pattern as BwAapClient and
// accept_rfcomm_connection() -- this is a one-time bootstrap step on
// WirelessSessionManager's own dedicated thread, not part of the
// io_service event loop.
class BluezClient {
public:
    BluezClient();
    ~BluezClient();

    BluezClient(const BluezClient &) = delete;
    BluezClient & operator=(const BluezClient &) = delete;

    // Connects to the system bus (DBUS_SYSTEM_BUS_ADDRESS if set,
    // otherwise libdbus's own compiled-in default -- see
    // tools/bluetoothd-test/bt-daemon-probe.sh's own comment for why
    // this device's real dbus-daemon resolves to the doubled
    // /var/run/run/dbus/system_bus_socket path; this project's own
    // bluetoothd/dbus-daemon pair, brought up by bluez_stack.h, uses
    // that same real path). Returns false on failure (logs why).
    bool connect();

    // Registers a NoInputNoOutput (Just Works, auto-accept) Agent1 at
    // a fixed object path and calls RequestDefaultAgent so bluetoothd
    // routes every pairing request here -- no head-unit UI for PIN/
    // passkey entry exists or is planned; matches this project's own
    // established finding that AA pairing is always phone-initiated.
    // Must be called before a phone attempts to pair. Returns false on
    // any D-Bus failure.
    bool register_agent();

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
    // Returns -1 on timeout or any D-Bus failure.
    int wait_for_connection(int timeoutSeconds);

    void close();

    // Public only so bluez_client.cpp's free-function D-Bus message
    // handlers (agent_message_handler/profile_message_handler -- plain
    // C-style callbacks, can't be private members since libdbus takes
    // a function pointer) can name and use this incomplete type as
    // their user_data; the real definition still lives entirely in the
    // .cpp, so this reveals nothing to actual callers of this class.
    struct Impl;

private:
    Impl * impl_;
};

}  // namespace androidauto
