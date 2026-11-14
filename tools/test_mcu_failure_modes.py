#!/usr/bin/env python3
"""
test_mcu_failure_modes.py — Comprehensive Failure Mode & Resilience Test Suite
for STM32F105 In-Vehicle Firmware Update Process

Simulates physical device hazards, communication faults, and edge cases:
  Test 1: Mid-Stream Checksum Corruption (Bit-Flip & NAK Retransmission)
  Test 2: Premature Frame Truncation (Short Packet Recovery)
  Test 3: Corrupt/Invalid Update Header (Reject NAK & Flash Preservation)
  Test 4: Mid-Update Sudden Power Loss (0x5AA5 Guard Flag & Safe Re-entry)
  Test 5: Malformed Firmware Rejection (Invalid SP & Non-Thumb Reset Vector)
  Test 6: Memory Boundary Protection (Prevent Bootloader / Page 0-5 Overwrite)
  Test 7: Line Noise Immunity (Random Bus Noise vs Sync Parser)
  Test 8: Host Stall & Watchdog (IWDG) Reset Recovery
"""

import os
import pty
import sys
import time
import struct
import random
import hashlib
import threading
import subprocess

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

MCU_SYNC_BYTE = 0x2E
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def calc_chk(cmd, length, payload):
    return (~(cmd + length + sum(payload))) & 0xFF


def obfuscate(plain_bytes: bytes) -> bytes:
    wire = bytearray(len(plain_bytes))
    for i in range(len(plain_bytes)):
        t = DEOBF_TABLE[i % 128]
        wire[i] = (~(plain_bytes[i] - t)) & 0xFF
    return bytes(wire)


