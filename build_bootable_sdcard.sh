#!/bin/bash
# build_bootable_sdcard.sh - Interactive bootable SD card builder for ARK1680
#
# Builds an SD card image that boots the kernel and rootfs from removable
# media WITHOUT writing to NAND — see build_update.sh for the (destructive)
# NAND flash update tool instead.
#
# CONFIRMED ON REAL HARDWARE to interrupt boot and drop to a U-Boot prompt —
# see docs/UBOOT_SDBOOT_INVESTIGATION.md §8 and experimental_sdboot/README.md.
# The U-Boot patch (env patch + NAND-offset redirect, always applied
# together — the patch alone with a valid env has no effect) gets you to an
# interactive U-Boot prompt; it does not auto-continue to a full boot. From
# that prompt, use the README's "Manual SD Card Boot" section to continue
# (types the boot commands by hand, no additional files needed).
#
# Run under Linux or WSL. Requires: parted, dosfstools, e2fsprogs, rsync, python3
#   sudo apt install parted dosfstools e2fsprogs rsync python3
#
# 2026-07-17: this is a rewrite of the previous build_bootable_sdcard.sh
# (archived at archive/build_bootable_sdcard.sh.pre-overlay). Rootfs patches
# (rcS, profile, wifi_ap.sh, inittab, libGAL.so) that used to be applied via
# python3/regex transforms at build time now ship as already-patched files
# in firmware_overlay/ — rsynced onto p2 straight after the main
# rootfs sync. See firmware_overlay/README.md for what's there and
# why. CarSyncTech CSTech-202511-IP17 rootfs support and the (confirmed
# non-functional) initramfs boot path were both dropped in this rewrite —
# see the archived script if either is needed again. The default
# diagnostic tools (i2c-scan, ark-ts-test, lcd-test, nano, htop, dmesg,
# *-test.sh, etc.) also moved into firmware_overlay/usr/bin/ and are
# now unconditionally part of the rootfs — no more install_diag_tools
# toggle or --diag-tools/--no-diag-tools flags.
#
# Usage:
#   ./build_bootable_sdcard.sh [options]
#
# All toggleable options can also be set in the interactive menu; paths and
# sizes are CLI-flag-only.
#
# Options:
#   --image PATH       Output image file path (default: sd_bootable/sd_boot.img)
#   --device PATH      Write directly to block device (e.g. /dev/sdb) instead
#   --size MB          Total image size in MB (default: 512)
#   --uboot PATH       Prebuilt UBOOT.BIN to place on p1 as-is (skips patching)
#   --uboot-src PATH   Raw uboot.bin source — patched via build_tools/patch_uboot.py, never modified
#   --no-patch-uboot   Use the source uboot.bin as-is without patching
#   --reloc-env        When patching the stock U-Boot (requires --no-new-uboot),
#                      RELOCATE the compiled-in default env via build_tools/patch_uboot_env.py
#                      so a full SD-boot command AUTO-boots. ON by default. See
#                      docs/UBOOT_SDBOOT_INVESTIGATION.md §10.
#   --no-reloc-env     Fall back to the sdscript patch that only drops to a
#                      U-Boot prompt (hardware-confirmed) instead of auto-booting.
#   --root DEVICE      Root device for the generated uEnv.txt bootargs (default: /dev/mmcblk0p2)
#   --dtb PATH         DTB file to place on p1 (only used if --new-uboot is active)
#   --new-uboot        Use the freshly compiled Limcet P305 U-Boot from
#                      linux-arkmicro/u-boot/UBOOT.BIN. Patching is bypassed
#                      and a uEnv.txt text environment is written to p1.
#                      ON by default.
#   --no-new-uboot     Disable the compiled U-Boot replacement (use stock U-Boot)
#   --bootlogo PATH    Raw 800x480x32bpp framebuffer (see build_tools/convert_bootlogo.py)
#                      to place on p1 as bootlogo.raw, for the boot logo
#                      shown by ark_show_bootlogo() in the compiled U-Boot.
#   --stock-uboot PATH Stock dumped U-Boot binary to place on p1 as
#                      stock_uboot.bin, used by the `bootstock` command to
#                      chainload the original bootloader (bypasses this
#                      build's NAND driver). Defaults to the dump already in
#                      this repo; pass --no-stock-uboot to skip copying it.
#                      Not required — boot proceeds normally without it.
#   --kernel PATH      zImage (or zImage.w_dtb) to place on p1
#   --new-kernel       Auto-detect and use the freshly compiled Limcet P305 kernel
#                      from linux-arkmicro/linux/arch/arm/boot/zImage (prefers
#                      zImage.w_dtb if present) and install compiled modules from
#                      linux-arkmicro/compiled_modules/ into the p2 rootfs.
#                      This replaces the stock NAND kernel on the SD image.
#                      Also determines whether firmware_overlay/ (which
#                      targets 4.19.192 kernel compatibility) gets applied.
#                      ON by default.
#   --no-new-kernel    Explicitly disable new-kernel replacement (use stock kernel;
#                      firmware_overlay/ is NOT applied in this mode)
#   --kernel-build-dir DIR  Path to linux-arkmicro build root (auto-detected if
#                           sibling of script dir or in ~/Downloads/linux-arkmicro)
#   --modules-dir DIR  Path to compiled_modules/ directory (default: auto-detected
#                      from kernel build dir). Modules installed to
#                      /lib/modules/<version>/ on p2.
#   --rootfs-dir DIR   Rootfs source directory (mounted as /)
#   --userdata-dir DIR Userdata source directory (mounted as /data)
#   --no-userdata      Leave p3 formatted but empty
#   --no-mtd-redirect  Leave bootlogo/bootanimation/reversingtrack/Unicode
#                      reading from existing NAND data instead of SD
#   --telnetd          Install a passwordless root telnetd (busybox telnetd
#                      -l /bin/sh, port 23) into rcS, started right after
#                      mdev -s. OFF by default — this is an unauthenticated
#                      root shell reachable by anything on the network. Same
#                      mechanism confirmed working via msn_autocopy on stock
#                      firmware — see msn_autocopy/README.md for the
#                      devpts-mount fix this depends on and its debugging
#                      history.
#   --no-telnetd       Explicitly disable telnetd (already the default)
#   --non-interactive  Skip the menu, use defaults/flags only
#   --dry-run          Show commands without executing
#   --help             Show this help

set -euo pipefail

# parted, mkfs.fat, mkfs.ext4, and losetup all install to /usr/sbin or /sbin
# on Debian/Ubuntu, which isn't always on $PATH for non-root shells (WSL,
# non-login shells). Without this, both the requirements check and the
# actual build steps below would report these tools missing even when
# installed.
export PATH="$PATH:/usr/sbin:/sbin"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Colours
# ---------------------------------------------------------------------------
RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'
CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; RESET='\033[0m'

