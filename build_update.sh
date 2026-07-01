#!/bin/bash
# build_update.sh — Build and SD card update tool for ARK1680 (Prado / Limcet-P306)
#
# This builds a NAND flash update package staged on an SD card — not an
# SD-boot image. On power-on, U-Boot detects UpConfig on the SD card and
# copies each selected partition below from the SD card into internal NAND
# (destructive — see "Safety notes" in README). For booting from SD/USB
# without touching NAND, see build_bootable_sdcard.sh instead.
#
# Interactive workflow combining what build_rootfs.sh, build_userdata.sh,
# and (legacy/generate_update.sh's) partition-selection + SD-package
# generation do separately, in one session with build steps for rootfs,
# userdata, and the U-Boot env image built in directly.
#
# Requires (build steps only): mkfs.ubifs, ubinize, mkenvimage
#   apt install mtd-utils u-boot-tools   (Debian/Ubuntu/WSL)
#
# ARK1680 NAND geometry:
#   Page size  = 2048 bytes   (nand_writesize)
#   PEB size   = 131072 bytes (nand_erasesize)
#   LEB size   = 126976 bytes (PEB - 2 * page)

set -e

# mtd-utils and u-boot-tools install to /usr/sbin on Debian/Ubuntu, which
# isn't always on $PATH for non-root shells (e.g. WSL, non-login shells).
# Without this, both the requirements check and the actual build steps
# below would report these tools missing even when installed.
export PATH="$PATH:/usr/sbin:/sbin"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── Paths ────────────────────────────────────────────────────────────────────

ROOTFS_DIR="$SCRIPT_DIR/Prado firmware reconstructed/mtd6_rootfs/rootfs"
ROOTFS_UBIFS="$SCRIPT_DIR/Prado firmware reconstructed/mtd6_rootfs/rootfs.ubifs"
ROOTFS_IMG="$SCRIPT_DIR/Prado firmware reconstructed/mtd6_rootfs/rootfs.img"

USERDATA_SRC="$SCRIPT_DIR/Prado firmware reconstructed/mtd7_userdata/userdata"
USERDATA_UBIFS="$SCRIPT_DIR/Prado firmware reconstructed/mtd7_userdata/userdata.ubifs"
USERDATA_IMG="$SCRIPT_DIR/Prado firmware reconstructed/mtd7_userdata/userdata.img"

ENV_SRC="$SCRIPT_DIR/env/uboot-env.txt"
ENV_BIN="$SCRIPT_DIR/env/uboot-env.bin"
ENV_SIZE=0x40000

OUTPUT_DIR="$SCRIPT_DIR/sd_update/output"

# ── NAND geometry ─────────────────────────────────────────────────────────────

MIN_IO=2048
LEB_SIZE=126976

# ── Colour helpers ────────────────────────────────────────────────────────────

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; NC='\033[0m'

ok()   { echo -e "  ${GREEN}✔${NC}  $*"; }
warn() { echo -e "  ${YELLOW}⚠${NC}  $*"; }
err()  { echo -e "  ${RED}✘${NC}  $*"; }
hdr()  { echo -e "\n${CYAN}${BOLD}$*${NC}"; }
dim()  { echo -e "${DIM}$*${NC}"; }

# ── Build: rootfs ─────────────────────────────────────────────────────────────

build_rootfs() {
    hdr "Building rootfs UBI image..."
    local cfg
    cfg="$(mktemp)"

    echo "  Source:  $ROOTFS_DIR"
    echo "  Output:  $ROOTFS_IMG"
    echo ""

    mkfs.ubifs \
        -r "$ROOTFS_DIR" \
        -m $MIN_IO \
        -e $LEB_SIZE \
        -c 875 \
        -o "$ROOTFS_UBIFS" \
        -v

    cat > "$cfg" << EOF
[rootfs]
mode=ubi
image=$ROOTFS_UBIFS
vol_id=0
vol_type=dynamic
vol_name=rootfs
vol_flags=autoresize
EOF

    ubinize \
        -o "$ROOTFS_IMG" \
        -m $MIN_IO \
        -p 131072 \
        -s 512 \
        "$cfg" \
        -v

    rm -f "$cfg"
    ok "rootfs.img ready  ($(du -h "$ROOTFS_IMG" | cut -f1))"
}

# ── Build: userdata ───────────────────────────────────────────────────────────

