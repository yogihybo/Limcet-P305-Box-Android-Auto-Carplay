#!/usr/bin/env python3
"""
Host-side Firmware Flasher for STM32F105 Companion MCU
Target: Limcet head unit companion MCU (Box-P305 / Box-P306)

Implements the authentic OEM 0x2E framed update protocol (E1/E2/E3/E4)
reverse-engineered from libMcuCenter.so (MCUAdapter_BoxP300) and verified
against the STM32F105 factory bootloader (docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md §35).

Protocol Summary:
  1. Header:   Host -> MCU: 2E E1 02 <hi> <lo> <chk>  (Total 128-byte block count)
  2. Ready:    MCU -> Host: 2E E2 01 01 1B           (Ready to receive)
  3. Payload:  Host -> MCU: 2E E3 82 <hi> <lo> <128 bytes wire data> <chk>
  4. Status:   MCU -> Host: 2E E4 02 <hi> <lo> <chk>  (Last received block index)
  5. Done:     MCU -> Host: 2E E2 01 02 1A           (Flashing complete)

Usage:
  # Flash plaintext binary (automatically obfuscated on the fly)
  python3 tools/mcu_update_sender.py --port /dev/ttyUSB0 --baud 38400 firmware.bin

  # Flash pre-obfuscated wire binary (e.g. can_app.bin from USB root)
  python3 tools/mcu_update_sender.py --port /dev/ttyUSB0 --wire-format can_app.bin
"""

import sys
import os
import time
import argparse
import hashlib

# 128-byte lookup table T from factory bootloader at 0x08001F72
DEOBF_TABLE = bytes([
    0x78, 0xF8, 0x79, 0xF9, 0x7A, 0xFA, 0x7B, 0xFB, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0xFF, 0x7F, 0xFE, 0x7E, 0xFD, 0x7D, 0xFC, 0x7C, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
    0x7C, 0xFC, 0x7D, 0xFD, 0x7E, 0xFE, 0x7F, 0xFF, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88,
    0xFB, 0x7B, 0xFA, 0x7A, 0xF9, 0x79, 0xF8, 0x78, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
    0x78, 0xF8, 0x79, 0xF9, 0x7A, 0xFA, 0x7B, 0xFB, 0x1F, 0x2E, 0x3D, 0x4C, 0x5A, 0x69, 0x78, 0x87,
    0xFF, 0x7F, 0xFE, 0x7E, 0xFD, 0x7D, 0xFC, 0x7C, 0x96, 0xA5, 0xB4, 0xC3, 0xD2, 0xE1, 0xF0, 0x00,
    0x7C, 0xFC, 0x7D, 0xFD, 0x7E, 0xFE, 0x7F, 0xFF, 0xF1, 0xE2, 0xD3, 0xC4, 0xA5, 0x96, 0x87, 0x78,
    0xFB, 0x7B, 0xFA, 0x7A, 0xF9, 0x79, 0xF8, 0x78, 0x69, 0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F, 0x00
])

MCU_SYNC_BYTE          = 0x2E
SOC_CMD_UPDATE_HEADER  = 0xE1
MCU_CMD_UPDATE_ACK     = 0xE2
SOC_CMD_UPDATE_BLOCK   = 0xE3
MCU_CMD_UPDATE_STATUS  = 0xE4

BLOCK_SIZE = 128


def calc_checksum(cmd: int, length: int, payload: bytes) -> int:
    """Calculates ~(cmd + len + sum(payload)) & 0xFF."""
    s = (cmd + length + sum(payload)) & 0xFF
    return (~s) & 0xFF


def obfuscate(plain_bytes: bytes) -> bytes:
    """wire[i] = ~(flash[i] - T[i % 128]) & 0xFF"""
    wire = bytearray(len(plain_bytes))
    for i in range(len(plain_bytes)):
        t = DEOBF_TABLE[i % 128]
        wire[i] = (~(plain_bytes[i] - t)) & 0xFF
    return bytes(wire)


def make_packet(cmd: int, payload: bytes) -> bytes:
    length = len(payload)
    chk = calc_checksum(cmd, length, payload)
    return bytes([MCU_SYNC_BYTE, cmd, length]) + payload + bytes([chk])