info()    { echo -e "${CYAN}  $*${RESET}"; }
success() { echo -e "${GREEN}  ✔ $*${RESET}"; }
warn()    { echo -e "${YELLOW}  ⚠ $*${RESET}"; }
die()     { echo -e "${RED}ERROR: $*${RESET}" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Step tracker — gives the build phase a Claude-Code-ish progress readout:
# a boxed header per step, an elapsed-time footer, and a final summary table
# so the user can see at a glance what ran and how long it took.
# ---------------------------------------------------------------------------
STEP_TOTAL=13
declare -a STEP_TITLES=() STEP_ELAPSED=() STEP_STATUS=()
STEP_T0=0

begin_step() {
    local n="$1" title="$2"
    STEP_TITLES[$n]="$title"
    STEP_T0=$(date +%s)
    echo ""
    echo -e "${CYAN}${BOLD}┌─ [$n/$STEP_TOTAL] $title${RESET}"
}

end_step() {
    local n="$1" status="${2:-ok}"
    local dt=$(( $(date +%s) - STEP_T0 ))
    STEP_ELAPSED[$n]="$dt"
    STEP_STATUS[$n]="$status"
    if [[ "$status" == "skip" ]]; then
        echo -e "${CYAN}└─${RESET} ${DIM}skipped${RESET}"
    else
        echo -e "${CYAN}└─${RESET} ${GREEN}done${RESET} ${DIM}(${dt}s)${RESET}"
    fi
}

print_step_summary() {
    local total=0 n mark
    echo ""
	echo -e "  ${DIM}───────────────────────────────────────────────────────────────────${RESET}"
    echo -e "  ${BOLD}BUILD SUMMARY${RESET}"
    for n in "${!STEP_TITLES[@]}"; do
        [[ "${STEP_STATUS[$n]}" == "skip" ]] && mark="${DIM}○${RESET}" || mark="${GREEN}✔${RESET}"
        printf "   %b %-2s %-28s ${DIM}%s${RESET}\n" "$mark" "$n" "${STEP_TITLES[$n]}" \
            "$([[ "${STEP_STATUS[$n]}" == "skip" ]] && echo "skipped" || echo "${STEP_ELAPSED[$n]}")"
        [[ "${STEP_STATUS[$n]}" != "skip" ]] && total=$(( total + STEP_ELAPSED[$n] ))
    done
    echo -e "  ${DIM}───────────────────────────────────────────────────────────────────${RESET}"
}

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
OUTPUT_DIR="$SCRIPT_DIR/sd_bootable"

IMAGE=""
DEVICE=""
IMAGE_SIZE_MB=512
P1_SIZE_MB=64
P2_SIZE_MB=300

UBOOT_BIN=""                                  # prebuilt binary (--uboot); used as-is
UBOOT_SRC=""                                  # raw source uboot.bin; patched, never modified
UBOOT_OUT="$OUTPUT_DIR/uboot_selfcontained.bin"  # patched output
UENV_OUT="$OUTPUT_DIR/uEnv.txt"                # generated uEnv.txt (see generate_uenv_txt)
# Confirmed on real hardware: patches U-Boot AND forces the NAND env CRC to
# fail in the same step (always passes --patch-nand-offset to build_tools/patch_uboot.py)
# — the patch alone with a valid env is confirmed to have no effect, so
# there's no reason to offer them as two separate toggles.
PATCH_UBOOT=false                             # run build_tools/patch_uboot.py on the source
# --reloc-env: when patching the stock U-Boot, relocate the compiled-in default
# env (build_tools/patch_uboot_env.py) into free image space so a full SD-boot command fits
# and AUTO-boots, rather than the sdscript patch that only drops to a prompt.
# ON by default; pass --no-reloc-env to fall back to the prompt-drop patch.
# Static-verified, not yet hardware-tested — see docs/UBOOT_SDBOOT_INVESTIGATION.md §10.
RELOC_ENV=true
ROOT_DEV="/dev/mmcblk0p2"                     # root= in the generated uEnv.txt (matches p2 rootfs)
KERNEL_BIN=""
BOOTLOGO_RAW="$SCRIPT_DIR/sd_bootable/bootlogo.raw"                               # raw framebuffer (--bootlogo) for p1/bootlogo.raw
STOCK_UBOOT_BIN="$SCRIPT_DIR/firmware_source/mtd1-mtd2_uboot/uboot.bin"  # for p1/uboot_stock.bin, used by the `bootstock` chainload command
HYBRID_UBOOT_BIN="$SCRIPT_DIR/sd_bootable/uboot_hybrid.bin"             # for p1/uboot_hybrid.bin, used by the `boothybrid` chainload command
ARKDATA_INI="$SCRIPT_DIR/sd_bootable/arkdata.ini"  # for p1/arkdata.ini -- real calibrated LCD timing/panel config, dumped from the NAND "arkdata" partition. Without this, ark1668_arkdata_ini.c's fatload always fails: U-Boot's own splash-screen screen_info falls back to compiled defaults, AND (2026-07-19) ft_board_setup() no longer has anything to patch the kernel's DTB display-timings node with either -- see docs/DISPLAY_SUBSYSTEM.md
                                                                         # (previously pointed at firmware_source/mtd5_kernel/modules/mtd4_arkdata/arkdata.ini, a path that never existed -- the -f guard below silently skipped copying arkdata.ini on every build. sd_bootable/'s copy is used here, not firmware_source/mtd4_arkdata/'s, because the latter has CRLF line endings and this project has repeatedly hit CRLF-parsing bugs elsewhere in the boot pipeline; both copies already have RgbMode=5 applied.)
DTB_BIN=""
ROOTFS_DIR=""
USERDATA_DIR=""
RECONSTRUCTED_DIR=""
OVERLAY_DIR="$SCRIPT_DIR/firmware_overlay"  # already-patched files rsynced onto p2 after the main rootfs sync — see firmware_overlay/README.md
SKIP_USERDATA=false
SKIP_MTD_REDIRECT=false
INSTALL_TELNETD=false                         # unauthenticated root telnetd on port 23 — OFF by default, opt-in only
NEW_KERNEL_MODE=true                          # replace stock kernel with freshly compiled Limcet P305 kernel — ON by default; pass --no-new-kernel to use the stock kernel
NEW_UBOOT_MODE=true                           # replace stock U-Boot with freshly compiled Limcet P305 U-Boot — ON by default; pass --no-new-uboot to use the stock U-Boot
KERNEL_BUILD_DIR=""                           # path to linux-arkmicro/ build root (auto-detected)
MODULES_DIR=""                                # path to compiled_modules/ (auto-detected from KERNEL_BUILD_DIR)
NON_INTERACTIVE=false
DRY_RUN=false

# NAND partition data — keyed by mtd number, value is relative path under RECONSTRUCTED_DIR
# Files are copied to /nanddata/ on p2 and symlinked from /dev/mtdN at boot
declare -A NAND_PARTS=(
    [8]="mtd8_bootlogo/bootlogo"
    [9]="mtd9_bootanimation/bootanimation"
    [10]="mtd10_reversingtrack/reversingtrack"
    [11]="mtd11_unicode/unicode"
)

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
usage() { grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \?//'; exit 0; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --image|-i)        IMAGE="$2"; shift 2 ;;
        --device|-d)       DEVICE="$2"; shift 2 ;;
        --size)            IMAGE_SIZE_MB="$2"; shift 2 ;;
        --uboot)           UBOOT_BIN="$2"; PATCH_UBOOT=false; shift 2 ;;
        --uboot-src)       UBOOT_SRC="$2"; shift 2 ;;
        --no-patch-uboot)  PATCH_UBOOT=false; shift ;;
        --reloc-env)       RELOC_ENV=true; shift ;;
        --no-reloc-env)    RELOC_ENV=false; shift ;;
        --root)            ROOT_DEV="$2"; shift 2 ;;
        --kernel)          KERNEL_BIN="$2"; shift 2 ;;
        --bootlogo)        BOOTLOGO_RAW="$2"; shift 2 ;;
        --stock-uboot)     STOCK_UBOOT_BIN="$2"; shift 2 ;;
        --no-stock-uboot)  STOCK_UBOOT_BIN=""; shift ;;
        --dtb)             DTB_BIN="$2"; shift 2 ;;
        --new-kernel)      NEW_KERNEL_MODE=true; shift ;;
        --no-new-kernel)   NEW_KERNEL_MODE=false; shift ;;
        --kernel-build-dir) KERNEL_BUILD_DIR="$2"; shift 2 ;;
        --modules-dir)     MODULES_DIR="$2"; shift 2 ;;
        --rootfs-dir)      ROOTFS_DIR="$2"; shift 2 ;;
        --userdata-dir)    USERDATA_DIR="$2"; shift 2 ;;
        --no-userdata)     SKIP_USERDATA=true; shift ;;
        --no-mtd-redirect) SKIP_MTD_REDIRECT=true; shift ;;
        --new-uboot)       NEW_UBOOT_MODE=true; shift ;;
        --no-new-uboot)    NEW_UBOOT_MODE=false; shift ;;
        --telnetd)         INSTALL_TELNETD=true; shift ;;
        --no-telnetd)      INSTALL_TELNETD=false; shift ;;
        --non-interactive) NON_INTERACTIVE=true; shift ;;
        --dry-run|-n)      DRY_RUN=true; shift ;;
        --help|-h)         usage ;;
        *) die "Unknown option: $1" ;;
    esac
done

[[ -z "$IMAGE" && -z "$DEVICE" ]] && IMAGE="$OUTPUT_DIR/sd_boot.img"
[[ -n "$IMAGE" && -n "$DEVICE" ]] && die "Specify --image or --device, not both"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
run() {
    if $DRY_RUN; then echo "  [dry-run] $*"; else "$@"; fi
}

# ---------------------------------------------------------------------------
# Auto-detect paths
# ---------------------------------------------------------------------------
autodetect() {
    [[ -z "$RECONSTRUCTED_DIR" ]] && {
        local c="$SCRIPT_DIR/firmware_source"
        [[ -d "$c" ]] && RECONSTRUCTED_DIR="$c"
    }
    # Auto-detect linux-arkmicro kernel build directory
    [[ -z "$KERNEL_BUILD_DIR" ]] && {
        for c in \
            "$SCRIPT_DIR/../linux-arkmicro" \
            "/home/osboxes/Downloads/linux-arkmicro" \
            "$HOME/Downloads/linux-arkmicro"
        do [[ -d "$c/linux" ]] && { KERNEL_BUILD_DIR="$(realpath "$c")"; break; }; done
    }
    # If new-kernel mode and build dir found, always resolve kernel + modules paths
    if $NEW_KERNEL_MODE && [[ -n "$KERNEL_BUILD_DIR" ]]; then
        local nk_paths=()
        if $NEW_UBOOT_MODE; then
            # With new U-Boot, we load the DTB separately so U-Boot can update bootargs (e.g. root=/dev/mmcblk0p2).
            # We MUST use the raw zImage (no appended DTB) because otherwise the kernel ignores U-Boot's DTB.
            nk_paths+=("$KERNEL_BUILD_DIR/linux/arch/arm/boot/zImage")
            nk_paths+=("$KERNEL_BUILD_DIR/zImage.w_dtb")
        else
            nk_paths+=("$KERNEL_BUILD_DIR/zImage.w_dtb")
            nk_paths+=("$KERNEL_BUILD_DIR/linux/arch/arm/boot/zImage")
        fi
        local nk
        for nk in "${nk_paths[@]}"; do
            [[ -f "$nk" ]] && { KERNEL_BIN="$nk"; break; };
        done
        # Always resolve modules dir when new-kernel is active
        [[ -d "$KERNEL_BUILD_DIR/compiled_modules" ]] && \
            MODULES_DIR="$KERNEL_BUILD_DIR/compiled_modules"
    else
        # Clear new-kernel-specific paths when mode is off
        MODULES_DIR=""
    fi
    # If new-uboot mode and build dir found, auto-detect the u-boot.bin
    if $NEW_UBOOT_MODE; then
        if [[ -n "$KERNEL_BUILD_DIR" ]]; then
            [[ -z "$UBOOT_BIN" ]] && {
                [[ -f "$KERNEL_BUILD_DIR/u-boot/u-boot.bin" ]] && \
                    UBOOT_BIN="$KERNEL_BUILD_DIR/u-boot/u-boot.bin"
            }
            [[ -z "$DTB_BIN" ]] && {
                [[ -f "$KERNEL_BUILD_DIR/linux/arch/arm/boot/dts/ark1668_limcet_p305.dtb" ]] && \
                    DTB_BIN="$KERNEL_BUILD_DIR/linux/arch/arm/boot/dts/ark1668_limcet_p305.dtb"
            }
        fi
        # Fallback to local sd_bootable dir
        [[ -z "$DTB_BIN" ]] && {
            [[ -f "$OUTPUT_DIR/ark1668_limcet_p305.dtb" ]] && \
                DTB_BIN="$OUTPUT_DIR/ark1668_limcet_p305.dtb"
        }
    fi
    # Raw source u-boot — patched into UBOOT_OUT, never modified in place.
    # The 'sdscript' patch mode (see prepare_uboot) only needs ~52 bytes of
    # compiled-in env space, so the raw NAND-dumped uboot.bin (no reserved
    # env buffer) works fine here — unlike the old 'sdboot' preset (~500 B),
    # which needed a real BSP-compiled binary this project doesn't have (see
    # docs/UBOOT_SDBOOT_INVESTIGATION.md, corrupted/README.md for why).
    [[ -z "$UBOOT_BIN" && -z "$UBOOT_SRC" ]] && {
        for c in \
            "$SCRIPT_DIR/firmware_source/mtd1-mtd2_uboot/uboot.bin" 
        do [[ -f "$c" ]] && { UBOOT_SRC="$c"; break; }; done
    }
    # Fall back to stock kernel if new-kernel not requested
    [[ -z "$KERNEL_BIN" ]] && {
        for c in \
            "$SCRIPT_DIR/firmware_source/mtd5_kernel/zImage"
        do [[ -f "$c" ]] && { KERNEL_BIN="$c"; break; }; done
    }
    [[ -z "$ROOTFS_DIR" ]] && {
        local c="$SCRIPT_DIR/firmware_source/mtd6_rootfs"
        [[ -d "$c" ]] && ROOTFS_DIR="$c"
    }
    [[ -z "$USERDATA_DIR" ]] && {
        local c="$SCRIPT_DIR/firmware_source/mtd7_userdata"
        [[ -d "$c" ]] && USERDATA_DIR="$c"
    }
    return 0
}

