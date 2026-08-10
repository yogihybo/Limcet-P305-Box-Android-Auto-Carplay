// Bluetooth pairing/device menu -- Phase 3 (docs/IMPLEMENTATION_PLAN.md),
// Basic tier. Backed by hal/bluetooth.h (the real `/dev/bw_serial`
// AT-command channel Feasycom's `blueware` daemon exposes -- see that
// header's top comment for the full protocol writeup and its known
// gaps, e.g. no confirmed "forget device" command). Deliberately does
// NOT use src/androidauto/bw_aap_client.h -- that's a separate,
// narrower channel (`/dev/bw_aap`) just for Android-Auto-Wireless
// WiFi-credential handoff, not general BT device management.
#pragma once

#include "lvgl.h"

namespace ui {

// Matches core::ScreenManager::ScreenFactory.
lv_obj_t * create_bluetooth_screen();

}  // namespace ui
