#!/bin/sh
# pin-dump.sh -- dump every ARK1668 pinmux pad's live hardware mux value
# directly via devmem, bypassing the Linux pinctrl subsystem entirely.
#
# Why this exists: our reconstructed kernel's pinctrl-ark.c driver exposes
# /sys/kernel/debug/pinctrl/*/pinmux-pins showing each pin's *software*
# claim, but that's meaningless on stock firmware (3.4 kernel, legacy
# board-file GPIO/platform-device init, no devicetree/pinctrl subsystem
# at all -- see docs/I2C_GPIO0_LCD_PIN_CONFLICT.md). devmem reads the raw
# physical register, which is fixed by the SoC silicon regardless of
# which kernel/driver model is running -- so this works identically on
# both stock and our own image, for a true apples-to-apples comparison.
#
# Register/offset/mask table extracted directly from this project's
# buildable kernel tree: drivers/pinctrl/pinctrl-ark.c's ark1668_pin_map[]
# (the exact table the "arkmicro,ark1668-pinctrl" compatible string uses
# for pin index -> {reg, offset, mask}). Index N in that array == pin N
# in pinmux-pins' own numbering (confirmed: pin 2/3 = LCD r0/r1, matching
# I2C_GPIO0_LCD_PIN_CONFLICT.md's independently-confirmed live pinmux-pins
# read for those same two pins).
#
# All registers are 32-bit, at PINCTRL_BASE + reg. mask is applied after
# an (val >> offset) shift to extract each pin's raw N-bit mux value
# (compare against ARK_PVAL_0..7 in
# include/dt-bindings/pinctrl/ark-pinfunc.h -- this script prints the raw
# number, not a decoded function name, since the *available* function set
# per pin varies and isn't captured in this table).
#
# Usage: pin-dump.sh [pin ...]
#   No args: dump all 131 known pins.
#   With args: dump only the listed pin numbers (e.g. pin-dump.sh 2 3 9 121).

PINCTRL_BASE=0xe4900000
DEVMEM="$(command -v devmem 2>/dev/null)"
[ -z "$DEVMEM" ] && DEVMEM="busybox devmem"