build_userdata() {
    hdr "Building userdata UBI image..."
    local build_dir cfg
    build_dir="$(mktemp -d)"
    cfg="$(mktemp)"

    echo "  Source:  $USERDATA_SRC"
    echo "  Output:  $USERDATA_IMG"
    echo ""

    cp -r "$USERDATA_SRC/." "$build_dir/fs/"
    mkdir -p "$build_dir/fs/msncfg"
    cp "$SCRIPT_DIR/msn_factory_configs/MsnProductInfo.ini" "$build_dir/fs/msncfg/"
    cp "$SCRIPT_DIR/msn_factory_configs/FactoryConfig.ini"  "$build_dir/fs/msncfg/"

    echo "  Userdata tree:"
    find "$build_dir/fs" -type f | sed "s|$build_dir/fs/||" | sort | sed 's/^/    /'
    echo ""

    mkfs.ubifs \
        -r "$build_dir/fs" \
        -m $MIN_IO \
        -e $LEB_SIZE \
        -c 50 \
        -o "$USERDATA_UBIFS" \
        -v

    cat > "$cfg" << EOF
[userdata]
mode=ubi
image=$USERDATA_UBIFS
vol_id=0
vol_type=dynamic
vol_name=userdata
vol_flags=autoresize
EOF

    ubinize \
        -o "$USERDATA_IMG" \
        -m $MIN_IO \
        -p 131072 \
        -s 512 \
        "$cfg" \
        -v

    rm -rf "$build_dir" "$cfg"
    ok "userdata.img ready  ($(du -h "$USERDATA_IMG" | cut -f1))"
    warn "This image erases all BT pairs, call history, and user settings on flash."
}

# ── Build: U-Boot env ─────────────────────────────────────────────────────────

build_uboot_env() {
    hdr "Building U-Boot env image..."

    echo "  Source:  $ENV_SRC"
    echo "  Output:  $ENV_BIN"
    echo ""

    mkenvimage \
        -s $((ENV_SIZE)) \
        -o "$ENV_BIN" \
        "$ENV_SRC"

    ok "uboot-env.bin ready  ($(du -h "$ENV_BIN" | cut -f1))"
}

# ── Partition / source table ──────────────────────────────────────────────────
#
# Format: "key|mtd|label|filename|offset|size|mode|description|default"
#
# Modes:
#   raw   — fatload + nand scrub + nand write ${filesize}
#   env   — fatload + nand scrub + nand write (fixed 4096 bytes)
#   ubi   — same as raw; file is a pre-built UBI image
#   uboot — writes to both U-Boot slots (0x20000 and 0xA0000)
#
# Source search order for each filename is defined in find_src().
#
# Default ON:  rootfs, userdata
# Default OFF: everything else (early boot / kernel — require deliberate opt-in)

PARTITIONS=(
    "uboot|1-2|U-Boot|uboot.bin|0x020000|0x080000|uboot|2nd-stage bootloader — written to both slots|OFF"
    "uboot-env|3|U-Boot Env|uboot-env.bin|0x120000|0x040000|env|Reconstructed env (bootdelay=9, 106m/6m layout) — build below if needed|OFF"
    "arkdata|4|Display Config|arkdata.ini|0x160000|0x040000|raw|TvoutType, display init parameters|OFF"
    "kernel|5|Linux Kernel|zImage|0x1a0000|0x400000|raw|Linux 3.4.0 zImage|OFF"
    "rootfs|6|Root Filesystem|rootfs.img|0x5a0000|0x6a00000|ubi|Reconstructed rootfs UBI image (build below if needed)|ON"
    "userdata|7|User Data|userdata.img|0x6fa0000|0x600000|ubi|Prado settings / userdata UBI image (build below if needed)|ON"
    "bootlogo|8|Boot Logo|bootlogo|0x75a0000|0x080000|raw|Splash screen image|OFF"
    "bootanimation|9|Boot Animation|bootanimation|0x7620000|0x300000|raw|Boot animation sequence|OFF"
    "reversingtrack|10|Reversing Track|reversingtrack|0x7920000|0x300000|raw|Reversing camera audio track|OFF"
    "unicode|11|Unicode Font|unicode|0x7c20000|0x040000|raw|Unicode font data for UI text rendering — no dump yet|OFF"
)

BUILD_ITEMS=(
    "rootfs|Build rootfs image|Compiles source tree into rootfs.img (~106 MB)|OFF"
    "userdata|Build userdata image|Overlays Prado settings and builds userdata.img (~6 MB)|OFF"
    "uboot-env|Build U-Boot env image|Compiles env/uboot-env.txt into uboot-env.bin (256 KB, mkenvimage)|OFF"
)

