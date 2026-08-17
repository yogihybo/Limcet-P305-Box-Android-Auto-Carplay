#!/usr/bin/env python3
"""
MCU Firmware Rebuilder & CAN Code Patcher
Target: STM32F105RBT6 companion MCU firmware (can_app.bin)
Link Address: 0x08004000
"""

import sys
import os
import struct
import json
import argparse
import hashlib

BASE_ADDR = 0x08004000

CAN_MODE_TABLES = {
    3: {"addr": 0x0800BAE8, "entries": 9, "name": "Mode 3"},
    2: {"addr": 0x0800BB30, "entries": 10, "name": "Mode 2"},
    1: {"addr": 0x0800BB80, "entries": 9, "name": "Mode 1"}
}

class MCURebuilder:
    def __init__(self, stock_bin_path, base_addr=BASE_ADDR):
        self.stock_bin_path = stock_bin_path
        self.base_addr = base_addr
        with open(stock_bin_path, "rb") as f:
            self.data = bytearray(f.read())
        self.size = len(self.data)
        
        # Verify basic sanity
        if self.size != 31996:
            print(f"[!] Warning: Input file size is {self.size} bytes (expected 31996 bytes).", file=sys.stderr)
            
    def write_u32(self, addr, val):
        offset = addr - self.base_addr
        if offset < 0 or offset + 4 > self.size:
            raise ValueError(f"Address 0x{addr:08x} out of bounds (offset {offset})")
        self.data[offset:offset+4] = struct.pack("<I", val)

    def write_u16(self, addr, val):
        offset = addr - self.base_addr
        if offset < 0 or offset + 2 > self.size:
            raise ValueError(f"Address 0x{addr:08x} out of bounds (offset {offset})")
        self.data[offset:offset+2] = struct.pack("<H", val)

    def write_u8(self, addr, val):
        offset = addr - self.base_addr
        if offset < 0 or offset + 1 > self.size:
            raise ValueError(f"Address 0x{addr:08x} out of bounds (offset {offset})")
        self.data[offset] = val

    def read_u32(self, addr):
        offset = addr - self.base_addr
        return struct.unpack("<I", self.data[offset:offset+4])[0]

    def patch_can_table_entry(self, mode, index, new_can_id, new_handler=None):
        if mode not in CAN_MODE_TABLES:
            raise ValueError(f"Invalid mode {mode}. Must be 1, 2, or 3.")
        info = CAN_MODE_TABLES[mode]
        if index < 0 or index >= info["entries"]:
            raise ValueError(f"Invalid index {index} for mode {mode} (max {info['entries']-1}).")
            
        entry_addr = info["addr"] + index * 8
        old_can_id = self.read_u32(entry_addr)
        self.write_u32(entry_addr, new_can_id)
        
        if new_handler is not None:
            old_handler = self.read_u32(entry_addr + 4)
            self.write_u32(entry_addr + 4, new_handler)
            print(f"  [Mode {mode}][{index}] CAN ID: 0x{old_can_id:03x} -> 0x{new_can_id:03x}, Handler: 0x{old_handler:08x} -> 0x{new_handler:08x}")
        else:
            print(f"  [Mode {mode}][{index}] CAN ID: 0x{old_can_id:03x} -> 0x{new_can_id:03x}")

    def apply_config(self, config_dict):
        print("[*] Applying CAN configuration...")
        
        # Apply Table overrides
        tables_cfg = config_dict.get("tables", {})
        for mode_key, mode_info in tables_cfg.items():
            mode_num = None
            if "mode1" in mode_key or mode_key == "1":
                mode_num = 1
            elif "mode2" in mode_key or mode_key == "2":
                mode_num = 2
            elif "mode3" in mode_key or mode_key == "3":
                mode_num = 3
                
            if mode_num and "entries" in mode_info:
                for entry in mode_info["entries"]:
                    idx = entry.get("index")
                    can_id_str = entry.get("can_id")
                    handler_str = entry.get("handler")
                    
                    if idx is not None and can_id_str is not None:
                        can_id = int(can_id_str, 0) if isinstance(can_id_str, str) else can_id_str
                        handler = int(handler_str, 0) if (handler_str and isinstance(handler_str, str)) else handler_str
                        self.patch_can_table_entry(mode_num, idx, can_id, handler)

        # Apply raw patches if specified
        raw_patches = config_dict.get("raw_patches", [])
        for p in raw_patches:
            addr = int(p["addr"], 0) if isinstance(p["addr"], str) else p["addr"]
            hex_data = bytes.fromhex(p["hex_data"].replace(" ", ""))
            offset = addr - self.base_addr
            self.data[offset:offset+len(hex_data)] = hex_data
            print(f"  [Raw Patch] Address 0x{addr:08x} ({len(hex_data)} bytes)")

    def apply_preset(self, preset_name):
        print(f"[*] Applying Preset: {preset_name}")
        if preset_name == "toyota_prado_150":
            # Prado SWC is CAN ID 0x3C4 (964 dec)
            # Patch Mode 1 Entry 7 (Stock 0x105 -> 0x3C4)
            self.patch_can_table_entry(1, 7, 0x3c4)
            # Mode 1 Entry 4 Reverse / Status (Stock 0x185 -> 0x025)
            self.patch_can_table_entry(1, 4, 0x025)
        else:
            raise ValueError(f"Unknown preset: {preset_name}")

    def save(self, out_path):
        # Validate output
        if len(self.data) != self.size:
            raise ValueError(f"Output binary size mismatch: {len(self.data)} vs {self.size}")
            
        with open(out_path, "wb") as f:
            f.write(self.data)
            
        md5 = hashlib.md5(self.data).hexdigest()
        print(f"[+] Successfully rebuilt MCU firmware:")
        print(f"    Path:   {out_path}")
        print(f"    Size:   {len(self.data)} bytes")
        print(f"    MD5:    {md5}")

def main():
    parser = argparse.ArgumentParser(description="Rebuild and patch STM32 MCU firmware (can_app.bin)")
    parser.add_argument("input_binary", help="Path to input stock can_app.bin")
    parser.add_argument("-o", "--output", default="can_app_rebuilt.bin", help="Path for rebuilt can_app.bin")
    parser.add_argument("--config", help="Path to JSON configuration file")
    parser.add_argument("--preset", choices=["toyota_prado_150"], help="Apply a predefined vehicle preset")
    parser.add_argument("--set-id", nargs=3, metavar=("MODE", "INDEX", "CAN_ID"), help="Set single CAN ID (e.g. --set-id 1 7 0x3c4)")
    args = parser.parse_args()

    if not os.path.isfile(args.input_binary):
        print(f"Error: input file not found: {args.input_binary}", file=sys.stderr)
        sys.exit(1)

    rebuilder = MCURebuilder(args.input_binary)

    if args.config:
        with open(args.config, "r") as f:
            cfg = json.load(f)
        rebuilder.apply_config(cfg)

    if args.preset:
        rebuilder.apply_preset(args.preset)

    if args.set_id:
        mode = int(args.set_id[0])
        idx = int(args.set_id[1])
        can_id = int(args.set_id[2], 0)
        rebuilder.patch_can_table_entry(mode, idx, can_id)

    rebuilder.save(args.output)

if __name__ == "__main__":
    main()
