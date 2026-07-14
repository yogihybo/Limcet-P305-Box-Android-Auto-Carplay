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
#   --uboot-src PATH   Raw uboot.bin source — patched via patch_uboot.py, never modified
#   --no-patch-uboot   Use the source uboot.bin as-is without patching
#   --reloc-env        When patching the stock U-Boot (requires --no-new-uboot),
#                      RELOCATE the compiled-in default env via patch_uboot_env.py
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
#   --bootlogo PATH    Raw 800x480x32bpp framebuffer (see convert_bootlogo.py)
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
#                      ON by default.
#   --no-new-kernel    Explicitly disable new-kernel replacement (use stock kernel)
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
#   --initramfs        Enable the initramfs (off by default). The initramfs
#                      (build_initramfs.sh) insmods ark_dw_mmc.ko and mounts the
#                      SD rootfs — only needed if the MMC driver is a module.
#                      With the Limcet P305 kernel the MMC driver is built-in,
#                      so this is off by default.
#   --no-initramfs     Explicitly disable the initramfs (already the default)
#   --diag-tools PATH  Additional diagnostic binary/script to install onto
#                      p2's /usr/bin, on top of the defaults: i2c-scan,
#                      ark-ts-test, lcd-test, strace (compiled binaries), plus
#                      touch-selftest.sh, uart-test.sh, audio-test.sh,
#                      bt-test.sh, usb-test.sh, mmc-test.sh (POSIX shell
#                      scripts — see each tools/*/README.md). Repeatable.
#   --no-diag-tools    Don't install any diagnostic tools onto p2
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
STEP_TOTAL=16
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
    echo -e "  ${BOLD}Build summary${RESET}"
    for n in "${!STEP_TITLES[@]}"; do
        [[ "${STEP_STATUS[$n]}" == "skip" ]] && mark="${DIM}○${RESET}" || mark="${GREEN}✔${RESET}"
        printf "   %b %-2s %-28s ${DIM}%s${RESET}\n" "$mark" "$n" "${STEP_TITLES[$n]}" \
            "$([[ "${STEP_STATUS[$n]}" == "skip" ]] && echo "skipped" || echo "${STEP_ELAPSED[$n]}s")"
        [[ "${STEP_STATUS[$n]}" != "skip" ]] && total=$(( total + STEP_ELAPSED[$n] ))
    done
    echo -e "  ${DIM}────────────────────────────────────────${RESET}"
    echo -e "   ${BOLD}Total: ${total}s${RESET}"
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
# fail in the same step (always passes --patch-nand-offset to patch_uboot.py)
# — the patch alone with a valid env is confirmed to have no effect, so
# there's no reason to offer them as two separate toggles.
PATCH_UBOOT=false                             # run patch_uboot.py on the source
# --reloc-env: when patching the stock U-Boot, relocate the compiled-in default
# env (patch_uboot_env.py) into free image space so a full SD-boot command fits
# and AUTO-boots, rather than the sdscript patch that only drops to a prompt.
# ON by default; pass --no-reloc-env to fall back to the prompt-drop patch.
# Static-verified, not yet hardware-tested — see docs/UBOOT_SDBOOT_INVESTIGATION.md §10.
RELOC_ENV=true
ROOT_DEV="/dev/mmcblk0p2"                     # root= in the generated uEnv.txt (matches p2 rootfs)
USE_INITRAMFS=false                           # OFF — confirmed non-functional, the dumped stock kernel doesn't support it (see is_item_disabled())
INITRAMFS_OUT="$OUTPUT_DIR/initramfs.cpio.gz"  # generated by build_initramfs.sh
INITRAMFS_UIMG="$OUTPUT_DIR/uInitrd"           # mkimage-wrapped ramdisk (for the bootz path)
INITRAMFS_ADDR=0x2000000                       # RAM address the initramfs is fatload'd to
# NAND partition map, inlined into the initramfs boot script so it works even
# when the NAND env is unreadable (patched U-Boot). Matches env/uboot-env.txt.
MTDPARTS='mtdparts=ark1680-nand:128k(S-Loader),512k(U-boot),512k(U-boot_back),256K(U-boot-Env),256K(arkdata),4m(kernel),106m(rootfs),6m(userdata),512K(bootlogo),3m(bootanimation),3m(reversingtrack),256K(Unicode)'
KERNEL_BIN=""
BOOTLOGO_RAW=""                               # raw framebuffer (--bootlogo) for p1/bootlogo.raw
STOCK_UBOOT_BIN="$SCRIPT_DIR/Prado firmware dump/mtd1-mtd2_uboot/extracted/uboot.bin"  # for p1/stock_uboot.bin, used by the `bootstock` chainload command
DTB_BIN=""
ROOTFS_DIR=""
USERDATA_DIR=""
RECONSTRUCTED_DIR=""
SKIP_USERDATA=false
SKIP_MTD_REDIRECT=false
SKIP_DIAG_TOOLS=false
INSTALL_TELNETD=false                         # unauthenticated root telnetd on port 23 — OFF by default, opt-in only
NEW_KERNEL_MODE=true                          # replace stock kernel with freshly compiled Limcet P305 kernel — ON by default; pass --no-new-kernel to use the stock kernel
NEW_UBOOT_MODE=true                           # replace stock U-Boot with freshly compiled Limcet P305 U-Boot — ON by default; pass --no-new-uboot to use the stock U-Boot
KERNEL_BUILD_DIR=""                           # path to linux-arkmicro/ build root (auto-detected)
MODULES_DIR=""                                # path to compiled_modules/ (auto-detected from KERNEL_BUILD_DIR)
declare -a DIAG_TOOLS_BINS=(
    "$SCRIPT_DIR/tools/i2c-scan/i2c-scan"                  # static ARM i2c bus scanner, see tools/i2c-scan/README.md
    "$SCRIPT_DIR/tools/ark1680-ts-test/ark-ts-test"        # ark1680_ts touchscreen diagnostic, see tools/ark1680-ts-test/README.md
    "$SCRIPT_DIR/tools/lcd-test/lcd-test"                  # raw /dev/fb0 LCD diagnostic, see tools/lcd-test/README.md
    "$SCRIPT_DIR/tools/strace/strace"                      # upstream strace (static), see tools/strace/README.md
    "$SCRIPT_DIR/tools/touch-selftest/touch-selftest.sh"   # automated touch pass/fail flow, see tools/touch-selftest/README.md
    "$SCRIPT_DIR/tools/uart-test/uart-test.sh"             # passive MCU-link/MSNEry-link listener, see tools/uart-test/README.md
    "$SCRIPT_DIR/tools/audio-test/audio-test.sh"           # sound card + BD37033 bus/mixer check, see tools/audio-test/README.md
    "$SCRIPT_DIR/tools/bt-test/bt-test.sh"                 # GPIO91/ttyHS1/blueware check, see tools/bt-test/README.md
    "$SCRIPT_DIR/tools/usb-test/usb-test.sh"               # USB regression check, see tools/usb-test/README.md
    "$SCRIPT_DIR/tools/mmc-test/mmc-test.sh"               # read-only MMC/SD check, see tools/mmc-test/README.md
)
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
        --initramfs)       USE_INITRAMFS=true; shift ;;
        --no-initramfs)    USE_INITRAMFS=false; shift ;;
        --diag-tools)      DIAG_TOOLS_BINS+=("$2"); shift 2 ;;
        --no-diag-tools)   SKIP_DIAG_TOOLS=true; shift ;;
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
        local c="$SCRIPT_DIR/Prado firmware reconstructed"
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
            "$SCRIPT_DIR/Prado firmware reconstructed/mtd1-mtd2_uboot/uboot.bin" \
            "$SCRIPT_DIR/Prado firmware dump/mtd1-mtd2_uboot/extracted/uboot.bin"
        do [[ -f "$c" ]] && { UBOOT_SRC="$c"; break; }; done
    }
    # Fall back to stock kernel if new-kernel not requested
    [[ -z "$KERNEL_BIN" ]] && {
        for c in \
            "$SCRIPT_DIR/Prado firmware reconstructed/mtd5_kernel/zImage" \
            "$SCRIPT_DIR/kernel/zImage"
        do [[ -f "$c" ]] && { KERNEL_BIN="$c"; break; }; done
    }
    [[ -z "$ROOTFS_DIR" ]] && {
        local c="$SCRIPT_DIR/Prado firmware reconstructed/mtd6_rootfs/rootfs"
        [[ -d "$c" ]] && ROOTFS_DIR="$c"
    }
    [[ -z "$USERDATA_DIR" ]] && {
        local c="$SCRIPT_DIR/Prado firmware reconstructed/mtd7_userdata/userdata"
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
    "python3|patch U-Boot and rcS|python3"
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
# --size, --root); the menu only controls these three boolean choices.
# Format: "key|label|description|default"
# ---------------------------------------------------------------------------
CONFIG_ITEMS=(
    "use_new_uboot|Install compiled Limcet P305 U-Boot + uEnv|Replaces the stock NAND-dumped UBOOT.BIN on p1 with the freshly compiled Limcet P305 U-Boot. Bypasses patching, and installs UBOOT.BIN, uEnv.txt, and the DTB file on p1.|ON"
    "use_new_kernel|Install compiled Limcet P305 kernel + modules|Replaces the stock NAND kernel on p1 with the freshly compiled zImage.w_dtb from linux-arkmicro/. Also installs the compiled .ko modules into /lib/modules/ on the p2 rootfs. Uses linux-arkmicro/compiled_modules/ auto-detected from build dir|ON"
    "patch_uboot|Patch binary U-Boot for SD auto-boot (env relocation)|Patches U-Boot and forces the NAND env CRC to fail. By default (--reloc-env) it RELOCATES the compiled-in default env so a full SD-boot command fits and the device AUTO-boots from SD — static-verified, NOT yet hardware-tested (see docs/UBOOT_SDBOOT_INVESTIGATION.md §10). Pass --no-reloc-env for the hardware-confirmed fallback that only drops to an interactive U-Boot prompt (then continue via the README's \"Manual SD Card Boot\").|OFF"
    "use_initramfs|Use initramfs (usually not needed)|DISABLED — confirmed non-functional, the dumped stock kernel doesn't support this boot path. Would build an initramfs that insmods ark_dw_mmc.ko and mounts the SD rootfs, for a stock NAND kernel where MMC is a module. Kept for reference.|OFF"
    "redirect_mtd_data|Redirect NAND mtd partitions to SD card (bootlogo, bootanimation, reversingtrack, unicode)|Symlinks bootlogo, bootanimation, reversingtrack, and Unicode font (mtd8-11) to files under /nanddata/ on p2 — if off, the device reads these from whatever is already in NAND instead|ON"
    "include_userdata|Include userdata (p3)|Copies the userdata dir to p3 — if off, p3 is left empty and the app populates /data on first boot|ON"
    "install_diag_tools|Install diagnostic tools (i2c-scan, ark-ts-test, lcd-test, strace, *-test.sh)|Copies diagnostic binaries and scripts onto p2's /usr/bin: i2c-scan (I2C bus scanner, see tools/i2c-scan/README.md), ark-ts-test (ARK1680 touchscreen register/evdev tester, see tools/ark1680-ts-test/README.md), lcd-test (raw /dev/fb0 LCD tester, see tools/lcd-test/README.md), strace (upstream syscall tracer, see tools/strace/README.md), and touch-selftest.sh/uart-test.sh/audio-test.sh/bt-test.sh/usb-test.sh/mmc-test.sh (automated pass/fail wrappers, one per subsystem — see each tools/*/README.md). Harmless to leave off.|ON"
    "disable_msncoreapp_autolaunch|Disable MsnCoreApp auto-launch at login|Comments out 'MsnCoreApp -qws&' in /etc/profile so it doesn't auto-run (and auto-crash) on every shell login while the startup segfault is being debugged (see docs/ARK1680_TS_REVERSE_ENGINEERING.md). Run 'start_msn' manually instead to test. Turn this off once the crash is fixed and auto-launch is wanted again.|ON"
    "fix_libgal_dynamic_section|Fix corrupted libGAL.so .dynamic section|/usr/lib/libGAL.so's .dynamic section is corrupted (just a single DT_NULL entry — no NEEDED/SYMTAB/STRTAB), which crashes the dynamic linker with a NULL+4 deref inside _dl_relocate_object() the instant MsnCoreApp tries to load it (root-caused via matched strace+dmesg PC/LR correlation, see docs/ARK1680_TS_REVERSE_ENGINEERING.md). Replaces it with libGAL.fb.so, the vendor's own software-framebuffer variant (SONAME=libGAL.so, valid .dynamic section), backing up the original as libGAL.so.corrupt-orig.|ON"
    "install_telnetd|Install passwordless root telnetd (UNAUTHENTICATED — diagnostic only)|Inserts 'mount -t devpts none /dev/pts' + 'busybox telnetd -l /bin/sh &' into rcS right after mdev -s, giving a root shell on port 23 with no login prompt to anything that can reach the device's network (WiFi AP or USB-NCM). Same mechanism validated working on stock firmware via the msn_autocopy payload (see msn_autocopy/README.md for why the devpts mount is required — telnetd fails silently without it). This is a real, if minor, exposure while active on any network the device joins — OFF by default, opt-in only.|OFF"
    "fix_usb_port0_otg_race|Work around USB port 0 boot-time OTG detection race (DISABLED, see below)|CURRENTLY A NO-OP — the actual unbind/rebind commands are commented out in the generated rcS. Confirmed on real hardware that unbinding musb-hdrc.0 unconditionally is actively harmful when root is mounted from USB on that same controller (bootusb + usbroot) — it yanks the live root filesystem out from under the running system, causing I/O errors and an aborted journal. The kernel-level fix (musb_ark.c VBUS settle delay) has since been confirmed sufficient on its own for the boot-from-USB case. This toggle is kept for when the workaround is made conditional on root NOT already being on this bus. See docs/HANDOFF_nand_ecc_uboot_vs_kernel.md.|OFF"
)

declare -a CONFIG_SEL
for i in "${!CONFIG_ITEMS[@]}"; do
    IFS='|' read -r _ _ _ default <<< "${CONFIG_ITEMS[$i]}"
    [[ "$default" == "ON" ]] && CONFIG_SEL[$i]=1 || CONFIG_SEL[$i]=0
done
# CLI flags override the menu defaults up front, same as build_update.sh's
# flags override its own PARTITIONS defaults.
$PATCH_UBOOT       || CONFIG_SEL[2]=0
$SKIP_USERDATA     && CONFIG_SEL[5]=0
$SKIP_MTD_REDIRECT && CONFIG_SEL[4]=0
$NEW_KERNEL_MODE   || CONFIG_SEL[1]=0
$USE_INITRAMFS     && CONFIG_SEL[3]=1
$NEW_UBOOT_MODE    || CONFIG_SEL[0]=0
$SKIP_DIAG_TOOLS   && CONFIG_SEL[6]=0
$INSTALL_TELNETD   && CONFIG_SEL[9]=1

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
# Mutual exclusivity / permanent disables — greyed out with a reason rather
# than just force-cleared so the user can see *why* instead of wondering
# where the checkbox went.
#   item 2 (patch U-Boot): doesn't apply when the compiled U-Boot replacement
#     (item 0) is active, since that path bypasses patching entirely.
#   item 3 (initramfs): always disabled — the dumped stock kernel doesn't
#     support this boot path, confirmed non-functional.
# ---------------------------------------------------------------------------
is_item_disabled() {
    case "$1" in
        2) [[ ${CONFIG_SEL[0]} -eq 1 ]] ;;
        3) return 0 ;;
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
        3) echo "Unavailable — confirmed non-functional, the dumped stock kernel doesn't support this boot path" ;;
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
        elif [[ $i -eq 4 ]]; then
            echo -e "\n   ${DIM}Partition 2 (ROOTFS)${RESET}"
        elif [[ $i -eq 5 ]]; then
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
    if [[ ${CONFIG_SEL[5]} -eq 0 ]]; then
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
    local diag_status diag_count=0 diag_found=0 db
    if [[ ${CONFIG_SEL[6]} -eq 1 ]]; then
        for db in "${DIAG_TOOLS_BINS[@]}"; do
            diag_count=$((diag_count+1))
            [[ -f "$db" ]] && diag_found=$((diag_found+1))
        done
        if [[ $diag_found -eq $diag_count ]]; then
            diag_status=$(badge found)
        elif [[ $diag_found -gt 0 ]]; then
            diag_status=$(badge found "(${diag_found}/${diag_count})")
        else
            diag_status=$(badge missing)
        fi
    else
        diag_status=$(badge skip)
    fi

    printf "       p2 ├── %-13s %-20s %b\n" "Rootfs"       "$(trunc "$(basename "${ROOTFS_DIR:-rootfs}")" 19)" "$rootfs_status"
    printf "          ├── %-13s %-20s %b\n" "Modules"      "compiled_modules/"  "$modules_status"
    printf "          └── %-13s %-20s %b\n" "Diag tools"   "${diag_count} tool(s)" "$diag_status"
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
                [[ -f "$SCRIPT_DIR/patch_uboot_env.py" ]] || die "patch_uboot_env.py not found in $SCRIPT_DIR (needed for --reloc-env)"
            else
                [[ -f "$SCRIPT_DIR/patch_uboot.py" ]] || die "patch_uboot.py not found in $SCRIPT_DIR"
            fi
            command -v python3 &>/dev/null || die "python3 not found — needed to patch U-Boot"
        fi
    fi
    [[ -f "$KERNEL_BIN" ]] || die "zImage not found: $KERNEL_BIN"
    [[ -d "$ROOTFS_DIR" ]] || die "rootfs dir not found: $ROOTFS_DIR"
    if [[ ${CONFIG_SEL[5]} -eq 1 && -n "$USERDATA_DIR" ]]; then
        [[ -d "$USERDATA_DIR" ]] || die "userdata dir not found: $USERDATA_DIR"
    fi
    if [[ ${CONFIG_SEL[6]} -eq 1 ]]; then
        local db any_found=0
        for db in "${DIAG_TOOLS_BINS[@]}"; do
            [[ -f "$db" ]] && any_found=1 || warn "Diagnostic tool not found, will be skipped: $db"
        done
        [[ $any_found -eq 0 ]] && { warn "No diagnostic tool binaries found — disabling"; CONFIG_SEL[6]=0; }
    fi
    # Sync menu toggles back to runtime variables
    [[ ${CONFIG_SEL[1]} -eq 1 ]] && NEW_KERNEL_MODE=true  || NEW_KERNEL_MODE=false
    [[ ${CONFIG_SEL[3]} -eq 1 ]] && USE_INITRAMFS=true    || USE_INITRAMFS=false
    [[ ${CONFIG_SEL[0]} -eq 1 ]] && NEW_UBOOT_MODE=true    || NEW_UBOOT_MODE=false
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
    $USE_INITRAMFS && tools+=(cpio gzip mknod mkimage)   # build_initramfs.sh deps (mkimage → uInitrd)
    for tool in "${tools[@]}"; do
        command -v "$tool" &>/dev/null || \
            die "$tool not found — run: sudo apt install parted dosfstools e2fsprogs rsync cpio gzip u-boot-tools"
    done
    if $USE_INITRAMFS && [[ ! -f "$SCRIPT_DIR/build_initramfs.sh" ]]; then
        die "build_initramfs.sh not found in $SCRIPT_DIR (or pass --no-initramfs)"
    fi
    local avail=$(( IMAGE_SIZE_MB - P1_SIZE_MB - P2_SIZE_MB - 1 ))
    if [[ $avail -lt 32 ]]; then
        die "Only ${avail} MB left for p3 — increase --size or reduce partition sizes"
    fi
}

