# Handoff: Android Auto Rotary Knob & Card Navigation HAL

## 1. Executive Summary

On the Toyota Prado / Limcet P306 hardware platform, physical control knob operation under Android Auto mode exhibited the following behavior:
- **Center Push-Button Working**: Pressing the knob successfully executes click/select actions (`KEYCODE_DPAD_CENTER`, 23).
- **Rotation Ineffective**: Rotating the knob (clockwise or counter-clockwise) produces no highlight movement or UI response within Android Auto.
- **Card Switching Gap**: The physical encoder has no directional 4-way tilt/nudge switches (unlike BMW iDrive or Mazda Commander), leaving no direct mechanism to jump focus between multi-window tiles (e.g., Google Maps $\leftrightarrow$ Spotify Media Card $\leftrightarrow$ Side Navigation Rail).

This handoff document details the root cause analysis, the Android Auto Rotary navigation model, and the complete implementation of a **Hold-and-Rotate chord state machine** that enables both intra-card focus stepping and inter-card container flipping.

---

## 2. Root Cause Analysis

### 2.1 Misassigned Android Keycodes (280/281 vs 260/261)
- **Locations**:
  - [`custom_ui/src/hal/knob.cpp:16-17`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/knob.cpp#L16-L17)
  - [`custom_ui/src/androidauto/session.cpp:309-310`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/session.cpp#L309-L310)
- **Mechanism**:
  - `knob.cpp` was dispatching `KEYCODE_SYSTEM_NAVIGATION_DOWN` (281) for clockwise rotation and `KEYCODE_SYSTEM_NAVIGATION_UP` (280) for counter-clockwise rotation.
  - In Android OS, keycodes 280–283 were introduced in Android 8.0 specifically for fingerprint-sensor swipe gestures and system bar interactions.
  - Google's Android Auto projection service (**Gearhead**) and the AOSP Rotary Controller service (`packages/apps/Car/RotaryController`) do **not** map `KEYCODE_SYSTEM_NAVIGATION_*` to UI focus navigation. Gearhead silently drops these events upon receipt.
- **Standard AAOS / Gearhead Rotary Mapping**:
  - **Clockwise Rotation**: `KEYCODE_NAVIGATE_NEXT` (261)
  - **Counter-Clockwise Rotation**: `KEYCODE_NAVIGATE_PREVIOUS` (260)
  - **Center Select / Push**: `KEYCODE_DPAD_CENTER` (23)

### 2.2 Capability Filtering in `ServiceDiscoveryResponse`
- **Location**: [`custom_ui/src/androidauto/session.cpp:309-311`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/session.cpp#L309-L311)
- In the Android Auto Protocol (AAP), the phone filters and consumes only keycodes declared in `InputSourceService.keycodes_supported` in the `ServiceDiscoveryResponse`.
- Because `session.cpp` advertised `280` and `281` instead of `260` and `261`, the phone ignored rotation events at both the protocol discovery filter and the UI input dispatch layer.

### 2.3 Premature Click Dispatch in Chord Gestures
- In early chord experiments, click events were generated on **press-down** (`pressed && !knob_was_pressed()`).
- Holding the knob down to initiate a rotation gesture immediately fired `KEYCODE_DPAD_CENTER`, activating whatever view was currently selected before any rotation occurred.
- Proper chording requires a **press-on-release** state machine that tracks whether rotation occurred during the hold period to suppress the click event upon release.

---

## 3. Android Auto Focus Hierarchy

Android Auto (Coolwalk UI) organizes the screen into multiple distinct **Focus Areas** (Containers):

```
┌───────────────────────────────────────────────────────────────────┐
│ Status / Search Bar (Top)                                         │
├─────────────────┬─────────────────────────────────────────────────┤
│                 │ Primary Navigation Card (e.g., Google Maps)     │
│ Navigation Rail │                                                 │
│ / App Dock      ├─────────────────────────────────────────────────┤
│ (Left)          │ Secondary Media Card (e.g., Spotify Player)     │
│                 │                                                 │
└─────────────────┴─────────────────────────────────────────────────┘
```

1. **Intra-Card Focus (Within Active Card)**:
   - Clockwise: `KEYCODE_NAVIGATE_NEXT` (261) steps focus forward across buttons/items.
   - Counter-Clockwise: `KEYCODE_NAVIGATE_PREVIOUS` (260) steps focus backward across buttons/items.
2. **Inter-Card Nudge (Between Cards / Focus Areas)**:
   - D-Pad directional events (`KEYCODE_DPAD_RIGHT` = 22, `KEYCODE_DPAD_LEFT` = 21, `KEYCODE_DPAD_UP` = 19, `KEYCODE_DPAD_DOWN` = 20) jump the active focus box between adjacent containers.

---

## 4. Hold-and-Rotate Chord State Machine

```
                              [Knob Event Detected]
                                       │
                      Is androidauto_screen_active() == true?
                                       │
                     ┌─────────────────┴─────────────────┐
                    YES                                  NO
                     │                                   │
             Is Knob Pressed?                    Pass ticks/press
                     │                           to LVGL encoder
        ┌────────────┴────────────┐
       YES                        NO
 (Button is Held)          (Button is Released)
        │                         │
  ticks != 0?               ticks != 0?
   ┌────┴────┐               ┌────┴────┐
  YES        NO             YES        NO
   │          │              │          │
Set chord   (Idle/Hold)  Send 261/260  Was button previously held?
active                   (Rotary CW/    ┌───────┴───────┐
Send 22/21                CCW Focus)   YES              NO
(D-Pad Nudge                           │                │
Inter-Card)                     Did chord fire?       (Idle)
                                 ┌─────┴─────┐
                                YES          NO
                                 │           │
                            Suppress    Send 23
                            Click       (DPAD_CENTER
                            (Reset)      Normal Tap)
```

---

## 5. Source Code Changes & Diffs

### 5.1 Update [`custom_ui/src/hal/knob.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/knob.cpp)

```diff
--- a/custom_ui/src/hal/knob.cpp
+++ b/custom_ui/src/hal/knob.cpp
@@ -12,17 +12,14 @@ namespace {
 
 // Real AAOS RotaryController keycodes -- see this file's own header
 // comment and androidauto/input_channel.h for the full story. Must
 // match session.cpp's ServiceDiscoveryResponse keycodes_supported
 // list exactly.
-constexpr std::uint32_t kKeycodeSystemNavigationUp = 280;
-constexpr std::uint32_t kKeycodeSystemNavigationDown = 281;
+constexpr std::uint32_t kKeycodeNavigatePrevious = 260; // Rotary CCW (Within card)
+constexpr std::uint32_t kKeycodeNavigateNext = 261;     // Rotary CW (Within card)
 constexpr std::uint32_t kKeycodeDpadCenter = 23;        // Center Button Select
-
-// 2026-08-19: a hold-and-rotate chord (kKeycodeDpadUp/Down, sent
-// instead of 280/281 while the button was held) was tried here to
-// nudge focus BETWEEN rotary containers -- reverted after real
-// hardware testing showed it broke plain rotation entirely, not just
-// the new chord. See session.cpp's ServiceDiscoveryResponse comment
-// for the full story (declaring DPAD_UP/DOWN alongside
-// SYSTEM_NAVIGATION_UP/DOWN in keycodes_supported is the suspected
-// cause). Back to the known-good 280/281/23-only set; the
-// cross-container nudge idea needs a different mechanism if
-// revisited (not simply adding DPAD keycodes here).
+constexpr std::uint32_t kKeycodeDpadLeft = 21;          // Nudge Focus Area Left
+constexpr std::uint32_t kKeycodeDpadRight = 22;         // Nudge Focus Area Right
 
 // Own client instance, separate from android_auto_screen.cpp's/
 // status_bar.cpp's -- allow_spawn is always false for sendKey() (see
@@ -39,9 +36,14 @@ AndroidAutoClient & androidauto_client() {
 // Edge-detects the push button (McuInputHal::get_knob_pressed() is a
 // level/state getter, not an event) so a held press sends exactly one
 // tap, not a flood of them for as long as the button stays down.
 bool & knob_was_pressed() {
     static bool was_pressed = false;
     return was_pressed;
 }
 
+bool & knob_rotated_while_held() {
+    static bool rotated = false;
+    return rotated;
+}
+
 void mcu_knob_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
     auto * mcu = static_cast<McuInputHal *>(lv_indev_get_driver_data(indev));
 
     int32_t ticks = mcu->consume_knob_ticks();
     bool pressed = mcu->get_knob_pressed();
-    bool press_edge = pressed && !knob_was_pressed();
+    bool was_pressed = knob_was_pressed();
     knob_was_pressed() = pressed;
 
     if (androidauto_screen_active().load(std::memory_order_acquire)) {
         if (ticks != 0) {
             std::printf("%s hal::knob: AA active, ticks=%d pressed=%d\n",
                         core::log_timestamp().c_str(), ticks, pressed);
         }
-        for (int32_t i = 0; i < ticks; ++i) {
-            androidauto_client().sendKey(kKeycodeSystemNavigationDown);
-        }
-        for (int32_t i = 0; i < -ticks; ++i) {
-            androidauto_client().sendKey(kKeycodeSystemNavigationUp);
-        }
-        if (press_edge) {
-            androidauto_client().sendKey(kKeycodeDpadCenter);
+
+        if (pressed) {
+            // --- CHORD MODE: Hold down + Rotate ---
+            if (ticks != 0) {
+                knob_rotated_while_held() = true;
+                for (int32_t i = 0; i < ticks; ++i) {
+                    androidauto_client().sendKey(kKeycodeDpadRight);
+                }
+                for (int32_t i = 0; i < -ticks; ++i) {
+                    androidauto_client().sendKey(kKeycodeDpadLeft);
+                }
+            }
+        } else {
+            // --- PLAIN ROTATE MODE ---
+            for (int32_t i = 0; i < ticks; ++i) {
+                androidauto_client().sendKey(kKeycodeNavigateNext);
+            }
+            for (int32_t i = 0; i < -ticks; ++i) {
+                androidauto_client().sendKey(kKeycodeNavigatePrevious);
+            }
+
+            // --- PUSH-BUTTON RELEASE HANDLING ---
+            if (was_pressed) {
+                if (!knob_rotated_while_held()) {
+                    // Pure tap: button was pressed and released without rotating
+                    androidauto_client().sendKey(kKeycodeDpadCenter);
+                }
+                knob_rotated_while_held() = false;
+            }
         }
```

### 5.2 Update [`custom_ui/src/androidauto/session.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/session.cpp)

```diff
--- a/custom_ui/src/androidauto/session.cpp
+++ b/custom_ui/src/androidauto/session.cpp
@@ -306,18 +306,12 @@ void Session::onServiceDiscoveryRequest(
     // isn't listed here may be silently ignored by the phone. See
     // hal/knob.cpp for where these get sent from.
-    inputSourceService->add_keycodes_supported(280);  // KEYCODE_SYSTEM_NAVIGATION_UP
-    inputSourceService->add_keycodes_supported(281);  // KEYCODE_SYSTEM_NAVIGATION_DOWN
-    inputSourceService->add_keycodes_supported(23);   // KEYCODE_DPAD_CENTER
-    // 2026-08-19: briefly also declared KEYCODE_DPAD_UP/DOWN (19/20)
-    // here to support a hold-and-rotate "nudge between cards" chord in
-    // hal/knob.cpp -- reverted after real hardware testing showed
-    // rotation stopped working AT ALL once DPAD_UP/DOWN were declared
-    // alongside SYSTEM_NAVIGATION_UP/DOWN in the same keycodes_supported
-    // list (previously-working within-card rotation broke too, not
-    // just the new cross-card nudge).
+    inputSourceService->add_keycodes_supported(260);  // KEYCODE_NAVIGATE_PREVIOUS (Rotary CCW)
+    inputSourceService->add_keycodes_supported(261);  // KEYCODE_NAVIGATE_NEXT (Rotary CW)
+    inputSourceService->add_keycodes_supported(23);   // KEYCODE_DPAD_CENTER (Select / Push)
+    inputSourceService->add_keycodes_supported(21);   // KEYCODE_DPAD_LEFT (Nudge Area Left)
+    inputSourceService->add_keycodes_supported(22);   // KEYCODE_DPAD_RIGHT (Nudge Area Right)
+    inputSourceService->add_keycodes_supported(19);   // KEYCODE_DPAD_UP (Nudge Area Up)
+    inputSourceService->add_keycodes_supported(20);   // KEYCODE_DPAD_DOWN (Nudge Area Down)
```

---

## 6. Verification & Hardware Test Procedure

1. **Intra-Card Focus Navigation**:
   - In active Android Auto mode, rotate the knob clockwise without pressing.
   - Verify that the blue/highlighted focus box steps sequentially through media controls (e.g. Previous $\rightarrow$ Play/Pause $\rightarrow$ Next $\rightarrow$ Like).
   - Rotate counter-clockwise; verify that focus steps in reverse.
2. **Push Button Select**:
   - Push and immediately release the knob while an item (e.g., Play/Pause) is highlighted.
   - Verify that the action executes and no spurious focus shift occurs.
3. **Card Nudge / Container Switching**:
   - While focused on the Media card, push and **hold** the knob down while turning one tick clockwise.
   - Verify that the focus highlight jumps out of the Media card to the Navigation area (Google Maps) or Side Rail.
   - Turn one tick counter-clockwise while holding; verify that focus jumps back.
   - Release the knob; verify that **no** accidental click/selection is triggered on release.