PIN_0="0x1e4 0 0x3"
PIN_1="0x1e4 2 0x3"
PIN_2="0x1c0 0 0xf"
PIN_3="0x1c0 4 0xf"
PIN_4="0x1c0 8 0xf"
PIN_5="0x1c0 12 0xf"
PIN_6="0x1c0 16 0xf"
PIN_7="0x1c0 20 0xf"
PIN_8="0x1c0 24 0xf"
PIN_9="0x1c0 28 0xf"
PIN_10="0x1c4 0 0xf"
PIN_11="0x1c4 4 0xf"
PIN_12="0x1c4 8 0xf"
PIN_13="0x1c4 12 0xf"
PIN_14="0x1c4 16 0xf"
PIN_15="0x1c4 20 0xf"
PIN_16="0x1c4 24 0xf"
PIN_17="0x1c4 28 0xf"
PIN_18="0x1c8 0 0xf"
PIN_19="0x1c8 4 0xf"
PIN_20="0x1c8 8 0xf"
PIN_21="0x1c8 12 0xf"
PIN_22="0x1c8 16 0xf"
PIN_23="0x1c8 20 0xf"
PIN_24="0x1c8 24 0xf"
PIN_25="0x1c8 28 0xf"
PIN_26="0x1cc 0 0xf"
PIN_27="0x1cc 4 0xf"
PIN_28="0x1cc 8 0xf"
PIN_29="0x1cc 12 0xf"
PIN_30="0x1dc 0 0x3"
PIN_31="0x1dc 2 0x3"
PIN_32="0x1dc 4 0x3"
PIN_33="0x1dc 6 0x3"
PIN_34="0x1dc 8 0x3"
PIN_35="0x1dc 10 0x3"
PIN_36="0x1dc 12 0x3"
PIN_37="0x1dc 14 0x3"
PIN_38="0x1dc 16 0x3"
PIN_39="0x1d0 0 0xf"
PIN_40="0x1d0 4 0xf"
PIN_41="0x1d0 8 0xf"
PIN_42="0x1d0 12 0xf"
PIN_43="0x1d0 16 0xf"
PIN_44="0x1d0 20 0xf"
PIN_45="0x1d0 24 0xf"
PIN_46="0x1d0 28 0xf"
PIN_47="0x1d4 0 0xf"
PIN_48="0x1d4 4 0xf"
PIN_49="0x1d4 8 0xf"
PIN_50="0x1d4 12 0xf"
PIN_51="0x1d4 16 0xf"
PIN_52="0x1d4 20 0xf"
PIN_53="0x1d4 24 0xf"
PIN_54="0x1d4 28 0xf"
PIN_55="0x1d8 0 0xf"
PIN_56="0x1d8 4 0xf"
PIN_57="0x1d8 8 0xf"
PIN_58="0x1d8 12 0xf"
PIN_59="0x1d8 16 0xf"
PIN_60="0x1d8 20 0xf"
PIN_61="0x1d8 24 0xf"
PIN_62="0x1e0 0 0x3"
PIN_63="0x1e0 2 0x3"
PIN_64="0x1e0 4 0x3"
PIN_65="0x1e0 6 0x3"
PIN_66="0x1e0 8 0x3"
PIN_67="0x1e0 10 0x3"
PIN_68="0x1e0 12 0x3"
PIN_69="0x1e0 14 0x3"
PIN_70="0x1e0 16 0x3"
PIN_71="0x1e0 18 0x3"
PIN_72="0x1e4 4 0x3"
PIN_73="0x1e4 6 0x3"
PIN_74="0x1e4 8 0x3"
PIN_75="0x1e4 10 0x3"
PIN_76="0x1e4 12 0x3"
PIN_77="0x1e4 14 0x3"
PIN_78="0x1e4 16 0x3"
PIN_79="0x1e4 18 0x3"
PIN_80="0x1e4 20 0x3"
PIN_81="0x1e4 22 0x3"
PIN_82="0x1e4 24 0x3"
PIN_83="0x1e4 26 0x3"
PIN_84="0x1e4 28 0x3"
PIN_85="0x1ec 0 0x1"
PIN_86="0x1ec 1 0x1"
PIN_87="0x1ec 2 0x1"
PIN_88="0x1ec 3 0x1"
PIN_89="0x1ec 4 0x1"
PIN_90="0x1ec 5 0x1"
PIN_91="0x1ec 6 0x1"
PIN_92="0x1ec 7 0x1"
PIN_93="0x1ec 8 0x1"
PIN_94="0x1ec 9 0x1"
PIN_95="0x1ec 10 0x1"
PIN_96="0x1ec 11 0x1"
PIN_97="0x1ec 12 0x1"
PIN_98="0x1ec 13 0x1"
PIN_99="0x1ec 14 0x1"
PIN_100="0x1ec 15 0x1"
PIN_101="0x1ec 16 0x1"
PIN_102="0x1ec 17 0x1"
PIN_103="0x1ec 18 0x1"
PIN_104="0x1ec 19 0x1"
PIN_105="0x1ec 20 0x1"
PIN_106="0x1ec 21 0x1"
PIN_107="0x1ec 22 0x1"
PIN_108="0x1ec 23 0x1"
PIN_109="0x1ec 24 0x1"
PIN_110="0x1ec 25 0x1"
PIN_111="0x1ec 26 0x1"
PIN_112="0x1ec 27 0x1"
PIN_113="0x1ec 28 0x1"
PIN_114="0x1ec 29 0x1"
PIN_115="0x1ec 30 0x1"
PIN_116="0x1ec 31 0x1"
PIN_117="0x1f0 0 0x1"
PIN_118="0x1f0 1 0x1"
PIN_119="0x1f0 2 0x1"
PIN_120="0x1f0 3 0x1"
PIN_121="0x1f0 4 0x1"
PIN_122="0x1f0 5 0x1"
PIN_123="0x1f0 6 0x1"
PIN_124="0x1f0 7 0x1"
PIN_125="0x1f0 8 0x1"
PIN_126="0x1f0 9 0x1"
PIN_127="0x1f0 10 0x1"
PIN_128="0x1e4 30 0x1"
PIN_129="0x1e4 31 0x1"
PIN_130="0x1d8 31 0x1"

MAXPIN=130

read_reg() {
	# $1 = register offset (e.g. 0x1c0); caches in a shell var named
	# after the offset to avoid re-reading the same register 8-10x.
	off="$1"
	varname="REGCACHE_${off}"
	eval "cached=\$$varname"
	if [ -z "$cached" ]; then
		val=$($DEVMEM $((PINCTRL_BASE + off)) 32 2>/dev/null)
		[ -z "$val" ] && val="ERROR"
		eval "$varname=\"\$val\""
		echo "$val"
	else
		echo "$cached"
	fi
}

dump_pin() {
	pin="$1"
	eval "entry=\$PIN_${pin}"
	if [ -z "$entry" ]; then
		echo "pin $pin: (no table entry)"
		return
	fi
	reg=$(echo "$entry" | cut -d' ' -f1)
	off=$(echo "$entry" | cut -d' ' -f2)
	mask=$(echo "$entry" | cut -d' ' -f3)
	regval=$(read_reg "$reg")
	if [ "$regval" = "ERROR" ]; then
		echo "pin $pin: reg=$reg offset=$off mask=$mask -- devmem read FAILED"
		return
	fi
	pval=$(( (regval >> off) & mask ))
	printf "pin %3d: reg=%s offset=%2d mask=%s -> PVAL=%d\n" "$pin" "$reg" "$off" "$mask" "$pval"
}

if [ $# -gt 0 ]; then
	for p in "$@"; do
		dump_pin "$p"
	done
else
	p=0
	while [ "$p" -le "$MAXPIN" ]; do
		dump_pin "$p"
		p=$((p + 1))
	done
fi
