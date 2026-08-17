#!/usr/bin/env python3
"""
MCU Firmware Decompiler & CAN Dispatch Subsystem Analyzer
Target: STM32F105RBT6 companion MCU firmware (can_app.bin)
Link Address: 0x08004000
"""

import sys
import os
import struct
import json
import argparse
import capstone

BASE_ADDR = 0x08004000

# Interrupt vector names for STM32F105 (Connectivity line)
SYS_VECTORS = [
    "Initial_SP", "Reset_Handler", "NMI_Handler", "HardFault_Handler",
    "MemManage_Handler", "BusFault_Handler", "UsageFault_Handler",
    "Reserved_0x1C", "Reserved_0x20", "Reserved_0x24", "Reserved_0x28",
    "SVC_Handler", "DebugMon_Handler", "Reserved_0x34", "PendSV_Handler",
    "SysTick_Handler"
]

IRQ_VECTORS = [
    "WWDG", "PVD", "TAMPER", "RTC", "FLASH", "RCC", "EXTI0", "EXTI1",
    "EXTI2", "EXTI3", "EXTI4", "DMA1_C1", "DMA1_C2", "DMA1_C3", "DMA1_C4",
    "DMA1_C5", "DMA1_C6", "DMA1_C7", "ADC1_2", "CAN1_TX", "CAN1_RX0", "CAN1_RX1",
    "CAN1_SCE", "EXTI9_5", "TIM1_BRK", "TIM1_UP", "TIM1_TRG_COM", "TIM1_CC",
    "TIM2", "TIM3", "TIM4", "I2C1_EV", "I2C1_ER", "I2C2_EV", "I2C2_ER",
    "SPI1", "SPI2", "USART1", "USART2", "USART3", "EXTI15_10", "RTCAlarm",
    "OTG_FS_WKUP", "Reserved_0xEC", "Reserved_0xF0", "Reserved_0xF4", "Reserved_0xF8",
    "Reserved_0xFC", "Reserved_0x100", "Reserved_0x104", "TIM5", "SPI3",
    "UART4", "UART5", "TIM6", "TIM7", "DMA2_C1", "DMA2_C2", "DMA2_C3",
    "DMA2_C4", "DMA2_C5", "ETH", "ETH_WKUP", "CAN2_TX", "CAN2_RX0", "CAN2_RX1",
    "CAN2_SCE", "OTG_FS"
]

# Known table addresses in stock can_app.bin
UART_CMD_TABLE_ADDR = 0x0800B9E4
UART_CMD_TABLE_ENTRIES = 9

CAN_MODE_TABLES = {
    3: {"addr": 0x0800BAE8, "entries": 9, "name": "Mode 3 (Profile 3)"},
    2: {"addr": 0x0800BB30, "entries": 10, "name": "Mode 2 (Profile 2)"},
    1: {"addr": 0x0800BB80, "entries": 9, "name": "Mode 1 (Profile 1)"}
}