# ── Selection state ───────────────────────────────────────────────────────────

declare -a PART_SEL
declare -a BUILD_SEL

for i in "${!PARTITIONS[@]}"; do
    IFS='|' read -r _ _ _ _ _ _ _ _ default <<< "${PARTITIONS[$i]}"
    if [[ "$default" == "ON" ]]; then
        PART_SEL[$i]=1
    elif [[ "$default" == "DISABLED" ]]; then
        PART_SEL[$i]=-1
    else
        PART_SEL[$i]=0
    fi
done

for i in "${!BUILD_ITEMS[@]}"; do
    IFS='|' read -r _ _ _ default <<< "${BUILD_ITEMS[$i]}"
    [[ "$default" == "ON" ]] && BUILD_SEL[$i]=1 || BUILD_SEL[$i]=0
done

# ── Source file lookup ────────────────────────────────────────────────────────

find_src() {
    local filename="$1"
    local candidates=(
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd6_rootfs/$filename"
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd7_userdata/$filename"
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd0_sloader/$filename"
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd1-mtd2_uboot/$filename"
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd4_arkdata/$filename"
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd5_kernel/$filename"
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd8_bootlogo/$filename"
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd9_bootanimation/$filename"
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd10_reversingtrack/$filename"
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd11_unicode/$filename"
        "$SCRIPT_DIR/kernel/$filename"
        "$SCRIPT_DIR/env/$filename"
        "$SCRIPT_DIR/display/$filename"
        "$SCRIPT_DIR/$filename"
    )
    for p in "${candidates[@]}"; do
        [[ -f "$p" ]] && echo "$p" && return
    done
    echo ""
}

# ── Navigation state ──────────────────────────────────────────────────────────
#
# CURSOR indexes into NAV_TYPE/NAV_IDX, a flat list built from BUILD_ITEMS
# then PARTITIONS in display order. Disabled partitions (PART_SEL == -1)
# are left out so the cursor can never land on them.

CURSOR=0
NAV_TYPE=()
NAV_IDX=()

build_nav_list() {
    NAV_TYPE=(); NAV_IDX=()
    for i in "${!BUILD_ITEMS[@]}"; do
        NAV_TYPE+=("build"); NAV_IDX+=("$i")
    done
    for i in "${!PARTITIONS[@]}"; do
        [[ ${PART_SEL[$i]} -eq -1 ]] && continue
        NAV_TYPE+=("part"); NAV_IDX+=("$i")
    done
}

is_current() {
    # is_current <type> <idx> — true if the cursor is on this row
    [[ "${NAV_TYPE[$CURSOR]}" == "$1" && "${NAV_IDX[$CURSOR]}" == "$2" ]]
}

toggle_current() {
    local t="${NAV_TYPE[$CURSOR]}" idx="${NAV_IDX[$CURSOR]}"
    if [[ "$t" == "build" ]]; then
        [[ ${BUILD_SEL[$idx]} -eq 1 ]] && BUILD_SEL[$idx]=0 || BUILD_SEL[$idx]=1
    else
        [[ ${PART_SEL[$idx]} -eq 1 ]] && PART_SEL[$idx]=0 || PART_SEL[$idx]=1
    fi
}

# Reads one keypress, resolving arrow-key escape sequences (\x1b[A / \x1b[B)
# to a single token. Never fails the script under `set -e` — a lone Escape
# or an EOF just yields an unmatched key that the caller ignores.
read_key() {
    local key rest
    IFS= read -rsn1 key 2>/dev/null || true
    if [[ "$key" == $'\x1b' ]]; then
        IFS= read -rsn2 -t 0.05 rest 2>/dev/null || true
        key+="$rest"
    fi
    printf '%s' "$key"
}

# ── Menu rendering ────────────────────────────────────────────────────────────
#
# One line per item — no per-item description/blank lines — so the whole
# menu fits in a standard ~24-line terminal without scrolling. Full detail
# for whichever row is highlighted is shown once, in the detail line below
# the list, instead of repeated for every row.