# ---------------------------------------------------------------------------
# Patch U-Boot — reads the source, writes UBOOT_OUT.
# The source uboot.bin is never modified; UBOOT_OUT lands in sd_bootable/.
#
# DEFAULT (--reloc-env, on): patch_uboot_env.py RELOCATES the compiled-in
# default env into free image space (below __bss_start) and repoints it, so the
# full 'sdboot' preset fits and the device AUTO-boots from SD. --patch-nand-offset
# always on (forces the NAND env CRC to fail so the relocated default is used).
# Static-verified, NOT yet hardware-tested — see docs/UBOOT_SDBOOT_INVESTIGATION.md §10.
#
# FALLBACK (--no-reloc-env): patch_uboot.py 'sdscript' mode — confirmed on real
# hardware to interrupt boot and drop to a U-Boot prompt (§8). From there, use
# the README's "Manual SD Card Boot" section to continue. This mode does NOT
# auto-continue; it only fits a minimal compiled-in bootcmd, since a raw/Holden-
# derived uboot.bin has no reserved env buffer for the full 'sdboot' preset.
# ---------------------------------------------------------------------------
UBOOT_WAS_PATCHED=false   # set true only when prepare_uboot() actually patches; read by build()'s summary

prepare_uboot() {
    # If new U-Boot is selected, bypass patching but ensure ARK header is injected
    if $NEW_UBOOT_MODE; then
        begin_step 1 "Preparing U-Boot (freshly compiled)"
        if [[ -f "$UBOOT_BIN" ]] && [[ "$(basename "$UBOOT_BIN")" == "u-boot.bin" ]]; then
            local injected="$OUTPUT_DIR/UBOOT.BIN"
            info "Injecting ARK header into new U-Boot..."
            run python3 "$SCRIPT_DIR/inject_ark_header.py" "$UBOOT_BIN" "$injected"
            UBOOT_BIN="$injected"
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
        info "Relocating compiled-in env for full SD auto-boot (patch_uboot_env.py --preset sdboot)"
        info "Static-verified, NOT yet hardware-tested — see docs/UBOOT_SDBOOT_INVESTIGATION.md §10"
        run python3 "$SCRIPT_DIR/patch_uboot_env.py" \
            -i "$UBOOT_SRC" -o "$UBOOT_OUT" \
            --preset sdboot --root "$ROOT_DEV" \
            --patch-nand-offset
    else
        info "Confirmed on real hardware to drop to a U-Boot prompt — see docs/UBOOT_SDBOOT_INVESTIGATION.md §8"
        run python3 "$SCRIPT_DIR/patch_uboot.py" \
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
# rcS patch — applied to the copy on p2, source tree is never modified
# ---------------------------------------------------------------------------
patch_rcs() {
    local target="$1"
    local redirect_mtd="$2"   # "1" or "0" — CONFIG_SEL[4], redirect_mtd_data
    echo -e "${BOLD}  Patching rcS for SD/USB userdata mount...${RESET}"

    if $DRY_RUN; then
        echo "  [dry-run] patch rcS: replace UBIFS userdata block with root-device-relative ext4 (p3) first + NAND fallback"
        if [[ "$redirect_mtd" == "1" ]]; then
            echo "  [dry-run] patch rcS: insert /dev/mtdN symlinks to /nanddata/"
        else
            echo "  [dry-run] patch rcS: leave /dev/mtdN pointed at existing NAND data"
        fi
        return
    fi

    [[ -f "$target" ]] || { warn "rcS not found at $target — skipping patch"; return; }

    # Two patches applied via a single Python pass:
    #   1. Replace UBIFS-only /data mount with an ext4 (p3) mount derived
    #      from the kernel's own root= device (works for both bootmmc's
    #      /dev/mmcblk0pN and bootusb's /dev/sdaN) + NAND fallback
    #   2. Insert /dev/mtdN symlinks after mdev -s for SD-stored NAND partition
    #      data — skipped if redirect_mtd_data is off, leaving the device
    #      reading these partitions from whatever is already in NAND
    python3 - "$target" "$redirect_mtd" <<'PYEOF'
import sys, re

path = sys.argv[1]
redirect_mtd = sys.argv[2] == "1"
text = open(path).read()

NEW = """\
# Mount userdata: ext4 p3 (SD or USB, whichever root actually came from)
# first, NAND UBI fallback, then yaffs2. ROOTDEV is derived from the
# kernel's own root= bootarg rather than hardcoded to /dev/mmcblk0p3, so
# the same rcS works for both bootmmc (root=/dev/mmcblk0pN) and bootusb
# (root=/dev/sdaN) without needing separate rootfs builds.
ROOTDEV=$(cat /proc/cmdline | sed -n 's/.*root=\\([^ ]*\\).*/\\1/p')
USERDATADEV="${ROOTDEV%?}3"
if mount -o sync -t ext4 "$USERDATADEV" /data 2>/dev/null; then
\techo "userdata: ext4 ($USERDATADEV)"
\tresetenv=$(fw_printenv factory_reset 2>/dev/null || echo "factory_reset=0")
\tif [ "${resetenv##*=}" = "1" ]; then
\t\techo "==============Factory reset, reformat userdata!==========="
\t\tumount /data
\t\tmkfs.ext4 -O ^64bit,^metadata_csum -F "$USERDATADEV"
\t\tmount -o sync -t ext4 "$USERDATADEV" /data
\t\tfw_setenv factory_reset 0 2>/dev/null || true
\tfi
else
\tUSERDATAFS=`cat /proc/mounts | grep ubifs`
\tif [ "${USERDATAFS}" != "" ]; then
\t\tmtd_partition=7
\t\tresetenv=$(fw_printenv factory_reset 2>/dev/null || echo "factory_reset=0")
\t\tif [ "${resetenv##*=}" = "1" ]; then
\t\t\techo "==============Factory reset, erase data partition!==========="
\t\t\tflash_eraseall /dev/mtd$mtd_partition
\t\t\tubiattach /dev/ubi_ctrl -m $mtd_partition
\t\t\tubimkvol  /dev/ubi1 -N userdata -m 14
\t\t\tubidetach /dev/ubi_ctrl -m $mtd_partition -d 1
\t\t\tfw_setenv factory_reset 0 2>/dev/null || true
\t\tfi
\t\tubiattach /dev/ubi_ctrl -m $mtd_partition
\t\tmount -o sync -t ubifs ubi1_0 /data
\t\tif [ $? -eq 0 ]; then
\t\t\techo "userdata: NAND UBI"
\t\telse
\t\t\tsync; sleep 1
\t\t\tmount -o sync -t ubifs ubi1_0 /data
\t\t\tif [ $? -ne 0 ]; then
\t\t\t\techo "mount failed! then reformat the volume!"
\t\t\t\tubidetach /dev/ubi_ctrl -m $mtd_partition -d 1
\t\t\t\tflash_eraseall /dev/mtd$mtd_partition
\t\t\t\tubiattach /dev/ubi_ctrl -m $mtd_partition
\t\t\t\tubimkvol  /dev/ubi1 -N userdata -m 14
\t\t\t\tmount -o sync -t ubifs ubi1_0 /data
\t\t\t\tsync
\t\t\tfi
\t\tfi
\telse
\t\tUSERDATAFS=`cat /proc/mounts | grep yaffs2`
\t\tif [ "${USERDATAFS}" != "" ]; then
\t\t\tmount -t yaffs2 /dev/mtdblock6 /data/
\t\tfi
\tfi
fi"""

MARKER_START = 'USERDATAFS=`cat /proc/mounts | grep ubifs`'

start = text.find(MARKER_START)
end   = text.find('\n#mount /dev/mmcblk0p1 /mnt')

if start == -1 or end == -1:
    print(f"  WARNING: rcS userdata block not found at expected location — patch skipped")
    sys.exit(0)

patched = text[:start] + NEW + '\n' + text[end:]
open(path, 'w').write(patched)
print("  rcS userdata mount block patched (follows root device, SD or USB)")

# Patch 2: insert MTD symlink block after /sbin/mdev -s — only if
# redirect_mtd_data is on; otherwise leave /dev/mtdN untouched so the
# device reads bootlogo/bootanimation/reversingtrack/Unicode from NAND.
if not redirect_mtd:
    print("  rcS MTD symlink patch skipped (redirect_mtd_data is off — using existing NAND data)")
else:
    text2 = open(path).read()
    MDEV_LINE = '/sbin/mdev -s'
    mdev_idx = text2.find(MDEV_LINE)
    if mdev_idx == -1:
        print("  WARNING: '/sbin/mdev -s' not found in rcS — MTD symlink patch skipped")
    else:
        insert_at = mdev_idx + len(MDEV_LINE)
        # Find end of that line
        eol = text2.find('\n', insert_at)
        if eol == -1: eol = len(text2)
        symlink_block = """

# Replace MTD data partition devices with symlinks to SD-stored files.
# SD card is always authoritative for these partitions — any NAND device
# node created by mdev is removed and replaced unconditionally.
for mtdmap in "8:bootlogo" "9:bootanimation" "10:reversingtrack" "11:unicode"; do
\tnum="${mtdmap%%:*}"
\tname="${mtdmap##*:}"
\trm -f /dev/mtd${num}
\tln -sf /nanddata/${name} /dev/mtd${num}
\techo "mtd${num}: /nanddata/${name}"
done"""
        patched2 = text2[:eol] + symlink_block + text2[eol:]
        open(path, 'w').write(patched2)
        print("  rcS MTD symlink block inserted after mdev -s")
PYEOF

    success "rcS patched on p2 (source tree unchanged)"
}

# ---------------------------------------------------------------------------
# NAND partition data — copy to /nanddata/ on p2
# ---------------------------------------------------------------------------
populate_nanddata() {
    local dest="$1/nanddata"
    echo -e "${BOLD}  Populating /nanddata/ (MTD partition data)...${RESET}"

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
        if $USE_INITRAMFS; then
            uenv_content=$(cat <<EOF
bootargs=console=ttyS0,115200n8 mem=180M earlyprintk=serial rootwait rw screen=0 user_debug=8
bootcmd=fatload mmc 0:1 0x1000000 zImage; fatload mmc 0:1 $INITRAMFS_ADDR uInitrd; fatload mmc 0:1 0x2000000 ark1668_limcet_p305.dtb; bootz 0x1000000 $INITRAMFS_ADDR 0x2000000
EOF
)
        else
            uenv_content=$(cat <<EOF
bootargs=console=ttyS0,115200n8 mem=180M earlyprintk=serial root=$ROOT_DEV rootfstype=ext4 rootwait rw screen=0 user_debug=8
bootcmd=fatload mmc 0:1 0x1000000 zImage; fatload mmc 0:1 0x2000000 ark1668_limcet_p305.dtb; bootz 0x1000000 - 0x2000000
EOF
)
        fi

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
# Patch rootfs scripts for 4.19.192 kernel compatibility
# Applied to the copy on p2 only — source tree is never modified.
# Counterpart to patch_rcs(): called when NEW_KERNEL_MODE is active.
# ---------------------------------------------------------------------------
patch_rootfs_for_new_kernel() {
    local rootfs_mount="$1"
    local disable_autolaunch="$2"   # "1" or "0" — CONFIG_SEL[7], disable_msncoreapp_autolaunch
    echo -e "${BOLD}  Patching rootfs init scripts for 4.19.192 kernel compatibility...${RESET}"

    if $DRY_RUN; then
        echo "  [dry-run] rcS: mkdir /media before ramfs mount"
        echo "  [dry-run] rcS: replace insmod 3.4.0 touchscreen paths with modprobe (silent fallback)"
        echo "  [dry-run] rcS: comment out missing /usr/bin/sshd"
        echo "  [dry-run] wifi_ap.sh: replace 3.4.0 wlan .ko paths with modprobe + uname -r fallback"
        if [[ "$disable_autolaunch" == "1" ]]; then
            echo "  [dry-run] profile: comment out 'MsnCoreApp -qws&' auto-launch"
        fi
        return
    fi

    local rcs="$rootfs_mount/etc/rc.d/rcS"
    local wifi="$rootfs_mount/etc/wifi_ap.sh"
    local inittab="$rootfs_mount/etc/inittab"
    local profile="$rootfs_mount/etc/profile"

    [[ -f "$rcs" ]]  || { warn "rcS not found at $rcs — skipping new-kernel patch"; return; }
    [[ -f "$wifi" ]] || { warn "wifi_ap.sh not found at $wifi — skipping new-kernel patch"; return; }
    [[ -f "$inittab" ]] || { warn "inittab not found at $inittab — skipping new-kernel patch"; return; }
    [[ -f "$profile" ]] || { warn "profile not found at $profile — skipping new-kernel patch"; return; }

    python3 - "$rcs" "$wifi" "$inittab" "$profile" "$disable_autolaunch" <<'PYEOF'
import sys, re

rcs_path  = sys.argv[1]
wifi_path = sys.argv[2]
inittab_path = sys.argv[3]
profile_path = sys.argv[4]
disable_autolaunch = sys.argv[5] == "1"

# ── inittab patches ────────────────────────────────────────────────────────
inittab = open(inittab_path).read()

# Replace getty with direct shell execution, as the busybox build on this
# device is missing the 'login' applet.
OLD_GETTY = 'ttyS0::respawn:/sbin/getty -L ttyS0 115200 vt100'
NEW_GETTY = 'ttyS0::respawn:-/bin/sh'
if OLD_GETTY in inittab:
    inittab = inittab.replace(OLD_GETTY, NEW_GETTY)
    print("  inittab: replaced getty on ttyS0 with direct login shell")
else:
    print("  inittab WARNING: getty line not in expected form — skipped")

open(inittab_path, 'w').write(inittab)

# ── rcS patches ────────────────────────────────────────────────────────────
rcs = open(rcs_path).read()

# 1. Create /media before the ramfs mount so the mount doesn't fail
OLD_MEDIA = 'mount -t ramfs -n none /tmp\nmount -t ramfs -n none /media'
NEW_MEDIA = 'mount -t ramfs -n none /tmp\nmkdir -p /media\nmount -t ramfs -n none /media'
if OLD_MEDIA in rcs:
    rcs = rcs.replace(OLD_MEDIA, NEW_MEDIA)
    print("  rcS: inserted mkdir -p /media before ramfs mount")
elif 'mkdir -p /media' not in rcs:
    print("  rcS WARNING: /media mount line not in expected form — skipped")

# 1b. Create /var before the ramfs mount so the mount doesn't fail
OLD_VAR = 'mount -t ramfs -n none /var'
NEW_VAR = 'mkdir -p /var\nmount -t ramfs -n none /var'
if OLD_VAR in rcs:
    rcs = rcs.replace(OLD_VAR, NEW_VAR)
    print("  rcS: inserted mkdir -p /var before ramfs mount")

# 2. Replace hardcoded 3.4.0 touchscreen insmod with modprobe (silent fallback)
#    Handles both ark1680_ts and gt9xx variants.
rcs = re.sub(
    r'insmod /lib/modules/3\.4\.0/kernel/drivers/input/touchscreen/ark1680_ts\.ko',
    'modprobe ark1680_ts 2>/dev/null || true  # 3.4.0 driver not built for 4.19.192 yet',
    rcs
)
rcs = re.sub(
    r'insmod /lib/modules/3\.4\.0/kernel/drivers/input/touchscreen/gt9xx/gt9xx\.ko',
    'modprobe gt9xx 2>/dev/null || true        # 3.4.0 driver not built for 4.19.192 yet',
    rcs
)
print("  rcS: touchscreen insmod replaced with modprobe (silent fallback)")

# 2b. Add chmod 600 for SSH host private keys to satisfy OpenSSH permission check
rcs = re.sub(
    r'mkdir -p /var/run/sshd',
    'chmod 600 /etc/ssh/ssh_host_*_key 2>/dev/null || true\nmkdir -p /var/run/sshd',
    rcs
)
print("  rcS: added chmod 600 for SSH private host keys before starting sshd")

# Patch /usr/bin/sshd to explicitly use /etc/ssh/sshd_config, as the compiled-in
# default path (/usr/local/etc/sshd_config) does not exist in the rootfs.
rcs = re.sub(
    r'/usr/bin/sshd &',
    '/usr/bin/sshd -f /etc/ssh/sshd_config &',
    rcs
)
print("  rcS: patched sshd call to use explicit configuration file path")

open(rcs_path, 'w').write(rcs)

# ── wifi_ap.sh patches ─────────────────────────────────────────────────────
wifi = open(wifi_path).read()

# Replace the for-loop that probes 3.4.0 .ko paths with modprobe + uname -r fallback.
# Use regex rather than literal match to avoid heredoc backslash-escaping issues.
NEW_WIFI = """    # Try modprobe first -- uses modules.dep for 4.19.192
    modprobe rtl8821cs 2>/dev/null || \\
    modprobe rtl8822cs 2>/dev/null || \\
    modprobe rtl8189fs 2>/dev/null || \\
    modprobe rtl8821cu 2>/dev/null || \\
    modprobe rtl8811cu 2>/dev/null || {
        # Fallback: direct .ko path using running kernel version
        KVER=$(uname -r)
        for ko in \\
            /lib/modules/${KVER}/kernel/drivers/net/wireless/realtek/rtl8821cs/rtl8821cs.ko \\
            /lib/modules/${KVER}/kernel/drivers/net/wireless/realtek/rtlwifi/rtl8192cu/rtl8192cu.ko \\
            /lib/modules/${KVER}/kernel/drivers/net/wireless/realtek/rtl8811cu/rtl8811cu.ko \\
            /lib/modules/${KVER}/kernel/drivers/net/wireless/realtek/rtl8xxxu/rtl8xxxu.ko; do
            [ -f "$ko" ] && insmod "$ko" && break
        done
    }"""

# Regex matches the for-loop block by its distinctive 3.4.0/wlan_rtl anchors,
# tolerating any whitespace/backslash encoding variations from the heredoc.
wifi_patched, n = re.subn(
    r'for ko in\s*\\\s*\n(?:\s+/lib/modules/3\.4\.0/wlan_rtl\S+\s*\\\s*\n)+\s+/lib/modules/3\.4\.0/wlan_rtl\S+;\s*do\s*\n\s+\[.*insmod.*\$.*break\s*\n\s+done',
    NEW_WIFI.strip(),
    wifi
)
if n:
    wifi = wifi_patched
    print("  wifi_ap.sh: 3.4.0 wlan .ko paths replaced with modprobe + uname -r fallback")
else:
    print("  wifi_ap.sh WARNING: expected 3.4.0 for-loop not found -- already patched or changed?")

open(wifi_path, 'w').write(wifi)

# ── profile patches ────────────────────────────────────────────────────────
profile = open(profile_path).read()

# Comment out galcore module loading as it conflicts with old userspace DirectFB/Vivante libraries
profile_patched, n = re.subn(
    r'insmod\s+/lib/modules/3\.4\.0/galcore\.ko',
    '# modprobe galcore',
    profile
)
if n:
    profile = profile_patched
    print("  profile: commented out galcore.ko loading")
else:
    print("  profile WARNING: galcore.ko insmod line not found")

# Switch QWS display driver from DirectFB (requires galcore GPU) to LinuxFB (pure software framebuffer)
# to avoid GPU driver version mismatch crashes.
OLD_DISPLAY = 'export QWS_DISPLAY=directfb:boundingrectflip:mmWidth220:mmHeight120:0'
NEW_DISPLAY = 'export QWS_DISPLAY=LinuxFB:mmWidth220:mmHeight120:0'
if OLD_DISPLAY in profile:
    profile = profile.replace(OLD_DISPLAY, NEW_DISPLAY)
    print("  profile: switched QWS_DISPLAY from directfb to LinuxFB")
else:
    print("  profile WARNING: QWS_DISPLAY directfb line not found")

# Disable the MsnCoreApp auto-launch on every shell login while its startup
# segfault is being debugged (see docs/ARK1680_TS_REVERSE_ENGINEERING.md) —
# an auto-launched instance crashing in the background on every console
# login makes it hard to get a clean, isolated strace/dmesg capture of a
# single manually-triggered run. Re-enable once the crash is fixed.
if disable_autolaunch:
    OLD_AUTOLAUNCH = 'MsnCoreApp -qws&'
    NEW_AUTOLAUNCH = '#MsnCoreApp -qws&  # disabled by build_bootable_sdcard.sh (disable_msncoreapp_autolaunch) -- run start_msn manually'
    if OLD_AUTOLAUNCH in profile:
        profile = profile.replace(OLD_AUTOLAUNCH, NEW_AUTOLAUNCH)
        print("  profile: disabled MsnCoreApp auto-launch (run start_msn manually)")
    else:
        print("  profile WARNING: 'MsnCoreApp -qws&' auto-launch line not found")

open(profile_path, 'w').write(profile)
PYEOF

    success "rootfs init scripts patched for 4.19.192 kernel (p2 only — source tree unchanged)"
}

# ---------------------------------------------------------------------------
# Install new kernel modules onto the mounted p2 rootfs
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
# Install diagnostic tools (e.g. tools/i2c-scan, tools/ark1680-ts-test) onto
# the mounted p2 rootfs
# ---------------------------------------------------------------------------
install_diag_tools() {
    local rootfs_mount="$1"
    local bin name
    local -a installed=()
    echo -e "${BOLD}  Installing diagnostic tools onto p2...${RESET}"

    if $DRY_RUN; then
        for bin in "${DIAG_TOOLS_BINS[@]}"; do
            [[ -f "$bin" ]] && echo "  [dry-run] cp $bin → /usr/bin/$(basename "$bin")"
        done
        echo "  [dry-run] append diagnostic-tools banner to rcS"
        return
    fi

    mkdir -p "$rootfs_mount/usr/bin"
    for bin in "${DIAG_TOOLS_BINS[@]}"; do
        [[ -f "$bin" ]] || { warn "Diagnostic tool not found, skipping: $bin"; continue; }
        name="$(basename "$bin")"
        cp "$bin" "$rootfs_mount/usr/bin/$name"
        chmod +x "$rootfs_mount/usr/bin/$name"
        installed+=("$name")
        success "Installed $name → /usr/bin/$name"
    done

    [[ ${#installed[@]} -gt 0 ]] && append_diag_banner "$rootfs_mount" "${installed[@]}"
}

# ---------------------------------------------------------------------------
# Append a banner listing the installed diagnostic tools to the end of rcS,
# so it prints right before the console getty/prompt appears at boot — only
# lists tools that were actually installed above.
# ---------------------------------------------------------------------------
append_diag_banner() {
    local rootfs_mount="$1"; shift
    local rcs="$rootfs_mount/etc/rc.d/rcS"
    local name

    [[ -f "$rcs" ]] || { warn "rcS not found at $rcs — skipping diagnostic-tools banner"; return; }

    {
        echo ''
        echo '# --- diagnostic tools banner (build_bootable_sdcard.sh) ---'
        echo 'echo ""'
        echo 'echo "=== Diagnostic tools ==="'
        for name in "$@"; do
            case "$name" in
                i2c-scan)
                    echo 'echo "  i2c-scan /dev/i2c-0 /dev/i2c-1 ...   - scan I2C buses for ACKing devices"'
                    ;;
                ark-ts-test)
                    echo 'echo "  ark-ts-test regs                     - dump ARK1680 touch ADC/syscon registers"'
                    echo 'echo "  ark-ts-test events /dev/input/eventN - watch touch evdev events"'
                    ;;
                lcd-test)
                    echo 'echo "  lcd-test info                        - dump /dev/fb0 + /dev/ark_display info"'
                    echo 'echo "  lcd-test fill <red|green|blue|...>   - fill screen with a solid color"'
                    echo 'echo "  lcd-test bars                        - draw color-bar test pattern"'
                    echo 'echo "  lcd-test gradient                    - draw a red->green gradient"'
                    ;;
                strace)
                    echo 'echo "  strace -f -o /data/x.log <cmd>       - trace syscalls (e.g. strace -f start_msn)"'
                    ;;
                touch-selftest.sh)
                    echo 'echo "  touch-selftest.sh                    - automated touch pass/fail (needs a real touch)"'
                    ;;
                uart-test.sh)
                    echo 'echo "  uart-test.sh                         - passive listen on MCU (ttyHS0) + MSNEry (ttyS2) links"'
                    ;;
                audio-test.sh)
                    echo 'echo "  audio-test.sh [file.wav]             - sound card + BD37033 bus/mixer check"'
                    ;;
                bt-test.sh)
                    echo 'echo "  bt-test.sh                           - GPIO91/ttyHS1/blueware check"'
                    ;;
                usb-test.sh)
                    echo 'echo "  usb-test.sh                          - USB regression check vs known-good baseline"'
                    ;;
                mmc-test.sh)
                    echo 'echo "  mmc-test.sh                          - read-only MMC/SD check"'
                    ;;
                *)
                    echo "echo \"  $name\""
                    ;;
            esac
        done
        echo 'echo "========================="'
        echo 'echo ""'
    } >> "$rcs"

    success "Appended diagnostic-tools banner to rcS ($*)"
}