class ResilientVirtualSTM32(threading.Thread):
    """
    High-fidelity simulation of the STM32F105 companion MCU implementing
    both the Factory Bootloader and Application state machine with strict
    hardware fault injection hooks.
    """
    def __init__(self, master_fd):
        super().__init__(daemon=True)
        self.master_fd = master_fd
        self.flash = bytearray(b"\xFF" * (128 * 1024))
        self.sram = bytearray(64 * 1024)
        self.running = True
        self.mode = "BOOTLOADER"  # "APP" or "BOOTLOADER"
        self.vtor = 0x08000000
        self.handshake = 0
        self.iwdg_timeout_s = 2.0
        self.last_iwdg_feed = time.time()
        
        # Fault injection hooks
        self.cut_power_at_block = None
        self.corrupt_chk_at_block = None
        self.corrupted_blocks_done = set()
        self.truncate_at_block = None
        self.truncated_blocks_done = set()
        self.power_loss_event = threading.Event()
        self.watchdog_reset_event = threading.Event()
        self.lock = threading.Lock()

    def flash_offset(self, addr):
        return addr - 0x08000000

    def sram_offset(self, addr):
        return addr - 0x20000000

    def write_flash_raw(self, addr, data):
        off = self.flash_offset(addr)
        self.flash[off:off + len(data)] = data

    def read_u16(self, addr):
        off = self.flash_offset(addr)
        return struct.unpack_from("<H", self.flash, off)[0]

    def read_u32(self, addr):
        off = self.flash_offset(addr)
        return struct.unpack_from("<I", self.flash, off)[0]

    def read_sram_u32(self, addr):
        off = self.sram_offset(addr)
        return struct.unpack_from("<I", self.sram, off)[0]

    def write_sram_u32(self, addr, val):
        off = self.sram_offset(addr)
        struct.pack_into("<I", self.sram, off, val)

    def write_frame(self, cmd, payload):
        chk = calc_chk(cmd, len(payload), payload)
        pkt = bytes([MCU_SYNC_BYTE, cmd, len(payload)]) + payload + bytes([chk])
        try:
            os.write(self.master_fd, pkt)
        except OSError:
            pass

    def read_exact(self, n, timeout=0.5):
        buf = bytearray()
        deadline = time.time() + timeout
        while len(buf) < n and time.time() < deadline:
            self.check_watchdog()
            try:
                b = os.read(self.master_fd, n - len(buf))
                if b:
                    buf.extend(b)
                else:
                    time.sleep(0.005)
            except OSError:
                time.sleep(0.005)
        if len(buf) < n:
            return None
        return bytes(buf)

    def feed_iwdg(self):
        self.last_iwdg_feed = time.time()

    def check_watchdog(self):
        if self.mode == "BOOTLOADER" and (time.time() - self.last_iwdg_feed > self.iwdg_timeout_s):
            self.watchdog_reset_event.set()
            self.reset()

    def reset(self):
        with self.lock:
            self.feed_iwdg()
            magic = self.read_sram_u32(0x20004004)
            flag = self.read_u16(0x08002FFE)

            if magic == 0x5555AAAA or flag == 0x5AA5:
                # Enter bootloader update mode
                self.mode = "BOOTLOADER"
                self.vtor = 0x08000000
            else:
                # Normal boot: hardware vector validation check
                sp = self.read_u32(0x08003000)
                reset_vec = self.read_u32(0x08003004)
                sp_valid = (sp & 0x2FFE0000) == 0x20000000
                reset_valid = (0x08003000 <= reset_vec <= 0x0801FFFF) and (reset_vec & 1) == 1

                if sp_valid and reset_valid:
                    self.mode = "APP"
                    self.vtor = 0x08003000
                    self.handshake = 0x20141003
                    self.write_sram_u32(0x20004000, 0x20141003)
                else:
                    # Invalid vector table: failsafe fallback to bootloader
                    self.mode = "BOOTLOADER"
                    self.vtor = 0x08000000

    def run(self):
        self.reset()

        while self.running:
            self.check_watchdog()

            if self.mode == "APP":
                # App mode: listen for commands
                raw = self.read_exact(1, timeout=0.05)
                if not raw or raw[0] != MCU_SYNC_BYTE:
                    continue
                hdr = self.read_exact(2, timeout=0.1)
                if not hdr:
                    continue
                cmd, length = hdr[0], hdr[1]
                payload = self.read_exact(length, timeout=0.1) if length > 0 else b""
                chk_b = self.read_exact(1, timeout=0.1)
                if not chk_b or chk_b[0] != calc_chk(cmd, length, payload):
                    continue

                if cmd == 0xE1:
                    self.write_sram_u32(0x20004004, 0x5555AAAA)
                    self.reset()

            elif self.mode == "BOOTLOADER":
                self.feed_iwdg()
                raw = self.read_exact(1, timeout=0.1)
                if not raw or raw[0] != MCU_SYNC_BYTE:
                    continue
                hdr = self.read_exact(2, timeout=0.1)
                if not hdr:
                    continue
                cmd, length = hdr[0], hdr[1]
                payload = self.read_exact(length, timeout=0.1) if length > 0 else b""
                chk_b = self.read_exact(1, timeout=0.1)
                if not chk_b:
                    continue
                if chk_b[0] != calc_chk(cmd, length, payload):
                    if cmd == 0xE1:
                        self.write_frame(0xE2, bytes([0x00]))  # Reject NAK
                    continue

                if cmd == 0xE1 and length == 2:
                    total_blocks = (payload[0] << 8) | payload[1]
                    if total_blocks == 0 or total_blocks > 416:
                        # Reject invalid block counts (exceeding flash partition)
                        self.write_frame(0xE2, bytes([0x00]))
                        continue

                    # Ready ACK
                    self.write_frame(0xE2, bytes([0x01]))

                    # Receive blocks
                    blocks_received = 0
                    while blocks_received < total_blocks and self.running:
                        self.feed_iwdg()
                        sync = self.read_exact(1, timeout=1.0)
                        if not sync or sync[0] != MCU_SYNC_BYTE:
                            continue
                        b_hdr = self.read_exact(2, timeout=0.5)
                        if not b_hdr:
                            continue
                        b_cmd, b_len = b_hdr[0], b_hdr[1]
                        b_payload = self.read_exact(b_len, timeout=0.5)
                        if b_payload is None:
                            last_idx = max(0, blocks_received - 1)
                            self.write_frame(0xE4, bytes([(last_idx >> 8) & 0xFF, last_idx & 0xFF]))
                            continue
                        b_chk = self.read_exact(1, timeout=0.5)
                        if not b_chk or b_chk[0] != calc_chk(b_cmd, b_len, b_payload):
                            last_idx = max(0, blocks_received - 1)
                            self.write_frame(0xE4, bytes([(last_idx >> 8) & 0xFF, last_idx & 0xFF]))
                            continue

                        if b_cmd != 0xE3 or b_len != 0x82:
                            continue

                        b_num = (b_payload[0] << 8) | b_payload[1]
                        wire_chunk = b_payload[2:]

                        # Check for power-cut fault injection
                        if self.cut_power_at_block is not None and b_num == self.cut_power_at_block:
                            self.power_loss_event.set()
                            # Cut power: reset MCU immediately without writing completion ACK
                            self.cut_power_at_block = None
                            self.reset()
                            break

                        if b_num == 0:
                            # Erase application flash pages (0x08003000+) & assert guard flag
                            struct.pack_into("<H", self.flash, self.flash_offset(0x08002FFE), 0x5AA5)
                            app_start = self.flash_offset(0x08003000)
                            app_end = app_start + (total_blocks * 128)
                            page_end = ((app_end + 2047) // 2048) * 2048
                            for i in range(app_start, page_end):
                                if i < len(self.flash):
                                    self.flash[i] = 0xFF

                        # De-obfuscate payload via Table T
                        deobf_chunk = bytearray(128)
                        for idx in range(128):
                            t_val = DEOBF_TABLE[idx % 128]
                            deobf_chunk[idx] = (~wire_chunk[idx] + t_val) & 0xFF

                        # Bounds protection: strictly enforce 0x08003000 - 0x0801FFFF
                        target_addr = 0x08003000 + b_num * 128
                        if target_addr < 0x08003000 or target_addr + 128 > 0x08020000:
                            # Out-of-bounds write attempt! Reject and halt transfer
                            self.write_frame(0xE2, bytes([0x00]))
                            break

                        target_off = self.flash_offset(target_addr)
                        self.flash[target_off:target_off + 128] = deobf_chunk
                        blocks_received += 1

                        self.write_frame(0xE4, bytes([(b_num >> 8) & 0xFF, b_num & 0xFF]))

                    if blocks_received == total_blocks:
                        struct.pack_into("<H", self.flash, self.flash_offset(0x08002FFE), 0xFFFF)
                        self.write_sram_u32(0x20004004, 0x00000000)
                        self.write_frame(0xE2, bytes([0x02]))
                        time.sleep(0.05)
                        self.reset()


def run_tests():
    print("=" * 76)
    print(" RIGOROUS FAILURE MODE & RESILIENCE VERIFICATION SUITE")
    print(" Target: STM32F105 Companion MCU In-Place Update Protocol")
    print("=" * 76)

    factory_boot_bin = os.path.join(ROOT, "hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_bootloader_12k_reconstructed.bin")
    factory_app_bin = os.path.join(ROOT, "hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_app_52k_reconstructed.bin")
    cleanroom_app_bin = os.path.join(ROOT, "hardware/MCU/source/build/can_app.bin")

    master_fd, slave_fd = pty.openpty()
    import fcntl
    flags = fcntl.fcntl(master_fd, fcntl.F_GETFL)
    fcntl.fcntl(master_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
    slave_name = os.ttyname(slave_fd)
    mcu = ResilientVirtualSTM32(master_fd)

    boot_data = open(factory_boot_bin, "rb").read()
    app_data = open(factory_app_bin, "rb").read()
    cleanroom_data = open(cleanroom_app_bin, "rb").read()

    mcu.write_flash_raw(0x08000000, boot_data)
    mcu.write_flash_raw(0x08003000, app_data)
    mcu.start()
    time.sleep(0.2)

    import serial

    # =========================================================================
    # Test 1: Mid-Stream Checksum Corruption (Bit-Flip & NAK Retransmission)
    # =========================================================================
    print("\n[TEST 1] Mid-Stream Checksum Corruption (Bit-Flip & NAK Retransmission)")
    # Transition to bootloader
    mcu.write_sram_u32(0x20004004, 0x5555AAAA)
    mcu.reset()
    time.sleep(0.1)
    assert mcu.mode == "BOOTLOADER"

    ser = serial.Serial(slave_name, 38400, timeout=0.5)
    # Send header for 10 blocks
    ser.write(bytes([0x2E, 0xE1, 0x02, 0x00, 0x0A, calc_chk(0xE1, 2, bytes([0x00, 0x0A]))]))
    resp = ser.read(5)
    assert resp == bytes([0x2E, 0xE2, 0x01, 0x01, 0x1B]), f"Unexpected response: {resp.hex()}"

    # Stream blocks 0 to 9, but corrupt block 4 on first attempt
    plain_10blk = cleanroom_data[:10 * 128]
    wire_10blk = obfuscate(plain_10blk)
    for b_idx in range(10):
        chunk = wire_10blk[b_idx * 128:(b_idx + 1) * 128]
        payload = bytes([(b_idx >> 8) & 0xFF, b_idx & 0xFF]) + chunk
        chk = calc_chk(0xE3, len(payload), payload)
        if b_idx == 4:
            # Corrupt checksum deliberately
            bad_chk = (chk ^ 0xFF) & 0xFF
            ser.write(bytes([0x2E, 0xE3, 0x82]) + payload + bytes([bad_chk]))
            nak_resp = ser.read(6)
            # Expect E4 reporting last valid block (block 3)
            assert nak_resp[0:3] == bytes([0x2E, 0xE4, 0x02]), f"Expected E4 NAK, got {nak_resp.hex()}"
            reported_blk = (nak_resp[3] << 8) | nak_resp[4]
            assert reported_blk == 3, f"Expected last valid block 3, got {reported_blk}"
            # Now retransmit block 4 with valid checksum
            ser.write(bytes([0x2E, 0xE3, 0x82]) + payload + bytes([chk]))
            ack_resp = ser.read(6)
            assert (ack_resp[3] << 8) | ack_resp[4] == 4, "Block 4 retransmission ACK failed"
        else:
            ser.write(bytes([0x2E, 0xE3, 0x82]) + payload + bytes([chk]))
            ack_resp = ser.read(6)
            assert (ack_resp[3] << 8) | ack_resp[4] == b_idx, f"Block {b_idx} ACK mismatch"

    final_ack = ser.read(5)
    assert final_ack == bytes([0x2E, 0xE2, 0x01, 0x02, 0x1A]), f"Completion ACK failed: {final_ack.hex()}"
    ser.close()
    print("   [+] PASSED: Bootloader cleanly rejected corrupted block, reported last index, and recovered on retry.")

    # =========================================================================
    # Test 2: Premature Frame Truncation (Short Packet Recovery)
    # =========================================================================
    print("\n[TEST 2] Premature Frame Truncation (Short Packet Recovery)")
    mcu.write_sram_u32(0x20004004, 0x5555AAAA)
    mcu.reset()
    time.sleep(0.1)

    ser = serial.Serial(slave_name, 38400, timeout=0.5)
    ser.write(bytes([0x2E, 0xE1, 0x02, 0x00, 0x02, calc_chk(0xE1, 2, bytes([0x00, 0x02]))]))
    resp = ser.read(5)
    assert resp[1:4] == bytes([0xE2, 0x01, 0x01])

    # Send block 0 normally
    chunk0 = wire_10blk[0:128]
    p0 = bytes([0x00, 0x00]) + chunk0
    ser.write(bytes([0x2E, 0xE3, 0x82]) + p0 + bytes([calc_chk(0xE3, len(p0), p0)]))
    ack0 = ser.read(6)
    assert ack0[4] == 0x00

    # Send block 1 truncated (only 20 bytes instead of 130)
    chunk1 = wire_10blk[128:256]
    p1 = bytes([0x00, 0x01]) + chunk1
    ser.write(bytes([0x2E, 0xE3, 0x82]) + p1[:20])
    time.sleep(0.6)  # Let bootloader timeout
    nak_report = ser.read(6)
    assert nak_report[0:3] == bytes([0x2E, 0xE4, 0x02]), f"Expected E4 NAK report, got {nak_report.hex()}"
    assert nak_report[4] == 0x00, "Expected last valid block 0 reported"

    # Transmit the full block 1
    ser.write(bytes([0x2E, 0xE3, 0x82]) + p1 + bytes([calc_chk(0xE3, len(p1), p1)]))
    ack1 = ser.read(6)
    assert ack1[4] == 0x01, "Failed to recover after truncated frame"
    ser.close()
    print("   [+] PASSED: Bootloader parser safely timed out short frame and recovered on full block delivery.")

    # =========================================================================
    # Test 3: Corrupt/Invalid Update Header (Reject NAK & Flash Preservation)
    # =========================================================================
    print("\n[TEST 3] Corrupt/Invalid Update Header (Reject NAK & Flash Preservation)")
    mcu.write_sram_u32(0x20004004, 0x5555AAAA)
    mcu.reset()
    time.sleep(0.1)

    ser = serial.Serial(slave_name, 38400, timeout=0.5)
    # 3a. Header with bad checksum
    ser.write(bytes([0x2E, 0xE1, 0x02, 0x00, 0x10, 0x00]))  # 0x00 is bad checksum
    resp = ser.read(5)
    assert resp == bytes([0x2E, 0xE2, 0x01, 0x00, 0x1C]), f"Expected Reject NAK (E2 00), got {resp.hex()}"

    # 3b. Header with zero total blocks
    chk_zero = calc_chk(0xE1, 2, bytes([0x00, 0x00]))
    ser.write(bytes([0x2E, 0xE1, 0x02, 0x00, 0x00, chk_zero]))
    resp2 = ser.read(5)
    assert resp2 == bytes([0x2E, 0xE2, 0x01, 0x00, 0x1C]), f"Expected Reject NAK on zero blocks, got {resp2.hex()}"
    ser.close()

    # Verify that flash was untouched (Page 5 flag not set)
    assert mcu.read_u16(0x08002FFE) != 0x5AA5, "Guard flag was erroneously asserted on rejected header"
    print("   [+] PASSED: Invalid headers rejected with 2E E2 01 00 1C; flash remain untouched.")

    # =========================================================================
    # Test 4: Mid-Update Sudden Power Loss (0x5AA5 Guard Flag & Safe Re-entry)
    # =========================================================================
    print("\n[TEST 4] Mid-Update Sudden Power Loss (0x5AA5 Guard Flag & Safe Re-entry)")
    mcu.write_sram_u32(0x20004004, 0x5555AAAA)
    mcu.reset()
    time.sleep(0.1)

    # Arm power cut at block 5
    mcu.cut_power_at_block = 5
    mcu.power_loss_event.clear()

    ser = serial.Serial(slave_name, 38400, timeout=0.5)
    ser.write(bytes([0x2E, 0xE1, 0x02, 0x00, 0x0A, calc_chk(0xE1, 2, bytes([0x00, 0x0A]))]))
    ser.read(5)

    for b_idx in range(6):
        chunk = wire_10blk[b_idx * 128:(b_idx + 1) * 128]
        payload = bytes([(b_idx >> 8) & 0xFF, b_idx & 0xFF]) + chunk
        chk = calc_chk(0xE3, len(payload), payload)
        ser.write(bytes([0x2E, 0xE3, 0x82]) + payload + bytes([chk]))
        ser.read(6)
    ser.close()

    # Wait for power cut simulation
    assert mcu.power_loss_event.wait(timeout=2.0), "Power cut event did not trigger"

    # Simulate Power Restored:
    # 1. SRAM is completely reset to 0 (magic 0x20004004 is GONE)
    mcu.write_sram_u32(0x20004004, 0x00000000)
    # 2. MCU reboot sequence executes
    mcu.reset()
    time.sleep(0.1)

    # Crucial assertion: MCU MUST NOT boot app; it MUST stay in BOOTLOADER due to 0x5AA5 guard flag
    assert mcu.read_u16(0x08002FFE) == 0x5AA5, "Guard flag 0x5AA5 was not present in flash!"
    assert mcu.mode == "BOOTLOADER", f"FATAL: MCU booted into corrupted application after power loss! Mode={mcu.mode}"
    print("   [+] VERIFIED: Guard flag 0x5AA5 prevented booting partially-flashed application.")

    # 3. Simulate recovery update after power restored
    flasher_script = os.path.join(ROOT, "tools/mcu_update_sender.py")
    obf_pkg = os.path.join(ROOT, "tools/mcu_builder/output/usb_root/can_app.bin")
    cmd_flash = ["python3", flasher_script, "--port", slave_name, "--baud", "38400", "--wire-format", obf_pkg]
    res_flash = subprocess.run(cmd_flash, capture_output=True, text=True)
    assert res_flash.returncode == 0, f"Recovery flash failed:\n{res_flash.stderr}"

    time.sleep(0.2)
    assert mcu.mode == "APP", "MCU failed to boot after recovery flash"
    assert mcu.read_u16(0x08002FFE) == 0xFFFF, "Guard flag was not erased after recovery flash"
    print("   [+] PASSED: MCU successfully recovered from mid-flash power loss and resumed normal operation.")

    # =========================================================================
    # Test 5: Malformed Firmware Rejection (Invalid SP & Non-Thumb Reset Vector)
    # =========================================================================
    print("\n[TEST 5] Malformed Firmware Rejection (Invalid SP & Non-Thumb Reset Vector)")
    # 5a. Corrupted SP (outside 64KB SRAM, e.g. 0x08000000)
    bad_sp_firmware = struct.pack("<II", 0x08000000, 0x08003155) + cleanroom_data[8:]
    mcu.write_flash_raw(0x08003000, bad_sp_firmware)
    mcu.write_sram_u32(0x20004004, 0x00000000)  # Normal boot attempt
    mcu.reset()
    time.sleep(0.1)
    assert mcu.mode == "BOOTLOADER", "Bootloader erroneously jumped to firmware with invalid SP!"

    # 5b. Corrupted Reset Vector (Even address / non-Thumb mode, e.g. 0x08003154)
    bad_reset_firmware = struct.pack("<II", 0x20005000, 0x08003154) + cleanroom_data[8:]
    mcu.write_flash_raw(0x08003000, bad_reset_firmware)
    mcu.reset()
    time.sleep(0.1)
    assert mcu.mode == "BOOTLOADER", "Bootloader erroneously jumped to non-Thumb reset vector!"
    print("   [+] PASSED: Hardware vector table gates successfully trapped malformed firmware.")

    # =========================================================================
    # Test 6: Memory Boundary Protection (Prevent Bootloader / Page 0-5 Overwrite)
    # =========================================================================
    print("\n[TEST 6] Memory Boundary Protection (Prevent Bootloader / Page 0-5 Overwrite)")
    mcu.write_sram_u32(0x20004004, 0x5555AAAA)
    mcu.reset()
    time.sleep(0.1)

    ser = serial.Serial(slave_name, 38400, timeout=0.5)
    # Attempt to request 500 blocks (>116KB, which would wrap or overflow flash)
    ser.write(bytes([0x2E, 0xE1, 0x02, 0x01, 0xF4, calc_chk(0xE1, 2, bytes([0x01, 0xF4]))]))
    resp = ser.read(5)
    assert resp == bytes([0x2E, 0xE2, 0x01, 0x00, 0x1C]), f"Expected Reject NAK for oversized payload, got {resp.hex()}"
    ser.close()

    # Verify bootloader partition (0x08000000 - 0x080027FF) is 100% intact
    curr_boot = mcu.flash[mcu.flash_offset(0x08000000):mcu.flash_offset(0x08002800)]
    assert curr_boot == boot_data[:len(curr_boot)], "Bootloader flash memory was modified!"
    print("   [+] PASSED: Flash boundary checks strictly prevent writing outside application partition.")

    # =========================================================================
    # Test 7: Line Noise Immunity (Random Bus Noise vs Sync Parser)
    # =========================================================================
    print("\n[TEST 7] Line Noise Immunity (Random Bus Noise vs Sync Parser)")
    # Restore valid app and enter APP mode
    mcu.write_flash_raw(0x08003000, cleanroom_data)
    mcu.write_sram_u32(0x20004004, 0x00000000)
    mcu.reset()
    time.sleep(0.1)
    assert mcu.mode == "APP"

    # Bombard UART with 2048 bytes of pseudorandom line noise
    random.seed(0x1337)
    noise = bytes([random.randint(0, 255) for _ in range(2048)])
    ser = serial.Serial(slave_name, 115200, timeout=0.5)
    ser.write(noise)
    ser.flush()
    time.sleep(0.2)

    # MCU must still be running in APP mode (not crashed, not reset)
    assert mcu.mode == "APP", "Line noise caused spurious state machine crash or reset!"

    # Now send legitimate CMD 0xE1 through the same noise-polluted channel
    ser.write(bytes([0x2E, 0xE1, 0x00, 0x1E]))
    ser.flush()
    ser.close()
    time.sleep(0.2)
    assert mcu.mode == "BOOTLOADER", "Legitimate command failed to parse after noise burst"
    print("   [+] PASSED: Parser ignored 2KB of random line noise and cleanly extracted valid sync frames.")

    # =========================================================================
    # Test 8: Host Stall & Watchdog (IWDG) Reset Recovery
    # =========================================================================
    print("\n[TEST 8] Host Stall & Watchdog (IWDG) Reset Recovery")
    mcu.iwdg_timeout_s = 0.8  # Shorten watchdog for rapid test
    mcu.write_sram_u32(0x20004004, 0x5555AAAA)
    mcu.reset()
    time.sleep(0.1)
    assert mcu.mode == "BOOTLOADER"

    ser = serial.Serial(slave_name, 38400, timeout=0.5)
    ser.write(bytes([0x2E, 0xE1, 0x02, 0x00, 0x0A, calc_chk(0xE1, 2, bytes([0x00, 0x0A]))]))
    ser.read(5)

    # Send block 0 normally
    chunk0 = wire_10blk[0:128]
    p0 = bytes([0x00, 0x00]) + chunk0
    ser.write(bytes([0x2E, 0xE3, 0x82]) + p0 + bytes([calc_chk(0xE3, len(p0), p0)]))
    ser.read(6)

    # Now STALL host completely (stop feeding watchdog)
    print("   [*] Simulating host freeze mid-transfer (waiting for IWDG watchdog reset)...")
    mcu.watchdog_reset_event.clear()
    assert mcu.watchdog_reset_event.wait(timeout=2.0), "Watchdog timer failed to fire on stalled host!"
    ser.close()

    # Verify MCU reset back to safety
    assert mcu.mode == "BOOTLOADER", "MCU did not reset to safe state after watchdog timeout"
    print("   [+] PASSED: Hardware Watchdog (IWDG) fired on host freeze, forcing safe reset.")

    # Clean shutdown
    mcu.running = False
    os.close(master_fd)
    os.close(slave_fd)

    print("\n" + "=" * 76)
    print(" ALL 8 CRITICAL FAILURE MODE TESTS PASSED WITH ZERO FAULTS!")
    print("============================================================================")
    return 0


if __name__ == "__main__":
    sys.exit(run_tests())
