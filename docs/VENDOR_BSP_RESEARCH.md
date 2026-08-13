# Vendor BSP Research (ark1668ed-bsp / cstech-ip17-rootfs)

**Status:** Reference
**Last Updated:** 2026-08-05

## Overview

This document covers a research pass through two vendor/sibling source trees that are
**not** part of this project's build (see `docs/SOURCES.md`), looking for anything
relevant to our USB, audio, and WiFi work:

- `/home/osboxes/Downloads/ark1668ed-bsp` — ArkMicro's own internal BSP for a newer SoC
  variant (ARK1668ED), Linux 6.12.56, from ArkMicro's internal Gogs server
  (`RD_Software/ark1668ed-bsp`), commit dated 2026-05-27. Different kernel generation and
  different USB PHY (Synopsys dwc2 vs our MUSB), but the **same WiFi/BT chip family** and
  the **same `com.arkmicro.*` platform conventions**, so several of its packages are
  directly useful as reference or literal upgrade material.
- `/home/osboxes/Downloads/cstech-ip17-rootfs` — a rootfs-only dump from a different,
  older ArkMicro product (ARK1680, Linux 3.4.0, board type `msn5g`). Confirmed wrong SoC
  generation for direct reuse; useful only for corroborating platform-wide naming
  conventions (see §1).

Everything below is organized by what was found and what to do with it. All vendor-tree
paths cited are **outside** this repo and outside `linux-arkmicro` — they are personal
Downloads on this machine, not tracked anywhere. Driver changes that came out of this
research were made in the separate `linux-arkmicro` kernel source repo
(`/home/osboxes/Downloads/linux-arkmicro`, see `docs/SOURCES.md`), not in this repo — see
§2 for details and branch names.

---

## 1. `cstech-ip17-rootfs` — confirmed wrong generation, one useful corroboration

ARK1680 (not any ARK1668 variant), Linux 3.4.0, compiled binaries only, no source tree —
too old/different a chip generation for any binary or register-level reuse.

The one useful finding: its `usr/share/dbus-1/services/` confirms `com.arkmicro.*` naming
(`com.arkmicro.audio`, `com.arkmicro.bluetooth`, `com.arkmicro.carplay`, etc.) is a
genuine platform-wide ArkMicro convention reused across at least two chip generations
(ARK1680 here, ARK1668-family on our board), not something `MsnCoreApp` invented — see
§4 below for a third, independent confirmation from `ark1668ed-bsp`'s own D-Bus service
names.

Its `etc/asound.conf` also has a multi-zone `pcm.softvolN` pattern (several softvol
stanzas sharing one `dmix`) that turned out to be the same underlying design documented
in more detail in §5 below via `ark1668ed-bsp`'s carlink source.

---

## 2. Driver-level findings in `ark1668ed-bsp/linux/`

### USB (MUSB glue) — no action taken, real difference in silicon

Our board's USB glue is `linux/drivers/usb/musb/musb_ark.c` (a fork of TI's `musb_dsps.c`
glue for the Mentor/Synopsys MUSB core). `ark1668ed-bsp` uses **Synopsys DesignWare
dwc2** instead — genuinely different USB PHY silicon on ARK1668ED, not a newer version of
the same driver. Nothing here is portable to our MUSB code.

Separately (found via a general web search, not this vendor tree): mainline Linux's own
`musb_dsps.c` gained real suspend/resume support after our fork diverged (commit
`869c5978`, "usb: musb: dsps: add support for suspend and resume") that `musb_ark.c`
still lacks (it has only a bare `printk("suspend save\n")` with no real state
save/restore). **Not yet backported** — flagged as a candidate, low risk, not started.

### Audio — no action taken, different hardware architecture

`ark1668ed-bsp`'s audio path (`ark1668ed_i2s.c` + `ark1668ed_audio_codec.c`) drives an
**external I2C codec** (`regmap`-based `CODEC_REG_*` writes), whereas our ARK1668 (non-E)
board uses the SoC's **internal** delta-sigma ADC/DAC through `ark1668_i2s.c` +
`ark1668-sdadc-codec.c`/`ark1668-sddac-codec.c`. No register-level correspondence; not
portable. (See `linux-arkmicro`'s `audio-driver-tidyup` branch, §3, for unrelated
cosmetic cleanup of our own actually-built audio driver files done in this same
session.)

### WiFi — backported, build-verified, done on a separate branch

**This is the one real driver upgrade that came out of this vendor tree.** ArkMicro's own
RTL8821CS (SDIO WiFi+BT combo chip) vendor driver in `ark1668ed-bsp` is dated
**2025-03-27** (`v5.15.9.6-1-g3fe4f4b91.20250327_COEX20230331-5d5d`), vs. our tree's
**2019-10-16/2021-01-18** drops. Same chip family, same `CONFIG_ARCH_ARKMICRO` Makefile
hook preserved essentially unchanged across BSP generations — a same-vendor version
bump, not a fork divergence.

Two commits, on branch **`wifi-rtl8821cs-driver-port`** in the `linux-arkmicro` repo
(branched off `master`, not merged, not hardware-tested):

1. **`rtl8821cs` (SDIO, WiFi+BT combo)** — direct swap of the whole vendor driver
   directory for the newer 2025-03-27 drop. Adds whole subsystems the old drop lacked:
   `core/crypto` (AES-CCM/GCM/SIV, SHA-256), 802.11r fast transition (`rtw_ft.c`), DFS
   (`rtw_dfs.c`), MBO (`rtw_mbo.c`), radiotap monitor mode, newer BT-coex table.
   Built cleanly as a module against this tree's real `CONFIG_ARCH_ARKMICRO=y` /
   `CONFIG_RTL8821CS=m` `.config` with `arm-linux-gnueabihf-gcc` — zero warnings, vermagic
   matches (`4.19.192 mod_unload ARMv7 p2v8`). Firmware is compiled into the module
   (`hal8821c_fw.c` array), not loaded from `/lib/firmware`, so no rootfs changes needed.