def read_frame(ser, timeout_s=1.0):
    """Read a single 0x2E framed packet from serial."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        b = ser.read(1)
        if not b or b[0] != MCU_SYNC_BYTE:
            continue

        hdr = ser.read(2)
        if len(hdr) < 2:
            continue

        cmd = hdr[0]
        length = hdr[1]

        data = ser.read(length)
        if len(data) < length:
            continue

        chk = ser.read(1)
        if len(chk) < 1:
            continue

        expected_chk = calc_checksum(cmd, length, data)
        if chk[0] != expected_chk:
            print(f"[!] Warning: Checksum mismatch (got 0x{chk[0]:02X}, expected 0x{expected_chk:02X})", file=sys.stderr)
            continue

        return cmd, data

    return None, None


def flash_firmware(port: str, baud: int, firmware_path: str, is_wire: bool = False):
    try:
        import serial
    except ImportError:
        print("[!] Error: pyserial is required. Install via: pip install pyserial", file=sys.stderr)
        sys.exit(1)

    with open(firmware_path, "rb") as f:
        file_bytes = f.read()

    # Ensure 128-byte block alignment with 0xFF padding
    pad_len = (BLOCK_SIZE - (len(file_bytes) % BLOCK_SIZE)) % BLOCK_SIZE
    file_bytes += (b"\xFF" * pad_len)

    if not is_wire:
        print(f"[*] Applying OEM wire-obfuscation transform to {len(file_bytes)} plaintext bytes...")
        wire_data = obfuscate(file_bytes)
    else:
        print(f"[*] Using pre-obfuscated wire image ({len(file_bytes)} bytes)...")
        wire_data = file_bytes

    total_blocks = len(wire_data) // BLOCK_SIZE
    print(f"[*] Total 128-byte blocks: {total_blocks} ({len(wire_data)} bytes)")

    print(f"[*] Opening serial port {port} at {baud} baud (8N1)...")
    ser = serial.Serial(port, baud, timeout=0.1)
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    # Step 1: Send CMD 0xE1 Header until MCU responds with E2 01 (Ready)
    hi = (total_blocks >> 8) & 0xFF
    lo = total_blocks & 0xFF
    e1_pkt = make_packet(SOC_CMD_UPDATE_HEADER, bytes([hi, lo]))

    print(f"[*] Sending update header (total blocks = {total_blocks})...")
    ready = False
    for attempt in range(30):
        ser.write(e1_pkt)
        cmd, data = read_frame(ser, timeout_s=0.5)
        if cmd == MCU_CMD_UPDATE_ACK and len(data) >= 1:
            status = data[0]
            if status == 0x01:
                print("[+] MCU ACKed update header! Ready to flash.")
                ready = True
                break
            elif status == 0x00:
                print("[-] MCU sent NAK (not ready/rejected).")
        time.sleep(0.1)

    if not ready:
        print("[!] Timeout waiting for MCU ACK (0xE2). Check baudrate and wiring.", file=sys.stderr)
        ser.close()
        sys.exit(1)

    # Step 2: Send blocks
    start_time = time.time()
    current_block = 0

    while current_block < total_blocks:
        offset = current_block * BLOCK_SIZE
        chunk = wire_data[offset:offset + BLOCK_SIZE]

        b_hi = (current_block >> 8) & 0xFF
        b_lo = current_block & 0xFF
        payload = bytes([b_hi, b_lo]) + chunk
        pkt = make_packet(SOC_CMD_UPDATE_BLOCK, payload)

        # Transmit block and wait for E4 status report
        acked = False
        for retry in range(5):
            ser.write(pkt)
            # Block 0 makes the bootloader erase all 58 application pages first
            # (1-2 s, with USART2 not serviced), so give its reply longer.
            cmd, data = read_frame(ser, timeout_s=5.0 if current_block == 0 else 1.0)
            if cmd == MCU_CMD_UPDATE_STATUS and len(data) >= 2:
                last_idx = (data[0] << 8) | data[1]
                if last_idx == current_block:
                    acked = True
                    break
            elif cmd == MCU_CMD_UPDATE_ACK and len(data) >= 1 and data[0] == 0x02:
                # Update completed
                acked = True
                break

        if not acked:
            print(f"\n[!] Block {current_block} transfer failed after 5 retries.", file=sys.stderr)
            ser.close()
            sys.exit(1)

        current_block += 1
        pct = (current_block / total_blocks) * 100.0
        elapsed = time.time() - start_time
        rate = (current_block * BLOCK_SIZE) / elapsed if elapsed > 0 else 0
        sys.stdout.write(f"\r[*] Progress: Block {current_block}/{total_blocks} ({pct:.1f}%) - {rate/1024:.2f} KB/s")
        sys.stdout.flush()

    print("\n[*] All blocks transmitted. Awaiting completion ACK...")
    # Step 3: Wait for final E2 02 ACK
    complete = False
    deadline = time.time() + 5.0
    while time.time() < deadline:
        cmd, data = read_frame(ser, timeout_s=1.0)
        if cmd == MCU_CMD_UPDATE_ACK and len(data) >= 1 and data[0] == 0x02:
            complete = True
            break

    if complete:
        print("[+] MCU firmware update SUCCESSFUL! MCU is resetting into new firmware.")
    else:
        print("[?] Final ACK timed out, but all blocks were accepted.")

    ser.close()


def main():
    parser = argparse.ArgumentParser(description="Limcet STM32F105 MCU Firmware Flasher")
    parser.add_argument("firmware", help="Path to firmware image (plain .bin or wire .bin)")
    parser.add_argument("-p", "--port", default="/dev/ttyHS0", help="Serial port (default: /dev/ttyHS0)")
    parser.add_argument("-b", "--baud", type=int, default=38400, help="Baud rate (default: 38400)")
    parser.add_argument("--wire-format", action="store_true", help="Binary is already in obfuscated wire format")
    args = parser.parse_args()

    if not os.path.isfile(args.firmware):
        print(f"[!] Error: File not found: {args.firmware}", file=sys.stderr)
        sys.exit(1)

    flash_firmware(args.port, args.baud, args.firmware, is_wire=args.wire_format)


if __name__ == "__main__":
    main()
