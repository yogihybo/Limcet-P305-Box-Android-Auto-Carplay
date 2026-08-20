# bt-agent (Static ARM BlueZ Auto-Pairing Daemon)

Dedicated, standalone, fully statically-linked BlueZ 5.66 Agent daemon with `NoInputNoOutput` capability.

## Purpose

Automotive head units require seamless Secure Simple Pairing (SSP) with phones without interactive PIN/passkey entry prompts. 

When a phone discovers and pairs with `Prado CustomUI`:
1. `bluetoothd` calls `RequestConfirmation` / `RequestAuthorization` / `AuthorizeService` on the registered D-Bus Agent.
2. `bt-agent` receives the D-Bus method call on `/org/bluez/agent` and **immediately auto-approves** the pairing handshake.
3. The phone's Bluetooth pairing dialog completes instantaneously with 100% success (no "incorrect PIN or passkey" error).

## Source Code

- `src/main.c` — Connects to the system D-Bus, registers the `/org/bluez/agent` object implementing the `org.bluez.Agent1` interface, calls `RegisterAgent` and `RequestDefaultAgent`, and runs the non-blocking dispatch loop.

## Key Changes & Settings

1. **Auto-Detects Double-Run D-Bus Socket**: Automatically detects `/var/run/run/dbus/system_bus_socket` or `/var/run/dbus/system_bus_socket`.
2. **NSS / dlopen Stub Wrapping**: Linked against `tools/nss-stub/nss_stub.o` with linker wrapping to prevent static glibc 2.34+ assertion failures.
3. **No Interactive Dependencies**: Runs without readline or terminal stdin requirements.

## Building

```sh
make
```