2. **`rtl8811cu` (USB, WiFi-only) — the module actually loaded at boot** (per
   `docs/WIRELESS_AND_INIT.md`, `wifi_ap.sh`/`rcS` load `rtl8811cu`, not `rtl8821cs`).
   **Key discovery**: RTL8811CU and RTL8821C(U/S) are the *same silicon die* — confirmed
   directly by the vendor tree itself, which names the 8811CU driver's chip HAL directory
   `hal/rtl8821c/` and its internal module string `RTL871X_MODULE_NAME "8821CU"` — "8811"
   vs "8821" is a Makefile/`autoconf.h` naming choice on top of identical source, not a
   different chip driver. This meant the newer 2025-03-27 code could be backported here
   too, not just to the SDIO variant.

   Built by taking the already-updated `rtl8821cs` (2025) tree as a base and restoring
   only the USB-specific pieces that have no SDIO equivalent from the old (2021)
   `rtl8811cu` tree: `hal/rtl8821c/usb/*`, `hal/hal_hci/hal_usb.c`, `hal/led/hal_usb_led.c`,
   `hal/halmac/halmac_88xx/halmac_usb_88xx.*` + `halmac_8821c/halmac_usb_8821c.*`,
   `os_dep/linux/{usb_intf,usb_ops_linux}.c`, and five matching `include/usb_*.h` headers.
   Everything else (PHY/RF/MAC `phydm`/`halrf`/`halmac`/`efuse`, and the chip-agnostic
   `core/`) is literally the same files as the SDIO variant.

   Four real build issues were found and fixed one at a time (not guessed in advance):
   - `Makefile`: bus-select flags (`CONFIG_USB_HCI=y`/`CONFIG_SDIO_HCI=n`) and the
     `CONFIG_ARCH_ARKMICRO` block's `MODULE_NAME` (back to `rtl8811cu`).
   - `include/autoconf.h`: still had the SDIO tree's hardcoded `#define CONFIG_SDIO_HCI`
     (needed `CONFIG_USB_HCI`) — the driver's own `#error "CONFIG_USB_HCI shall be on!"`
     caught this immediately.
   - `include/autoconf.h`: missing `CONFIG_USB_VENDOR_REQ_BUFFER_PREALLOC` /
     `_MUTEX`/`CONFIG_VENDOR_REQ_RETRY` — the newer SDIO-only `autoconf.h` had dropped
     this whole USB-vendor-request-buffer section entirely, which the old `usb_ops_linux.c`
     needs (a genuinely undeclared-variable bug in the merged tree, not present in either
     source drop alone).
   - `include/autoconf.h`: **did not** carry over `CONFIG_SDIO_INDIRECT_ACCESS`/
     `DBG_SDIO_INDIRECT_ACCESS` from the SDIO tree — two call sites in `ioctl_linux.c`
     guard on that macro alone without also checking `CONFIG_SDIO_HCI`, so defining it for
     USB pulls in calls to `rtw_sd_iread8/16/32` that don't exist outside the SDIO HAL.

   Final result: clean build, zero warnings, correct vermagic, correct USB device alias
   (`usb:v0BDAp8811d*` — Realtek VID, PID 0x8811).

**Update, first real hardware boot of `rtl8811cu`**: hit a real bug --
`RTW: ### rtw_hal_ops_check - Error : Please hook hal_func.recv_hdl ###`. Root cause: the
merged `autoconf.h` (based on the newer SDIO-only drop) defined `CONFIG_RECV_THREAD_MODE`,
which makes `hal_intf.c` require `hal_func.recv_hdl` to be hooked -- only
`hal/rtl8821c/sdio/rtl8821cs_ops.c` populates that hook; the USB ops file (kept from the
2021 tree, predates this feature) never does, since USB receive is URB-driven, not
thread-polled. Confirmed the original 2021 USB `autoconf.h` never defined
`CONFIG_RECV_THREAD_MODE`, `CONFIG_XMIT_THREAD_MODE` ("necessary for SDIO" per its own
comment), or `CONFIG_SDIO_RX_COPY` (SDIO-named directly) -- all three were carried over
unintentionally during the merge; undefined them to match the original working config
(`CONFIG_TX_AGGREGATION`, genuinely generic, kept). Fixed, rebuilt clean, commit
`d4ebc10ec`. **Still needs a retest** to confirm the module now probes past this point and
actually associates.

**Update, the DFS crash persisted even with `regulatory.db` fixed — real root cause
found and fixed.** A live boot with the regdb fix in place confirmed both certs load
(`sforshee` and `wens`) and the module now inits fully (`module init ret=0`, `wlan0`/
`wlan1` both created) -- but the *same* `rtw_dfs_rd_en_decision` NULL-deref crash still
hit later, when `hostapd` starts the AP on `wlan0`. The register dump confirmed the exact
mechanism: `r3 == 0` faulting at `r3+0xfc8`, matching `ALINK_GET_BAND`/`_CH`/`_BW`/
`_OFFSET` (`drv_types.h`), which all dereference `alink->adapter` before reading into the
`_adapter` struct. Root cause: `os_intfs.c`'s `rtw_drv_add_vir_if()` (creates virtual/
secondary interfaces, e.g. `wlan1`) sets `padapter->adapter_link.adapter = padapter`, but
`rtw_init_netdev()` -- the **primary** adapter's own init path, i.e. `wlan0`, the
interface `hostapd` actually uses -- never does. Confirmed `os_intfs.c` is byte-identical
between `rtl8811cu` and `rtl8821cs` (same missing line, same file, both from the same
2025-03-27 SDK drop) -- a genuine upstream vendor bug, not something the USB backport
introduced. Fixed in both drivers (added the same assignment the vendor's own virtual-
interface code already uses), rebuilt clean, commit `c79611076`. **Still needs a
retest** -- this is now three real, sequential hardware-found-and-fixed bugs on this one
driver update (`recv_hdl` hook, missing `regulatory.db`, missing `adapter_link.adapter`),
each only surfacing after the previous one was fixed and the driver got further.

**Both drivers on this branch are otherwise build-verified only — not fully
hardware-tested.** Same caveats apply as any vendor driver bump: association behavior,
BT-coex timing, and power management are real code paths that changed. Test
`carplay_wifi` AP mode (WiFi) and BT pairing/audio before trusting either.

---

## 3. Audio driver cosmetic cleanup (separate, unrelated branch)

Branch **`audio-driver-tidyup`** in `linux-arkmicro` (off `master`, not related to the
WiFi work above) did a cosmetic-only pass on the *actually-built* audio driver set
(`ark1668_i2s.c`, `ark1668-sdadc-codec.c`, `ark1668-sddac-codec.c`, `ark1668_i2s.h` —
confirmed live via `.config`: `CONFIG_SOC_ARK1668=y` + `CONFIG_SND_SOC_ARK1668_{I2S,ADC,DAC}=y`;
the `ark1668e_*` variants and `BD37033` are not compiled for this board). No functional
changes: removed dead/commented-out code, generic printk trace litter (careful to leave
every dated, investigation-context printk untouched), `__FUNCTION__`→`__func__`, and
replaced magic address literals with already-defined-but-unused macros. Verified with a
real ARM cross-compile, no new warnings.

One finding worth a functional follow-up, not fixed here (out of scope for a cosmetic
pass): `ark_i2s_suspend`/`ark_i2s_resume` and `ark_i2s_remove` are fully implemented in
`ark1668_i2s.c` but never wired into `ark_i2s_dai`'s `.suspend`/`.resume`/`.remove`
fields, even though this kernel's `struct snd_soc_dai_driver` supports them — they're
dead code today.

---

## 4. `ark1668ed-bsp/buildroot-external/package/` — full survey

Read through all ~24 packages. Full one-line verdicts:

| Package | Verdict |
|---|---|
| `libarkapi` | **Real source**, most valuable package — see §4a |
| `carlink` | **Real source**, second most valuable — see §5, §6 |
| `hx170dec` (kernel driver) | Useful negative confirmation — see §4b |
| `hxtest`, `mfc` | Reference Hantro decode API usage, redundant with hx170dec |
| `libgal`, `libvglite` | Newer Vivante GC (6.4.5 vs our 6.2.4.p1.8), wrong kernel ABI (6.12) to load directly — diff/reference only |
| `libmali` | Inactive legacy GPU path even in this newer BSP, skip |
| `ark-mplayer` | Stock upstream MPlayer, skip |
| `libdns_sd` | Genuine Apple mDNSResponder/Bonjour build (Aug 2025), useful reference `dns_sd.h` |
| `ark1668edApp`, `DashBoard` | Qt reference apps — see §4c |
| `demo-display` | Has a real display-layer design doc (README) — see §4c |
| `libbt_feasycom`, `libbt_gukai`, `libsd818` | Three BT vendor stack options behind a shared `gocsdk` daemon name — see §4d |
| `libcheck` | Generic "Check" C unit-test framework, skip |

### 4a. `libarkapi` — real source for the platform API family this project has reverse-engineered

Full C source: `ark_video.c`, `ark_display.c`, `ark_scalar.c`, `ark_2d.c`, `ark_vin.c`,
`ark_memalloc.c`. `ark_memalloc.c`'s ioctl names (`MEMALLOC_IOCXGETBUFFER`,
`MEMALLOC_IOCSFREEBUFFER`) match this project's independently reverse-engineered
protocol exactly — external confirmation the understanding is correct. Also surfaces a
disabled/`#if 0`'d `MEMALLOC_IOCSFLUSHRAMBUFFER` cache-flush ioctl path
(`ark1668_memalloc_flush`/`ark1668_cache_flush`), unused in this reference too, but worth
knowing exists if a buffer-coherency bug is ever suspected.

### 4b. `hx170dec` kernel driver — negative confirmation for the fasync fix

ArkMicro's own newer reference driver (`linux/drivers/soc/arkmicro/hx170dec/hx170dec.c`
in `ark1668ed-bsp`) has **removed fasync/SIGIO support entirely** — no
`fasync_helper`/`kill_fasync`/`.fasync` fop — whereas our tree's driver still has
`vdec_misc_fasync()` etc. This confirms the fasync fix this project made
(`project_hx170_fasync_root_cause` memory) is a genuine local necessity for our kernel
line, not something ArkMicro carried forward or fixed differently upstream.

### 4c. Qt apps (`ark1668edApp`, `DashBoard`) — mostly irrelevant UI, two real finds

`AutoConnect` is **not** a connection state machine — it's a 13-line Qt
signal/slot-by-naming-convention helper used ~50 times as UI plumbing sugar, unrelated to
BT/WiFi/USB connection logic despite the name. `DashBoard`'s `cornerlampwidget.cpp`/
`speedpainter.cpp` are pure self-driven animation mockups with zero real vehicle-signal
input (no CAN, no GPIO) — not usable as a turn-signal/speedometer wiring reference.

Two genuine finds:

- **`DashBoard/BusinessLogic/carback.cpp`** — the complete userspace reverse-camera
  handshake protocol:
  ```c
  #define CARBACK_IOCTL_BASE           0x9A
  #define CARBACK_IOCTL_SET_APP_READY  _IO(CARBACK_IOCTL_BASE, 0)
  #define CARBACK_IOCTL_APP_ENTER_DONE _IO(CARBACK_IOCTL_BASE, 1)
  #define CARBACK_IOCTL_APP_EXIT_DONE  _IO(CARBACK_IOCTL_BASE, 2)
  #define CARBACK_IOCTL_GET_STATUS     _IOR(CARBACK_IOCTL_BASE, 3, int)
  #define CARBACK_IOCTL_DETECT_SIGNAL  _IOR(CARBACK_IOCTL_BASE, 4, int)
  #define VIN_UPDATE_WINDOW  _IOWR('n', 50, struct vin_screen)  // on /dev/video0
  ```
  Sequence: `open("/dev/carback")` → `ioctl(SET_APP_READY)` → worker thread blocks on a
  1-byte `read()` of the fd (not polled) → on `CBS_On`: `arkapi_enter_carback()` →
  `ioctl(APP_ENTER_DONE)` ack → `ioctl(video0Fd, VIN_UPDATE_WINDOW, &vin_para)` to set the
  capture window. This corroborates and extends what this project had only from kernel
  disassembly (`docs/HARDWARE_AND_SOC_REFERENCE.md`, GPIO 5, `request_threaded_irq`) and
  strace (`arkapi_enter_carback()`/`arkapi_exit_carback()` sharing a `shmget(0x4449,...)`
  struct). Worth diffing against the actual driver/glue code for exact ioctl-number
  confirmation.

- **`ArkIVI/BusinessLogic/Bluetooth.cpp`** — the full Feasycom AT-command vocabulary over
  `/dev/bw_serial` (a second vendor branch talks to `/dev/goc_serial`, see §4d):
  `HFPCONN=<mac>`, `DSCA`, `HFPDIAL=<num>`, `HFPDTMF=<num>`, `HFPANSW`, `HFPCHUP`/`CF`,
  `HFPSTAT`, `BTEN=1/0`, `HFPCFG=<bitmask>` (bit0=auto-connect, bit1=auto-answer),
  `SCAN=1/0`, `PLIST`, `PBDOWN=1..5`, `AVRCPCFG=<n>`, `MICMUTE=<0/1>`, `HFPADTS=1/2`
  (voice route: phone vs. car BT), `PIN=<code>`, `NAME=<devname>`, `ADDR`,
  `A2DPCONN`/`A2DPDISC`, `PLAY`/`PAUSE`/`STOP`/`FORWARD`/`BACKWARD`. Corroborates and
  extends `docs/WIRELESS_AND_INIT.md` §5's `libBlueTooth.so` findings — likely fills gaps
  in commands not yet observed live (`HFPCFG` bits, `AVRCPCFG`, `HFPADTS`).

`demo-display`'s README (`ark1668显示接口相关说明.txt`) is a genuine ArkMicro design
document (not just headers) for the display-layer model: 5 layers (`PRIMARY_LAYER`,
`VIDEO_LAYER`, `OVER_LAYER`, `TVOUT_LAYER`, `AUX_LAYER`) with defined z-order and the full
`arkapi_display_*` API (open/close/show/hide/force_show/force_hide/set_pos/set_size/
set_format). Worth reading in full as a cross-check against this project's own
independently-reconstructed display API documentation.

### 4d. Bluetooth — three vendor stack options behind one shared daemon name

- `libbt_feasycom` — Feasycom stack (already known — this is what our board uses, per
  `docs/WIRELESS_AND_INIT.md` §5). Its `gocsdk` binary in `ark1668ed-bsp` is a *different*
  build than ours, but its own git history literally says "first debug version" — too
  weak to act on as an upgrade candidate.
- `libbt_gukai` — a **previously-unknown second BT stack option**: CSR/Qualcomm BlueCore
  (`CsrBt*`/`CsrSched*` symbols — CSR was acquired by Qualcomm in 2015; this signature is
  the hallmark of their proprietary BT SDK), wrapped by ArkMicro's own `gbts_*`-prefixed
  glue for A2DP/ringtone routing.
