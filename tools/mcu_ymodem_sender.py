#!/usr/bin/env python3
"""
Minimal YMODEM-CRC host sender for the clean-room STM32F105 bootloader's
ymodem_receive_and_flash() (hardware/MCU/bootloader/src/ymodem.c).

This is NOT a general-purpose YMODEM implementation -- it is written
specifically against that receiver's own logic (read directly from
ymodem.c, not from a generic YMODEM spec), since this project's clean-room
bootloader is the only thing on the other end of the wire. See
docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md section 11 for why the real
factory-vendor bootloader's own UART behavior does not need to match
this: the wire protocol between this tool and hardware/MCU/bootloader/
is entirely within this project's own control.

Receiver behavior this sender is built against (from ymodem.c):
  - Sends 'C' (0x43) repeatedly to request YMODEM-CRC mode.
  - Accepts SOH (128-byte) or STX (1024-byte) packets.
  - Packet 0 is the YMODEM header packet (filename, size); receiver ACKs
    it and sends 'C' again before the first data packet.
  - Data packets are ACKed if pkt_num == expected_pkt_num (starts at 1),
    silently ACKed-but-ignored on a duplicate, and written to flash via
    flash_write_page() otherwise.
  - EOT handling: first EOT -> NAK, second EOT -> ACK + 'C', then expects
    one more (empty) header packet to end the session, ACKs it and
    returns true.
  - CRC is CRC16-CCITT (poly 0x1021, init 0), matching update_crc16() in
    ymodem.c exactly.

Usage:
    python3 mcu_ymodem_sender.py --port /dev/ttyUSB0 --baud 38400 firmware.bin
"""

import argparse
import sys
import time

SOH = 0x01
STX = 0x02
EOT = 0x04
ACK = 0x06
NAK = 0x15
CANCEL = 0x18
CRC16_REQ = 0x43  # 'C'

PACKET_SIZE = 1024  # use STX packets; matches clean-room's larger path
PAD_BYTE = 0x1A


def update_crc16(crc, byte):
    """Bit-for-bit port of update_crc16() in ymodem.c."""
    crc ^= (byte << 8)
    for _ in range(8):
        if crc & 0x8000:
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF
        else:
            crc = (crc << 1) & 0xFFFF
    return crc


def crc16(data):
    crc = 0
    for b in data:
        crc = update_crc16(crc, b)
    return crc


def build_packet(pkt_num, payload):
    """payload must already be exactly PACKET_SIZE (or 128) bytes, padded."""
    start = STX if len(payload) == 1024 else SOH
    pkt = bytearray()
    pkt.append(start)
    pkt.append(pkt_num & 0xFF)
    pkt.append((~pkt_num) & 0xFF)
    pkt.extend(payload)
    crc = crc16(payload)
    pkt.append((crc >> 8) & 0xFF)
    pkt.append(crc & 0xFF)
    return bytes(pkt)


