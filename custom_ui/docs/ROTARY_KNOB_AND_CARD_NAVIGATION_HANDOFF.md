# Handoff: Android Auto Rotary Knob & Card Navigation Investigation

## 1. Executive Summary

This document records the empirical hardware findings, protocol behavior, and systematic testing roadmap for physical rotary knob operation under Android Auto in `custom_ui`.

### 1.1 Live Hardware Observations & Historical Context
1. **Push-Button Select (`KEYCODE_DPAD_CENTER`, 23)**: Hardware-confirmed functional across all builds.
2. **Rotation within a Container (`280/281`)**: Prior hardware testing confirmed `280` (`KEYCODE_SYSTEM_NAVIGATION_UP`) and `281` (`KEYCODE_SYSTEM_NAVIGATION_DOWN`) **did produce real focus movement** across fields, but navigation was restricted strictly to the currently focused card/container with no way to jump focus to other cards.
3. **The Mixed-Keycode Regression**: A prior hardware test that declared raw D-Pad directions (`19, 20, 21, 22`) alongside rotary keycodes in `session.cpp`'s `keycodes_supported` caused Gearhead to **completely disable/break all rotary navigation**.
4. **Current Status in Recent Builds**: In the latest builds, rotation using `280/281` was reported as having no visible effect. This requires isolated re-verification to determine whether it is caused by active UI focus state, video corruption masking, or Gearhead version behavior.

---

## 2. Key Findings & Constraints

### 2.1 Hardware Capability vs. Multi-Card Navigation
- The physical encoder hardware relays clockwise (`b3=65`), counter-clockwise (`b3=64`), and push-button (`b3=13`) events over MCU UART `/dev/ttyHS0`. It lacks physical 4-way tilt/nudge switches.
- In Android Auto (Coolwalk UI), single-encoder rotation navigates focus within the active container (e.g., media playback controls). Navigating between containers (e.g., jumping from Media to Navigation/Maps or the Side Rail) normally requires an inter-container "nudge" event.

### 2.2 The Capability Advertisement Conflict in `session.cpp`
- Gearhead parses `keycodes_supported` in the `ServiceDiscoveryResponse` to configure its internal input state machine.
- Empirical testing showed that declaring D-Pad directional keycodes (`19-22`) alongside rotary keycodes corrupted Gearhead's rotary mode, breaking intra-card rotation entirely.
- **Rule**: Never bundle multi-key D-Pad expansions with rotary keycode declarations in a single unverified step. Any keycode set modifications must be tested in strict isolation.

### 2.3 Edge Detection & Chording Dynamics
- If a hold-and-rotate chord is used to provide container nudging:
  - The push button must trigger on **release**, not on press-down.
  - If triggered on press-down (`pressed && !knob_was_pressed()`), holding the knob down to initiate a rotation chord immediately executes an unwanted click/select action on the currently focused field.

---

## 3. Two-Stage Isolated Verification Strategy

To prevent repeating regressions and isolate the exact input behavior on real hardware, changes are split into two discrete test stages:

```
                  ┌──────────────────────────────────────────────┐
                  │ Stage A: Isolated 3-Keycode Test             │
                  │ keycodes_supported = [260, 261, 23]          │
                  │ Test rotary focus within active card         │
                  └──────────────────────┬───────────────────────┘
                                         │
                        Did intra-card rotation work?
                         ┌───────────────┴───────────────┐
                        YES                              NO
                         │                               │
        ┌────────────────┴──────────────┐       Revert to [280, 281, 23]
        │ Stage B: Card Nudge Mechanism │       and investigate initial
        │ Test isolated chord or Tab    │       focus acquisition/state
        │ without polluting main list   │
        └───────────────────────────────┘
```

---

## 4. Stage A: Isolated Rotary Test (260/261 vs 280/281)

**Objective**: Test whether `KEYCODE_NAVIGATE_PREVIOUS` (260) and `KEYCODE_NAVIGATE_NEXT` (261) function cleanly for intra-card focus when strictly declared as a 3-key set (`[260, 261, 23]`), without any D-Pad keycodes present.

