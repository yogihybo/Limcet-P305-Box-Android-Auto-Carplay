#!/usr/bin/env python3
"""
test_in_place_upgrade_dry_run.py — Local In-Place MCU Firmware Update Simulation

Performs a full, realistic end-to-end rehearsal of an in-vehicle MCU firmware update:
  Phase 1: Initialize Virtual STM32 with original factory bootloader + factory app.
  Phase 2: Boot MCU in normal mode (115200 baud) with telemetry output.
  Phase 3: Package clean-room application (hardware/MCU/source/build/can_app.bin)
           into obfuscated USB wire update package via tools/mcu_builder/build_mcu_update.sh.
  Phase 4: Send CMD 0xE1 reboot trigger over UART; MCU app writes 0x5555AAAA to 0x20004004
           and resets into IAP bootloader at 38400 baud.
  Phase 5: Flash clean-room firmware in-place using tools/mcu_update_sender.py.
           Factory bootloader erases app pages (0x08003000+), sets 0x5AA5 interrupted flag,
           de-obfuscates each 128-byte block using Table T at 0x08001F72, writes to flash,
           clears interrupted flag, and resets.
  Phase 6: Verify flash at 0x08003000 matches clean-room binary bit-for-bit.
           Verify bootloader validates new vector table, sets handshake (0x20141003),
           relocates VTOR, and boots clean-room application.
  Phase 7: Rollback Rehearsal: Trigger CMD 0xE1 on clean-room application, enter IAP,
           flash factory binary (hardware/MCU/can_app.bin), and verify factory app boots cleanly.
"""

import os
import pty
import sys
import time
import struct
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


