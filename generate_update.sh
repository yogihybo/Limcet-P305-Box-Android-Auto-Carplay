#!/bin/bash
# generate_update.sh — Interactive SD card update builder for ARK1680 (Prado / Limcet-P306)
#
# This script lets you choose which partitions to include in a NAND flash update,
# then generates the 'update' script that U-Boot's arkupdate command will execute.
#
# Run this under Linux or WSL. The generated output/ files then go onto a FAT32 SD card.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTPUT_DIR="$SCRIPT_DIR/sd_update/output"

# ─── Colour helpers ──────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
ok()   { echo -e "${GREEN}[OK]${NC}  $*"; }
warn() { echo -e "${YELLOW}[!!]${NC}  $*"; }
err()  { echo -e "${RED}[ERR]${NC} $*"; }
hdr()  { echo -e "\n${CYAN}$*${NC}"; }

# ─── Partition table (Prado — 106m/6m layout) ────────────────────────────────
# Format: "name|label|filename|flash_offset|flash_size|write_mode|description"
#
#   write_mode:
#     raw     — fatload + nand scrub + nand write ${filesize}
#     env     — fatload + nand scrub + nand write (fixed 0x1000 / 4096 bytes)
#     ubi     — same as raw; file is a pre-built UBI image
#     uboot   — writes to BOTH U-Boot (0x20000) and U-Boot_back (0xA0000)

PARTITIONS=(
    "uboot-env|U-Boot Environment|uboot-env.bin|0x120000|0x040000|env|U-Boot env vars (bootargs, mtdparts, etc.)"
    "uboot|U-Boot Bootloader|uboot.bin|0x020000|0x080000|uboot|2nd-stage bootloader (written to both slots)"
    "arkdata|Display Config|arkdata.ini|0x160000|0x040000|raw|TvoutType, display init parameters"
    "kernel|Linux Kernel|zImage|0x1a0000|0x400000|raw|Linux 3.4.0 zImage"
    "rootfs|Root Filesystem|rootfs.img|0x5a0000|0x6a00000|ubi|Holden-base rootfs UBI image (built by build_rootfs.sh)"
    "userdata|User Data|userdata.img|0x6fa0000|0x600000|ubi|Prado userdata UBI image (built by build_userdata.sh)"
    "bootlogo|Boot Logo|bootlogo|0x75a0000|0x080000|raw|Splash screen image"
    "reversingtrack|Reversing Audio|reversingtrack|0x7920000|0x300000|raw|Reversing camera audio track"
    "unicode|Unicode Font|unicode|0x7c20000|0x040000|raw|Unicode font data for UI text rendering — no dump yet"
)

# ─── Selection state ─────────────────────────────────────────────────────────
declare -a SELECTED
for i in "${!PARTITIONS[@]}"; do SELECTED[$i]=0; done

print_menu() {
    hdr "═══════════════════════════════════════════════════════"
    hdr "  ARK1680 Prado — SD Update Partition Selector"
    hdr "═══════════════════════════════════════════════════════"
    echo ""
    echo "  #   [ ]  Partition              Source file"
    echo "  ─   ─── ─────────────────────  ─────────────────────────────"

    for i in "${!PARTITIONS[@]}"; do
        IFS='|' read -r name label filename offset size mode desc <<< "${PARTITIONS[$i]}"
        src_path=$(find_src "$filename" "$name")
        if [[ ${SELECTED[$i]} -eq 1 ]]; then
            mark="${GREEN}[X]${NC}"
        else
            mark="[ ]"
        fi
        if [[ -n "$src_path" ]]; then
            found="${GREEN}found${NC}"
        else
            found="${RED}missing${NC}"
        fi
        printf "  %d   ${mark}  %-22s %s (%b)\n" "$((i+1))" "$label" "$filename" "$found"
        echo "           $desc"
        echo "           Flash: $offset  size: $size"
        echo ""
    done

    echo "  Commands:  1-${#PARTITIONS[@]} toggle   a=all   n=none   g=go   q=quit"
    echo ""
}

