# Deconstructing `MsnCoreApp` for UI editing

Goal: make the compiled UI editable. This documents how far that can go, what
tooling does it, and the concrete workflow for changing a screen's layout.

**Honest verdict up front:** the application cannot be turned back into editable
source. It ships as machine code, and only the *bundled WebRTC library* was
built with debug info — the vendor's own code has none. What you **can** do is
treat the unstripped build as a **map**, locate any screen's layout code, read
the literal geometry constants, and **patch** them. Styling and images stay
fully data-editable (see [`UI_RESOURCES.md`](UI_RESOURCES.md)).

## What the binary actually is

The vendor left two unstripped builds in the image — `MsnCoreApp-original` and
`MsnCoreApp-auth` (byte-identical, 4.8 MB, symbol table intact) — alongside the
stripped 664 KB `MsnCoreApp` that actually runs. Composition of the unstripped
build (1854 functions):

| Part | Functions | Notes |
|------|-----------|-------|
| **WebRTC audio** (AEC/AGC/NS/VAD/resampler/ISAC) | ~880 | Hands-free echo cancellation; the only part with **DWARF debug info** (120 `.c/.cc` source files). Explains the 4.8 MB. |
| **`MsnCoreApp`** | ~96 | The main app/UI controller |
| **Dialog classes** | ~90 | `CalibrateDialog`, `VersionDialog`, `CopyFactoryConfigDialog`, `ModeSwitchDialog`, `MsnDialog`, … |
| Helpers | rest | `TouchKeyMonitor`, `SimulateCtrlKey`, `DiskDeviceWatcher`, `MediaFileScanner`, `MsnBeep`, … |

So the debug info is a red herring for UI work (it's all DSP). The UI is
recoverable only at the **symbol-table** level: every C++ method has a name,
address, and size — but no types, parameters, or line numbers.

## Two flavours of layout, both patchable

1. **uic-generated dialogs** — classes named `Ui_*` (e.g. `Ui_VersionDialog`,
   `Ui_CopyFactoryConfigDialog`). Their `setupUi()` is a mechanical 1:1
   translation of an original `.ui` file: it news each widget, calls
   `setGeometry(QRect(x,y,w,h))`, builds `QVBoxLayout`/`QHBoxLayout`, and wires
   text. The literal coordinates are right there.
2. **Hand-coded screens** — the main `MsnCoreApp` UI and the home grid, built in
   C++ with the same `QWidget::setGeometry` / `QBoxLayout` calls but not from a
   `.ui`. Same patch surface, just no `.ui` correspondence.

Neither is loaded from disk (there are **no `.ui`/`.qml` files** and no
`QUiLoader` in the binary) — layout is compiled in, which is why it takes a
binary patch to change.

## Tooling: `tools/msncore_analyze.py`

Reads the unstripped ELF with pyelftools + capstone and:

```
python tools/msncore_analyze.py MsnCoreApp-original --list CalibrateDialog
python tools/msncore_analyze.py MsnCoreApp-original --func Ui_VersionDialog::setupUi
```

- `--list [substr]` — every app function with address, size, demangled name (a
  built-in mini-demangler recovers `Class::method`; `cxxfilt` used if present).
- `--func <symbol>` — disassembles one function, **resolves every Qt call through
  the PLT** (so you see `QWidget::setGeometry`, `QLabel::QLabel`, …), and dumps
  the immediate constants it loads (the candidate x/y/w/h/spacing values).

The PLT-resolution + immediate-dump also works on the **stripped** shipped
binary — you just navigate by address instead of name.

## Worked example — `Ui_VersionDialog::setupUi` @ `0x55248`

`--func` resolves the call sequence and constants, which decode to:

- Dialog `resize(480, 320)` — the dialog is **480×320**.
- A `QVBoxLayout` with `setGeometry(QRect(...))` at `(20, …)`, width `261`.
- A `QLabel` (title), a `QListView` with `setGridSize(QSize(126, 42))`, and a
  `QPushButton` row (`QHBoxLayout`), with margins/spacing among the constants
  `20, 48, 132, 113, 126, 42, 36, 16`.

To make the version list two columns wider, or move the button row, you change
the relevant `QRect`/`QSize` immediates.

## Patch workflow (move / resize a control)

1. `--list` to find the screen's class; `--func <Class>::setupUi` (or the
   constructor / an `onXxx` builder for hand-coded screens).
2. In the disassembly, find the `setGeometry` / `resize` / `setGridSize` call and
   the `mov`/`movw` immediates feeding it (ARM passes `QRect(x,y,w,h)` via
   r1–r3 + stack right before the `bl`).
3. Patch those immediate bytes in `.text` (a `movw` encodes the constant in-place;
   for values that share a literal-pool entry, edit the pool word). Keep the
   instruction length identical — **never** grow the function.
4. Repack the rootfs (`build_rootfs.sh`) and flash.

Adding/removing widgets or new screens is **not** feasible by patching (it needs
new code and relocations) — that requires the vendor's source, which is not in
the firmware.

## What's editable, summarised

| Change | How | Feasible |
|--------|-----|----------|
| Colours, fonts, accent, borders | `msnprofile/DefaultStyleSheet.xml` (QSS) | ✅ easy, data |
| Icons, tiles, artwork | `msnprofile/resources/*.rcc` (`tools/rcc_extract.py`) | ✅ easy, data |
| Move / resize existing controls | patch `setGeometry`/`QRect` immediates | ⚠️ binary patch |
| Add / remove widgets, new screens | needs source | ❌ not without vendor source |

See also [`MSNCOREAPP_REVIEW.md`](MSNCOREAPP_REVIEW.md) (earlier behavioural /
security disassembly) and [`UI_RESOURCES.md`](UI_RESOURCES.md) (style + sprites).