def wait_for_byte(ser, want, timeout_s, poll_all=False):
    """Read bytes until `want` is seen (or any byte if poll_all) or timeout."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        val = b[0]
        if poll_all:
            return val
        if val == want:
            return val
        if val == CANCEL:
            raise RuntimeError("Receiver sent CANCEL")
    return None


def send_and_wait_ack(ser, packet, retries=10, ack_timeout=3.0):
    for attempt in range(retries):
        ser.write(packet)
        ser.flush()
        resp = wait_for_byte(ser, ACK, ack_timeout, poll_all=True)
        if resp == ACK:
            return True
        if resp == NAK:
            continue  # resend
        if resp is None:
            continue  # resend on timeout too
    return False


def send_file(ser, filepath, verbose=True):
    with open(filepath, "rb") as f:
        data = f.read()

    filename = filepath.split("/")[-1].encode("ascii", "ignore")
    size_str = str(len(data)).encode("ascii")

    if verbose:
        print(f"[*] Waiting for receiver 'C' (CRC-mode request)...")
    if wait_for_byte(ser, CRC16_REQ, 30.0) is None:
        print("[!] Timed out waiting for bootloader to request transfer.", file=sys.stderr)
        return False

    # Packet 0: header (filename + size), null-padded to PACKET_SIZE.
    header = filename + b"\x00" + size_str + b"\x00"
    header_payload = header.ljust(PACKET_SIZE, b"\x00")
    if verbose:
        print(f"[*] Sending header packet ({filename.decode(errors='replace')}, {len(data)} bytes)")
    if not send_and_wait_ack(ser, build_packet(0, header_payload)):
        print("[!] Header packet not ACKed.", file=sys.stderr)
        return False

    if verbose:
        print("[*] Waiting for second 'C' before data packets...")
    if wait_for_byte(ser, CRC16_REQ, 10.0) is None:
        print("[!] Timed out waiting for post-header 'C'.", file=sys.stderr)
        return False

    pkt_num = 1
    offset = 0
    total = len(data)
    while offset < total:
        chunk = data[offset:offset + PACKET_SIZE]
        if len(chunk) < PACKET_SIZE:
            chunk = chunk + bytes([PAD_BYTE]) * (PACKET_SIZE - len(chunk))
        if verbose:
            pct = 100 * (offset + len(chunk)) // total if total else 100
            print(f"\r[*] Sending packet {pkt_num} ({pct}%)", end="", flush=True)
        if not send_and_wait_ack(ser, build_packet(pkt_num, chunk)):
            print(f"\n[!] Packet {pkt_num} not ACKed after retries.", file=sys.stderr)
            return False
        pkt_num = (pkt_num + 1) & 0xFF
        offset += PACKET_SIZE
    if verbose:
        print()

    # EOT sequence: first EOT -> expect NAK, second EOT -> expect ACK.
    if verbose:
        print("[*] Sending EOT (1/2)...")
    ser.write(bytes([EOT]))
    ser.flush()
    resp = wait_for_byte(ser, NAK, 3.0, poll_all=True)
    if resp != NAK:
        print(f"[!] Expected NAK after first EOT, got {resp!r}.", file=sys.stderr)
        return False

    if verbose:
        print("[*] Sending EOT (2/2)...")
    ser.write(bytes([EOT]))
    ser.flush()
    resp = wait_for_byte(ser, ACK, 3.0, poll_all=True)
    if resp != ACK:
        print(f"[!] Expected ACK after second EOT, got {resp!r}.", file=sys.stderr)
        return False

    if verbose:
        print("[*] Waiting for final 'C' (session-end request)...")
    if wait_for_byte(ser, CRC16_REQ, 5.0) is None:
        print("[!] Timed out waiting for final 'C'.", file=sys.stderr)
        return False

    # Final empty header packet (session end marker). The receiver's EOT
    # handling hardcodes an SOH (128-byte) packet here regardless of what
    # size was used for data packets -- see ymodem.c's `final_start == SOH`
    # check, which reads exactly 132 more bytes (2 header + 128 data + 2 CRC)
    # unconditionally. Sending STX here would desync the receiver's read.
    empty_payload = b"\x00" * 128
    if verbose:
        print("[*] Sending final empty header packet...")
    if not send_and_wait_ack(ser, build_packet(0, empty_payload)):
        print("[!] Final empty packet not ACKed.", file=sys.stderr)
        return False

    if verbose:
        print("[*] Transfer complete.")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("file", help="Firmware image (.bin) to send")
    parser.add_argument("--port", required=True, help="Serial device, e.g. /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=38400, help="Baud rate (default: 38400, matches clean-room's uart_init(38400))")
    args = parser.parse_args()

    try:
        import serial
    except ImportError:
        print("This tool requires pyserial: pip install pyserial", file=sys.stderr)
        sys.exit(1)

    with serial.Serial(args.port, args.baud, timeout=0.1) as ser:
        ok = send_file(ser, args.file)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
