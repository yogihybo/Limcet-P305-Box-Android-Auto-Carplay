# Qt UI Analysis

## Overview

The head unit runs **Qt 4.7.4** on ARK1680 (ARM Cortex-A5). The UI is structured as a set of independently loadable modules, each compiled into its own Qt resource bundle (`.rcc`). Translations are shipped as compiled `.qm` files. All assets live on the NAND rootfs under `/msnprofile/`.

Qt libraries present:
- `libQtCore.so.4.7.4` (2.68 MB)
- `libQtGui.so.4.7.4` (8.76 MB)
- `libQtXml.so.4.7.4`, `libQtNetwork.so.4.7.4`, `libQtSql.so.4.7.4`, `libQtDBus.so.4.7.4`

---

## Key Binaries

| Binary | Size | Role |
|---|---|---|
| `MsnCoreApp` | 0.33 MB | Main coordinator/launcher — starts all other modules |
| `MsnFirstInit` | 0.02 MB | First-boot initialisation |
| `libarkcmn.so` | 0.05 MB | ARK common library |
| `libArkReverseWidget.so` | 0.02 MB | Reverse camera overlay widget |
| `libDetach.so` | 0.19 MB | Detachable panel module |

MsnCoreApp is small — individual UI screens are separate binaries (carplay, carlife, etc.) loaded by the coordinator. C++ symbol names are **not stripped** (mangled names visible, e.g. `_ZN26Ui_CopyFactoryConfigDialog7setupUiEP7QDialog`), which significantly aids reverse engineering.

---

## Filesystem Layout (`/msnprofile/`)

```
/msnprofile/
├── resources/          # Qt compiled resource bundles (.rcc)
├── lng/                # Compiled translation files (.qm)
├── DefaultStyleSheet.xml
├── FactoryConfig.ini
├── MsnProductInfo.ini
├── arkdata/            # Per-unit configuration .ini files
├── bootlogo/           # Boot animation JPEG frames
├── wallpaper/          # Home screen wallpapers
└── carplay_dimens_*/   # CarPlay layout configs per resolution
```

---

## What Can Be Extracted

| Asset | Method | Status |
|---|---|---|
| UI images (PNG) | `extract_rcc.py` — parses Qt RCC v1 binary | Done — 1,134 files |
| UI strings | `extract_qm.py` — parses Qt .qm message format | Done — 13 languages |
| Style definitions | `DefaultStyleSheet.xml` — plain XML on filesystem | Readable as-is |
| Widget layouts | Ghidra/IDA — `setupUi()` functions compiled from `.ui` files | Requires decompiler |
| Signal/slot wiring | Ghidra/IDA — symbol names visible, aids tracing | Requires decompiler |

---

## Qt RCC Resource Files

Qt resource bundles are compiled with `rcc` from `.qrc` project files. Each `.rcc` contains a tree of embedded files (images, stylesheets) for one UI module and screen resolution.

**File format (RCC v1):**

```
Offset  Size  Field
0x00    4B    Magic: "qres"
0x04    4B    Version: 1 (big-endian)
0x08    4B    Tree section offset (big-endian, from file start)
0x0C    4B    Data section offset (from file start)
0x10    4B    Names section offset (from file start)
```

Layout within file: `[header 20B] [data section] [names section] [tree section]`

**Tree node (14 bytes each):**

```
Offset  Size  Field
0       4B    Name offset into names section
4       2B    Flags: 0x02=Directory, 0x01=Compressed
6       4B    Directory: child count  |  File: locale
10      4B    Directory: first child index  |  File: offset into data section
```

**Data section (per file entry):**

```
4B  uncompressed length
nB  data (zlib-compressed if flag 0x01 set; prefixed by 4B original size)
```

**Names section (per name entry):**

```
2B  name length (UTF-16 character count)
4B  hash value
nB  UTF-16BE encoded name
```

### Extraction

```bash
# Single file (verbose)
python ui/tools/extract_rcc.py msnprofile/resources/Setting.rcc -o out/ -v

# All bundles
python ui/tools/extract_rcc.py msnprofile/resources/*.rcc -o ui/rcc_extracted/
```

Output is organised as `<out_dir>/<module_name>/<virtual_path_inside_rcc>`.

### Extracted Modules

| Module | Resolution(s) | Files |
|---|---|---|
| Launcher-Box | 800x480, 1024x600, 400x240 | 63 each |
| Launcher-Box-P301 | 800x480, 1024x600, 400x240 | 20 each |
| Launcher-Car | 800x480, 1024x600 | 46 each |
| BlueTooth | 800x480, 1024x600, 400x240 | 109–112 each |
| FMRadio | 800x480, 1024x600 | 54–55 each |
| MusicPlayer | 800x480, 1024x600, 400x240 | 27 each |
| VideoPlayer | 800x480, 1024x600, 400x240 | 31 each |
| Photo | 800x480, 1024x600 | 22 each |
| Setting | (single) | 69 |
| StatusBar | (single) | 64 |

