#!/bin/bash
# build.sh — Build and SD card update tool for ARK1680 (Prado / Limcet-P306)
#
# Interactive workflow combining:
#   build_rootfs.sh    — builds rootfs UBI image from source tree
#   build_userdata.sh  — builds userdata UBI image with Prado settings overlay
#   generate_update.sh — selects partitions and generates SD card update package
#
# Requires (build steps only): mkfs.ubifs, ubinize
#   apt install mtd-utils   (Debian/Ubuntu/WSL)
#
# ARK1680 NAND geometry:
#   Page size  = 2048 bytes   (nand_writesize)
#   PEB size   = 131072 bytes (nand_erasesize)
#   LEB size   = 126976 bytes (PEB - 2 * page)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── Paths ────────────────────────────────────────────────────────────────────

ROOTFS_DIR="$SCRIPT_DIR/Prado reconstructed/mtd6_rootfs/rootfs"
ROOTFS_UBIFS="$SCRIPT_DIR/Prado reconstructed/mtd6_rootfs/rootfs.ubifs"
ROOTFS_IMG="$SCRIPT_DIR/Prado reconstructed/mtd6_rootfs/rootfs.img"

USERDATA_SRC="$SCRIPT_DIR/Prado reconstructed/mtd7_userdata/userdata"
USERDATA_UBIFS="$SCRIPT_DIR/Prado reconstructed/mtd7_userdata/userdata.ubifs"
USERDATA_IMG="$SCRIPT_DIR/Prado reconstructed/mtd7_userdata/userdata.img"

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

# ── Partition / source table ──────────────────────────────────────────────────
#
# Format: "key|label|filename|offset|size|mode|description"
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
    "rootfs|Root Filesystem|rootfs.img|0x5a0000|0x6a00000|ubi|Reconstructed rootfs UBI image (build below if needed)|ON"
    "userdata|User Data|userdata.img|0x6fa0000|0x600000|ubi|Prado settings / userdata UBI image (build below if needed)|ON"
    "kernel|Linux Kernel|zImage|0x1a0000|0x400000|raw|Linux 3.4.0 zImage|OFF"
    "arkdata|Display Config|arkdata.ini|0x160000|0x040000|raw|TvoutType, display init parameters|OFF"
    "uboot-env|U-Boot Env|uboot-env.bin|0x120000|0x040000|env|U-Boot environment variables|OFF"
    "uboot|U-Boot|uboot.bin|0x020000|0x080000|uboot|2nd-stage bootloader — written to both slots|OFF"
    "bootlogo|Boot Logo|bootlogo|0x75a0000|0x080000|raw|Splash screen image|OFF"
    "reversingtrack|Reversing Audio|reversingtrack|0x7920000|0x300000|raw|Reversing camera audio track|OFF"
)

BUILD_ITEMS=(
    "rootfs|Build rootfs image|Compiles source tree into rootfs.img (~106 MB)|OFF"
    "userdata|Build userdata image|Overlays Prado settings and builds userdata.img (~6 MB)|OFF"
)

# ── Selection state ───────────────────────────────────────────────────────────

declare -a PART_SEL
declare -a BUILD_SEL

for i in "${!PARTITIONS[@]}"; do
    IFS='|' read -r _ _ _ _ _ _ _ default <<< "${PARTITIONS[$i]}"
    [[ "$default" == "ON" ]] && PART_SEL[$i]=1 || PART_SEL[$i]=0
done

for i in "${!BUILD_ITEMS[@]}"; do
    IFS='|' read -r _ _ _ default <<< "${BUILD_ITEMS[$i]}"
    [[ "$default" == "ON" ]] && BUILD_SEL[$i]=1 || BUILD_SEL[$i]=0
done

# ── Source file lookup ────────────────────────────────────────────────────────