find_src() {
    local filename="$1"
    local name="$2"
    local candidates=(
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd1-mtd2_uboot/$filename"
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd4_arkdata/$filename"
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd5_kernel/$filename"
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd6_rootfs/$filename"
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd7_userdata/$filename"
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

check_sources() {
    local any_selected=0
    local any_missing=0
    for i in "${!PARTITIONS[@]}"; do
        [[ ${SELECTED[$i]} -eq 0 ]] && continue
        any_selected=1
        IFS='|' read -r name label filename offset size mode desc <<< "${PARTITIONS[$i]}"
        src_path=$(find_src "$filename" "$name")
        if [[ -z "$src_path" ]]; then
            warn "Missing: $filename  (needed for $label)"
            any_missing=1
        fi
    done
    [[ $any_selected -eq 0 ]] && { err "No partitions selected."; return 1; }
    if [[ $any_missing -eq 1 ]]; then
        echo ""
        warn "Some source files are missing. Check:"
        warn "  rootfs.img   — run build_rootfs.sh (Linux/WSL)"
        warn "  userdata.img — run build_userdata.sh (Linux/WSL)"
        warn "  uboot.bin    — copy from Prado firmware reconstructed/mtd1-mtd2_uboot/ or Holden firmware package"
        warn "  zImage       — copy from kernel/ or Holden firmware package"
        echo ""
        read -rp "Continue anyway and skip missing files? [y/N] " ans
        [[ "$ans" =~ ^[Yy]$ ]] || return 1
    fi
    return 0
}

generate_update_script() {
    mkdir -p "$OUTPUT_DIR"
    local update_file="$OUTPUT_DIR/update"
    > "$update_file"

    echo "# ARK1680 Prado NAND update script" >> "$update_file"
    echo "# Generated: $(date)" >> "$update_file"
    echo "# Partition layout: 106m rootfs / 6m userdata" >> "$update_file"
    echo "" >> "$update_file"

    local copied_files=()

    for i in "${!PARTITIONS[@]}"; do
        [[ ${SELECTED[$i]} -eq 0 ]] && continue
        IFS='|' read -r name label filename offset size mode desc <<< "${PARTITIONS[$i]}"
        src_path=$(find_src "$filename" "$name")
        [[ -z "$src_path" ]] && { warn "Skipping $label — file not found"; continue; }

        echo "# --- $label ---" >> "$update_file"

        case "$mode" in
            uboot)
                # Write to U-Boot_back first (0xA0000), then U-Boot (0x20000)
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
                # Env is fixed 4096 bytes (mkenvimage output)
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

        copied_files+=("$src_path|$filename")
    done

    ok "Generated: $update_file"
    echo ""
    echo "${copied_files[@]}"
}

copy_to_output() {
    local files_str=("$@")
    mkdir -p "$OUTPUT_DIR"

    # Copy UpConfig trigger
    cp "$SCRIPT_DIR/sd_update/UpConfig" "$OUTPUT_DIR/UpConfig"
    ok "Copied: UpConfig"

    for entry in "${files_str[@]}"; do
        [[ -z "$entry" ]] && continue
        IFS='|' read -r src_path dest_name <<< "$entry"
        cp "$src_path" "$OUTPUT_DIR/$dest_name"
        local size
        size=$(du -h "$OUTPUT_DIR/$dest_name" | cut -f1)
        ok "Copied: $dest_name  ($size)"
    done
}

print_summary() {
    hdr "═══════════════════════════════════════════════════════"
    hdr "  Output ready in: sd_update/output/"
    hdr "═══════════════════════════════════════════════════════"
    echo ""
    echo "  Files to copy to SD card root:"
    ls -lh "$OUTPUT_DIR"
    echo ""
    echo "  Next steps:"
    echo "    1. Format SD card as FAT32 (≤32GB recommended)"
    echo "    2. Copy ALL files from sd_update/output/ to the SD card root"
    echo "    3. Safely eject the SD card"
    echo "    4. Insert SD into head unit SD slot"
    echo "    5. Power on — U-Boot will detect UpConfig and run arkupdate"
    echo "    6. Wait for all partitions to flash (do not power off)"
    echo "    7. Remove SD card when unit reboots"
    echo ""
    warn "Bad block at 0x5FA0000 in this device — nand scrub handles this automatically."
    warn "Do NOT flash S-Loader (Nboot) via SD update — brick risk."
}

# ─── Main loop ───────────────────────────────────────────────────────────────
while true; do
    clear
    print_menu

    read -rp "  Selection: " input

    case "$input" in
        [1-9]|1[0-9])
            idx=$((input - 1))
            (( idx < 0 || idx >= ${#PARTITIONS[@]} )) && continue
            if [[ ${SELECTED[$idx]} -eq 1 ]]; then
                SELECTED[$idx]=0
            else
                SELECTED[$idx]=1
            fi
            ;;
        a|A)
            for i in "${!PARTITIONS[@]}"; do SELECTED[$i]=1; done
            ;;
        n|N)
            for i in "${!PARTITIONS[@]}"; do SELECTED[$i]=0; done
            ;;
        g|G)
            echo ""
            check_sources || continue
            echo ""
            hdr "Generating update script..."
            mapfile -t file_list < <(generate_update_script)
            # Last lines are the file list
            copy_to_output "${file_list[@]}"
            echo ""
            print_summary
            exit 0
            ;;
        q|Q)
            echo "Aborted."
            exit 0
            ;;
    esac
done