# ---------------------------------------------------------------------------
# Requirements — shown once at startup, same pattern as build_update.sh.
# ---------------------------------------------------------------------------
REQUIREMENTS=(
    "parted|partition the image|parted"
    "mkfs.fat|format p1 as FAT32|dosfstools"
    "mkfs.ext4|format p2/p3 as ext4|e2fsprogs"
    "losetup|attach the image as a loop device|util-linux"
    "rsync|copy rootfs/userdata onto the image|rsync"
    "python3|patch U-Boot|python3"
)

check_requirements() {
    echo -e "${BOLD}  Requirements${RESET}"
    local entry tool desc pkg any_missing=0
    for entry in "${REQUIREMENTS[@]}"; do
        IFS='|' read -r tool desc pkg <<< "$entry"
        if command -v "$tool" &>/dev/null; then
            success "$tool  (${desc})"
        else
            warn "$tool  (${desc}) — not found, install: sudo apt install $pkg"
            any_missing=1
        fi
    done
    echo ""
    if [[ $any_missing -eq 1 ]]; then
        warn "Missing tools will block the build — install them before pressing g."
        $NON_INTERACTIVE || read -rp "  Press Enter to continue..." _
    fi
}

# ---------------------------------------------------------------------------
# Config toggles — the interactive menu's navigable items. Paths and sizes
# stay CLI-flag-only (--uboot-src, --kernel, --rootfs-dir, --userdata-dir,
# --size, --root); the menu only controls these boolean choices.
# Format: "key|label|description|default"
# ---------------------------------------------------------------------------
CONFIG_ITEMS=(
    "use_new_uboot|Install compiled Limcet P305 U-Boot + uEnv|Replaces the stock NAND-dumped UBOOT.BIN on p1 with the freshly compiled Limcet P305 U-Boot. Bypasses patching, and installs UBOOT.BIN, uEnv.txt, and the DTB file on p1.|ON"
    "use_new_kernel|Install compiled Limcet P305 kernel + modules|Replaces the stock NAND kernel on p1 with the freshly compiled zImage.w_dtb from linux-arkmicro/. Also installs the compiled .ko modules into /lib/modules/ on the p2 rootfs, and applies firmware_overlay/ (rcS/profile/wifi_ap.sh/inittab/libGAL.so fixes for 4.19.192 kernel compatibility — see firmware_overlay/README.md). Uses linux-arkmicro/compiled_modules/ auto-detected from build dir|ON"
    "patch_uboot|Patch binary U-Boot for SD auto-boot (env relocation)|Patches U-Boot and forces the NAND env CRC to fail. By default (--reloc-env) it RELOCATES the compiled-in default env so a full SD-boot command fits and the device AUTO-boots from SD — static-verified, NOT yet hardware-tested (see docs/UBOOT_SDBOOT_INVESTIGATION.md §10). Pass --no-reloc-env for the hardware-confirmed fallback that only drops to an interactive U-Boot prompt (then continue via the README's \"Manual SD Card Boot\").|OFF"
    "redirect_mtd_data|Redirect NAND mtd partitions to SD card (bootlogo, bootanimation, reversingtrack, unicode)|Symlinks bootlogo, bootanimation, reversingtrack, and Unicode font (mtd8-11) to files under /nanddata/ on p2 — if off, the device reads these from whatever is already in NAND instead|ON"
    "include_userdata|Include userdata (p3)|Copies the userdata dir to p3 — if off, p3 is left empty and the app populates /data on first boot|ON"
    "disable_msncoreapp_autolaunch|Disable MsnCoreApp auto-launch at login|firmware_overlay/etc/profile already ships with 'MsnCoreApp -qws&' commented out (see docs/ARK1680_TS_REVERSE_ENGINEERING.md) so it doesn't auto-run on every shell login. Turning this OFF re-enables the auto-launch line instead. Run 'start_msn' manually when this is on.|ON"
    "install_telnetd|Install passwordless root telnetd (UNAUTHENTICATED — diagnostic only)|Inserts 'mount -t devpts none /dev/pts' + 'busybox telnetd -l /bin/sh &' into rcS right after mdev -s, giving a root shell on port 23 with no login prompt to anything that can reach the device's network (WiFi AP or USB-NCM). Same mechanism validated working on stock firmware via the msn_autocopy payload (see msn_autocopy/README.md for why the devpts mount is required — telnetd fails silently without it). This is a real, if minor, exposure while active on any network the device joins — OFF by default, opt-in only.|OFF"
)

declare -a CONFIG_SEL
for i in "${!CONFIG_ITEMS[@]}"; do
    IFS='|' read -r _ _ _ default <<< "${CONFIG_ITEMS[$i]}"
    [[ "$default" == "ON" ]] && CONFIG_SEL[$i]=1 || CONFIG_SEL[$i]=0
done
# CLI flags override the menu defaults up front, same as build_update.sh's
# flags override its own PARTITIONS defaults.
# Indices: 0 use_new_uboot, 1 use_new_kernel, 2 patch_uboot, 3 redirect_mtd_data,
#          4 include_userdata, 5 disable_msncoreapp_autolaunch, 6 install_telnetd
$PATCH_UBOOT       || CONFIG_SEL[2]=0
$SKIP_USERDATA     && CONFIG_SEL[4]=0
$SKIP_MTD_REDIRECT && CONFIG_SEL[3]=0
$NEW_KERNEL_MODE   || CONFIG_SEL[1]=0
$NEW_UBOOT_MODE    || CONFIG_SEL[0]=0
$INSTALL_TELNETD   && CONFIG_SEL[6]=1

# ---------------------------------------------------------------------------
# Navigation state — identical pattern to build_update.sh's CURSOR/read_key.
# ---------------------------------------------------------------------------
CURSOR=0

is_current() { [[ $CURSOR -eq $1 ]]; }

toggle_current() {
    [[ ${CONFIG_SEL[$CURSOR]} -eq 1 ]] && CONFIG_SEL[$CURSOR]=0 || CONFIG_SEL[$CURSOR]=1
}

read_key() {
    local key rest
    if ! IFS= read -rsn1 key 2>/dev/null; then
        # read only fails like this on true EOF (stdin closed, no more input
        # ever coming) — a real Enter keypress still reads its \n and
        # returns 0. Signal EOF distinctly so the caller can exit instead of
        # spinning forever redrawing the menu at full speed.
        [[ -z "$key" ]] && { printf '__EOF__'; return; }
    fi
    if [[ "$key" == $'\x1b' ]]; then
        IFS= read -rsn2 -t 0.05 rest 2>/dev/null || true
        key+="$rest"
    fi
    printf '%s' "$key"
}

# ---------------------------------------------------------------------------
# Shared 64-char divider — reused for every rule in the menu so top/bottom
# borders and column widths agree with each other.
# ---------------------------------------------------------------------------
DIVIDER="────────────────────────────────────────────────────────────"

# ---------------------------------------------------------------------------
# Status badge — one consistent icon+word for found/missing/skip across the
# whole SD IMAGE CONTENTS table, with an optional dim qualifier appended
# (e.g. "[NEW]", a kernel version, a reason for skipping).
# ---------------------------------------------------------------------------
badge() {
    local kind="$1" note="${2:-}"
    case "$kind" in
        found)   printf '%b' "${GREEN}✔ found${RESET}" ;;
        missing) printf '%b' "${RED}✖ missing${RESET}" ;;
        skip)    printf '%b' "${DIM}○ skip${RESET}" ;;
    esac
    [[ -n "$note" ]] && printf '%b' " ${DIM}${note}${RESET}"
    return 0
}

# ---------------------------------------------------------------------------
# Mutual exclusivity — greyed out with a reason rather than just
# force-cleared so the user can see *why* instead of wondering where the
# checkbox went.
#   item 2 (patch U-Boot): doesn't apply when the compiled U-Boot replacement
#     (item 0) is active, since that path bypasses patching entirely.
# ---------------------------------------------------------------------------
is_item_disabled() {
    case "$1" in
        2) [[ ${CONFIG_SEL[0]} -eq 1 ]] ;;
        *) return 1 ;;
    esac
}

# Clears CONFIG_SEL/flag state for anything is_item_disabled() currently
# disqualifies, so a greyed-out item never holds a stale "on" value —
# called from both the interactive toggle handler and validate() (the
# non-interactive/CLI entry point) so the invariant holds regardless of path.
enforce_exclusivity() {
    if is_item_disabled 2; then
        CONFIG_SEL[2]=0
        PATCH_UBOOT=false
    fi
}

disabled_reason() {
    case "$1" in
        2) echo "Unavailable — 'Install compiled U-Boot' replaces patching entirely" ;;
    esac
}

