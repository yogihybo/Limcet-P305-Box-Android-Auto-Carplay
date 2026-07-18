# Holden identity reference (not shipped)

Holden's own device-identity files, kept here purely for reference —
NOT part of the built rootfs. When the rootfs base was switched to
Holden's 2024-02-21 build (2026-07-18), these three files were
deliberately kept as Prado's own (Limcet-P306 / McuType=6 / BoxP300)
instead of Holden's (Box-C211 / McuType=16), since McuType=6/BoxP300
is the already-confirmed-correct MCU protocol for this hardware.

- `usr/config.ini.holden` — Holden's IAP2/BT command-set config (minor
  SPP/vendor-name additions vs Prado's, not model-identity data)
- `msnprofile/MsnProductInfo.ini.holden` — Holden's product identity
  (Box-C211, McuType=16, ScreenType=3, TouchScreen=0)
- `msnprofile/FactoryConfig.ini.holden` — Holden's factory defaults
  (different vehicle name, no BlueTooth device-name/pair-code block,
  different SettingItemTypes)

Actual shipped rootfs keeps Prado's `usr/config.ini`,
`msnprofile/MsnProductInfo.ini`, `msnprofile/FactoryConfig.ini`,
`msnprofile/bootlogo/`, and `msnprofile/icon_120x120.png`.
