#!/usr/bin/env python3
import sys
import time
import argparse
import serial

# BoxP300 (McuType=6) Handshake Mapping
# maps incoming byte[3] (subcommand) to outbound byte[6] (ip)
MAP_BYTE3_TO_IP = {
    3: 9,
    4: 8,
    7: 24,
    8: 21,
    9: 22,
    10: 1,
    11: 2,
    12: 7,
    13: 5,
    14: 13,
    15: 12,
    16: 6,
}

def calc_xor_checksum(data):
    chk = 0
    for b in data:
        chk ^= b
    return chk

def build_response(arg1, arg2, arg3, payload):
    # Packet layout: [0xFA] [arg1] [arg2] [arg3] [length] [payload...] [checksum] [0xAF]
    frame = bytearray([0xFA, arg1, arg2, arg3])
    if payload:
        frame.append(len(payload))
        frame.extend(payload)
    
    chk = calc_xor_checksum(frame)
    frame.append(chk)
    frame.append(0xAF)
    return bytes(frame)

def parse_and_respond(ser, verbose=False):
    while True:
        # Search for start signature 0x2E
        sig = ser.read(1)
        if not sig or sig[0] != 0x2E:
            continue

        # We found a start signature. Read the command and payload length.
        # Format: [0x2E] [cmd] [length]
        header = ser.read(2)
        if len(header) < 2:
            continue
        
        cmd = header[0]
        length = header[1]

        # Read the payload + checksum
        # Total bytes remaining = length + 1 (checksum)
        remaining = ser.read(length + 1)
        if len(remaining) < (length + 1):
            continue

        payload = remaining[:-1]
        chk_recv = remaining[-1]

        # Validate checksum
        full_packet = bytearray([0x2E, cmd, length])
        full_packet.extend(payload)
        chk_calc = calc_xor_checksum(full_packet)

        if chk_calc != chk_recv:
            print(f"[-] Checksum mismatch: calculated {chk_calc:02X}, received {chk_recv:02X}")
            continue

        if verbose:
            raw_hex = " ".join(f"{b:02X}" for b in [0x2E, cmd, length] + list(remaining))
            print(f"[RX] CMD {cmd:02X} (len={length}): {raw_hex}")

        # Dispatch commands
        if cmd == 0x02:
            # Handshake connection command
            # Incoming payload has: byte[3] (index 0), byte[4] (index 1), etc.
            if len(payload) < 2:
                print("[-] CMD 0x02 payload too short")
                continue
            
            b3 = payload[0]
            b4 = payload[1]

            # Outbound payload[0] = r6 (byte[4] or 3 if byte[4] == 0)
            r6 = b4 if b4 != 0 else 3
            # Outbound payload[1] = ip (mapped from byte[3])
            ip = MAP_BYTE3_TO_IP.get(b3, None)

            if ip is None:
                print(f"[-] Received unmapped byte[3]={b3} in CMD 0x02")
                continue

            resp_payload = bytes([r6, ip])
            resp = build_response(0x00, 0x13, 0x21, resp_payload)

            ser.write(resp)
            if verbose:
                resp_hex = " ".join(f"{b:02X}" for b in resp)
                print(f"[TX] Handshake Response (0x02 -> 0x21): {resp_hex}")
            else:
                print(f"[+] Handshake completed successfully (b3={b3} -> ip={ip}, b4={b4} -> r6={r6})")

        elif cmd == 0x20:
            # Status command
            # Incoming payload expected to have at least 5 bytes: byte[3], byte[4], byte[5], byte[6], byte[7]
            # (which correspond to indices 0, 1, 2, 3, 4 of the payload)
            if len(payload) < 5:
                print("[-] CMD 0x20 payload too short")
                continue
            
            b3 = payload[0]
            b4 = payload[1]
            b5 = payload[2]
            b6 = payload[3]
            b7 = payload[4]

            # Outbound payload: [ byte[7], byte[4], byte[3], byte[6], byte[5] ]
            resp_payload = bytes([b7, b4, b3, b6, b5])
            resp = build_response(0x00, 0x13, 0x23, resp_payload)

            ser.write(resp)
            if verbose:
                resp_hex = " ".join(f"{b:02X}" for b in resp)
                print(f"[TX] Status Response (0x20 -> 0x23): {resp_hex}")

def main():
    parser = argparse.ArgumentParser(description="Simulate Prado MCU Handshake and Status Response (BoxP300/McuType=6)")
    parser.add_argument("-p", "--port", default="/dev/ttyHS0", help="UART port connected to MCU (default: /dev/ttyHS0)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("-v", "--verbose", action="store_true", help="Print verbose frame hex logs")
    args = parser.parse_args()

    print(f"[*] Opening {args.port} at {args.baud} baud...")
    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except Exception as e:
        print(f"[-] Failed to open serial port: {e}")
        sys.exit(1)

    print("[*] Listening for MCU frames. Press Ctrl+C to stop.")
    try:
        parse_respond(ser, args.verbose)
    except KeyboardInterrupt:
        print("\n[*] Exiting.")
    finally:
        ser.close()

if __name__ == "__main__":
    # Workaround for typo in function call during fast writing
    parse_respond = parse_and_respond
    main()