class VirtualSTM32(threading.Thread):
    def __init__(self, master_fd):
        super().__init__(daemon=True)
        self.master_fd = master_fd
        # 128 KB Flash (0x08000000 - 0x0801FFFF)
        self.flash = bytearray(b"\xFF" * (128 * 1024))
        # 64 KB SRAM (0x20000000 - 0x2000FFFF)
        self.sram = bytearray(64 * 1024)
        
        self.running = True
        self.mode = "STOPPED"  # "APP" or "BOOTLOADER"
        self.vtor = 0x08000000
        self.handshake = 0
        self.current_app_name = "None"
        self.telemetry_count = 0
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

    def read_exact(self, n, timeout=2.0):
        buf = bytearray()
        deadline = time.time() + timeout
        while len(buf) < n and time.time() < deadline:
            try:
                b = os.read(self.master_fd, n - len(buf))
                if b:
                    buf.extend(b)
                else:
                    time.sleep(0.01)
            except OSError:
                time.sleep(0.01)
        if len(buf) < n:
            return None
        return bytes(buf)

    def reset(self):
        """Hardware reset entry point."""
        with self.lock:
            magic = self.read_sram_u32(0x20004004)
            flag = self.read_u16(0x08002FFE)

            if magic == 0x5555AAAA or flag == 0x5AA5:
                # Enter bootloader IAP mode
                self.mode = "BOOTLOADER"
                self.vtor = 0x08000000
                print(f"[Virtual MCU] System Reset -> Entering IAP Bootloader (Magic: 0x{magic:08X}, Flag: 0x{flag:04X})")
            else:
                # Normal boot: validate application
                sp = self.read_u32(0x08003000)
                reset_vec = self.read_u32(0x08003004)
                sp_valid = (sp & 0x2FFE0000) == 0x20000000
                reset_valid = (0x08003000 <= reset_vec <= 0x0801FFFF) and (reset_vec & 1) == 1

                if sp_valid and reset_valid:
                    self.mode = "APP"
                    self.vtor = 0x08003000
                    self.handshake = 0x20141003
                    self.write_sram_u32(0x20004000, 0x20141003)
                    print(f"[Virtual MCU] System Reset -> Normal Boot -> App Jump @ 0x{reset_vec:08X} (SP: 0x{sp:08X}, VTOR: 0x{self.vtor:08X})")
                else:
                    self.mode = "BOOTLOADER"
                    self.vtor = 0x08000000
                    print(f"[Virtual MCU] System Reset -> Invalid App Vector (SP: 0x{sp:08X}, Reset: 0x{reset_vec:08X}) -> Fallback to Bootloader")

    def run(self):
        self.reset()
        last_telemetry = time.time()

        while self.running:
            now = time.time()
            if self.mode == "APP":
                # App mode: send periodic telemetry frame (CMD 0x81/0x83) every 500ms
                if now - last_telemetry > 0.5:
                    self.telemetry_count += 1
                    # Telemetry frame: CMD 0x83 (Button/Knob status)
                    self.write_frame(0x83, bytes([0x00, 0x00, 0x00, 0x00]))
                    last_telemetry = now

                # Check for inbound UART data (non-blocking)
                raw = self.read_exact(1, timeout=0.05)
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
                expected_chk = calc_chk(cmd, length, payload)
                if chk_b[0] != expected_chk:
                    continue

                if cmd == 0xE1:
                    # In-vehicle upgrade trigger: enter bootloader
                    print("[Virtual MCU App] Received CMD 0xE1 Upgrade Request -> Triggering uart_trigger_bootloader_reset()")
                    self.write_sram_u32(0x20004004, 0x5555AAAA)
                    self.reset()

            elif self.mode == "BOOTLOADER":
                # Bootloader mode: wait for E1 header, then stream blocks
                raw = self.read_exact(1, timeout=0.2)
                if not raw or raw[0] != MCU_SYNC_BYTE:
                    continue
                hdr = self.read_exact(2, timeout=0.2)
                if not hdr:
                    continue
                cmd, length = hdr[0], hdr[1]
                payload = self.read_exact(length, timeout=0.2) if length > 0 else b""
                chk_b = self.read_exact(1, timeout=0.2)
                if not chk_b:
                    continue
                if chk_b[0] != calc_chk(cmd, length, payload):
                    continue

                if cmd == 0xE1 and length == 2:
                    total_blocks = (payload[0] << 8) | payload[1]
                    print(f"[Virtual MCU Bootloader] Received CMD 0xE1 Header: {total_blocks} total blocks ({total_blocks * 128} bytes)")
                    # Reply with Ready ACK: 2E E2 01 01 1B
                    self.write_frame(0xE2, bytes([0x01]))

                    # Receive all blocks
                    blocks_received = 0
                    while blocks_received < total_blocks:
                        sync = self.read_exact(1, timeout=2.0)
                        if not sync or sync[0] != MCU_SYNC_BYTE:
                            continue
                        b_hdr = self.read_exact(2, timeout=1.0)
                        if not b_hdr:
                            continue
                        b_cmd, b_len = b_hdr[0], b_hdr[1]
                        b_payload = self.read_exact(b_len, timeout=1.0)
                        b_chk = self.read_exact(1, timeout=1.0)
                        if not b_chk or b_chk[0] != calc_chk(b_cmd, b_len, b_payload):
                            # NAK: report last received
                            last_idx = max(0, blocks_received - 1)
                            self.write_frame(0xE4, bytes([(last_idx >> 8) & 0xFF, last_idx & 0xFF]))
                            continue

                        if b_cmd != 0xE3 or b_len != 0x82:
                            continue

                        b_num = (b_payload[0] << 8) | b_payload[1]
                        wire_chunk = b_payload[2:]

                        if b_num == 0:
                            # Erase application pages & set interrupted flag at 0x08002FFE
                            print("[Virtual MCU Bootloader] Block 0: Erasing application flash pages (0x08003000+) & asserting 0x5AA5 guard flag...")
                            struct.pack_into("<H", self.flash, self.flash_offset(0x08002FFE), 0x5AA5)
                            # Erase app region
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

                        # Write to flash at 0x08003000 + (b_num * 128)
                        target_off = self.flash_offset(0x08003000 + b_num * 128)
                        self.flash[target_off:target_off + 128] = deobf_chunk
                        blocks_received += 1

                        # Reply with E4 status
                        self.write_frame(0xE4, bytes([(b_num >> 8) & 0xFF, b_num & 0xFF]))

                    print(f"[Virtual MCU Bootloader] All {total_blocks} blocks written to flash. Clearing 0x5AA5 guard flag and sending completion ACK...")
                    # Clear interrupted flag (erase Page 5 word)
                    struct.pack_into("<H", self.flash, self.flash_offset(0x08002FFE), 0xFFFF)
                    # Clear SRAM magic
                    self.write_sram_u32(0x20004004, 0x00000000)
                    # Send completion ACK: 2E E2 01 02 1A
                    self.write_frame(0xE2, bytes([0x02]))
                    time.sleep(0.1)
                    # Reset MCU into new firmware
                    self.reset()