find_src() {
    local filename="$1"
    local candidates=(
        "$SCRIPT_DIR/Prado reconstructed/mtd6_rootfs/$filename"
        "$SCRIPT_DIR/Prado reconstructed/mtd7_userdata/$filename"
        "$SCRIPT_DIR/Prado reconstructed/mtd0_sloader/$filename"
        "$SCRIPT_DIR/Prado reconstructed/mtd1-mtd2_uboot/$filename"
        "$SCRIPT_DIR/Prado reconstructed/mtd8_bootlogo/$filename"
        "$SCRIPT_DIR/Prado reconstructed/mtd10_reversingtrack/$filename"
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

# ── Menu rendering ────────────────────────────────────────────────────────────

print_menu() {
    clear
    echo ""
    echo -e "${CYAN}${BOLD}  ╔══════════════════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}${BOLD}  ║   ARK1680 Prado — Build & Flash Tool                ║${NC}"
    echo -e "${CYAN}${BOLD}  ╚══════════════════════════════════════════════════════╝${NC}"

    # ── Build section ──
    echo ""
    echo -e "  ${BOLD}BUILD${NC}"
    echo -e "  ${DIM}──────────────────────────────────────────────────────${NC}"

    local total=${#PARTITIONS[@]}

    for i in "${!BUILD_ITEMS[@]}"; do
        IFS='|' read -r key label desc _ <<< "${BUILD_ITEMS[$i]}"
        local num=$((total + i + 1))
        local img_path=""
        local status=""
        if [[ "$key" == "rootfs" ]]; then
            img_path="$ROOTFS_IMG"
        else
            img_path="$USERDATA_IMG"
        fi
        if [[ -f "$img_path" ]]; then
            status="${GREEN}image exists — $(du -h "$img_path" | cut -f1)${NC}"
        else
            status="${YELLOW}no image yet${NC}"
        fi
        if [[ ${BUILD_SEL[$i]} -eq 1 ]]; then
            mark="${GREEN}[B]${NC}"
        else
            mark="${DIM}[ ]${NC}"
        fi
        printf "  %d  %b  %-24s" "$num" "$mark" "$label"
        echo -e "  $status"
        echo -e "      ${DIM}$desc${NC}"
        echo ""
    done

    # ── Partition section ──
    echo -e "  ${BOLD}SD CARD PARTITIONS${NC}"
    echo -e "  ${DIM}──────────────────────────────────────────────────────${NC}"
    echo -e "  ${DIM}#    [ ]  Partition              File              Status${NC}"
    echo ""

    for i in "${!PARTITIONS[@]}"; do
        IFS='|' read -r key label filename offset size mode desc _ <<< "${PARTITIONS[$i]}"
        local src
        src=$(find_src "$filename")
        if [[ ${PART_SEL[$i]} -eq 1 ]]; then
            mark="${GREEN}[X]${NC}"
        else
            mark="${DIM}[ ]${NC}"
        fi
        if [[ -n "$src" ]]; then
            found="${GREEN}found${NC}"
        else
            found="${RED}missing${NC}"
        fi
        # Safety flag for early-boot partitions
        local caution=""
        [[ "$key" == "uboot" ]] && caution=" ${RED}⚠ brick risk${NC}"
        printf "  %d    %b  %-22s %-18s %b%b\n" \
            "$((i+1))" "$mark" "$label" "$filename" "$found" "$caution"
        echo -e "       ${DIM}$desc   $offset  $size${NC}"
        echo ""
    done

    echo -e "  ${DIM}──────────────────────────────────────────────────────${NC}"
    echo ""
    echo -e "  ${BOLD}Commands:${NC}"
    echo -e "    ${BOLD}1–${#PARTITIONS[@]}${NC}  toggle partition     ${BOLD}${#PARTITIONS[@]}+${NC}  toggle build step"
    echo -e "    ${BOLD}a${NC}    select all partitions   ${BOLD}n${NC}   deselect all partitions"
    echo -e "    ${BOLD}g${NC}    go (build selected, generate SD package)"
    echo -e "    ${BOLD}q${NC}    quit"
    echo ""
}

# ── Pre-flight checks ─────────────────────────────────────────────────────────

check_build_tools() {
    local need_tools=0
    for i in "${!BUILD_SEL[@]}"; do
        [[ ${BUILD_SEL[$i]} -eq 1 ]] && need_tools=1 && break
    done
    [[ $need_tools -eq 0 ]] && return 0

    local missing=()
    command -v mkfs.ubifs &>/dev/null || missing+=("mkfs.ubifs")
    command -v ubinize    &>/dev/null || missing+=("ubinize")

    if [[ ${#missing[@]} -gt 0 ]]; then
        err "Missing build tools: ${missing[*]}"
        err "Install with:  sudo apt install mtd-utils"
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
        IFS='|' read -r key label filename _ _ _ _ _ <<< "${PARTITIONS[$i]}"
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

generate_sd() {
    hdr "Generating SD card update package..."
    mkdir -p "$OUTPUT_DIR"

    local update_file="$OUTPUT_DIR/update"
    > "$update_file"

    cat >> "$update_file" << EOF
# ARK1680 Prado NAND update script
# Generated: $(date)
# Partition layout: 106m rootfs / 6m userdata

EOF

    local copied_files=()

    for i in "${!PARTITIONS[@]}"; do
        [[ ${PART_SEL[$i]} -eq 0 ]] && continue
        IFS='|' read -r key label filename offset size mode desc _ <<< "${PARTITIONS[$i]}"
        local src
        src=$(find_src "$filename")
        if [[ -z "$src" ]]; then
            warn "Skipping $label — file not found"
            continue
        fi

        echo "# --- $label ---" >> "$update_file"

        case "$mode" in
            uboot)
                cat >> "$update_file" << EOF
fatload mmc 0 4000000 $filename
nand scrub 0xa0000 0x80000 0xa0000 0x80000
nand write 0x4000000 0xa0000 \${filesize}
fatload mmc 0 4000000 $filename
nand scrub 0x20000 0x80000 0x20000 0x80000
nand write 0x4000000 0x20000 \${filesize}

EOF
                ;;
            env)
                cat >> "$update_file" << EOF
fatload mmc 0 4000000 $filename
nand scrub $offset $size $offset $size
nand write 0x4000000 $offset 0x1000

EOF
                ;;
            raw|ubi)
                cat >> "$update_file" << EOF
fatload mmc 0 4000000 $filename
nand scrub $offset $size $offset $size
nand write 0x4000000 $offset \${filesize}

EOF
                ;;
        esac

        copied_files+=("$src|$filename")
    done

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

total_parts=${#PARTITIONS[@]}
total_build=${#BUILD_ITEMS[@]}

while true; do
    print_menu
    read -rp "  Selection: " input

    # Number keys
    if [[ "$input" =~ ^[0-9]+$ ]]; then
        if (( input >= 1 && input <= total_parts )); then
            idx=$((input - 1))
            [[ ${PART_SEL[$idx]} -eq 1 ]] && PART_SEL[$idx]=0 || PART_SEL[$idx]=1
        elif (( input > total_parts && input <= total_parts + total_build )); then
            idx=$((input - total_parts - 1))
            [[ ${BUILD_SEL[$idx]} -eq 1 ]] && BUILD_SEL[$idx]=0 || BUILD_SEL[$idx]=1
        fi
        continue
    fi

    case "$input" in
        a|A)
            for i in "${!PARTITIONS[@]}"; do PART_SEL[$i]=1; done
            ;;
        n|N)
            for i in "${!PARTITIONS[@]}"; do PART_SEL[$i]=0; done
            ;;
        g|G)
            echo ""
            check_build_tools || { read -rp "  Press Enter to continue..." _; continue; }
            check_partition_sources || continue

            # Run selected build steps
            for i in "${!BUILD_ITEMS[@]}"; do
                [[ ${BUILD_SEL[$i]} -eq 0 ]] && continue
                IFS='|' read -r key _ _ _ <<< "${BUILD_ITEMS[$i]}"
                case "$key" in
                    rootfs)   build_rootfs   ;;
                    userdata) build_userdata ;;
                esac
            done

            # Check if any partitions selected before generating SD package
            local any_part=0
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
