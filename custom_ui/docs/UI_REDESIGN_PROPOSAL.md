# LVGL UI: Material 3 & Android Auto Coolwalk Design Specification

## 1. Executive Summary

This design specification provides a unified, production-grade overhaul of the `custom_ui` interface for the **ARK1668 800x480** automotive head unit.

The interface standardizes on a **persistent 5-icon left navigation rail** across every screen, combining **Google Material 3 (M3)** tonal depth, **Android Auto (Coolwalk)** card layouts, and discrete tactile controls without feature creep.

---

## 2. Unified Navigation Rail Architecture

Every screen shares the **identical left navigation dock (68px wide)** with 5 consistent destinations (top to bottom):

```
┌──────┬──────────────────────────────────────────────────────────────┐
│ [⌂]  │  1. Home Dashboard                                           │
│ [A]  │  2. Android Auto Projection                                  │
│ [ᛒ]  │  3. Bluetooth Device Manager                                 │
│ [📷] │  4. Reverse Camera Live Preview                              │
│ [⚙]  │  5. Settings (Display, Audio, System)                        │
└──────┴──────────────────────────────────────────────────────────────┘
```

The active tab is indicated by an elevated glowing Material stadium badge (`#8ab4f8`).

---

## 3. Screen Mockups

### 3.1 Home Dashboard

* **Left Navigation Rail**: 5 uniform icons with **Home** active.
* **Card 1 (Android Auto)**: Projection status (*"Ready to pair"* / *"Connected"*) + single-tap **Quick Connect** stadium button.
* **Card 2 (Audio Volume)**: Master ALSA `softvol` volume arc gauge + media play/pause controls.
* **Top Status Bar**: Compact header with time (`10:09 AM`).

---

### 3.2 Settings Screen

* **Left Navigation Rail**: Exact same 5 uniform icons with **Settings** active.
* **Top Segmented Chips**: `[ Display ]` (Active), `[ Audio ]`, `[ System ]`.
* **Setting Rows**:
  * **Brightness**: Horizontal progress level bar with circular `-` and `+` steppers.
  * **Contrast**: `-` / `+` steppers with live level bar.
  * **Saturation**: `-` / `+` steppers with live level bar.

---

## 4. Visual Tokens & Theme Variables

| Token | Hex Value | Role |
| :--- | :--- | :--- |
| `theme::bg()` | `0x111318` | Dark canvas background |
| `theme::surface()` | `0x1c2024` | Card & container fill |
| `theme::accent()` | `0x8ab4f8` | Primary Google Blue (active tabs, focus rings) |
| `theme::accent_secondary()` | `0x78d9ec` | Soft Cyan/Teal (progress bars, gauges) |
| `theme::text_primary()` | `0xe2e2e9` | High-contrast body text |
| `theme::text_secondary()` | `0x90909a` | Muted labels |

---

## 5. File References

The screen mockup images this doc originally embedded were host-machine-local
paths (never portable in the repo) and have since been removed; this document
is kept for its written spec/token reference only.