### 4.1 Implementation Changes for Stage A

#### A. [`custom_ui/src/hal/knob.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/knob.cpp)
```diff
--- a/custom_ui/src/hal/knob.cpp
+++ b/custom_ui/src/hal/knob.cpp
@@ -13,8 +13,8 @@ namespace {
 // comment and androidauto/input_channel.h for the full story. Must
 // match session.cpp's ServiceDiscoveryResponse keycodes_supported
 // list exactly.
-constexpr std::uint32_t kKeycodeSystemNavigationUp = 280;
-constexpr std::uint32_t kKeycodeSystemNavigationDown = 281;
+constexpr std::uint32_t kKeycodeNavigatePrevious = 260; // Rotary CCW
+constexpr std::uint32_t kKeycodeNavigateNext = 261;     // Rotary CW
 constexpr std::uint32_t kKeycodeDpadCenter = 23;
 
@@ -58,9 +58,9 @@ void mcu_knob_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
         }
         for (int32_t i = 0; i < ticks; ++i) {
-            androidauto_client().sendKey(kKeycodeSystemNavigationDown);
+            androidauto_client().sendKey(kKeycodeNavigateNext);
         }
         for (int32_t i = 0; i < -ticks; ++i) {
-            androidauto_client().sendKey(kKeycodeSystemNavigationUp);
+            androidauto_client().sendKey(kKeycodeNavigatePrevious);
         }
         if (press_edge) {
             androidauto_client().sendKey(kKeycodeDpadCenter);
```

#### B. [`custom_ui/src/androidauto/session.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/session.cpp)
```diff
--- a/custom_ui/src/androidauto/session.cpp
+++ b/custom_ui/src/androidauto/session.cpp
@@ -306,8 +306,8 @@ void Session::onServiceDiscoveryRequest(
     // isn't listed here may be silently ignored by the phone. See
     // hal/knob.cpp for where these get sent from.
-    inputSourceService->add_keycodes_supported(280);  // KEYCODE_SYSTEM_NAVIGATION_UP
-    inputSourceService->add_keycodes_supported(281);  // KEYCODE_SYSTEM_NAVIGATION_DOWN
+    inputSourceService->add_keycodes_supported(260);  // KEYCODE_NAVIGATE_PREVIOUS (CCW)
+    inputSourceService->add_keycodes_supported(261);  // KEYCODE_NAVIGATE_NEXT (CW)
     inputSourceService->add_keycodes_supported(23);   // KEYCODE_DPAD_CENTER
```

---

## 5. Stage B: Inter-Card Navigation Roadmap (Pending Stage A)

Once Stage A validates stable intra-card rotation:
1. **Option 1: Hold-and-Rotate Chord with Single Direction Pair**:
   Test adding *only* horizontal nudge keycodes (`KEYCODE_DPAD_LEFT=21`, `KEYCODE_DPAD_RIGHT=22`) alongside the rotary pair, accompanied by the press-on-release state machine:
   - Plain turn: sends `260` / `261` (Intra-card focus).
   - Press held + turn: sends `21` / `22` (Card nudge).
   - Release without turn: sends `23` (Select).
2. **Option 2: Focus Traversal via Tab Keycode (`KEYCODE_TAB`, 61)**:
   If D-Pad keycodes conflict with rotary mode under Gearhead, evaluate `KEYCODE_TAB` / `KEYCODE_FORWARD` as a single universal focus progression key.

---

## 6. Hardware Verification Checklist

1. **Stage A Verification**:
   - Build and deploy custom_ui with only `[260, 261, 23]` declared.
   - Open Android Auto media card.
   - Rotate knob clockwise and counter-clockwise.
   - Verify whether highlight focus steps across media buttons.
   - Press knob to confirm center select action.
2. **Log Audit**:
   - Check `hal::knob: AA active, ticks=%d` in runtime console.
   - Confirm tick deltas correspond 1:1 with detents turned.