The `Launcher-Box` vs `Launcher-Car` split reflects two UI skin styles (box/dashboard form factor vs car-dash form factor). `P301` is a variant product model with a smaller icon set.

---

## Qt Translation Files (.qm)

Translation files are compiled from `.ts` (XML) source by Qt's `lrelease` tool. The `.ts` source is not present on the device — only the compiled `.qm` binaries.

**File format:**

```
Offset  Size  Field
0x00    16B   Magic: 3C B8 64 18 CA EF 9C 95 CD 21 1C BF 60 A1 BD DD
0x10    ...   Sections (tag + 4B length + body, repeated)
```

**Section types:**

| Tag | Name | Description |
|---|---|---|
| 0x42 | Hashes | Hash table for fast message lookup (8B entries: hash + offset) |
| 0x69 | Messages | Actual message records |
| 0x88 | NumerusRules | Plural form rules |

**Message record tags (inside section 0x69):**

| Tag | Name | Encoding |
|---|---|---|
| 1 | End | No data — terminates a record |
| 3 | Translation | 4B length + UTF-16BE text; `0xFFFFFFFF` = use source unchanged |
| 6 | SourceText | 4B length + ASCII text |
| 7 | Context | 4B length + ASCII class/window name |
| 8 | Comment | 4B length + ASCII comment |

### Extraction

```bash
# Single file
python ui/tools/extract_qm.py msnprofile/lng/lang_en.qm -o qm_extracted/

# All languages
python ui/tools/extract_qm.py msnprofile/lng/*.qm -o ui/qm_extracted/
```

Output: one `.txt` per language, with `[ContextName]` headers and `source`/`translation` pairs.

### Language Coverage

| File | Strings | Language |
|---|---|---|
| `lang_en.qm` | 335 | English |
| `lang_italian.qm` | 387 | Italian |
| `lang_spanish.qm` | 360 | Spanish |
| `lang_dansk.qm` | 337 | Danish |
| `lang_brazil.qm` | 327 | Brazilian Portuguese |
| `lang_portuguese.qm` | 316 | European Portuguese |
| `lang_arabic.qm` | 259 | Arabic |
| `lang_hebrew.qm` | 723 | Hebrew |
| `lang_japanese.qm` | 280 | Japanese |
| `lang_korean.qm` | 215 | Korean |
| `lang_zh-cn.qm` | 679 | Simplified Chinese |
| `lang_zh-cn-xinri.qm` | 545 | Simplified Chinese (Xinri variant) |
| `lang_zh-tw.qm` | 468 | Traditional Chinese |

The string count variation between languages reflects that some strings fall back to source (English) and are not explicitly translated.

---

## Widget Layout Reconstruction (Ghidra)

The `.ui` XML files used at build time are not present on the device — they were compiled into C++ by Qt's `uic` tool. The generated `setupUi(QWidget*)` functions are compiled into the binaries.

To reconstruct widget layouts:

1. Load the target binary into **Ghidra** (ARM 32-bit little-endian).
2. Search for mangled symbol names containing `setupUi` — these are the entry points for each dialog/window's layout.
3. Decompile the `setupUi` function — it will contain a linear sequence of `new QWidget(parent)`, `setGeometry()`, `setText()`, `addWidget()` calls that directly map to the original `.ui` file structure.
4. The `Context` field from the `.qm` files (`[BtMainWindow]`, `[AvinWindow]`, etc.) corresponds to the C++ class name, which can be used to locate the correct `setupUi` in Ghidra.

**Known UI classes** (from `.qm` context fields in `lang_en.qm`):

```
AvinWindow, BootlogoWindow, BoxP800SettingWindow, BtDialPanel,
BtMainWindow, CameraWindow, CarlifeWindow, CarplayWindow,
DVRWindow, EQWindow, FMWindow, GPSWindow, GeneralSettingWindow,
ImageWindow, LauncherWindow, MusicWindow, NaviSettingWindow,
PhoneSettingWindow, PhotoWindow, RadioWindow, SettingWindow,
StatusBar, SystemSettingWindow, VideoWindow, WifiWindow, ...
```

Each context name maps to a C++ class with a corresponding `setupUi` function in the binary.

---

## DefaultStyleSheet.xml

Located at `/msnprofile/DefaultStyleSheet.xml`. This is a plain XML file defining Qt stylesheets (QSS) for the global UI theme — colours, fonts, border radii, button states. No extraction needed; readable directly.

Additional QSS is embedded as string literals in the binaries (visible via `strings` output), including `QListView{alternate-background-color:#333;...}` and named stylesheets like `ButtonStyleSheet`, `SliderStyleSheet`, `ScrollBarStyleSheet`.