print_detail() {
    local t="${NAV_TYPE[$CURSOR]}" idx="${NAV_IDX[$CURSOR]}"
    if [[ "$t" == "build" ]]; then
        IFS='|' read -r _ label desc _ <<< "${BUILD_ITEMS[$idx]}"
        echo -e "  ${DIM}${label}:${NC} ${DIM}$desc${NC}"
    else
        IFS='|' read -r _ mtd label _ offset size _ desc _ <<< "${PARTITIONS[$idx]}"
        echo -e "  ${DIM}${label}:${NC} ${DIM}$desc  (mtd$mtd, offset $offset, size $size)${NC}"
    fi
}

print_menu() {
    clear
    echo -e "${CYAN}${BOLD}  ARK1680 Prado — Build & Flash Tool${NC}"
    echo -e "  ${DIM}────────────────────────────────────────────────────────${NC}"

    echo -e "  ${BOLD}BUILD${NC}"
    for i in "${!BUILD_ITEMS[@]}"; do
        IFS='|' read -r key label desc _ <<< "${BUILD_ITEMS[$i]}"
        local img_path=""
        case "$key" in
            rootfs)     img_path="$ROOTFS_IMG" ;;
            userdata)   img_path="$USERDATA_IMG" ;;
            uboot-env)  img_path="$ENV_BIN" ;;
        esac
        local status
        if [[ -f "$img_path" ]]; then
            status="${GREEN}image exists ($(du -h "$img_path" | cut -f1))${NC}"
        else
            status="${YELLOW}no image yet${NC}"
        fi
        local mark
        if [[ ${BUILD_SEL[$i]} -eq 1 ]]; then
            mark="${GREEN}[B]${NC}"
        else
            mark="${DIM}[ ]${NC}"
        fi
        local cursor="  "
        is_current "build" "$i" && cursor="${CYAN}▶ ${NC}"
        printf "  %b%b  %-26s %b\n" "$cursor" "$mark" "$label" "$status"
    done

    echo ""
    echo -e "  ${BOLD}NAND PARTITIONS${NC}  ${DIM}(staged on SD, flashed to internal NAND on boot)${NC}"
    printf "${DIM}%-9s%-4s %-22s %s${NC}\n" "" "MTD" "Partition" "File"
    for i in "${!PARTITIONS[@]}"; do
        IFS='|' read -r key mtd label filename offset size mode desc _ <<< "${PARTITIONS[$i]}"
        if [[ ${PART_SEL[$i]} -eq -1 ]]; then
            printf "    ${DIM}[-]  %-4s %-22s %-16s disabled${NC}\n" "$mtd" "$label" "$filename"
            continue
        fi
        local src
        src=$(find_src "$filename")
        local mark
        if [[ ${PART_SEL[$i]} -eq 1 ]]; then
            mark="${GREEN}[X]${NC}"
        else
            mark="${DIM}[ ]${NC}"
        fi
        local found
        if [[ -n "$src" ]]; then
            found="${GREEN}found${NC}"
        elif [[ "$key" == "rootfs" || "$key" == "userdata" || "$key" == "uboot-env" ]]; then
            found="${RED}missing - build first${NC}"
        else
            found="${RED}missing${NC}"
        fi
        local caution=""
        [[ "$key" == "uboot" ]] && caution=" ${RED}⚠${NC}"
        local cursor="  "
        is_current "part" "$i" && cursor="${CYAN}▶ ${NC}"
        printf "  %b%b  %-4s %-22s %-16s %b%b\n" "$cursor" "$mark" "$mtd" "$label" "$filename" "$found" "$caution"
    done

    echo -e "  ${DIM}────────────────────────────────────────────────────────${NC}"
    print_detail
    echo -e "  ${BOLD}↑/↓${NC} move   ${BOLD}Space${NC}/${BOLD}Enter${NC} toggle   ${BOLD}a${NC}/${BOLD}n${NC} all/none   ${BOLD}g${NC} go   ${BOLD}q${NC} quit"
}

# ── Pre-flight checks ─────────────────────────────────────────────────────────
#
# Requirements — shown once at startup so missing tools are obvious before
# you've picked build steps. Informational only; check_build_tools() below
# still gates the actual build at "go" time in case something changed
# mid-session (tool installed/removed, etc).

REQUIREMENTS=(
    "mkfs.ubifs|rootfs/userdata build steps|mtd-utils"
    "ubinize|rootfs/userdata build steps|mtd-utils"
    "mkenvimage|U-Boot env build step|u-boot-tools"
)