# ---------------------------------------------------------------------------
# Menu rendering — one line per item, detail line for whichever row is
# highlighted, same layout as build_update.sh's compact menu.
# ---------------------------------------------------------------------------
print_detail() {
    IFS='|' read -r _ label desc _ <<< "${CONFIG_ITEMS[$CURSOR]}"
    echo -e "  ${BOLD}${label}${RESET}"
    # Wrapped lines line up under column 11 — where "Label" starts in the
    # BUILD OPTIONS rows above ("    " + cursor(2) + mark(3) + "  ") — so the
    # description visually nests under the highlighted row instead of using
    # an indent unrelated to it.
    #
    # The fold width is derived from the real terminal width (not a fixed
    # constant): a fixed indent+width that adds up to more columns than the
    # terminal actually has means the *terminal* wraps the overflow itself,
    # flush to column 0 with no indent at all — which is what "indent
    # doesn't work" looks like. Recomputed on every redraw so a resize is
    # picked up immediately.
    local indent="           "
    local cols; cols=$(tput cols 2>/dev/null || echo 80)
    local wrap_width=$(( cols - ${#indent} - 2 ))
    (( wrap_width < 20 )) && wrap_width=20
    local line
    while IFS= read -r line; do
        echo -e "${indent}${DIM}${line}${RESET}"
    done < <(fold -s -w "$wrap_width" <<< "$desc")
    if is_item_disabled "$CURSOR"; then
        echo -e "${indent}${YELLOW}⚠ $(disabled_reason "$CURSOR")${RESET}"
    fi
}

# ---------------------------------------------------------------------------
# print_menu — pins the keybinding footer to the last row of the terminal,
# the same way an input bar stays anchored to the bottom of the window
# regardless of how much is scrolled above it. Content height varies between
# redraws (a longer description, an extra "unavailable" warning line, more
# NEW-uboot rows), so a footer that just follows the last content line
# bounces up and down as you navigate. Rendering the body into a buffer
# first lets us measure it against the real terminal height and pad the gap
# so the footer always lands on the same row.
# ---------------------------------------------------------------------------
render_menu_body() {
    echo -e "  ${CYAN}${DIVIDER}${RESET}"
    echo -e "${CYAN}${BOLD}  ARK1680 Prado — Bootable SD Card Builder${RESET}"
    echo -e "  ${CYAN}${DIVIDER}${RESET}"

    echo -e "  ${BOLD}BUILD OPTIONS${RESET}"
    for i in "${!CONFIG_ITEMS[@]}"; do
        if [[ $i -eq 0 ]]; then
            echo -e "   ${DIM}Partition 1 (BOOT)${RESET}"
        elif [[ $i -eq 3 ]]; then
            echo -e "\n   ${DIM}Partition 2 (ROOTFS)${RESET}"
        elif [[ $i -eq 4 ]]; then
            echo -e "\n   ${DIM}Partition 3 (USERDATA)${RESET}"
        fi

        IFS='|' read -r key label desc _ <<< "${CONFIG_ITEMS[$i]}"
        local mark label_str
        if is_item_disabled "$i"; then
            # "Dim" (SGR 2/faint) renders as plain text in a lot of terminals,
            # so disabled rows use an explicit yellow marker + suffix instead
            # of relying on intensity to read as "different".
            mark="${YELLOW}[×]${RESET}"
            label_str="${YELLOW}${label} (unavailable)${RESET}"
        elif [[ ${CONFIG_SEL[$i]} -eq 1 ]]; then
            mark="${GREEN}[X]${RESET}"
            label_str="$label"
        else
            mark="${DIM}[ ]${RESET}"
            label_str="$label"
        fi
        local cursor="  "
        is_current "$i" && cursor="${CYAN}▶ ${RESET}"
        printf "    %b%b  %b\n" "$cursor" "$mark" "$label_str"
    done

    echo ""
    echo -e "  ${DIM}${DIVIDER}${RESET}"
    echo -e "  ${BOLD}SD IMAGE CONTENTS${RESET}  ${DIM}→ ${OUTPUT_DIR#$SCRIPT_DIR/}/$(basename "${IMAGE:-$DEVICE}")${RESET}"
    printf "   ${BOLD}%-11s %-13s %-20s %s${RESET}\n" "Partition" "Item" "File" "Status"

    # Truncate long filenames so the Status column never drifts out of alignment.
    trunc() { local s="$1" w="$2"; [[ ${#s} -gt $w ]] && echo "${s:0:$((w-1))}…" || echo "$s"; }

    local uboot_status kernel_status rootfs_status userdata_status modules_status
    if [[ -n "$UBOOT_BIN" && -f "$UBOOT_BIN" ]] || [[ -n "$UBOOT_SRC" && -f "$UBOOT_SRC" ]]; then
        uboot_status="found"
    else
        uboot_status="missing"
    fi
    [[ -n "$KERNEL_BIN" && -f "$KERNEL_BIN" ]] && kernel_status="found" || kernel_status="missing"
    [[ -n "$ROOTFS_DIR" && -d "$ROOTFS_DIR" ]] && rootfs_status=$(badge found) || rootfs_status=$(badge missing)
    if [[ ${CONFIG_SEL[4]} -eq 0 ]]; then
        userdata_status=$(badge skip "(first boot)")
    elif [[ -n "$USERDATA_DIR" && -d "$USERDATA_DIR" ]]; then
        userdata_status=$(badge found)
    else
        userdata_status=$(badge missing)
    fi
    if [[ ${CONFIG_SEL[1]} -eq 1 ]]; then
        if [[ -n "$MODULES_DIR" && -d "$MODULES_DIR" ]]; then
            local kver; kver=$(ls "$MODULES_DIR/lib/modules/" 2>/dev/null | head -1)
            modules_status=$(badge found "(${kver:-?})")
        else
            modules_status=$(badge missing "(run kernel build)")
        fi
    else
        modules_status=$(badge skip "(stock NAND)")
    fi

    local uboot_label; uboot_label="$(trunc "$(basename "${UBOOT_BIN:-${UBOOT_SRC:-uboot.bin}}")" 19)"
    local uboot_status_str; uboot_status_str=$(badge "$uboot_status" $([[ ${CONFIG_SEL[0]} -eq 1 && "$uboot_status" == found ]] && echo "[NEW]"))

    local kernel_label; kernel_label="$(trunc "$(basename "${KERNEL_BIN:-zImage}")" 19)"
    local k_status; k_status=$(badge "$kernel_status" $([[ ${CONFIG_SEL[1]} -eq 1 && "$kernel_status" == found ]] && echo "[NEW]"))

    # The "Boot script" row only applies to the new-U-Boot path (uEnv.txt) —
    # the patched-U-Boot path no longer writes a boot script file at all
    # (confirmed non-functional, removed; it drops to a U-Boot prompt for
    # manual continuation instead, see README's "Manual SD Card Boot").
    local show_bootscript=$([[ ${CONFIG_SEL[0]} -eq 1 ]] && echo true || echo false)

    # Determine branch lines dynamically
    local uboot_branch="├──"
    local kernel_branch="├──"
    if ! $show_bootscript; then
        kernel_branch="└──"
    fi

    printf "       p1 %s %-13s %-20s %b\n" "$uboot_branch" "U-Boot"       "$uboot_label"       "$uboot_status_str"
    printf "          %s %-13s %-20s %b\n" "$kernel_branch" "Kernel"       "$kernel_label"      "$k_status"
    if $show_bootscript; then
        printf "          └── %-13s %-20s %b\n" "Boot script"  "uEnv.txt"  "${DIM}generated${RESET}"
    fi
    printf "       p2 ├── %-13s %-20s %b\n" "Rootfs"       "$(trunc "$(basename "${ROOTFS_DIR:-rootfs}")" 19)" "$rootfs_status"
    printf "          └── %-13s %-20s %b\n" "Modules"      "compiled_modules/"  "$modules_status"
    printf "       p3 └── %-13s %-20s %b\n" "Userdata"     "$(trunc "$(basename "${USERDATA_DIR:-userdata}")" 19)" "$userdata_status"

    echo -e "  ${DIM}${DIVIDER}${RESET}"
    print_detail
}

print_menu() {
    local rule="  ${DIM}${DIVIDER}${RESET}"
    local footer="  ${BOLD}↑/↓${RESET} move   ${BOLD}Space${RESET}/${BOLD}Enter${RESET} toggle   ${BOLD}g${RESET} go   ${BOLD}q${RESET} quit"
    local rows; rows=$(tput lines 2>/dev/null || echo 24)
    local body; body=$(render_menu_body)
    local body_lines; body_lines=$(printf '%s\n' "$body" | wc -l)
    # Reserve 3 rows for the pinned bar: a rule above the footer, the footer
    # itself, and a rule below it — boxing it off from the content like a
    # status bar rather than letting it blend into whatever's above.
    local pad=$(( rows - body_lines - 3 ))
    (( pad < 0 )) && pad=0

    clear
    printf '%s\n' "$body"
    local i
    for (( i = 0; i < pad; i++ )); do echo; done
    echo -e "$rule"
    echo -e "$footer"
    # No trailing newline on the last line: once the buffer exactly fills
    # the terminal height, one more \n would scroll the whole screen up by
    # a row — pushing the footer bar we just pinned right back off the
    # bottom edge.
    echo -en "$rule"
}

# ---------------------------------------------------------------------------
# Validate
# ---------------------------------------------------------------------------
validate() {
    enforce_exclusivity

    if [[ -n "$UBOOT_BIN" ]]; then
        [[ -f "$UBOOT_BIN" ]] || die "UBOOT.BIN not found: $UBOOT_BIN"
    else
        [[ -n "$UBOOT_SRC" ]] || die "No U-Boot source — use --uboot-src or --uboot"
        [[ -f "$UBOOT_SRC" ]] || die "U-Boot source not found: $UBOOT_SRC"
        if [[ ${CONFIG_SEL[2]} -eq 1 ]]; then
            if $RELOC_ENV; then
                [[ -f "$SCRIPT_DIR/build_tools/patch_uboot_env.py" ]] || die "build_tools/patch_uboot_env.py not found in $SCRIPT_DIR (needed for --reloc-env)"
            else
                [[ -f "$SCRIPT_DIR/build_tools/patch_uboot.py" ]] || die "build_tools/patch_uboot.py not found in $SCRIPT_DIR"
            fi
            command -v python3 &>/dev/null || die "python3 not found — needed to patch U-Boot"
        fi
    fi
    [[ -f "$KERNEL_BIN" ]] || die "zImage not found: $KERNEL_BIN"
    [[ -d "$ROOTFS_DIR" ]] || die "rootfs dir not found: $ROOTFS_DIR"
    if [[ ${CONFIG_SEL[4]} -eq 1 && -n "$USERDATA_DIR" ]]; then
        [[ -d "$USERDATA_DIR" ]] || die "userdata dir not found: $USERDATA_DIR"
    fi
    if [[ ${CONFIG_SEL[1]} -eq 1 && ! -d "$OVERLAY_DIR" ]]; then
        die "firmware_overlay not found at $OVERLAY_DIR — needed when the new kernel is selected"
    fi
    # Sync menu toggles back to runtime variables
    [[ ${CONFIG_SEL[1]} -eq 1 ]] && NEW_KERNEL_MODE=true  || NEW_KERNEL_MODE=false
    [[ ${CONFIG_SEL[0]} -eq 1 ]] && NEW_UBOOT_MODE=true    || NEW_UBOOT_MODE=false
    enforce_exclusivity
    # Re-run autodetect so new-kernel paths resolve after menu toggle
    autodetect

    if $NEW_UBOOT_MODE; then
        [[ -f "$UBOOT_BIN" ]] || \
            die "New U-Boot not found: ${UBOOT_BIN:-linux-arkmicro/u-boot/u-boot.bin}\n  Run the U-Boot build first, or set --kernel-build-dir"
        [[ -f "$DTB_BIN" ]] || \
            die "DTB file not found: ${DTB_BIN:-ark1668_limcet_p305.dtb}\n  Build the kernel device-tree (make dtbs), supply --dtb, or place in sd_bootable/"
    fi
    if $NEW_KERNEL_MODE; then
        [[ -f "$KERNEL_BIN" ]] || \
            die "New kernel not found: ${KERNEL_BIN:-linux-arkmicro/zImage.w_dtb or linux/arch/arm/boot/zImage}\n  Run the kernel build first, or set --kernel-build-dir"
        [[ -n "$MODULES_DIR" && -d "$MODULES_DIR" ]] || \
            die "Modules dir not found: ${MODULES_DIR:-linux-arkmicro/compiled_modules/}\n  Run 'make modules && make modules_install INSTALL_MOD_PATH=../compiled_modules' first"
    fi
    local tools=(parted mkfs.fat mkfs.ext4 losetup rsync)
    for tool in "${tools[@]}"; do
        command -v "$tool" &>/dev/null || \
            die "$tool not found — run: sudo apt install parted dosfstools e2fsprogs rsync"
    done
    local avail=$(( IMAGE_SIZE_MB - P1_SIZE_MB - P2_SIZE_MB - 1 ))
    if [[ $avail -lt 32 ]]; then
        die "Only ${avail} MB left for p3 — increase --size or reduce partition sizes"
    fi
}

# ---------------------------------------------------------------------------
# Patch U-Boot — reads the source, writes UBOOT_OUT.
# The source uboot.bin is never modified; UBOOT_OUT lands in sd_bootable/.
#
# DEFAULT (--reloc-env, on): build_tools/patch_uboot_env.py RELOCATES the compiled-in
# default env into free image space (below __bss_start) and repoints it, so the
# full 'sdboot' preset fits and the device AUTO-boots from SD. --patch-nand-offset
# always on (forces the NAND env CRC to fail so the relocated default is used).
# Static-verified, NOT yet hardware-tested — see docs/UBOOT_SDBOOT_INVESTIGATION.md §10.
#
# FALLBACK (--no-reloc-env): build_tools/patch_uboot.py 'sdscript' mode — confirmed on real
# hardware to interrupt boot and drop to a U-Boot prompt (§8). From there, use
# the README's "Manual SD Card Boot" section to continue. This mode does NOT
# auto-continue; it only fits a minimal compiled-in bootcmd, since a raw/Holden-
# derived uboot.bin has no reserved env buffer for the full 'sdboot' preset.
# ---------------------------------------------------------------------------
UBOOT_WAS_PATCHED=false   # set true only when prepare_uboot() actually patches; read by build()'s summary

# ---------------------------------------------------------------------------
# Checks the actual file content (not the filename) for the ARK magic at
# offset 0x3c, so a freshly compiled u-boot.bin (no header yet) and an
# already-injected UBOOT.BIN (e.g. produced by linux-arkmicro's own
# build_uboot.sh, which now runs inject_ark_header.py itself) are told apart
# correctly regardless of what either happens to be named. Read-only, so it
# runs even under --dry-run — this is a check, not a build action.
# ---------------------------------------------------------------------------
uboot_has_ark_header() {
    local bin="$1"
    [[ -f "$bin" ]] || return 1
    python3 -c "
import struct, sys
with open('$bin', 'rb') as f:
    data = f.read(0x40)
sys.exit(0 if len(data) >= 0x40 and struct.unpack_from('<I', data, 0x3c)[0] == 0x12345678 else 1)
" 2>/dev/null
}

prepare_uboot() {
    # If new U-Boot is selected, bypass patching but ensure ARK header is injected
    if $NEW_UBOOT_MODE; then
        begin_step 1 "Preparing U-Boot (freshly compiled)"
        if [[ -f "$UBOOT_BIN" ]]; then
            if uboot_has_ark_header "$UBOOT_BIN"; then
                info "ARK header already present in $UBOOT_BIN (magic found at 0x3c) — using as-is, skipping inject_ark_header.py"
            else
                local injected="$OUTPUT_DIR/UBOOT.BIN"
                info "No ARK header found in $UBOOT_BIN — injecting via inject_ark_header.py..."
                run python3 "$SCRIPT_DIR/build_tools/inject_ark_header.py" "$UBOOT_BIN" "$injected"
                UBOOT_BIN="$injected"
            fi
        fi
        PATCH_UBOOT=false
        generate_uenv_txt
        success "U-Boot: using freshly compiled U-Boot: $UBOOT_BIN"
        end_step 1
        return 0
    fi

    # Explicit prebuilt binary — use as-is.
    if [[ -n "$UBOOT_BIN" ]]; then
        begin_step 1 "Preparing U-Boot (supplied binary)"
        success "U-Boot: using supplied binary $UBOOT_BIN"
        end_step 1
        return 0
    fi

    # Patch U-Boot toggled off — use the untouched source directly.
    if [[ ${CONFIG_SEL[2]} -eq 0 ]]; then
        begin_step 1 "Preparing U-Boot (unpatched source)"
        UBOOT_BIN="$UBOOT_SRC"
        warn "U-Boot: using source unpatched — may not boot from SD: $UBOOT_BIN"
        end_step 1
        return 0
    fi

    begin_step 1 "Patching U-Boot for SD boot"
    mkdir -p "$OUTPUT_DIR"
    UBOOT_WAS_PATCHED=true

    info "Source: $UBOOT_SRC (unchanged)"
    info "Output: $UBOOT_OUT"
    if $RELOC_ENV; then
        info "Relocating compiled-in env for full SD auto-boot (build_tools/patch_uboot_env.py --preset sdboot)"
        info "Static-verified, NOT yet hardware-tested — see docs/UBOOT_SDBOOT_INVESTIGATION.md §10"
        run python3 "$SCRIPT_DIR/build_tools/patch_uboot_env.py" \
            --non-interactive \
            -i "$UBOOT_SRC" -o "$UBOOT_OUT" \
            --preset sdboot --root "$ROOT_DEV" \
            --patch-nand-offset
    else
        info "Confirmed on real hardware to drop to a U-Boot prompt — see docs/UBOOT_SDBOOT_INVESTIGATION.md §8"
        run python3 "$SCRIPT_DIR/build_tools/patch_uboot.py" \
            -i "$UBOOT_SRC" -o "$UBOOT_OUT" \
            --mode sdscript --replace-env \
            --patch-nand-offset
    fi

    if ! $DRY_RUN; then
        [[ -f "$UBOOT_OUT" ]] || die "U-Boot patch did not produce $UBOOT_OUT"
        success "U-Boot patched: $UBOOT_OUT"
    fi
    UBOOT_BIN="$UBOOT_OUT"
    end_step 1
}

# ---------------------------------------------------------------------------
# Apply the firmware overlay — rsyncs firmware_overlay/ on top of the
# already-synced p2 rootfs, unconditionally overwriting whatever's there.
# Only applied when NEW_KERNEL_MODE is active: the overlay's rcS/profile/
# wifi_ap.sh fixes specifically target 4.19.192 kernel compatibility and
# would be wrong for a stock-kernel build. See firmware_overlay/README.md
# for exactly what's in here and why — this replaces what used to be
# patch_rootfs_for_new_kernel() + fix_libgal_so() (python3/regex transforms
# against a copy of the rootfs at build time).
# ---------------------------------------------------------------------------
apply_overlay() {
    local rootfs_mount="$1"
    echo -e "${BOLD}  Applying firmware overlay (firmware_overlay/)...${RESET}"

    if $DRY_RUN; then
        echo "  [dry-run] rsync -a $OVERLAY_DIR/ → $rootfs_mount/"
        return
    fi

    [[ -d "$OVERLAY_DIR" ]] || { warn "Overlay dir not found at $OVERLAY_DIR — skipping"; return; }

    rsync -a --info=progress2 "$OVERLAY_DIR/" "$rootfs_mount/"
    success "RootFS overlay applied — rcS/profile/wifi_ap.sh/inittab/libGAL.so, etc."
}

# ---------------------------------------------------------------------------
# Diagnostic tools (tools/) — copies every compiled binary/script/data file
# from each tools/<name>/ directory into /usr/bin on the built image, so
# mem-dump, fb-scan, lcdc-regdump, pin-force, pinmux-watch, etc. are always
# present without hand-copying them onto the device or manually curating
# firmware_overlay/usr/bin/ (which had drifted stale — several tools built
# in later sessions were never added there). Runs after apply_overlay() so
# tools/ — the canonical, up-to-date source — wins over any older duplicate
# firmware_overlay/usr/bin/ may still carry.
#
# Copies every file under each tools/*/ directory except source/doc/build
# artifacts (*.py host-side analysis scripts, *.c/*.h source, *.md docs,
# *.cmd/*.o build leftovers) — this picks up compiled ELF binaries, .sh
# wrapper scripts, and any data files a tool needs (e.g. audio-test's
# test-tone.wav) in one pass, regardless of a tool's exact internal layout.
# ---------------------------------------------------------------------------
install_diag_tools() {
    local rootfs_mount="$1"
    local tools_dir="$SCRIPT_DIR/tools"
    echo -e "${BOLD}  Installing diagnostic tools (tools/) into /usr/bin...${RESET}"

    [[ -d "$tools_dir" ]] || { warn "tools/ dir not found at $tools_dir — skipping"; return; }

    if $DRY_RUN; then
        echo "  [dry-run] copy tools/*/<binaries/scripts/data> → $rootfs_mount/usr/bin/"
        return
    fi

    mkdir -p "$rootfs_mount/usr/bin"
    local count=0
    local d f base
    for d in "$tools_dir"/*/; do
        [[ -d "$d" ]] || continue
        for f in "$d"*; do
            [[ -f "$f" ]] || continue
            base="$(basename "$f")"
            case "$base" in
                *.py|*.c|*.h|*.inc|*.md|*.cmd|*.o|*.orig) continue ;;
                *-debug) continue ;;  # e.g. tools/htop/htop-debug — unstripped debug build, not for routine deployment
            esac
            cp -f "$f" "$rootfs_mount/usr/bin/$base"
            chmod +x "$rootfs_mount/usr/bin/$base" 2>/dev/null || true
            count=$((count + 1))
        done
    done
    success "Installed $count diagnostic tool file(s) from tools/ into /usr/bin"
}

# ---------------------------------------------------------------------------
# Busybox applet symlinks — firmware_overlay/busybox-applets.manifest lists
# every applet path + symlink target for the rebuilt busybox
# (firmware_overlay/bin/busybox, 2026-07-27, defconfig-based build with
# ipcs/ipcrm added — see docs/USERDATA_REVIEW.md or the commit message for
# why). Stored as plain-text data, not real symlinks in the overlay tree,
# because this repo's working copy sits on a VirtualBox shared folder
# (vboxsf), which cannot create symlinks at all (`ln -s` fails with
# "Operation not permitted") — this materializes them for real onto the
# properly-mounted rootfs image at build time instead, where symlinks work
# normally. Two applet names are deliberately excluded from the manifest
# already (dmesg, less) because this project already ships better
# standalone replacements for both (tools/dmesg, real GNU less) and a
# busybox-provided /bin/dmesg would shadow /usr/bin/dmesg via $PATH order,
# reintroducing a bug already fixed once (see firmware_overlay/README.md).
# ---------------------------------------------------------------------------
install_busybox_applets() {
    local rootfs_mount="$1"
    local manifest="$OVERLAY_DIR/busybox-applets.manifest"
    echo -e "${BOLD}  Creating busybox applet symlinks...${RESET}"

    [[ -f "$manifest" ]] || { warn "busybox-applets.manifest not found at $manifest — skipping"; return; }

    if $DRY_RUN; then
        echo "  [dry-run] create $(wc -l < "$manifest") busybox applet symlinks in $rootfs_mount/"
        return
    fi

    local count=0
    local path target dir
    while read -r path target; do
        [[ -n "$path" ]] || continue
        dir="$(dirname "$path")"
        mkdir -p "$rootfs_mount/$dir"
        ln -sf "$target" "$rootfs_mount/$path"
        count=$((count + 1))
    done < "$manifest"
    success "Created $count busybox applet symlink(s)"
}

# ---------------------------------------------------------------------------
# MTD partition redirect — inserts /dev/mtdN symlinks to /nanddata/ after
# mdev -s in rcS, only if redirect_mtd_data is on. Genuinely conditional
# (unlike the rest of what used to live in patch_rcs()/
# patch_rootfs_for_new_kernel(), now baked into firmware_overlay/ —
# see that directory's README), so this stays a small, focused build-time
# insertion rather than overlay content.
# ---------------------------------------------------------------------------
patch_rcs_mtd_redirect() {
    local target="$1"
    echo -e "${BOLD}  Patching rcS to redirect NAND partitions to SD/USB...${RESET}"

    if $DRY_RUN; then
        echo "  [dry-run] insert /dev/mtdN -> /nanddata/ symlinks after mdev -s in rcS"
        return
    fi

    [[ -f "$target" ]] || { warn "rcS not found at $target — skipping MTD redirect patch"; return; }

    python3 - "$target" <<'PYEOF'
import sys

path = sys.argv[1]
text = open(path).read()

MDEV_LINE = '/sbin/mdev -s'
mdev_idx = text.find(MDEV_LINE)
if mdev_idx == -1:
    print("  WARNING: '/sbin/mdev -s' not found in rcS — MTD redirect patch skipped")
    sys.exit(0)

insert_at = mdev_idx + len(MDEV_LINE)
eol = text.find('\n', insert_at)
if eol == -1:
    eol = len(text)

symlink_block = """

# Replace MTD data partition devices with symlinks to SD-stored files.
# SD card is always authoritative for these partitions -- any NAND device
# node created by mdev is removed and replaced unconditionally.
for mtdmap in "8:bootlogo" "9:bootanimation" "10:reversingtrack" "11:unicode"; do
\tnum="${mtdmap%%:*}"
\tname="${mtdmap##*:}"
\trm -f /dev/mtd${num}
\tln -sf /nanddata/${name} /dev/mtd${num}
\techo "mtd${num}: /nanddata/${name}"
done"""

patched = text[:eol] + symlink_block + text[eol:]
open(path, 'w').write(patched)
print("  rcS MTD symlink block inserted after mdev -s")
PYEOF

    success "rcS patched for NAND partition redirect to SD/USB "nanddata" folder"
}

# ---------------------------------------------------------------------------
# MsnCoreApp autolaunch toggle — firmware_overlay/etc/profile already
# ships with 'MsnCoreApp -qws&' commented out (the default-ON state for this
# toggle), so this only has to act when the toggle is explicitly turned OFF
# (re-enable the line).
# ---------------------------------------------------------------------------
toggle_msncoreapp_autolaunch() {
    local rootfs_mount="$1"
    local enable_autolaunch="$2"   # "1" if disable_msncoreapp_autolaunch is OFF (i.e. re-enable it)
    local profile="$rootfs_mount/etc/profile"

    [[ "$enable_autolaunch" == "1" ]] || return 0   # default state from the overlay is already correct

    echo -e "${BOLD}  Re-enabling MsnCoreApp auto-launch in /etc/profile...${RESET}"
    if $DRY_RUN; then
        echo "  [dry-run] uncomment 'MsnCoreApp -qws&' in /etc/profile"
        return
    fi
    [[ -f "$profile" ]] || { warn "profile not found at $profile — skipping"; return; }
    sed -i 's/^#MsnCoreApp -qws&/MsnCoreApp -qws\&/' "$profile"
    success "MsnCoreApp auto-launch re-enabled"
}

# ---------------------------------------------------------------------------
# Directories to emulate NAND partition data — copy to /nanddata/ on p2
# ---------------------------------------------------------------------------
populate_nanddata() {
    local dest="$1/nanddata"
    echo -e "${BOLD}  Populating folders to emulate NAND /nanddata/...${RESET}"

    if $DRY_RUN; then
        echo "  [dry-run] mkdir /nanddata/ on p2"
        for mtdnum in "${!NAND_PARTS[@]}"; do
            echo "  [dry-run] copy mtd${mtdnum}: ${NAND_PARTS[$mtdnum]}"
        done
        return
    fi

    mkdir -p "$dest"

    for mtdnum in "${!NAND_PARTS[@]}"; do
        local relpath="${NAND_PARTS[$mtdnum]}"
        local filename="${relpath##*/}"
        local src="$RECONSTRUCTED_DIR/$relpath"
        local dst="$dest/$filename"

        if [[ -f "$src" ]] && [[ -s "$src" ]]; then
            cp "$src" "$dst"
            local sz; sz=$(du -sh "$dst" | cut -f1)
            success "mtd${mtdnum} → /nanddata/${filename}  (${sz})"
        else
            # Create empty placeholder — real dump can be dropped in later
            touch "$dst"
            warn "mtd${mtdnum} → /nanddata/${filename}  (placeholder — no dump yet)"
        fi
    done
}

# ---------------------------------------------------------------------------
# Generate uEnv.txt for new U-Boot env loading
# ---------------------------------------------------------------------------
generate_uenv_txt() {
    info "Generating uEnv.txt..."
    local uenv_content
    uenv_content=$(cat <<EOF
bootargs=console=ttyS0,115200n8 mem=180M earlyprintk=serial root=$ROOT_DEV rootfstype=ext4 rootwait rw screen=0 user_debug=8
bootcmd=fatload mmc 0:1 0x1000000 zImage; fatload mmc 0:1 0x2000000 ark1668_limcet_p305.dtb; bootz 0x1000000 - 0x2000000
EOF
)

    if $DRY_RUN; then
        echo "  [dry-run] would write uEnv.txt to $UENV_OUT:"
        echo "$uenv_content" | sed 's/^/    /'
        return
    fi

    mkdir -p "$OUTPUT_DIR"
    printf '%s\n' "$uenv_content" > "$UENV_OUT"
    success "uEnv.txt generated: $UENV_OUT"
}

# ---------------------------------------------------------------------------
# Install compiled Limcet P305 kernel modules onto the mounted p2 rootfs
# ---------------------------------------------------------------------------
install_new_kernel_modules() {
    local rootfs_mount="$1"
    echo -e "${BOLD}  Installing compiled kernel modules onto p2...${RESET}"

    if $DRY_RUN; then
        echo "  [dry-run] rsync compiled_modules/lib/modules/ → /tmp/sd_p2/lib/modules/"
        echo "  [dry-run] depmod -a -b $rootfs_mount <kernel-version>"
        return
    fi

    local kver; kver=$(ls "$MODULES_DIR/lib/modules/" 2>/dev/null | head -1)
    [[ -z "$kver" ]] && die "No kernel version found in $MODULES_DIR/lib/modules/"

    # Remove any existing modules for this kernel version to avoid stale .ko files
    rm -rf "$rootfs_mount/lib/modules/$kver"
    mkdir -p "$rootfs_mount/lib/modules"

    rsync -a --info=progress2 \
        "$MODULES_DIR/lib/modules/$kver" \
        "$rootfs_mount/lib/modules/"

    # Replace legacy 3.4.0 module directory with a symlink to the new kernel version
    # so that any hardcoded insmods inside MsnCoreApp dynamically redirect to 4.19.192.
    rm -rf "$rootfs_mount/lib/modules/3.4.0"
    ln -sf "$kver" "$rootfs_mount/lib/modules/3.4.0"

    # Create legacy wlan_rtl*.ko symlinks so MsnCoreApp and wifi_ap.sh can load them
    # using their expected legacy filenames:
    (
        cd "$rootfs_mount/lib/modules/$kver"
        find kernel -name "*.ko" | while read -r kopath; do
            local kobasename; kobasename=$(basename "$kopath")
            if [[ "$kopath" == *"drivers/net/wireless/realtek"* ]]; then
                ln -sf "$kopath" "wlan_$kobasename"
            fi
        done
    )

    success "Modules installed: /lib/modules/$kver  ($(find "$rootfs_mount/lib/modules/$kver" -name '*.ko' | wc -l) .ko files)"
    success "Redirected legacy module path and created wlan_*.ko symlinks"

    # Run depmod to regenerate module dependency map for the target rootfs
    if command -v depmod &>/dev/null; then
        depmod -a -b "$rootfs_mount" "$kver" 2>/dev/null && \
            success "depmod: module dependency map regenerated for $kver" || \
            warn "depmod failed (non-fatal — modules.dep may be stale)"
    else
        warn "depmod not found on host — modules.dep will not be regenerated"
        warn "  Run 'depmod -a' on the device after first boot"
    fi
}

# ---------------------------------------------------------------------------
# Insert a passwordless root telnetd into rcS, right after mdev -s (same
# insertion point as the MTD symlink patch above). Requires mounting
# /dev/pts first — busybox telnetd fails silently at startup without it,
# discovered the hard way getting the same mechanism working on stock
# firmware via the msn_autocopy payload (see msn_autocopy/README.md for the
# full debugging history: v1 had no devpts mount and no output redirection,
# so it looked like rcS ran fine but nothing was ever listening on port 23).
# UNAUTHENTICATED — `-l /bin/sh` skips /bin/login entirely, no password
# prompt. OFF by default; this is a real network-exposed root shell while
# active.
# ---------------------------------------------------------------------------
install_telnetd() {
    local rootfs_mount="$1"
    local rcs="$rootfs_mount/etc/rc.d/rcS"
    echo -e "${BOLD}  Installing passwordless root telnetd into rcS...${RESET}"

    if $DRY_RUN; then
        echo "  [dry-run] insert devpts mount + busybox telnetd -l /bin/sh after mdev -s in rcS"
        return
    fi

    [[ -f "$rcs" ]] || { warn "rcS not found at $rcs — skipping telnetd install"; return; }

    python3 - "$rcs" <<'PYEOF'
import sys

path = sys.argv[1]
text = open(path).read()

MDEV_LINE = '/sbin/mdev -s'
mdev_idx = text.find(MDEV_LINE)
if mdev_idx == -1:
    print("  WARNING: '/sbin/mdev -s' not found in rcS — telnetd install skipped")
    sys.exit(0)

insert_at = mdev_idx + len(MDEV_LINE)
eol = text.find('\n', insert_at)
if eol == -1:
    eol = len(text)

block = """

# --- passwordless root telnetd (build_bootable_sdcard.sh --telnetd) ---
# UNAUTHENTICATED root shell on port 23. devpts must be mounted first or
# busybox telnetd fails silently at startup (confirmed via the msn_autocopy
# payload's debugging history -- see msn_autocopy/README.md).
mkdir -p /dev/pts
mount -t devpts none /dev/pts 2>/dev/null
busybox telnetd -l /bin/sh &"""

patched = text[:eol] + block + text[eol:]
open(path, 'w').write(patched)
print("  rcS telnetd block inserted after mdev -s")
PYEOF

    success "telnetd installed (port 23, no auth — root shell for anything on the device's network)"
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
LOOP=""
cleanup() {
    [[ -n "$LOOP" ]] && ! $DRY_RUN && {
        umount /tmp/sd_p1 /tmp/sd_p2 /tmp/sd_p3 2>/dev/null || true
        losetup -d "$LOOP" 2>/dev/null || true
    }
    return 0
}
trap cleanup EXIT

build() {
    mkdir -p "$OUTPUT_DIR"
    local TARGET="${IMAGE:-$DEVICE}"
    local P3_START=$(( P1_SIZE_MB + P2_SIZE_MB + 1 ))
    local do_userdata=0
    [[ ${CONFIG_SEL[4]} -eq 1 && -n "$USERDATA_DIR" ]] && do_userdata=1
    local do_mtd_redirect=${CONFIG_SEL[3]}

    # 1. Create image file
    if [[ -n "$IMAGE" ]]; then
        begin_step 2 "Creating blank image (${IMAGE_SIZE_MB} MB)"
        run dd if=/dev/zero of="$IMAGE" bs=1M count="$IMAGE_SIZE_MB" status=progress
        end_step 2
    else
        begin_step 2 "Using device $DEVICE"
        end_step 2
    fi

    # 2. Partition table
    begin_step 3 "Partitioning"
    run parted -s "$TARGET" mklabel msdos
    run parted -s "$TARGET" mkpart primary fat32 1MiB "${P1_SIZE_MB}MiB"
    run parted -s "$TARGET" mkpart primary ext4  "$((P1_SIZE_MB+1))MiB" "$((P1_SIZE_MB+P2_SIZE_MB))MiB"
    run parted -s "$TARGET" mkpart primary ext4  "${P3_START}MiB" "100%"
    run parted -s "$TARGET" set 1 boot on
    success "p1 (BOOT), p2 (ROOTFS), p3 (USERDATA) written to partition table"
    end_step 3

    # 3. Loop device (image files only)
    local P1 P2 P3
    begin_step 4 "Attaching loop device"
    if [[ -n "$IMAGE" ]] && ! $DRY_RUN; then
        LOOP=$(losetup -Pf --show "$IMAGE")
        info "Loop device: $LOOP"
        P1="${LOOP}p1"; P2="${LOOP}p2"; P3="${LOOP}p3"
        end_step 4
    elif [[ -n "$DEVICE" ]]; then
        P1="${DEVICE}1"; P2="${DEVICE}2"; P3="${DEVICE}3"
        end_step 4 skip
    else
        P1="/dev/loopXp1"; P2="/dev/loopXp2"; P3="/dev/loopXp3"
        end_step 4 skip
    fi

    # 4. Format
    begin_step 5 "Creating partitions"
    # The target runs a Linux 3.4.0 kernel (and U-Boot 2012.10), whose ext4
    # drivers predate the 64bit and metadata_csum features that modern
    # e2fsprogs enables by default. Leaving them on makes the kernel reject the
    # root fs ("unsupported optional features") and U-Boot's ext4ls fail. Strip
    # them so both can mount p2/p3. The remaining features (extents, flex_bg,
    # huge_file, dir_nlink, extra_isize, …) are all supported by 3.4.
    local EXT4_COMPAT="^64bit,^metadata_csum"
    run mkfs.fat -F32 -n BOOT    "$P1"
    run mkfs.ext4 -O "$EXT4_COMPAT" -L rootfs   -F "$P2"
    run mkfs.ext4 -O "$EXT4_COMPAT" -L userdata -F "$P3"
    success "Filesystem partitions p1 FAT32, p2 ext4 (rootfs), p3 ext4 (userdata) created"
    end_step 5

    # 5. Mount
    begin_step 6 "Temporarily Mounting Partitions"
    if ! $DRY_RUN; then
        mkdir -p /tmp/sd_p1 /tmp/sd_p2 /tmp/sd_p3
        mount "$P1" /tmp/sd_p1
        mount "$P2" /tmp/sd_p2
        mount "$P3" /tmp/sd_p3
        success "p1 → /tmp/sd_p1, p2 → /tmp/sd_p2, p3 → /tmp/sd_p3"
        end_step 6
    else
        end_step 6 skip
    fi

    # 6. Populate p1 — boot files
    begin_step 7 "Populating Partition p1 (boot FAT)"
    run cp "$UBOOT_BIN"  /tmp/sd_p1/UBOOT.BIN
    run cp "$KERNEL_BIN" /tmp/sd_p1/zImage
    local bootlogo_label=""
    if [[ -n "$BOOTLOGO_RAW" && -f "$BOOTLOGO_RAW" ]]; then
        run cp "$BOOTLOGO_RAW" /tmp/sd_p1/bootlogo.raw
        bootlogo_label=" + bootlogo.raw"
    fi
    if [[ -n "$ARKDATA_INI" && -f "$ARKDATA_INI" ]]; then
        run cp "$ARKDATA_INI" /tmp/sd_p1/arkdata.ini
        bootlogo_label="$bootlogo_label + arkdata.ini"
    fi
    if [[ -n "$STOCK_UBOOT_BIN" && -f "$STOCK_UBOOT_BIN" ]]; then
        run cp "$STOCK_UBOOT_BIN" /tmp/sd_p1/uboot_stock.bin
        bootlogo_label="$bootlogo_label + uboot_stock.bin"
    fi
    if [[ -n "$HYBRID_UBOOT_BIN" && -f "$HYBRID_UBOOT_BIN" ]]; then
        run cp "$HYBRID_UBOOT_BIN" /tmp/sd_p1/uboot_hybrid.bin
        bootlogo_label="$bootlogo_label + uboot_hybrid.bin"
    fi
    local stock_kernel_src=""
    if [[ -f "$SCRIPT_DIR/sd_bootable/zImage_stock" ]]; then
        stock_kernel_src="$SCRIPT_DIR/sd_bootable/zImage_stock"
    elif [[ -f "$SCRIPT_DIR/firmware_source/mtd5_kernel/zImage" ]]; then
        stock_kernel_src="$SCRIPT_DIR/firmware_source/mtd5_kernel/zImage"
    fi
    if [[ -n "$stock_kernel_src" && -f "$stock_kernel_src" ]]; then
        run cp "$stock_kernel_src" /tmp/sd_p1/zImage_stock
        bootlogo_label="$bootlogo_label + zImage_stock"
    fi
    if $NEW_UBOOT_MODE; then
        if [[ -n "$DTB_BIN" && -f "$DTB_BIN" ]]; then
            run cp "$DTB_BIN" /tmp/sd_p1/ark1668_limcet_p305.dtb
        fi
        run cp "$UENV_OUT" /tmp/sd_p1/uEnv.txt
        success "UBOOT.BIN + zImage + uEnv.txt + DTB${bootlogo_label} written to p1"
    elif $UBOOT_WAS_PATCHED; then
        success "UBOOT.BIN (patched) + zImage${bootlogo_label} written to p1"
    else
        success "UBOOT.BIN + zImage${bootlogo_label} written to p1"
    fi
    end_step 7

    # 7. Populate p2 — rootfs
    begin_step 8 "Populating Partition p2 (rootfs EXT4)"
    # rsync -a copies the source tree verbatim, so repair the metadata a
    # Windows checkout drops before copying — otherwise p2 is unbootable:
    #   - build_tools/restore_rootfs_symlinks.sh recreates the lost symlinks (/bin/sh,
    #     /sbin/init, /lib/libc.so.6, …) — rsync -a would just copy the gap.
    #   - build_tools/apply_rootfs_perms.sh restores exec bits — else no executable binaries.
    run rsync -a --info=progress2 \
        --exclude=/proc/ \
        --exclude=/sys/ \
        --exclude=/dev/ \
        --exclude=/tmp/ \
        --exclude='*.ubifs' \
        --exclude='rootfs.img' \
        --exclude='ubi.cfg' \
        "$ROOTFS_DIR/" /tmp/sd_p2/
    ! $DRY_RUN && mkdir -p /tmp/sd_p2/{proc,sys,dev,tmp}
	echo -e "    ${BOLD}Restore rootfs symlinks...${RESET}"
    run bash "$SCRIPT_DIR/build_tools/restore_rootfs_symlinks.sh" "/tmp/sd_p2"
	echo -e "    ${BOLD}Restore rootfs permissions...${RESET}"
    run bash "$SCRIPT_DIR/build_tools/apply_rootfs_perms.sh" "/tmp/sd_p2"
    if ! $DRY_RUN; then
        echo "    Converting CRLF line endings to LF on target configuration files and scripts..."
        find /tmp/sd_p2/etc -type f -exec sed -i 's/\r$//' {} + 2>/dev/null || true
        find /tmp/sd_p2 -type f \( -name "*.sh" -o -name "rcS" -o -name "inittab" -o -name "profile" -o -name "fstab" \) -exec sed -i 's/\r$//' {} + 2>/dev/null || true
    fi
    success "Rootfs synced to p2"
    end_step 8

    # 8. Populate p3 — userdata
    begin_step 9 "Populating Partition p3 (userdata EXT4)"
    if [[ $do_userdata -eq 0 ]]; then
        warn "Skipped — p3 is empty. App will populate /data on first boot."
        end_step 9 skip
    else
        run rsync -a --info=progress2 \
            --exclude='*.ubifs' \
            --exclude='userdata.img' \
            "$USERDATA_DIR/" /tmp/sd_p3/
        success "Userdata synced to p3"
        end_step 9
    fi

    # 9. Install compiled Limcet P305 kernel modules
    begin_step 10 "Installing new kernel modules"
    if $NEW_KERNEL_MODE; then
        install_new_kernel_modules /tmp/sd_p2
        end_step 10
    else
        info "Skipped — stock NAND firmware_source/mtd5_kernel/modules in use"
        end_step 10 skip
    fi

    # 10. Apply firmware overlay (rcS/profile/wifi_ap.sh/inittab/libGAL.so) +
    #     the two remaining genuinely-conditional toggles on top of it
    begin_step 11 "Applying firmware overlay + rcS toggles"
    if $NEW_KERNEL_MODE; then
        apply_overlay /tmp/sd_p2
    else
        info "Skipped overlay — targets 4.19.192 kernel compatibility, not applicable to the stock kernel"
    fi
    install_diag_tools /tmp/sd_p2
    install_busybox_applets /tmp/sd_p2
    patch_rcs_mtd_redirect /tmp/sd_p2/etc/rc.d/rcS
    if [[ "$do_mtd_redirect" != "1" ]]; then
        info "MTD redirect symlinks skipped (redirect_mtd_data is off — using existing NAND data)"
    fi
    toggle_msncoreapp_autolaunch /tmp/sd_p2 "$([[ ${CONFIG_SEL[5]} -eq 1 ]] && echo 0 || echo 1)"
    if ! $DRY_RUN; then
        # apply_overlay's rsync (above) copies firmware_overlay/ verbatim on
        # top of the already-CRLF-cleaned rootfs from step 8 -- if the
        # overlay source tree itself carries CRLF (this repo's working tree
        # can pick that up independent of git history, e.g. via a Windows-
        # side checkout feeding this shared folder), that reintroduces the
        # exact stray \r step 8 already stripped. inittab is the case that
        # actually breaks boot: busybox parses everything after the last
        # ':' up to the line terminator as the sysinit/respawn command, so
        # a trailing \r becomes part of the exec path and every attempt to
        # run rcS/the respawned shell fails with "can't run '/bin/...'" in
        # a loop, even though root mounts fine. Re-run the same CRLF strip
        # after the overlay (and its rcS/autolaunch patches) have all been
        # applied, so nothing placed after step 8 can undo it.
        echo "    Re-converting CRLF line endings introduced by the overlay..."
        find /tmp/sd_p2/etc -type f -exec sed -i 's/\r$//' {} + 2>/dev/null || true
        find /tmp/sd_p2 -type f \( -name "*.sh" -o -name "rcS" -o -name "inittab" -o -name "profile" -o -name "fstab" \) -exec sed -i 's/\r$//' {} + 2>/dev/null || true
    fi
    end_step 11

    # 11. Populate "nanddata" folder (bootlogo/bootanimation/reversingtrack/Unicode)
    begin_step 12 "Populating "nanddata" folder on SD/USB for"
    if [[ $do_mtd_redirect -eq 1 ]]; then
        populate_nanddata /tmp/sd_p2
        end_step 12
    else
        info "Skipped — bootlogo/bootanimation/reversingtrack/Unicode stay on NAND"
        end_step 12 skip
    fi

    # 12. Install passwordless root telnetd (UNAUTHENTICATED — opt-in diagnostic tool)
    # Diagnostic tools themselves are no longer a build step — they're baked
    # into firmware_overlay/usr/bin/ unconditionally, see that
    # directory's README.
    begin_step 13 "Installing telnetd (diagnostic, unauthenticated)"
    if [[ ${CONFIG_SEL[6]} -eq 1 ]]; then
        install_telnetd /tmp/sd_p2
        end_step 13
    else
        info "Skipped — telnetd not installed"
        end_step 13 skip
    fi

    # Unmount and detach
    if ! $DRY_RUN; then
        sync
        umount /tmp/sd_p1 /tmp/sd_p2 /tmp/sd_p3
        [[ -n "$LOOP" ]] && { losetup -d "$LOOP"; LOOP=""; }
    fi

    print_step_summary

    echo ""
    echo -e "${GREEN}${BOLD}====== BUILD COMPLETE ======${RESET}"
    echo ""
    if [[ -n "$IMAGE" ]] && ! $DRY_RUN; then
        local sz; sz=$(du -sh "$IMAGE" | cut -f1)
        echo "Image Generated:"
        success "$IMAGE  ($sz)"
		echo ""
        echo -e "${BOLD}  Write to SD card or USB drive manually:${RESET}"
        echo    "    Linux:"
		echo    "    sudo dd if=\"$IMAGE\" of=/dev/sdX bs=4M status=progress && sync"
		echo    "    Windows:"
        echo    "    Write image with Etcher or Raspberry Pi Imager"
    elif [[ -n "$DEVICE" ]]; then
        success "Written directly to $DEVICE"
    fi
    echo ""
    echo -e "${BOLD}  Boot sequence:${RESET}"
    echo    "    Stepldr  → loads UBOOT.BIN from BOOT partition"
    if $NEW_UBOOT_MODE; then
        echo "    U-Boot   → imports environment variables from uEnv.txt on BOOT partition (if present)"
        echo "    U-Boot   → 1. bootusb: attempts booting kernel (zImage) + DTB from attached USB drive"
        echo "    U-Boot   → 2. boothybrid: falls back to chainloading uboot_hybrid.bin from SD card (loads zImage_stock + NAND rootfs)"
        echo "    U-Boot   → 3. bootstock: falls back to chainloading uboot_stock.bin from SD card"
        echo "    U-Boot   → 4. nandboot: last-resort fallback to direct NAND kernel boot"
    elif $UBOOT_WAS_PATCHED; then
        echo "    U-Boot   → NAND env CRC forced invalid, drops to interactive prompt"
        echo "    (manual) → continue with the README's \"Manual SD Card Boot\" section"
    else
        echo "    U-Boot   → whatever bootcmd this UBOOT.BIN was built/patched with"
    fi
    echo "    Kernel   → mounts partition p2 ext4 as the root filesystem"
    echo "    rcS      → mounts partition p3 ext4 as /data"
    if [[ ${CONFIG_SEL[3]} -eq 1 ]]; then
        echo "    rcS      → symlinks NAND partitions to /nanddata/ directory on partition p2 "
    fi
    echo ""
    if $UBOOT_WAS_PATCHED; then
        warn "This U-Boot patch is confirmed to interrupt boot and drop to a prompt"
        warn "  on real hardware (see docs/UBOOT_SDBOOT_INVESTIGATION.md §8). It does"
        warn "  not auto-continue to a full boot — use the README's \"Manual SD Card"
        warn "  Boot\" section to type the boot commands by hand at that prompt."
    fi
}

# ---------------------------------------------------------------------------
# Non-interactive path — skip the menu, use flags/autodetected values as-is.
# ---------------------------------------------------------------------------
run_non_interactive() {
    check_requirements
    validate
    prepare_uboot
    build
}

# ---------------------------------------------------------------------------
# Main loop — same interaction model as build_update.sh: arrow keys move the
# highlighted row, Space/Enter toggles it, g/q act immediately.
# ---------------------------------------------------------------------------
run_interactive() {
    clear
    echo -e "  ${CYAN}${DIVIDER}${RESET}"
    echo -e "${CYAN}${BOLD}  ARK1680 Prado — Bootable SD Card Builder${RESET}"
    echo -e "  ${CYAN}${DIVIDER}${RESET}"
    echo ""
    check_requirements

    if [[ -n "$DEVICE" ]]; then
        [[ -b "$DEVICE" ]] || die "$DEVICE is not a block device"
    fi

    while true; do
        print_menu
        key=$(read_key)
        [[ "$key" == "__EOF__" ]] && { echo ""; echo "  No more input — exiting."; exit 0; }

        case "$key" in
            $'\x1b[A')  (( CURSOR > 0 )) && CURSOR=$((CURSOR - 1)) ;;
            $'\x1b[B')  (( CURSOR < ${#CONFIG_ITEMS[@]} - 1 )) && CURSOR=$((CURSOR + 1)) ;;
            ''|' ')
                if is_item_disabled "$CURSOR"; then continue; fi
                toggle_current
                # Keep path-sensitive flags in sync so autodetect() can
                # immediately resolve KERNEL_BIN and MODULES_DIR.
                [[ ${CONFIG_SEL[1]} -eq 1 ]] && NEW_KERNEL_MODE=true || NEW_KERNEL_MODE=false
                [[ ${CONFIG_SEL[0]} -eq 1 ]] && NEW_UBOOT_MODE=true   || NEW_UBOOT_MODE=false
                PATCH_UBOOT=$([[ ${CONFIG_SEL[2]} -eq 1 ]] && echo true || echo false)
                enforce_exclusivity

                KERNEL_BIN=""   # let autodetect re-resolve based on new mode
                UBOOT_BIN=""    # let autodetect re-resolve based on new mode
                autodetect
                ;;
            g|G)
                echo ""
                if [[ -n "$DEVICE" ]]; then
                    echo -e "${RED}${BOLD}  WARNING: ALL DATA ON $DEVICE WILL BE ERASED${RESET}"
                    echo ""
                    echo -ne "${BOLD}  Type YES to continue: ${RESET}"
                    local word; read -r word
                    [[ "$word" == "YES" ]] || { echo "Aborted."; continue; }
                fi

                (validate && prepare_uboot && build) || {
                    read -rp "  Press Enter to continue..." _
                    continue
                }

                echo ""
                read -rp "  Press Enter to return to menu, or q to quit: " done_input
                [[ "$done_input" =~ ^[Qq]$ ]] && exit 0
                ;;
            q|Q)
                echo ""
                echo "  Exited."
                exit 0
                ;;
        esac
    done
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
autodetect

if $NON_INTERACTIVE; then
    run_non_interactive
else
    run_interactive
fi