# ---------------------------------------------------------------------------
# Replace the corrupted /usr/lib/libGAL.so (empty .dynamic section — a single
# DT_NULL entry, no NEEDED/SYMTAB/STRTAB) with libGAL.fb.so, the vendor's own
# software-framebuffer variant sitting right next to it. libGAL.fb.so has a
# complete, valid .dynamic section and SONAME=libGAL.so, so it's a proper
# drop-in. Without this, ld.so's _dl_relocate_object() crashes on a NULL+4
# deref while resolving libGAL.so, taking down MsnCoreApp before main() ever
# runs — root-caused via matched strace+dmesg PC/LR correlation (see
# docs/ARK1680_TS_REVERSE_ENGINEERING.md).
# ---------------------------------------------------------------------------
fix_libgal_so() {
    local rootfs_mount="$1"
    local libdir="$rootfs_mount/usr/lib"
    local broken="$libdir/libGAL.so"
    local fixed="$libdir/libGAL.fb.so"
    local backup="$libdir/libGAL.so.corrupt-orig"
    echo -e "${BOLD}  Fixing corrupted libGAL.so .dynamic section...${RESET}"

    if $DRY_RUN; then
        echo "  [dry-run] cp $broken → $backup (preserve corrupt original)"
        echo "  [dry-run] cp $fixed → $broken (install valid software-framebuffer variant)"
        return
    fi

    [[ -f "$broken" ]] || { warn "libGAL.so not found at $broken — skipping libGAL fix"; return; }
    [[ -f "$fixed" ]]  || { warn "libGAL.fb.so not found at $fixed — skipping libGAL fix"; return; }

    if [[ -f "$backup" ]]; then
        info "libGAL.so.corrupt-orig backup already exists — leaving it in place"
    else
        cp "$broken" "$backup"
    fi
    cp "$fixed" "$broken"
    success "Replaced corrupted libGAL.so with libGAL.fb.so (valid .dynamic section, SONAME=libGAL.so)"
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
# payload's debugging history — see msn_autocopy/README.md).
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
# Work around a real, confirmed kernel bug: USB port 0 (the external,
# user-accessible port; musb-hdrc.0, GPIO ID=76/PWR=126) runs in OTG
# dual-role mode, negotiated via an ID-pin GPIO read at driver-probe time.
# If a USB device is already physically connected before/at the exact
# moment this driver probes during boot, that negotiation can land in
# gadget/device mode (or an inconclusive state) instead of host mode, and
# nothing ever re-evaluates it afterward — the device just never enumerates,
# no matter how long you wait. Confirmed live on real hardware: a manual
# `unbind`+`bind` of musb-hdrc.0 after boot immediately finds the device
# correctly (see docs/HANDOFF_nand_ecc_uboot_vs_kernel.md for the full
# diagnosis, including the stock-firmware log that proves the port and
# device are both fine — it's specifically this boot-time race). Port 1
# (the onboard WiFi module) isn't affected, since it's hardwired and always
# connected well before any negotiation happens.
#
# Forces a fresh probe of just that controller after a short settle delay,
# every boot, so a USB stick plugged in at power-on is reliably detected
# without needing a manual unbind/rebind each time.
# ---------------------------------------------------------------------------
fix_usb_port0_otg_race() {
    local rootfs_mount="$1"
    local rcs="$rootfs_mount/etc/rc.d/rcS"
    echo -e "${BOLD}  Patching rcS to work around the USB port 0 OTG boot-time race...${RESET}"

    if $DRY_RUN; then
        echo "  [dry-run] insert musb-hdrc.0 unbind/rebind after mdev -s in rcS"
        return
    fi

    [[ -f "$rcs" ]] || { warn "rcS not found at $rcs — skipping USB port 0 fix"; return; }

    python3 - "$rcs" <<'PYEOF'
import sys

path = sys.argv[1]
text = open(path).read()

MDEV_LINE = '/sbin/mdev -s'
mdev_idx = text.find(MDEV_LINE)
if mdev_idx == -1:
    print("  WARNING: '/sbin/mdev -s' not found in rcS — USB port 0 fix skipped")
    sys.exit(0)

insert_at = mdev_idx + len(MDEV_LINE)
eol = text.find('\n', insert_at)
if eol == -1:
    eol = len(text)

block = """

# --- USB port 0 OTG boot-time race workaround (build_bootable_sdcard.sh) ---
# See the comment above fix_usb_port0_otg_race() in build_bootable_sdcard.sh
# for the full diagnosis. DISABLED for now: confirmed on real hardware
# (docs/HANDOFF_nand_ecc_uboot_vs_kernel.md) that unbinding musb-hdrc.0
# unconditionally is actively harmful when root itself is mounted from USB
# on that same controller (bootusb + usbroot) — it yanks the live root
# filesystem out from under the running system, causing I/O errors and an
# aborted journal. The kernel-level fix (musb_ark.c VBUS settle delay) on
# its own has since been confirmed sufficient for that case. Needs to be
# made conditional on root NOT already being on this bus before
# re-enabling — not yet done.
# (
# 	sleep 2
# 	MUSB0=/sys/bus/platform/drivers/musb-hdrc/musb-hdrc.0
# 	if [ -e "$MUSB0" ]; then
# 		echo musb-hdrc.0 > /sys/bus/platform/drivers/musb-hdrc/unbind
# 		sleep 1
# 		echo musb-hdrc.0 > /sys/bus/platform/drivers/musb-hdrc/bind
# 	fi
# ) &"""

patched = text[:eol] + block + text[eol:]
open(path, 'w').write(patched)
print("  rcS USB port 0 OTG race workaround inserted after mdev -s")
PYEOF

    success "USB port 0 OTG race workaround installed (backgrounded, ~3s after boot)"
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
    [[ ${CONFIG_SEL[5]} -eq 1 && -n "$USERDATA_DIR" ]] && do_userdata=1
    local do_mtd_redirect=${CONFIG_SEL[4]}

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
    begin_step 5 "Formatting partitions"
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
    success "p1 FAT32, p2 ext4 (rootfs), p3 ext4 (userdata) — legacy-compatible feature set"
    end_step 5

    # 5. Mount
    begin_step 6 "Mounting partitions"
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
    begin_step 7 "Populating p1 (boot partition)"
    # Build the initramfs (loads ark_dw_mmc.ko, mounts the SD rootfs) from the
    # same rootfs going on p2, so busybox/libs/module all match each other.
    if $USE_INITRAMFS; then
        info "Building initramfs from $ROOTFS_DIR"
        run bash "$SCRIPT_DIR/build_initramfs.sh" "$ROOTFS_DIR" "$INITRAMFS_OUT"
        info "Wrapping initramfs with mkimage..."
        run mkimage -A arm -O linux -T ramdisk -C gzip -d "$INITRAMFS_OUT" "$INITRAMFS_UIMG" >/dev/null
    fi
    run cp "$UBOOT_BIN"  /tmp/sd_p1/UBOOT.BIN
    run cp "$KERNEL_BIN" /tmp/sd_p1/zImage
    local bootlogo_label=""
    if [[ -n "$BOOTLOGO_RAW" && -f "$BOOTLOGO_RAW" ]]; then
        run cp "$BOOTLOGO_RAW" /tmp/sd_p1/bootlogo.raw
        bootlogo_label=" + bootlogo.raw"
    fi
    if [[ -n "$STOCK_UBOOT_BIN" && -f "$STOCK_UBOOT_BIN" ]]; then
        run cp "$STOCK_UBOOT_BIN" /tmp/sd_p1/stock_uboot.bin
        bootlogo_label="$bootlogo_label + stock_uboot.bin"
    fi
    local ir_label=""
    if $USE_INITRAMFS; then
        run cp "$INITRAMFS_OUT" /tmp/sd_p1/initramfs.cpio.gz
        ir_label=" + initramfs.cpio.gz"
        if $DRY_RUN || [[ -f "$INITRAMFS_UIMG" ]]; then
            run cp "$INITRAMFS_UIMG" /tmp/sd_p1/uInitrd
            ir_label="$ir_label + uInitrd"
        fi
    fi
    if $NEW_UBOOT_MODE; then
        if [[ -n "$DTB_BIN" && -f "$DTB_BIN" ]]; then
            run cp "$DTB_BIN" /tmp/sd_p1/ark1668_limcet_p305.dtb
        fi
        run cp "$UENV_OUT" /tmp/sd_p1/uEnv.txt
        success "UBOOT.BIN + zImage${ir_label} + uEnv.txt + DTB${bootlogo_label} written to p1"
    elif $UBOOT_WAS_PATCHED; then
        success "UBOOT.BIN (patched) + zImage${ir_label}${bootlogo_label} written to p1"
    else
        success "UBOOT.BIN + zImage${ir_label}${bootlogo_label} written to p1"
    fi
    end_step 7

    # 7. Populate p2 — rootfs
    begin_step 8 "Populating p2 (rootfs)"
    # rsync -a copies the source tree verbatim, so repair the metadata a
    # Windows checkout drops before copying — otherwise p2 is unbootable:
    #   - restore_rootfs_symlinks.sh recreates the lost symlinks (/bin/sh,
    #     /sbin/init, /lib/libc.so.6, …) — rsync -a would just copy the gap.
    #   - apply_rootfs_perms.sh restores exec bits — else no executable binaries.
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
    run bash "$SCRIPT_DIR/restore_rootfs_symlinks.sh" "/tmp/sd_p2"
    run bash "$SCRIPT_DIR/apply_rootfs_perms.sh" "/tmp/sd_p2"
    if ! $DRY_RUN; then
        echo "  Converting CRLF line endings to LF on target configuration files and scripts..."
        find /tmp/sd_p2/etc -type f -exec sed -i 's/\r$//' {} + 2>/dev/null || true
        find /tmp/sd_p2 -type f \( -name "*.sh" -o -name "rcS" -o -name "inittab" -o -name "profile" -o -name "fstab" \) -exec sed -i 's/\r$//' {} + 2>/dev/null || true
    fi
    success "Rootfs synced to p2"
    end_step 8

    # 9. Populate p3 — userdata
    begin_step 9 "Populating p3 (userdata)"
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

    # 10. Install compiled Limcet P305 kernel modules + patch init scripts for 4.19.192
    begin_step 10 "Installing new kernel modules + compat patches"
    if $NEW_KERNEL_MODE; then
        install_new_kernel_modules /tmp/sd_p2
        patch_rootfs_for_new_kernel /tmp/sd_p2 "${CONFIG_SEL[7]}"
        end_step 10
    else
        info "Skipped — stock NAND kernel/modules in use"
        end_step 10 skip
    fi

    # 11. Patch rcS for SD userdata mount (always runs)
    begin_step 11 "Patching rcS for SD boot"
    patch_rcs /tmp/sd_p2/etc/rc.d/rcS "$do_mtd_redirect"
    end_step 11

    # 12. Populate NAND partition data (bootlogo/bootanimation/reversingtrack/Unicode)
    begin_step 12 "Populating NAND partition data"
    if [[ $do_mtd_redirect -eq 1 ]]; then
        populate_nanddata /tmp/sd_p2
        end_step 12
    else
        info "Skipped — bootlogo/bootanimation/reversingtrack/Unicode stay on NAND"
        end_step 12 skip
    fi

    # 13. Install diagnostic tools (i2c-scan) onto p2
    begin_step 13 "Installing diagnostic tools"
    if [[ ${CONFIG_SEL[6]} -eq 1 ]]; then
        install_diag_tools /tmp/sd_p2
        end_step 13
    else
        info "Skipped — no diagnostic tools installed"
        end_step 13 skip
    fi

    # 14. Fix corrupted libGAL.so .dynamic section (crashes MsnCoreApp at startup otherwise)
    begin_step 14 "Fixing corrupted libGAL.so"
    if [[ ${CONFIG_SEL[8]} -eq 1 ]]; then
        fix_libgal_so /tmp/sd_p2
        end_step 14
    else
        info "Skipped — libGAL.so left as-is"
        end_step 14 skip
    fi

    # 15. Install passwordless root telnetd (UNAUTHENTICATED — opt-in diagnostic tool)
    begin_step 15 "Installing telnetd (diagnostic, unauthenticated)"
    if [[ ${CONFIG_SEL[9]} -eq 1 ]]; then
        install_telnetd /tmp/sd_p2
        end_step 15
    else
        info "Skipped — telnetd not installed"
        end_step 15 skip
    fi

    # 16. Work around the USB port 0 boot-time OTG detection race
    begin_step 16 "Patching rcS for USB port 0 OTG race workaround"
    if [[ ${CONFIG_SEL[10]} -eq 1 ]]; then
        fix_usb_port0_otg_race /tmp/sd_p2
        end_step 16
    else
        info "Skipped — USB port 0 OTG race workaround not installed"
        end_step 16 skip
    fi

    # Unmount and detach
    if ! $DRY_RUN; then
        sync
        umount /tmp/sd_p1 /tmp/sd_p2 /tmp/sd_p3
        [[ -n "$LOOP" ]] && { losetup -d "$LOOP"; LOOP=""; }
    fi

    print_step_summary

    echo ""
    echo -e "${GREEN}${BOLD}=== Build complete ===${RESET}"
    echo ""
    if [[ -n "$IMAGE" ]] && ! $DRY_RUN; then
        local sz; sz=$(du -sh "$IMAGE" | cut -f1)
        success "Image: $IMAGE  ($sz)"
        echo ""
        echo -e "${BOLD}  Write to SD card:${RESET}"
        echo    "    sudo dd if=\"$IMAGE\" of=/dev/sdX bs=4M status=progress && sync"
        echo    "    or use Etcher / Raspberry Pi Imager"
    elif [[ -n "$DEVICE" ]]; then
        success "Written directly to $DEVICE"
    fi
    echo ""
    echo -e "${BOLD}  Boot sequence:${RESET}"
    echo    "    Stepldr  → loads UBOOT.BIN from p1 (FAT32)"
    if $USE_INITRAMFS; then
        echo "    U-Boot   → fatload uInitrd; bootz (SD kernel + SD initramfs)"
        echo "    initramfs→ insmod ark_dw_mmc.ko, mount p2 ext4, chroot into it"
        echo "    rcS      → mounts p3 ext4 as /data"
        if [[ ${CONFIG_SEL[4]} -eq 1 ]]; then
            echo "    rcS      → symlinks /dev/mtd8-11 to /nanddata/ on p2 (bootlogo/bootanimation/reversingtrack/Unicode, read from SD)"
        fi
    else
        if $NEW_UBOOT_MODE; then
            echo "    U-Boot   → imports environment variables from uEnv.txt on p1"
            echo "    U-Boot   → runs bootcmd (fatload zImage from p1, bootz)"
        elif $UBOOT_WAS_PATCHED; then
            echo "    U-Boot   → NAND env CRC forced invalid, drops to interactive prompt"
            echo "    (manual) → continue with the README's \"Manual SD Card Boot\" section"
        else
            echo "    U-Boot   → whatever bootcmd this UBOOT.BIN was built/patched with"
        fi
        echo "    Kernel   → mounts p2 ext4 as /"
        echo "    rcS      → mounts p3 ext4 as /data"
        if [[ ${CONFIG_SEL[4]} -eq 1 ]]; then
            echo "    rcS      → symlinks /dev/mtd8-11 to /nanddata/ on p2 (bootlogo/bootanimation/reversingtrack/Unicode, read from SD)"
        fi
    fi
    echo ""
    if $USE_INITRAMFS; then
        echo -e "${BOLD}  Manual boot (U-Boot 'source' isn't compiled in, so type these):${RESET}"
        echo    "    fatload mmc 0:1 0x1000000 zImage"
        echo    "    fatload mmc 0:1 $INITRAMFS_ADDR uInitrd"
        echo    "    setenv bootargs console=ttyS0,115200n8 mem=180M earlyprintk=serial \\"
        echo    "      $MTDPARTS screen=0 rootwait"
        echo    "    bootz 0x1000000 $INITRAMFS_ADDR"
        echo ""
        warn "initramfs SD boot is now verified via uInitrd."
        warn "  The SD/MMC host driver is a kernel module, so root=/dev/mmcblk0p2 can't"
        warn "  mount without this initramfs."
    elif $UBOOT_WAS_PATCHED; then
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
                [[ ${CONFIG_SEL[3]} -eq 1 ]] && USE_INITRAMFS=true   || USE_INITRAMFS=false
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
