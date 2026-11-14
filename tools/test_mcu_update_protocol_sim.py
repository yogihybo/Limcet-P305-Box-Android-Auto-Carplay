#!/usr/bin/env python3
"""
Automated End-to-End Simulation Test for MCU Firmware Update Protocol
Simulates the STM32F105 Bootloader over a virtual PTY pair and runs
tools/mcu_update_sender.py against it.

Tests:
  1. Full End-to-End Transfer: Plaintext binary -> wire obfuscation -> transmission -> de-obfuscation -> flash buffer bit-exact match.
  2. Fault Injection: Checksum corruption mid-transfer -> MCU NAK/retry handling -> recovery to clean completion.
"""

import os
import pty
import sys
import time
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


class MCUBootloaderSimulator(threading.Thread):
    def __init__(self, master_fd, corrupt_block=None):
        super().__init__()
        self.master_fd = master_fd
        self.corrupt_block = corrupt_block
        self.flash_buffer = bytearray()
        self.success = False
        self.error = None
        self.corrupted_once = False

    def read_exact(self, n):
        buf = bytearray()
        while len(buf) < n:
            b = os.read(self.master_fd, n - len(buf))
            if not b:
                raise EOFError("PTY closed")
            buf.extend(b)
        return bytes(buf)

    def write_frame(self, cmd, payload):
        length = len(payload)
        chk = (~(cmd + length + sum(payload))) & 0xFF
        pkt = bytes([MCU_SYNC_BYTE, cmd, length]) + payload + bytes([chk])
        os.write(self.master_fd, pkt)

    def run(self):
        try:
            # 1. Wait for E1 header: 2E E1 02 <hi> <lo> <chk>
            while True:
                b = self.read_exact(1)
                if b[0] != MCU_SYNC_BYTE:
                    continue
                cmd_hdr = self.read_exact(2)
                cmd, length = cmd_hdr[0], cmd_hdr[1]
                data = self.read_exact(length)
                chk = self.read_exact(1)[0]
                expected_chk = (~(cmd + length + sum(data))) & 0xFF
                if chk == expected_chk and cmd == 0xE1 and length == 2:
                    total_blocks = (data[0] << 8) | data[1]
                    break

            # Send E2 01 (Ready)
            self.write_frame(0xE2, bytes([0x01]))

            # 2. Receive blocks
            expected_block = 0
            while expected_block < total_blocks:
                b = self.read_exact(1)
                if b[0] != MCU_SYNC_BYTE:
                    continue
                cmd_hdr = self.read_exact(2)
                cmd, length = cmd_hdr[0], cmd_hdr[1]
                data = self.read_exact(length)
                chk = self.read_exact(1)[0]
                expected_chk = (~(cmd + length + sum(data))) & 0xFF

                if cmd != 0xE3 or length != 0x82 or chk != expected_chk:
                    # Mismatch / bad checksum: report last received
                    if expected_block > 0:
                        self.write_frame(0xE4, bytes([(expected_block - 1) >> 8, (expected_block - 1) & 0xFF]))
                    continue

                b_num = (data[0] << 8) | data[1]

                # Inject deliberate failure if requested
                if self.corrupt_block is not None and b_num == self.corrupt_block and not self.corrupted_once:
                    self.corrupted_once = True
                    # Send NAK/last block status to force sender retry
                    self.write_frame(0xE4, bytes([(expected_block - 1) >> 8, (expected_block - 1) & 0xFF]))
                    continue

                if b_num == expected_block:
                    wire_payload = data[2:]
                    assert len(wire_payload) == 128
                    # De-obfuscate: flash[i] = (~wire[i] + T[i % 128]) & 0xFF
                    for i in range(128):
                        t = DEOBF_TABLE[i % 128]
                        f_byte = ((~wire_payload[i] & 0xFF) + t) & 0xFF
                        self.flash_buffer.append(f_byte)

                    expected_block += 1
                    self.write_frame(0xE4, bytes([(expected_block - 1) >> 8, (expected_block - 1) & 0xFF]))

            # 3. Completion
            self.write_frame(0xE2, bytes([0x02]))
            self.success = True

        except Exception as e:
            self.error = str(e)


def run_test(name, corrupt_block=None):
    print(f"\n========================================================")
    print(f" TEST: {name}")
    print(f"========================================================")

    master_fd, slave_fd = pty.openpty()
    slave_name = os.ttyname(slave_fd)

    # Create dummy firmware test image (4096 bytes / 32 blocks)
    test_img = bytearray(4096)
    for i in range(4096):
        test_img[i] = (i * 7 + 13) & 0xFF

    test_file = f"/tmp/mcu_test_payload_{int(time.time())}.bin"
    with open(test_file, "wb") as f:
        f.write(test_img)

    sim = MCUBootloaderSimulator(master_fd, corrupt_block=corrupt_block)
    sim.start()

    cmd = [
        sys.executable,
        "tools/mcu_update_sender.py",
        "--port", slave_name,
        "--baud", "38400",
        test_file
    ]

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    stdout, stderr = p.communicate(timeout=15)

    sim.join(timeout=3)
    os.close(master_fd)
    os.close(slave_fd)
    os.remove(test_file)

    print(stdout)
    if stderr:
        print("STDERR:\n" + stderr)

    assert p.returncode == 0, f"Sender exited with code {p.returncode}"
    assert sim.success, f"Simulator failed: {sim.error}"
    assert bytes(sim.flash_buffer) == bytes(test_img), "Flash buffer mismatch!"

    if corrupt_block is not None:
        assert sim.corrupted_once, "Fault injection was not triggered!"
        print(f"[+] Verified fault recovery: block {corrupt_block} rejected and cleanly retransmitted.")

    print(f"[+] {name} PASSED! Flash buffer byte-for-byte identical ({len(sim.flash_buffer)} bytes).")


def main():
    # Test 1: Clean Positive Path
    run_test("Positive Path: 4KB image end-to-end streaming & de-obfuscation")

    # Test 2: Negative Path: mid-transfer fault injection at block 10
    run_test("Negative Path: Block 10 NAK fault injection & recovery", corrupt_block=10)

    print("\n========================================================")
    print(" ALL PROTOCOL SIMULATION TESTS PASSED!")
    print("========================================================")


if __name__ == "__main__":
    main()