- `libsd818` — a third option, generic vendor-agnostic HCI-level stack (`HCI_EV_Vendor_Command`,
  a full Bluetooth SIG company-ID table).

All three link a binary of the **same name**, `gocsdk` — i.e. `gocsdk` is ArkMicro's
umbrella daemon name across BT vendor choices, not a Feasycom-specific artifact. If a
board's actual BT chip identity is ever in question, grepping its `gocsdk` binary for
`CsrBt*` (Gukai/CSR) vs Feasycom-specific strings vs generic HCI vendor-command strings
(sd818) is a fast way to tell which of the three it links.

---

## 5. Android Auto — real, unstripped reference implementation, directly relevant

`carlink/lib/auto/libAndroidAuto.so` is a genuine AASDK/OpenAuto-style Android Auto
receiver — OpenSSL for the auth handshake, protobuf for the control channel, the full
real Google AA message set as linked symbols (`AudioConfiguration`, `AuthResponse`,
`BluetoothPairingRequest`, `ByeByeRequest`, `ChannelCloseNotification`, `AbsoluteEvent`,
etc.) — **unstripped, with debug symbols**, unlike our board's own stripped copy of the
same library (`firmware_source/mtd6_rootfs/usr/lib/libAndroidAuto.so`).

### API shape (`include/auto/AndroidAuto.h`, `IUserAutoCbs.h`)

```cpp
class AndroidAuto {
    void registerCallbacks(IUserAutoCbs *cbs);
    void startSession(bool isWifi = true);   // false = wired (AOA), true = wireless
    void getVideoFocus(); void releaseVideoFocus();
    void getAudioFocus(); void releaseAudioFocus();
    void sendTouchEvent(...); void sendKeyEvent(...); void sendKnobEvent(...);
};
class IUserAutoCbs {
    virtual void videoStart(int width, int height, int offsetX, int offsetY) = 0;
    virtual void videoPlay(char *buf, int len) = 0;      // raw H.264, still encoded
    virtual void audioStart(int type, int rate, int channels, int bits) = 0;
    virtual void audioPlay(int type, char *buf, int len) = 0;
    virtual void recordStart(int rate, int channels, int bits) = 0;
    virtual void recordProc(char *buf, int len) = 0;
    virtual void notifyStatus(int state) = 0;   // LINK_STARTING/LINK_SUCCESS/LINK_EXITING/...
};
```

Confirms the architecture already inferred: the AA library only delivers still-encoded
transport data via callbacks; the platform integrator owns decode and display entirely.

### Video pipeline (`UserInterface/IUserLinkPlayer.cpp` + `include/user/VideoDecoder.h`)

One `VideoDecoder` class, shared by every link type (Auto, CarPlay, CarLife, HiCar,
Mirror — one pipeline for all of them):

```cpp
class VideoDecoder {
    bool Init(VideoFrame*);                              // set up decode+display state
    bool Open(VideoFrame*);                               // open video layer
    bool Show(bool bVisible);                              // show/hide the video layer
    int  InputDecoder(const void *data, int length);      // feed H.264 to hx170dec/mfcapi
    display_info *disp_layer_init(enum ark_disp_layer layer, int format, int w, int h, int buf_cnt);
    // wraps ark_api.h (libarkapi display layers), mfcapi.h/dwl.h (Hantro/hx170dec)
};
```

`app_status(APP_FOREGROUND/APP_BACKGROUND)` — not the AA session's video-start event
itself — is what calls `VideoDecoder::instance()->Show(true/false)`. This project's own
AA video black-screen bug is already resolved (per session context), but this confirms
the intended design: video visibility is gated by app lifecycle state, decode/feed by
session state — two separate triggers, not one.

### Audio pipeline — directly relevant to the ongoing AA audio-stutter investigation

`IUserLinkPlayer`'s constructor opens **four persistent ALSA PCM handles, one per stream
type, once, not per-connection**:

```cpp
mpMusicDecoder(new AudioDecoder("plug:softvol2")),   // music
mpTTSDecoder(new AudioDecoder("plug:softvol1")),     // nav/TTS
mpVRDecoder(new AudioDecoder("plug:softvol4")),      // voice recognition prompts
mpCallDecoder(new AudioDecoder("plug:softvol3")),    // phone call
```

**This is an exact, independent corroboration of a finding already in
`docs/AUDIO_SUBSYSTEM_INVESTIGATION.md`.** That investigation traced `sink`'s own
`ArkMediaPlayer::setup()` via disassembly and found `mode==3 → "plug:softvol2"`,
`mode==1 → "plug:softvol1"`, `mode==2 → "plug:softvol4"` — the *exact same*
softvol-channel-to-stream-type mapping (1=TTS, 2=music, 4=VR), reverse-engineered
independently from a different binary. Two unrelated sources agreeing on this numbering
is strong confirmation it's a real, stable ArkMicro-platform-wide convention, not
coincidence.

Mixing happens entirely at the ALSA `softvol`/`dmix` layer (all three/four softvol
instances share one underlying `dmix`), not in application code — matches
`AUDIO_SUBSYSTEM_INVESTIGATION.md`'s own finding that `softvol1`/`2`/`4` differ **only**
in `max_dB` gain ceiling and control name, with identical `period_size 1024`/
`buffer_size 16384`/`slave.pcm "hw:0,0"`.

**`AudioDecoder`/`AudioRecord` both declare `xrunRecover(snd_pcm_t*, int)`.** No
implementation source available for these two specific classes (headers only in this
package), but the same method exists, implemented and compiled, in a *sibling* class pair
used by the CarLife link (`MediaDecode::xrunRecover`, `MicCapture::xrunRecover`, in
`lib/carlife/libcarlifeplayer.so`, unstripped). Disassembled it directly:

```
err == -EPIPE (32)    → snd_pcm_prepare(handle)
err == -ESTRPIPE (86) → loop: snd_pcm_resume(handle), sleep(1000ms) while -EAGAIN,
                          fall back to snd_pcm_prepare() if resume fails
```

This is the **textbook alsa-lib reference `xrun_recovery()`** — no custom tuning, no
vendor-specific tricks. Useful negative result for the audio-stutter investigation: if
recovery mechanics were the gap, you'd expect this vendor reference to differ; it
doesn't. Consistent with `AUDIO_SUBSYSTEM_INVESTIGATION.md`'s own conclusion that the real
mechanism is scheduling-starvation-driven XRUNs inside `AlsaHandle::play()`, not a
recovery-logic or buffer-sizing bug.

**Mic capture is opened once and gated by a pause flag, never re-opened per session:**

```cpp
// on LINK_SUCCESS (AA session connects):
record_start(info); record_pause(true);      // capture PCM opens immediately, but muted
// only on an actual Siri/VR request:
recordStart() → record_pause(false)          // un-gate data flow
recordStop()  → record_pause(true)           // re-gate; PCM stays open the whole time
```

A concrete design signal for the "silent capture-XRUN path" concern in
`project_mic_capture_investigation`: the reference implementation deliberately avoids
repeated open/close of the capture device (a classic XRUN trigger), keeping one
continuously-running capture thread and gating only at the data-callback level. Worth
checking whether this project's own mic path re-opens the capture PCM per recognition
session instead of doing this.

