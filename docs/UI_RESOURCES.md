# UI Resources & Reskinning (`.rcc` sprites + `DefaultStyleSheet.xml`)

The head unit's look comes from **two** sources:

1. **`msnprofile/DefaultStyleSheet.xml`** — colours, fonts, focus highlight,
   borders and the gold accent (Qt Style Sheets in XML). Plain text, loaded from
   disk, **directly editable — no repack** ([see below](#colours--fonts-defaultstylesheetxml)).
2. **`msnprofile/resources/*.rcc`** — the **PNG sprites** (icons, tiles,
   artwork), Qt binary resource bundles ([see below](#sprite-bundles-rcc)).

Neither carries layout: widget positions and sizes are compiled into the Qt
binaries (`MsnCoreApp`, `Launcher-*`), so reskinning can change the look but
cannot move or resize controls.

See [`SETTINGS_REFERENCE.md`](SETTINGS_REFERENCE.md) for how `ResourceName` /
`LauncherName` / `ScreenType` select which resources load, and
[`SCREEN.md`](SCREEN.md) for the panel resolution.

## Colours & fonts: `DefaultStyleSheet.xml`

`MsnCoreApp` reads `msnprofile/DefaultStyleSheet.xml` at startup, selects the
`<Screen_WxH>` block matching the panel, and applies each named fragment via
`QApplication::setStyleSheet` / `QWidget::setStyleSheet`. The fragments are Qt
Style Sheets (QSS, CSS-like) wrapped in XML.

- **Per-resolution blocks** with a `fontsize`: `Screen_400x240`, `Screen_800x480`,
  `Screen_1280x480`, `Screen_1024x600`, `Screen_1280x720`. **The Prado (800×480)
  uses `Screen_800x480`, `fontsize="22"`.**
- **Fragments per block:**

| Fragment | Controls |
|----------|----------|
| `WidgetStyleSheet` | Base `QWidget` — transparent bg, white text `#fff` |
| `FocusStyleSheet` | Focus/selection outline — blue `rgba(0,128,250,230)` |
| `ButtonStyleSheet` | Buttons; pressed/checked = the **gold gradient** `rgba(255,114,2)`→`rgba(255,251,0)` |
| `SliderStyleSheet` | `QSlider` (handle = `:/images/hd2_yuan.png`) |
| `ScrollBarStyleSheet` | `QScrollBar` (arrows = `:/images/hd_shang_n.png` / `hd_xia_n.png`) |
| `CheckBoxStyleSheet` / `RadioBoxStyleSheet` | Indicators (`checked.png` / `unchecked.png`) |
| `MsnDialogStyleSheet` | Popups — `rgba(0,0,0,155)` bg, grey border |

**To recolour the UI** (e.g. change the gold accent, focus colour, or text
colour): edit the `rgba(...)` / `#hex` values in the matching `Screen_` block and
rebuild the rootfs — no `rcc` repack. The `url(:/images/...)` references point
into the `.rcc` bundles, so image swaps still go through those. `MsnCoreApp` also
probes a dash-suffixed `DefaultStyleSheet-<...>.xml` before the default — a hook
for a per-skin override, though only the base file ships. (Note the vendor file
carries a harmless typo, `height:32x;`, in the 1024×600/1280×720 slider handle.)

## Sprite bundles (`.rcc`)

The icons, tiles and artwork live in Qt binary resource files (`.rcc`) under
`msnprofile/resources/`. There is **no stylesheet, font, or layout data** in
these bundles — only images.

See [`SETTINGS_REFERENCE.md`](SETTINGS_REFERENCE.md) for how `ResourceName` /
`LauncherName` / `ScreenType` select *which* bundle loads, and
[`SCREEN.md`](SCREEN.md) for the panel resolution.

## What `.rcc` is

A `.rcc` is the output of Qt's `rcc -binary` — the `qres` container format: a
header, a resource *tree*, a *name* pool, and a *data* pool. Qt ships the packer
(`rcc`) but **no unpacker**, so this repo includes one:
[`../tools/rcc_extract.py`](../tools/rcc_extract.py). This firmware uses qres
**version 1**; the tool also handles v2/v3 and zlib/zstd-compressed entries.

## Bundle inventory (P306 2025 rootfs)

23 bundles, ~1,150 PNGs total. Most ship in three resolutions
(`400x240`, `800x480`, `1024x600`); the device loads the one matching its panel.

| Bundle | Role |
|--------|------|
| `Launcher-Box-*` | Full home screen (source tiles, clock, nav bar) — used when `LauncherName=Launcher-Box` (e.g. Box-C235) |
| `Launcher-Box-P301-*` | Slim home screen (22 sprites) — used when `ResourceName=Box-P301` (**the Prado/P306**) |
| `Launcher-Car-*` | Alternate "car" home layout |
| `StatusBar` | Global top bar / chrome (single resolution) |
| `Setting` | Settings screen (steering-wheel key icons, region tiles, toggles) |
| `BlueTooth-*` | Phone/BT UI (dial pad, contacts, call screen) |
| `FMRadio-*` | Radio UI |
| `MusicPlayer-* / VideoPlayer-* / Photo-*` | Media app skins |

**The Prado (P306)** is `ResourceName=Box-P301` on an **800×480** panel, so its
active skin is `Launcher-Box-P301-800x480.rcc` + `StatusBar.rcc` +
`Setting.rcc` + the `-800x480` media/BT bundles.

### Home-screen sprite naming (`launcher/…`)
`*_di_h` / `*_di_n` are the per-source tiles (pressed / normal): `carplay`,
`androidauto`, `hicar`, `bt`, `radio`, `music`, `video`, `photo`, `avin`, `dvr`,
`dtv`, `mirror`, `hulian` (互联/link), `file`, `mycar`, `carinfo`, `set`.
`nm_0`–`nm_9` + `nm_dian` are clock digit glyphs; `baitian` (白天) is day mode;
`guanpin` (关屏) is screen-off; `top_back` / `top_home` are the nav buttons.

## Unpack

```
python tools/rcc_extract.py msnprofile/resources/Setting.rcc  out/Setting
python tools/rcc_extract.py --list msnprofile/resources/Setting.rcc   # list only
```

Extraction also writes a matching `<name>.qrc` next to the output, listing every
path — ready to feed back to `rcc`.

## Reskin & repack

1. Edit the PNGs in the extracted `out/<bundle>/` — **keep each filename and its
   pixel dimensions** (the compiled layout expects them).
2. Repack with a Qt `rcc` (not included — needs a Qt install):
   ```
   rcc -binary out/Setting/Setting.qrc -o Setting.rcc          # Qt4 rcc → qres v1
   rcc --binary --format-version 1 out/Setting/Setting.qrc -o Setting.rcc   # Qt5 rcc
   ```
   Qt 4.7.4 on the device reads **version-1** qres — make sure the repacked file
   is v1 (Qt4's `rcc` defaults to it; for Qt5 pass `--format-version 1`).
3. Drop the new `.rcc` back into `msnprofile/resources/` and rebuild the rootfs
   (`build_rootfs.sh` / `build_update.sh`).