class MCUDecompiler:
    def __init__(self, bin_path, base_addr=BASE_ADDR):
        self.bin_path = bin_path
        self.base_addr = base_addr
        with open(bin_path, "rb") as f:
            self.data = f.read()
        self.size = len(self.data)
        
        self.md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
        self.md.detail = True
        
    def read_u32(self, addr):
        offset = addr - self.base_addr
        if offset < 0 or offset + 4 > self.size:
            return None
        return struct.unpack("<I", self.data[offset:offset+4])[0]

    def read_u16(self, addr):
        offset = addr - self.base_addr
        if offset < 0 or offset + 2 > self.size:
            return None
        return struct.unpack("<H", self.data[offset:offset+2])[0]

    def read_u8(self, addr):
        offset = addr - self.base_addr
        if offset < 0 or offset + 1 > self.size:
            return None
        return self.data[offset]

    def get_code_slice(self, addr, max_bytes=256):
        offset = addr - self.base_addr
        if offset < 0 or offset >= self.size:
            return b""
        return self.data[offset:min(self.size, offset + max_bytes)]

    def parse_vectors(self):
        vectors = {}
        for i, name in enumerate(SYS_VECTORS):
            addr = self.base_addr + i * 4
            val = self.read_u32(addr)
            vectors[name] = {
                "vector_num": i,
                "addr": hex(addr),
                "target": hex(val) if val is not None else None,
                "is_active": (val != 0 and val != 0x08004253)
            }
        for i, name in enumerate(IRQ_VECTORS):
            v_num = i + 16
            addr = self.base_addr + v_num * 4
            val = self.read_u32(addr)
            if val is not None:
                vectors[name] = {
                    "vector_num": v_num,
                    "irq_num": i,
                    "addr": hex(addr),
                    "target": hex(val),
                    "is_active": (val != 0 and val != 0x08004253)
                }
        return vectors

    def parse_uart_table(self):
        entries = []
        for i in range(UART_CMD_TABLE_ENTRIES):
            addr = UART_CMD_TABLE_ADDR + i * 8
            cmd_byte = self.read_u8(addr)
            handler_ptr = self.read_u32(addr + 4)
            entries.append({
                "index": i,
                "table_addr": hex(addr),
                "cmd": hex(cmd_byte) if cmd_byte is not None else None,
                "handler": hex(handler_ptr) if handler_ptr is not None else None,
                "handler_raw": handler_ptr
            })
        return entries

    def parse_can_tables(self):
        tables = {}
        for mode, info in CAN_MODE_TABLES.items():
            tbl_entries = []
            for i in range(info["entries"]):
                addr = info["addr"] + i * 8
                can_id = self.read_u32(addr)
                handler_ptr = self.read_u32(addr + 4)
                tbl_entries.append({
                    "index": i,
                    "entry_addr": hex(addr),
                    "can_id": hex(can_id) if can_id is not None else None,
                    "can_id_dec": can_id,
                    "handler": hex(handler_ptr) if handler_ptr is not None else None,
                    "handler_raw": handler_ptr
                })
            tables[mode] = {
                "name": info["name"],
                "base_addr": hex(info["addr"]),
                "entry_count": info["entries"],
                "entries": tbl_entries
            }
        return tables

    def disassemble_function(self, func_addr, max_instructions=80):
        target_addr = func_addr & ~1
        code_bytes = self.get_code_slice(target_addr, max_instructions * 4)
        instructions = []
        
        for ins in self.md.disasm(code_bytes, target_addr):
            instructions.append({
                "addr": hex(ins.address),
                "mnemonic": ins.mnemonic,
                "op_str": ins.op_str,
                "bytes": ins.bytes.hex()
            })
            if len(instructions) >= max_instructions:
                break
            if ins.mnemonic in ("bx", "pop") and any(r in ins.op_str for r in ("pc", "lr")):
                break
                
        return instructions

    def analyze_can_handler(self, handler_addr):
        disas = self.disassemble_function(handler_addr)
        offsets_accessed = []
        masks_used = []
        calls_made = []

        for ins in disas:
            op = ins["op_str"]
            mn = ins["mnemonic"]
            if "ldrb" in mn or "strb" in mn or "ldr" in mn or "str" in mn:
                if "#0x" in op or "#" in op:
                    offsets_accessed.append(f"{mn} {op}")
            if mn in ("and", "ands", "tst", "cmp", "movw", "movs", "mov"):
                if "#0x" in op or "#" in op:
                    masks_used.append(f"{mn} {op}")
            if mn in ("bl", "blx"):
                calls_made.append(f"{mn} {op}")

        return {
            "disassembly": disas,
            "calls": calls_made,
            "potential_masks": masks_used[:10],
            "accesses": offsets_accessed[:10]
        }

    def generate_full_report(self):
        vectors = self.parse_vectors()
        uart_table = self.parse_uart_table()
        can_tables = self.parse_can_tables()
        
        can_analysis = {}
        for mode, tbl in can_tables.items():
            can_analysis[str(mode)] = []
            for entry in tbl["entries"]:
                raw_ptr = entry["handler_raw"]
                if raw_ptr and raw_ptr != 0:
                    analysis = self.analyze_can_handler(raw_ptr)
                    entry_copy = dict(entry)
                    entry_copy["analysis"] = analysis
                    can_analysis[str(mode)].append(entry_copy)
                else:
                    can_analysis[str(mode)].append(entry)

        report = {
            "metadata": {
                "file": self.bin_path,
                "size_bytes": self.size,
                "base_addr": hex(self.base_addr),
                "initial_sp": hex(self.read_u32(self.base_addr)),
                "reset_handler": hex(self.read_u32(self.base_addr + 4))
            },
            "vectors": vectors,
            "uart_commands": uart_table,
            "can_tables": can_tables,
            "can_handler_analysis": can_analysis
        }
        return report

def main():
    parser = argparse.ArgumentParser(description="Decompile and analyze STM32 MCU can_app.bin")
    parser.add_argument("binary", help="Path to can_app.bin")
    parser.add_argument("--json-out", help="Path to save full JSON analysis report")
    parser.add_argument("--summary", action="store_true", help="Print summary table to console")
    args = parser.parse_args()

    if not os.path.isfile(args.binary):
        print(f"Error: file not found: {args.binary}", file=sys.stderr)
        sys.exit(1)

    decompiler = MCUDecompiler(args.binary)
    report = decompiler.generate_full_report()

    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(report, f, indent=2)
        print(f"[+] Full decompilation report saved to: {args.json_out}")

    print(f"\n================ MCU FIRMWARE ANALYSIS: {args.binary} ================")
    print(f"Size: {report['metadata']['size_bytes']} bytes")
    print(f"Base Address: {report['metadata']['base_addr']}")
    print(f"Initial SP:   {report['metadata']['initial_sp']}")
    print(f"Reset:        {report['metadata']['reset_handler']}")

    print("\n--- Active Peripheral Interrupts ---")
    for name, v in report['vectors'].items():
        if v.get("is_active") and "irq_num" in v:
            print(f"  IRQ {v['irq_num']:2d} ({name:<12}): Target -> {v['target']}")

    print("\n--- UART Command Dispatch Table (0x0800B9E4) ---")
    for u in report['uart_commands']:
        print(f"  Cmd: {u['cmd']:<6} -> Handler: {u['handler']}")

    print("\n--- CAN Mode Dispatch Tables ---")
    for mode, tbl in report['can_tables'].items():
        print(f"\n{tbl['name']} @ {tbl['base_addr']} ({tbl['entry_count']} entries):")
        for e in tbl['entries']:
            print(f"  [{e['index']}] CAN ID: {e['can_id']:<6} ({e['can_id_dec']:4d}) -> Handler: {e['handler']}")

if __name__ == "__main__":
    main()