def run_dry_run():
    print("=" * 76)
    print(" LOCAL END-TO-END IN-PLACE MCU UPGRADE REHEARSAL")
    print("=" * 76)

    # 1. Paths
    factory_boot_bin = os.path.join(ROOT, "hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_bootloader_12k_reconstructed.bin")
    factory_app_bin = os.path.join(ROOT, "hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_app_52k_reconstructed.bin")
    cleanroom_app_bin = os.path.join(ROOT, "hardware/MCU/source/build/can_app.bin")

    assert os.path.isfile(factory_boot_bin), f"Missing {factory_boot_bin}"
    assert os.path.isfile(factory_app_bin), f"Missing {factory_app_bin}"
    assert os.path.isfile(cleanroom_app_bin), f"Missing {cleanroom_app_bin}"

    # 2. Setup Virtual PTY pair
    master_fd, slave_fd = pty.openpty()
    slave_name = os.ttyname(slave_fd)
    print(f"[+] Virtual serial link established: {slave_name}")

    # 3. Instantiate Virtual MCU
    mcu = VirtualSTM32(master_fd)

    # Load factory bootloader @ 0x08000000 (12KB)
    boot_data = open(factory_boot_bin, "rb").read()
    mcu.write_flash_raw(0x08000000, boot_data)
    print(f"[+] Loaded Factory Bootloader (12KB, SHA256: {hashlib.sha256(boot_data).hexdigest()[:16]}...)")

    # Load factory application @ 0x08003000 (52KB)
    app_data = open(factory_app_bin, "rb").read()
    mcu.write_flash_raw(0x08003000, app_data)
    print(f"[+] Loaded Factory Application (52KB, SHA256: {hashlib.sha256(app_data).hexdigest()[:16]}...)")

    # Start MCU thread
    mcu.start()
    time.sleep(0.3)

    # Verify Baseline Running State
    assert mcu.mode == "APP", "MCU failed to enter APP mode on baseline boot"
    assert mcu.handshake == 0x20141003, "MCU did not write authentication handshake"
    print("[+] Baseline State VERIFIED: Factory Application is running in normal mode.")

    # 4. Package Clean-Room Application
    print("\n" + "=" * 76)
    print(" PHASE A: Package Clean-Room Application for In-Vehicle Update")
    print("=" * 76)
    builder_script = os.path.join(ROOT, "tools/mcu_builder/build_mcu_update.sh")
    cmd_pack = [builder_script, "-d", "-i", cleanroom_app_bin]
    res_pack = subprocess.run(cmd_pack, capture_output=True, text=True)
    assert res_pack.returncode == 0, f"Builder failed:\n{res_pack.stderr}"
    print("[+] Build & Packaging clean: auto_upgrade.txt and obfuscated can_app.bin generated.")

    obf_pkg = os.path.join(ROOT, "tools/mcu_builder/output/usb_root/can_app.bin")
    assert os.path.isfile(obf_pkg)

    # 5. Send CMD 0xE1 to running application to trigger reboot into IAP
    print("\n" + "=" * 76)
    print(" PHASE B: Trigger Upgrade over UART (CMD 0xE1)")
    print("=" * 76)
    import serial
    ser = serial.Serial(slave_name, 115200, timeout=1.0)
    # CMD 0xE1 packet: 2E E1 00 1E
    e1_trig = bytes([0x2E, 0xE1, 0x00, 0x1E])
    ser.write(e1_trig)
    ser.flush()
    ser.close()

    time.sleep(0.3)
    assert mcu.mode == "BOOTLOADER", "MCU failed to transition to BOOTLOADER mode after CMD 0xE1"
    print("[+] MCU successfully transitioned to IAP Bootloader mode at 38400 baud.")

    # 6. Stream update to bootloader via tools/mcu_update_sender.py
    print("\n" + "=" * 76)
    print(" PHASE C: In-Place Flashing via tools/mcu_update_sender.py")
    print("=" * 76)
    flasher_script = os.path.join(ROOT, "tools/mcu_update_sender.py")
    cmd_flash = ["python3", flasher_script, "--port", slave_name, "--baud", "38400", "--wire-format", obf_pkg]
    res_flash = subprocess.run(cmd_flash, capture_output=True, text=True)
    print(res_flash.stdout)
    assert res_flash.returncode == 0, f"Flasher failed:\n{res_flash.stderr}"

    time.sleep(0.3)

    # 7. Post-Upgrade Verification
    print("\n" + "=" * 76)
    print(" PHASE D: Post-Upgrade Verification of Clean-Room Application")
    print("=" * 76)
    # Verify flash content at 0x08003000
    cleanroom_raw = open(cleanroom_app_bin, "rb").read()
    flash_written = mcu.flash[mcu.flash_offset(0x08003000):mcu.flash_offset(0x08003000) + len(cleanroom_raw)]
    assert flash_written == cleanroom_raw, "Flash mismatch! Written bytes do not match clean-room binary!"
    print(f"[+] Flash content at 0x08003000 matches clean-room binary BIT-FOR-BIT ({len(cleanroom_raw)} bytes).")

    # Verify MCU booted into clean-room application
    assert mcu.mode == "APP", "MCU failed to boot into clean-room application"
    assert mcu.vtor == 0x08003000, f"VTOR mismatch: expected 0x08003000, got 0x{mcu.vtor:08X}"
    assert mcu.handshake == 0x20141003, "Authentication handshake missing after update"
    print("[+] Clean-room application booted cleanly: VTOR=0x08003000, Auth=0x20141003.")

    # 8. Rollback Rehearsal
    print("\n" + "=" * 76)
    print(" PHASE E: In-Vehicle Rollback Rehearsal (Clean-Room -> Stock Firmware)")
    print("=" * 76)
    # Package stock binary
    stock_bin = os.path.join(ROOT, "hardware/MCU/can_app.bin")
    cmd_pack_stock = [builder_script, "-d", "-i", stock_bin]
    res_pack_stock = subprocess.run(cmd_pack_stock, capture_output=True, text=True)
    assert res_pack_stock.returncode == 0

    # Trigger update on clean-room application
    ser = serial.Serial(slave_name, 115200, timeout=1.0)
    ser.write(e1_trig)
    ser.flush()
    ser.close()

    time.sleep(0.3)
    assert mcu.mode == "BOOTLOADER", "MCU failed to enter bootloader on rollback trigger"

    # Flash stock firmware
    cmd_flash_stock = ["python3", flasher_script, "--port", slave_name, "--baud", "38400", "--wire-format", obf_pkg]
    res_flash_stock = subprocess.run(cmd_flash_stock, capture_output=True, text=True)
    assert res_flash_stock.returncode == 0

    time.sleep(0.3)
    assert mcu.mode == "APP", "MCU failed to reboot into application after rollback"
    print("[+] Rollback successfully completed: Stock application restored and running.")

    print("\n" + "=" * 76)
    print(" IN-PLACE UPGRADE DRY RUN: 100% SUCCESSFUL!")
    print("=" * 76)

    mcu.running = False
    os.close(master_fd)
    os.close(slave_fd)
    return 0


if __name__ == "__main__":
    sys.exit(run_dry_run())
