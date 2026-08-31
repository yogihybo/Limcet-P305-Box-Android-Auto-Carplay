#pragma once

namespace hal {

// 2026-09-01: runtime toggle for the dyn rootfs's SSH daemon, driven by
// the "SSH Access" switch on the Settings screen. Real motivation: this
// rootfs's root account has an intentionally empty password (see
// firmware_overlay_dyn/etc/ssh/sshd_config's PermitEmptyPasswords yes) so
// a plain `ssh root@<device-ip>` works with zero prompts on the private
// carplay_wifi network -- genuinely convenient, but that also means
// sshd being reachable at all is a real (if low-stakes, private-network-
// only) exposure. Rather than have it always running from boot (the old
// rcS behavior), rcS no longer starts it -- this HAL owns start/stop,
// gated by the persisted "SshAccess" setting, checked once at app
// startup (main.cpp) and live-toggled from the Settings switch.

// Starts (`enabled=true`) or stops (`enabled=false`) sshd. Idempotent --
// safe to call repeatedly with the same value (checks `pidof` first,
// same pattern as hal::ensure_bluetooth_daemon_running()).
void set_ssh_enabled(bool enabled);

}  // namespace hal