check_requirements() {
    echo -e "${BOLD}  Requirements${NC}"
    local entry tool desc pkg any_missing=0
    for entry in "${REQUIREMENTS[@]}"; do
        IFS='|' read -r tool desc pkg <<< "$entry"
        if command -v "$tool" &>/dev/null; then
            ok "$tool  (${desc})"
        else
            warn "$tool  (${desc}) — not found, install: sudo apt install $pkg"
            any_missing=1
        fi
    done
    echo ""
    if [[ $any_missing -eq 1 ]]; then
        warn "Missing tools only block the build steps that need them — everything"
        warn "else (partition selection, SD package generation) still works."
        read -rp "  Press Enter to continue..." _
    fi
}

check_build_tools() {
    local need_ubi=0 need_env=0
    for i in "${!BUILD_SEL[@]}"; do
        [[ ${BUILD_SEL[$i]} -eq 0 ]] && continue
        IFS='|' read -r key _ _ _ <<< "${BUILD_ITEMS[$i]}"
        case "$key" in
            rootfs|userdata) need_ubi=1 ;;
            uboot-env)       need_env=1 ;;
        esac
    done
    [[ $need_ubi -eq 0 && $need_env -eq 0 ]] && return 0

    local missing=()
    if [[ $need_ubi -eq 1 ]]; then
        command -v mkfs.ubifs &>/dev/null || missing+=("mkfs.ubifs")
        command -v ubinize    &>/dev/null || missing+=("ubinize")
    fi
    if [[ $need_env -eq 1 ]]; then
        command -v mkenvimage &>/dev/null || missing+=("mkenvimage")
    fi

    if [[ ${#missing[@]} -gt 0 ]]; then
        err "Missing build tools: ${missing[*]}"
        err "Install with:  sudo apt install mtd-utils u-boot-tools"
        return 1
    fi
    return 0
}

check_partition_sources() {
    local any_selected=0
    local any_missing=0

    for i in "${!PARTITIONS[@]}"; do
        [[ ${PART_SEL[$i]} -eq 0 ]] && continue
        any_selected=1
        IFS='|' read -r key mtd label filename _ _ _ _ _ <<< "${PARTITIONS[$i]}"
        local src
        src=$(find_src "$filename")
        if [[ -z "$src" ]]; then
            warn "Missing: $filename  ($label)"
            any_missing=1
        fi
    done

    if [[ $any_selected -eq 0 ]]; then
        # No partitions selected — only building
        return 0
    fi

    if [[ $any_missing -eq 1 ]]; then
        echo ""
        warn "Some source files are missing. Build them first or select the build steps above."
        echo ""
        read -rp "  Continue anyway and skip missing files? [y/N] " ans
        [[ "$ans" =~ ^[Yy]$ ]] || return 1
    fi
    return 0
}

# ── SD card generation ────────────────────────────────────────────────────────
#
# The "update" file is a plain list of arkupdate partition keywords, one per
# line — NOT raw U-Boot nand scrub/write commands. Confirmed against the
# reference packages (Holden firmware update/update,
# Prado firmware recovery holden based/update, sd_update/update.example, all
# identical) and cross-checked against the literal
# "*****Now update <name> ......" strings compiled into uboot.bin. Offsets
# and sizes are compiled into arkupdate itself; the SD file never states them.
#
# uboot-env and unicode are intentionally left out of this map: no reference
# update file includes either, and their compiled-in messages use a
# different format ("Update U-boot-Env ......" vs "*****Now update X
# ......" for everything below), so the actual trigger keyword/mechanism for
# those two is unconfirmed. They stay flashable manually from the U-Boot
# prompt (see README) but are skipped here rather than guessed at.
declare -A ARKUPDATE_KEYWORD=(
    [uboot]=uboot
    [bootlogo]=bootlogo
    [kernel]=kernel
    [rootfs]=filesystem
    [userdata]=userdata
    [arkdata]=arkdata
    [reversingtrack]=reversingtrack
    [bootanimation]=bootanimation
)

generate_sd() {
    hdr "Generating SD card update package..."
    mkdir -p "$OUTPUT_DIR"

    local update_file="$OUTPUT_DIR/update"
    > "$update_file"

    local copied_files=() unconfirmed=()

    for i in "${!PARTITIONS[@]}"; do
        [[ ${PART_SEL[$i]} -eq 0 ]] && continue
        IFS='|' read -r key mtd label filename offset size mode desc _ <<< "${PARTITIONS[$i]}"
        local src
        src=$(find_src "$filename")
        if [[ -z "$src" ]]; then
            warn "Skipping $label — file not found"
            continue
        fi

        local keyword="${ARKUPDATE_KEYWORD[$key]:-}"
        if [[ -z "$keyword" ]]; then
            unconfirmed+=("$label")
            continue
        fi

        echo "$keyword" >> "$update_file"
        copied_files+=("$src|$filename")
    done

    if [[ ${#unconfirmed[@]} -gt 0 ]]; then
        warn "Not included in update (unconfirmed arkupdate keyword): ${unconfirmed[*]}"
        warn "Flash these manually from the U-Boot prompt instead — see README."
    fi

    # Copy UpConfig trigger and all selected files to output
    cp "$SCRIPT_DIR/sd_update/UpConfig" "$OUTPUT_DIR/UpConfig"
    ok "UpConfig"

    for entry in "${copied_files[@]}"; do
        [[ -z "$entry" ]] && continue
        IFS='|' read -r src dest <<< "$entry"
        cp "$src" "$OUTPUT_DIR/$dest"
        ok "$dest  ($(du -h "$OUTPUT_DIR/$dest" | cut -f1))"
    done

    ok "update script"
}

print_summary() {
    hdr "Output ready"
    echo ""
    echo "  Files to copy to SD card root (FAT32):"
    echo ""
    ls -lh "$OUTPUT_DIR" | tail -n +2 | sed 's/^/    /'
    echo ""
    echo -e "  ${BOLD}Next steps:${NC}"
    echo "    1. Format SD card as FAT32 (≤32 GB)"
    echo "    2. Copy ALL files from sd_update/output/ to the SD card root"
    echo "    3. Safely eject the SD card"
    echo "    4. Insert SD into head unit SD slot"
    echo "    5. Power on — U-Boot detects UpConfig and runs arkupdate"
    echo "    6. Wait for all partitions to flash (do not interrupt power)"
    echo "    7. Remove SD card when unit reboots"
    echo ""
    warn "Bad block at 0x5FA0000 in this device — nand scrub handles this automatically."
    warn "Never flash S-Loader (Nboot) via SD — brick risk with no software recovery."
}

# ── Main loop ─────────────────────────────────────────────────────────────────
#
# Arrow keys move the highlighted row; Space/Enter toggles it. a/n/g/q act
# immediately on keypress — no need to press Enter afterwards.

clear
echo -e "${CYAN}${BOLD}  ARK1680 Prado — Build & Flash Tool${NC}"
echo ""
check_requirements

build_nav_list

while true; do
    print_menu
    key=$(read_key)

    case "$key" in
        $'\x1b[A')  (( CURSOR > 0 )) && CURSOR=$((CURSOR - 1)) ;;
        $'\x1b[B')  (( CURSOR < ${#NAV_TYPE[@]} - 1 )) && CURSOR=$((CURSOR + 1)) ;;
        ''|' ')     toggle_current ;;
        a|A)
            for i in "${!PARTITIONS[@]}"; do
                [[ ${PART_SEL[$i]} -ne -1 ]] && PART_SEL[$i]=1
            done
            ;;
        n|N)
            for i in "${!PARTITIONS[@]}"; do
                [[ ${PART_SEL[$i]} -ne -1 ]] && PART_SEL[$i]=0
            done
            ;;
        g|G)
            echo ""
            check_build_tools || { read -rp "  Press Enter to continue..." _; continue; }

            # Run selected build steps first, so check_partition_sources below
            # doesn't warn about files these steps are about to create.
            for i in "${!BUILD_ITEMS[@]}"; do
                [[ ${BUILD_SEL[$i]} -eq 0 ]] && continue
                IFS='|' read -r key _ _ _ <<< "${BUILD_ITEMS[$i]}"
                case "$key" in
                    rootfs)    build_rootfs    ;;
                    userdata)  build_userdata  ;;
                    uboot-env) build_uboot_env ;;
                esac
            done

            check_partition_sources || continue

            # Check if any partitions selected before generating SD package
            any_part=0
            for i in "${!PARTITIONS[@]}"; do
                [[ ${PART_SEL[$i]} -eq 1 ]] && any_part=1 && break
            done

            if [[ $any_part -eq 1 ]]; then
                generate_sd
                echo ""
                print_summary
            elif [[ ${BUILD_SEL[0]} -eq 1 || ${BUILD_SEL[1]} -eq 1 ]]; then
                echo ""
                ok "Build complete. No partitions selected — SD package not generated."
            else
                err "Nothing selected."
            fi

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