### USB hotplug / sink-side connection detection

`UsbHostServicePrivate` (in `lib/user/libUserInterface.so`, unstripped) uses real
`libusb_hotplug_register_callback`-based detection (not polling):
`usbHotplugCallback()` dispatches to `usbInsertProc()`/`usbRemoveProc()` on
`LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED`/`_LEFT`. Insert goes through a gate function,
`isValid(libusb_device*, int*)`, which pulls the active USB config descriptor and rejects
anything whose interface class isn't `0xFF` (vendor-specific) or `6` (still-image/PTP).
**Vendor/product ID matching (Apple `0x05ac`, Google AOA `0x18d1`) is not done at this
layer** — confirmed by checking the disassembly directly for those immediates and finding
none. This is a generic first-pass class filter; per-protocol identification happens one
layer up.

### **The big one: `libAndroidAuto.so` has its own complete, self-contained AOA implementation** — directly relevant to the open wired-AA investigation

`libAndroidAuto.so` (the `ark1668ed-bsp` copy) exports a full `Accessory`/
`AccessoryPrivate`/`AccessoryWifi`/`AccessoryWifiPrivate` class hierarchy with its own
independent USB hotplug handling, **separate from** `UsbHostService` above:

```
Accessory::startSession() / stopSession() / Read() / Write()
AccessoryPrivate::usbHotplugCallback / usbInsertProc / usbRemoveProc
AccessoryPrivate::startAccessoryMode()   ← the actual "switch phone to AOA" call
AccessoryPrivate::switchAoa()
AccessoryPrivate::waitConnectReady()
```
Strings in the same library: `"AOA %d.%d"`, `"found aoa dev"`,
`"Found Google device not in accessory mode. Trying to turn on."`,
`"Phone is already in aoa state"`, `"%s:%d start usb aoa!"`.

**Checked our own deployed device's copy directly**
(`firmware_source/mtd6_rootfs/usr/lib/libAndroidAuto.so`, fully stripped): it contains the
exact string `"Found Google device not in accessory mode. Trying to turn on."` and the
mangled symbol text `_ZN9Accessory18startAccessoryModeEv` — i.e. **our board's own
`libAndroidAuto.so` has the same `Accessory::startAccessoryMode()` method**, just with the
rest of the symbol table stripped.

**This directly reframes the open question in `docs/WIRELESS_AND_INIT.md`'s "Wired
Android Auto never completes" thread.** That investigation's live testing found AOA
*detection* works (`"Device is support acessory mode, AOA version:2"` fires correctly),
but the connection then falls straight through to `wirelessConnectionProc` with "no AOA
'turn on accessory mode' control request" ever attempted, and speculated the gap might be
that `MsnCoreApp` needs to issue a raw `USBDEVFS_CONTROL` ioctl itself. **This vendor
tree shows the AOA switch-to-accessory-mode logic is supposed to live *inside*
`libAndroidAuto.so`'s own `Accessory`/`AccessoryPrivate` classes, self-contained** —
triggered by `AndroidAuto::startSession(false)` (the `isWifi=false` wired path), not by
anything the app is expected to do at the raw USB ioctl level itself.

That reframes the likely real gap: either (a) `MsnCoreApp` never actually calls
`AndroidAuto::startSession(false)` for a wired connection at all (going straight to the
wireless path instead, which would make this a pure app-logic bug, matching the
`AndroidLinkType=6→3` config-value investigation already in progress), or (b)
`startSession(false)` is called but `AccessoryPrivate`'s own internal hotplug/switch logic
isn't firing for some other reason. Either way, **the next concrete step recommended by
this research** is: get the unstripped `ark1668ed-bsp` copy of `libAndroidAuto.so` beside
the stripped deployed one, and either (1) trace `AndroidAuto::startSession()` call sites
in `sink`/`MsnCoreApp` via `strace`/disassembly to see if `Accessory::startAccessoryMode()`
is ever reached at all during a wired attempt, or (2) if a debug/symboled build of the
exact same `libAndroidAuto.so` version deployed on the Prado can be obtained, load it
with symbols to trace this directly instead of working from stripped addresses.

---

## Handoff — what a future session should pick up here

In rough priority order:

1. **Wired Android Auto (`docs/WIRELESS_AND_INIT.md`'s open thread)** — see the
   `Accessory::startAccessoryMode()` finding in §5 above. This is the strongest new lead
   from this research pass. Next step: confirm (via strace or disassembly) whether
   `AndroidAuto::startSession(false)` / `Accessory::startAccessoryMode()` is ever reached
   during a real wired connection attempt on hardware. If it's never reached, the bug is
   purely in `MsnCoreApp`'s decision to start a wired vs. wireless session — not a
   USB/AOA/kernel issue at all.

2. **Hardware-test the two WiFi driver updates** on branch `wifi-rtl8821cs-driver-port`
   in `linux-arkmicro` (§2) — both build clean but are unverified on real hardware. Test
   `rtl8811cu` first (it's the one actually loaded at boot); `rtl8821cs` is lower priority
   since it's not currently in the boot path.

3. **AA audio-stutter investigation** — the softvol1/2/4 corroboration and the
   textbook-`xrun_recovery()` finding in §5 don't point to a new fix, but they do further
   rule out "buffer/recovery misconfiguration" as the mechanism, reinforcing
   `AUDIO_SUBSYSTEM_INVESTIGATION.md`'s existing conclusion that the real bottleneck is
   scheduling-starvation inside `AlsaHandle::play()`'s `WorkQueue` thread. The two staged
   mitigations there (`chrt -f 50`, `busy_poll` sysctls) are still the next thing to
   hardware-test, unrelated to anything new found here.

4. **`carback.cpp`'s exact ioctl protocol** (§4c) — worth a direct diff against this
   project's own reverse-camera driver/glue code to confirm ioctl numbers and the
   `VIN_UPDATE_WINDOW` companion call are handled identically.

5. **Feasycom AT-command vocabulary** (§4c) — worth exercising the previously-unobserved
   commands (`HFPCFG` bits, `AVRCPCFG`, `HFPADTS`) live to fill gaps in
   `docs/WIRELESS_AND_INIT.md` §5's BT protocol documentation.

6. Lower priority / no action needed: `libgal`/`libvglite` (reference only, wrong kernel
   ABI), `libbt_gukai`/`libsd818` (confirmed alternate vendor options, not applicable
   unless BT chip identity is ever in doubt), `demo-display`'s README (good cross-check
   reading, not urgent), MUSB suspend/resume backport (low-risk, low-urgency, still
   un-started).

### Where things live

- This document: `docs/VENDOR_BSP_RESEARCH.md` (this repo).
- WiFi driver updates: `linux-arkmicro` repo, branch `wifi-rtl8821cs-driver-port`
  (2 commits, off `master`).
- Audio driver cosmetic cleanup: `linux-arkmicro` repo, branch `audio-driver-tidyup`
  (1 commit, off `master`, unrelated to the WiFi branch).
- Vendor source trees referenced: `/home/osboxes/Downloads/ark1668ed-bsp`,
  `/home/osboxes/Downloads/cstech-ip17-rootfs` — neither is tracked in any repo, both are
  personal Downloads on this machine (see `docs/SOURCES.md` for how this project usually
  registers reference-only external sources).

---

## 6. `ArkPro` (github.com/cphatt/ArkPro) — public vendor reference app, ARK1680 generation

**Cloned:** `/home/osboxes/Downloads/ArkPro` (not tracked in any repo, personal Downloads),
358MB, 1650 files, last upstream push 2017-10-31.

This is ArkMicro's own Qt4-based automotive head-unit application framework — genuine
vendor source, publicly available on GitHub, no license attached. It targets ARK1680
("ec803"), the same **earlier/sibling chip generation** as `cstech-ip17-rootfs` in §1
above, not our ARK1668. Architecture is a service-oriented split communicating over D-Bus:
`Launcher` (the main Qt UI, dozens of widgets under `UserInterface/MainWidget/*` —
CarPlay/CarLife/MirrorLink, AV, Disk, FM, Home, Setting, Volume), plus standalone service
processes (`AudioService`, `AutoConnect`, `DbusService`, `DiskDeviceWatcher`,
`EventEngine`, `MultimediaService`, `SettingPersistent`, `SettingService`). `AVService`
carries a full embedded BSP snapshot (`kernel/`, `arm-lib/`, `uboot/`) under a
Chinese-named subfolder ("背光和vp调节" = "backlight and vp adjustment").

Given the scale, this was a targeted pass against this project's own open/recent
investigations rather than an exhaustive file-by-file read (most of `Launcher`'s ~100 UI
widget files are generic Qt boilerplate for screens/features not relevant to a
one-generation-removed device).

### 6a. Decisive confirmation: real on-device D-Bus protocol matches ArkPro's proxy exactly

`Launcher/BusinessLogic/Link/CarplayLinkProxy.{h,cpp}` (qdbusxml2cpp-generated) defines
interface `Local.DbusServer.Carplay` with:

```cpp
QDBusPendingReply<> requestLinkStatus(int type, int status);
QDBusPendingReply<> requestTouchStatus(int deviceType, int touchType, const QString &touchPointXml);
// signals:
void onLinkStatusChange(int type, int status);
void onTouchStatusChange(int type, int x_src, int y_src, int x_dst, int y_dst);
```

This is an **exact signature match** against real on-device log output already captured
in `docs/logs/android auto log v1/v2/v3.txt` (`ArkDbus::reply_to_requestLinkStatus_call
223 param1=11 param2=3`, `IArkCallbacks::requestLinkStatus:77`, etc.) — independent
confirmation that MsnCoreApp's `sink`/`ArkDbus` layer is a direct descendant of this same
`Local.DbusServer.Carplay` D-Bus contract, not a from-scratch reimplementation. `type=11`
recurs across all three logs and looks like a fixed link/device-type constant; `status`
values seen live: 3, 4, 8, 9, 15, 31, 32, 39, 40 — an enum this project hasn't fully
decoded yet.

**Real gap found**: the logs also show `ArkDbus::reply_to_requestWheelStatus_call` and
`ArkDbus::reply_to_requestKeyValue_call` — neither exists in ArkPro's
`CarplayLinkProxy.h`. The on-device protocol is a **superset** of what's in this public
snapshot (steering-wheel-control passthrough and a generic key/value call were added
after ArkPro's 2017 vintage). Don't assume ArkPro's interface is complete — treat it as a
confirmed common ancestor, not the current spec.

### 6b. `ark_display_fb.h` ioctl table — direct line-by-line diff against our driver, much stronger match than first thought

Follow-up pass: diffed ArkPro's full `ARKFB_*` ioctl table (`ark_display_fb.h`) against
the actual implementation in this project's own
`linux-arkmicro/linux/drivers/video/fbdev/arkmicro/ark1668_lcdc_funcs.c` +
`ark_lcdc_common.h` (not the empty `ark1668_lcdfb.c` — the real ioctl switch lives in
`ark1668_lcdc_funcs.c`, `.fb_ioctl = ark1668_lcdfb_ioctl` just wires it up). That header
carries two generations of ioctl definitions side by side: an older "reconstructed"
guess set (commented as not what stock actually calls) and a `_REAL`/disassembly- or
decompile-confirmed set (each with its own dated comment citing
`docs/historical/DEVICE_TEST_CHECKLIST_2026-07-18.md`). Only the confirmed set is meaningful to
compare.

**Every one of our disassembly-confirmed ioctl numbers matches ArkPro's vendor-source
number at the same `nr`, same `_IOW`/`_IOR`/`_IO` direction, same operation** — not just
the show/hide-window pair already known about:

| nr | direction | ArkPro (ARK1680, 2013) | Ours (ARK1668, disassembly-confirmed) | Match |
|----|-----------|-------------------------|----------------------------------------|-------|
| 38 | `_IO` | `ARKFB_WAITFORVSYNC` | `ARKFB_WAITFORVSYNC` | exact |
| 41 | `_IOW` | `ARKFB_SET_BLEND` | `ARKFB_SET_BLEND` (real, §65) | exact |
| 42 | `_IOW` | `ARKFB_SET_WINDOW_ADDR` | `ARKFB_SET_FB_ADDR` (real, §73) | same op, renamed |
| 43 | `_IO` | `ARKFB_SHOW_WINDOW` | `ARKFB_SHOW_WINDOW_REAL` | exact ([[project_hide_window_ioctl_fix]]) |
| 44 | `_IO` | `ARKFB_HIDE_WINDOW` | `ARKFB_HIDE_WINDOW_REAL` | exact ([[project_hide_window_ioctl_fix]]) |
| 54 | `_IOR` | `ARKFB_GET_WINDOW_ADDR` | `ARKFB_GET_FB_ADDR` (real) | same op, renamed |
| 55 | `_IOW` | `ARKFB_UPDATE_VIDEO_WINDOW` | `ARKFB_INIT_VIDEO_DISPLAY` (real) | same op family |
| 56 | `_IOW` | `ARKFB_SET_VIDEO_WINDOW_ADDR` | `ARKFB_SET_VIDEO_ADDR_RAW` (real) | exact |

Seven confirmed matches, all on the *disassembly-confirmed* side of our header, zero
mismatches. This is decisive independent validation of essentially the entire real ioctl
table this project reverse-engineered the hard way (Ghidra decompile + ARM disassembly of
the real deployed `libarkcmn.so`/stock kernel), not just the two ioctls already known
about. Strengthens confidence in that whole reverse-engineering effort — the numbering
scheme (`nr` + `_IOW`/`_IOR`/`_IO` direction) is a genuine stable ArkMicro platform
convention carried across at least two SoC generations, not something that drifted.

**Correction to the prior note in this section**: earlier I flagged `ARKFB_SET_OSD_ALPHA`
(49), `ARKFB_GET/SET_OSD_CFG` (51/52), `ARKFB_SET_PRIORITY` (47), `ARKFB_SET_WINDOW_POINT`
(53) as "worth checking against our numbers." Having now done that check: **don't** —
those exact `nr` values are already used on our board for different, independently
disassembly-confirmed ioctls (47=`ARKFB_SET_REG_VALUE`, 49=`ARKFB_GET_WINDOW_ADDR`,
51=`ARKFB_SET_SCREEN_INFO`, 52=`ARKFB_GET_PLATFORM_INFO`, 53=`ARKFB_GET_WINDOW_FORMAT`).
The `nr` space was evidently reused for different purposes as the window/layer model
evolved between ARK1680 and ARK1668 — same convention, different assignment. Treat any
`nr` above 44 as generation-specific unless independently confirmed like the table above.

One open item this comparison surfaces: our own `ARKFB_SET_WINDOW_PRIORITY` (`nr` 63, a
5-layer-priority struct: video/video2/win1/win2/win3) has **no disassembly-confirmed
`_REAL` counterpart** in our header, unlike every other ioctl in the confirmed set — it's
implemented with vendor-realistic detail (matches naming of real
`ark1668_lcdc_set_video_priority()` etc.) but wasn't flagged with the usual "confirmed via
disassembly, see checklist §NN" comment the others have. ArkPro's `ARKFB_SET_PRIORITY`
(47, a single `ark_osd_priority` struct) doesn't help confirm it — different `nr`,
different shape, different generation. Worth flagging next time this ioctl is touched: it
may still be an unverified assumption, not a confirmed number.

### 6c. `DiskDeviceWatcher` — different mechanism than ours, not a gap

`DiskDeviceWatcher/DiskDeviceWatcher.cpp` implements USB hotplug detection via a **raw
`NETLINK_KOBJECT_UEVENT` socket read directly in the Qt app** (regex-parsing
`add@/.../block/...` uevent lines), not via `mdev`/`mdev.conf`. This is architecturally
different from this project's just-implemented, hardware-confirmed
[[project_usb_udisk_automount]] approach (`mdev.conf` + `usb_domount.sh` shell hooks).
Both are valid ways to solve the same problem — this is a **generation/design
difference**, not evidence our mdev-based approach is wrong. No action needed.

### 6d. `SettingPersistent` — confirms the settings-persistence pattern, not the values

`SettingPersistent/SettingPersistent.cpp` stores `Language`/`Brightness`/`Contrast`/`Hue`
in a `QSettings` INI file at `/data/Setting.ini` (`/tmp/Setting.ini` under a `gcc`/x86
dev build), with `settings.sync()` called on every write. This confirms the general
pattern already found on our own device
([[project_language_setting_userdata]] — `/data/msncfg/Setting.config` as the live
runtime source, `FactoryConfig.ini` only seeding it once) is a genuine platform-wide
ArkMicro convention, not something specific to our board. Default values don't transfer
(`Language` defaults to `1` here vs. our device's `4096`/`4097` — different enum scale
for a different generation) so this doesn't resolve the open Language-default question.

### 6e. `AutoConnect` — false lead, not Android-Auto-related

Despite the name, `AutoConnect/AutoConnect.cpp` is a generic Qt signal/slot
auto-connection-by-namesake utility (reflection over `QMetaObject` to wire up
same-named signals/slots between two `QObject`s) — nothing to do with Android Auto or USB
accessory connection. Ruled out.

### 6f. Audio driver comparison — ours has already surpassed ArkPro's, no gap

Diffed `kernel/drivers/ark/audio/ark_i2s.c` (536 lines) and `ark-sddac-codec.c` (332
lines) against our own `ark1668_i2s.c` (953 lines) and `ark1668-sddac-codec.c` (312
lines). Ours is substantially larger for the I2S driver specifically — expected, given
the extensive DMA/scheduling work from the just-resolved
[[project_aa_audio_stutter_investigation]] (tasklet priority, DMA channel priority,
dmaengine_pcm bypass — none of which existed in this 2012-2013-era ARK1680 driver). The
register-bitfield header (`ark_i2s_sddac_regs.h`) diff shows our version has *more*
bitfield definitions (`ARK_SYS_I2S_BCLK/MCLK/SADATA/SYNC`), not fewer. No gap here —
this is an area where the project's own hardware-confirmed work has already moved past
what this reference snapshot offers.

### 6g. `display_effect` fn-pointer (the open ITU656 second-Oops bug) — genuinely absent, ArkPro can't help

Checked whether ArkPro's driver tree could shed light on
[[project_itu656_dvr_ioctl_second_oops]] (a `display_effect` function-pointer struct
member getting corrupted). Confirmed: **no `itu656` driver, no `rn6752`/`ark7116`
camera-decoder-chip code, no `display_effect`/`dvr_start_cb`/`get_progressive` struct
member anywhere in the ArkPro tree.** That whole subsystem (`ark1668_itu656.c`,
`ark1668_vin.h`, the `priv_data` ops struct with `display_effect`) is genuinely
ARK1668-specific hardware (an external analog-video decoder chip, `rn6752`) that doesn't
exist in the ARK1680 generation at all — not a case of ArkPro having an older/different
version of the same struct, the concept isn't present. This bug needs to keep being
chased on our own kernel tree/live hardware; this vendor reference has nothing to
contribute to it.

### 6h. Not present in this snapshot

No DVR/reverse-camera kernel driver source beyond the userspace `arkapi_dvr_*` function
declarations in `ark_api.h` (spot-checked earlier — no implementation body, see §6g for
why), no CAN bus code, no MCU-handshake/UART protocol code searchable anywhere in the
tree. The PWM/backlight driver (`kernel/drivers/ark/pwm/ark_pwm.c`) is `/proc`-driven,
not ioctl-driven, and this project's backlight handling is already documented elsewhere
(`docs/HARDWARE_AND_SOC_REFERENCE.md`) — no new finding there. `ark_display_v4l2.c`
implements the ARK1680 camera-capture path via **standard V4L2** (`video_ioctl2`, no
custom ioctls) rather than a private ioctl device — confirms the two generations took
different architectural approaches to camera capture, so it's not useful as a structural
reference for our custom-ioctl `itu656` driver either.

### 6i. Net assessment

More valuable than first assessed. §6b's ioctl-table diff is genuine, strong independent
corroboration of nearly this project's entire disassembly-confirmed display ioctl table
(7 of 7 checked numbers match ArkPro's vendor source exactly in `nr` + direction), not
just the two ioctls already known about — and it surfaced one real open gap
(`ARKFB_SET_WINDOW_PRIORITY` still lacks a confirmed-via-disassembly citation). Also
confirms the `/data`-based settings-persistence pattern (§6d) and produces one genuinely
new, concrete lead for continued work (§6a's `Local.DbusServer.Carplay` interface and its
`type`/`status` enum, with named gaps — `requestWheelStatus`, `requestKeyValue` — for
anyone decoding the wired-AA/CarPlay control-channel protocol further). Confirmed **not**
useful for the open ITU656/`display_effect` bug (§6g — genuinely absent hardware/code) or
as directly reusable code anywhere (wrong chip generation, wrong Qt-app architecture at
the top level, audio driver already surpassed) the way `ark1668ed-bsp` was in §2-§5.

### 6j. Is the application itself buildable? Yes — and it already was, by the vendor

Checked whether `ArkSdk.pro` (the top-level `TEMPLATE = subdirs` project covering all 13
components) is actually a complete, buildable source tree, or just a source dump.

**Source completeness, static checks:**
- All D-Bus proxy/adaptor code (`CarplayLinkProxy`, `MirrorLinkProxy`, `CarlifeLinkProxy`,
  `SettingServiceProxy`, `MultimediaServiceProxy`, `AudioServiceProxy`) is **checked in
  pre-generated**, not produced by a `DBUS_ADAPTORS`/`DBUS_INTERFACES` qmake build step —
  confirmed no `.pro` file uses either directive. The raw `.xml` interface definitions
  present alongside them are reference copies only, not live build inputs. No codegen gap.
- `Launcher.pro` has a handful of `#`-commented `SOURCES`/`HEADERS` lines
  (`LinkWidget/CarplayLinkWidget/*`, `MusicInformation.*`) — checked whether anything
  live still references the disabled copy: it doesn't. `LinkWidget.cpp` has its own
  `#include`/instantiation of that same class commented out in matching fashion, and the
  actually-used `CarplayLinkWidget` lives in a separate, still-compiled directory
  (`UserInterface/MainWidget/CarPlayWidget/CarplayLinkWidget/`) — a leftover duplicate
  from a refactor, not a build-breaking omission.
- **Real structural bug found**: `MultimediaService/MultimediaService.pro` unconditionally
  links `-L$$PWD/TagLib/Library/arm -ltag -lConvert -lQtConvert` under a bare
  `unix:!macx` guard, with no x86 branch — unlike `ArkSdk.pri`'s `-larkcmn`, which is
  properly gated behind an `arm-none-linux-gnueabi-gcc` compiler-name check. Those three
  `.so` files are genuine ARM EABI5 shared objects (confirmed via `file`), so this line
  would fail an x86 link on its face. In practice this is harmless: `CONFIG +=
  staticlib` means qmake never actually invokes the linker for this target (`LIBS +=` is
  a no-op for a `.a` archive step), and the top-level `MultimediaService.pro` (the one in
  `ArkSdk.pro`'s `SUBDIRS`) doesn't even compile `ID3TagParser.cpp` — the one file that
  calls into TagLib — so no unresolved symbol ever reaches a real link step either. Still
  a genuine authoring bug in the vendor's own project file (harmless only by accident),
  and a second oddity sits right next to it: `QMAKE_POST_LINK` is assigned twice in the
  same file, so the second assignment (`rm -f .../Launcher/$$ARCHITECTURE/Launcher`)
  silently discards the first (`Script.sh`) — looks like a copy-paste leftover from
  another component's `.pro` file, not intentional.

**Decisive practical evidence — don't need to guess, the vendor already built it**:
`Package/Launcher/x86/Launcher` is a genuine, unstripped **x86-64 ELF executable with
debug_info** checked into the repo (confirmed via `file`), alongside matching `.o`/`.a`
build output for every component in both `arm/` and `x86/` subfolders under `Package/`.
This is conclusive: the vendor's own build system produced a complete, working desktop
build of this exact source tree, not just the ARM target. We don't need to prove
buildability by rebuilding — the artifact already answers it.

**The actual blocker on this machine isn't source completeness, it's toolchain age**:
`ldd` on that x86-64 binary shows `libQtCore.so.4`, `libQtGui.so.4`,
`libQtDBus.so.4`, `libQtXml.so.4`, `libQtSvg.so.4`, `libQtSerialPort.so.1` — all
**Qt4**, not Qt5, none present on this machine, and Qt4 has been out of Debian's repos
for years (`libqt4-core` etc. don't exist in bookworm). Cross-checked against
`cstech-ip17-rootfs` (§1): it ships a genuine **Qt 4.7.4** ARM build at
`/usr/local/Qt4.7.4/lib/` — confirms the whole ArkMicro platform family, including our
own board's MsnCoreApp, standardizes on that exact Qt4 version. Not directly usable here
(ARM binaries, wrong architecture for running the x86 `Launcher` executable), but
confirms what version we'd need to target for a real from-source rebuild. Some source
files do carry `#if QT_VERSION >= 0x050000` guards (e.g. `AutoConnect.cpp`), suggesting
partial, inconsistent effort toward Qt5 portability that was never fully carried through
the whole tree.

**Practical options, in order of effort**: (1) treat the checked-in `Package/` binaries
and objects as sufficient — no rebuild needed to read/run/inspect this reference
implementation; (2) run the existing unstripped x86-64 `Launcher` binary inside an old
Debian/Ubuntu release (or container) that still carries Qt4 packages, to see the
reference UI live; (3) attempt a real Qt5 port using the existing `QT_VERSION` guards as
a starting point — genuine, non-trivial work across ~1650 files, not attempted here.

### 6k. Option 2 done — the reference UI actually runs, live, on this dev machine

No container/VM turned out to be necessary. Debian's Qt4 packages (`libqtcore4`,
`libqtdbus4`, `libqtgui4`, `libqt4-svg`, `libqt4-xml`, `libqt4-network`, `libqt4-sql`,
all `4:4.8.6+dfsg-2~bpo70+1`) are still hosted on `snapshot.debian.org` and install
cleanly into a **local, non-root prefix** via `apt-get download` (works without root,
same pattern as this project's earlier `qemu-user-static` extraction workflow) +
`dpkg-deb -x`. Two remaining gaps, both worked around:
- `libpng12.so.0` — same snapshot-archive approach, one extra package.
- `libQtSerialPort.so.1` — genuinely doesn't exist anywhere as a Qt4 Debian package (Qt5
  only). Built as an empty stub (`gcc -shared -Wl,-soname,libQtSerialPort.so.1`) — the
  dynamic loader only needs the SONAME to resolve at load time; Linux's default lazy PLT
  binding means real serial-port symbols are only needed if a code path that actually
  calls them gets exercised. Never happened during a basic UI walkthrough.

With `LD_LIBRARY_PATH` pointing at that prefix and a real `DISPLAY=:0` (this dev machine
runs a normal desktop session), `Package/Launcher/x86/Launcher` **launches as a real,
live 800x480 window** — spawns its service subprocesses (`-multimedia`, `-setting`,
`-audiomanager`) exactly as the multi-process/D-Bus architecture in §6a-6d implied, shows
a real-time clock (confirms it's genuinely executing, not a cached asset), and responds
to real mouse input (`xdotool`, itself fetched the same no-root way). Two confirmed
screens: **Home** (CarPlay / 双屏互联 dual-screen-mirroring / 系统设置 tiles) and
**Settings > General** (sidebar: General/Calibration+Time, Language, Volume, Version —
same settings taxonomy as this project's own `docs/SETTINGS_REFERENCE.md`).

This is now a repeatable recipe, not a one-off — worth reaching for again if a future
UI/UX question about the ArkMicro reference design comes up (e.g. what a screen is
*supposed* to look like, or how a settings flow is organized), rather than re-deriving it
from static source reading alone.
