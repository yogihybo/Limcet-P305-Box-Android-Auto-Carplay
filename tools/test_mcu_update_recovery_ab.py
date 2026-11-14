#!/usr/bin/env python3
"""Bench test: does a bootloader recover update mode the way the factory one does?

Runs against whatever bootloader is on the bench board (factory reconstruction or
clean-room), over the Pico (SWD via OpenOCD + USART2 on the Pico's CDC port). It
enters update mode by writing the SRAM request (0x20004004 = 0x5555AAAA) over SWD
and resetting, then plays one scenario and checks the replies against the factory
bootloader's behaviour measured on the same board (MCU_FIRMWARE_VERIFIED_FINDINGS.md
section 38):

  lost    header accepted, then block 0 arrives with its tail missing and the host
          goes quiet. Expect E2 01 re-sent ~19.6 s after the last byte. Non-destructive.
  lost1   block 0 arrives intact (this ERASES the application and sets the 0x5AA5
          flag), then block 1's tail is lost. Expect E4 00 00 re-sent ~19.6 s later.
          Leaves the board needing a full update (tools/mcu_update_sender.py).
  silent  the host never speaks. Expect E2 00 every ~7.0 s, then a give-up at
          ~358 s: request cleared and the application booted. Takes ~6.5 minutes.

Usage:
  python3 tools/test_mcu_update_recovery_ab.py lost
  python3 tools/test_mcu_update_recovery_ab.py silent --port /dev/ttyACM0
Exit status 0 = behaved like the factory bootloader.
"""
import argparse, os, subprocess, sys, time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = os.path.join(ROOT, "tools", "pico_stm32.cfg")

# Measured on the bench against the reconstructed factory bootloader (section 38).
REREQUEST_S = 19.6
PROMPT_S = 7.0
GIVE_UP_S = 358.0
TOLERANCE = 0.15  # fraction


def openocd(*cmds):
    args = ["openocd", "-f", CFG, "-c", "init"]
    for c in cmds:
        args += ["-c", c]
    return subprocess.run(args + ["-c", "shutdown"], capture_output=True, text=True, timeout=60).stderr


def frame(cmd, payload):
    s = (cmd + len(payload) + sum(payload)) & 0xFF
    return bytes([0x2E, cmd, len(payload)]) + bytes(payload) + bytes([(~s) & 0xFF])


def near(value, target):
    return abs(value - target) <= target * TOLERANCE


def collect(ser, until, t0, frames):
    buf = b""
    while time.monotonic() < until:
        buf += ser.read(64)
        while True:
            i = buf.find(b"\x2e")
            if i < 0:
                buf = b""
                break
            buf = buf[i:]
            if len(buf) < 3 or len(buf) < 4 + buf[2]:
                break
            n = 4 + buf[2]
            frames.append((time.monotonic() - t0, buf[:n]))
            print(f"  t={frames[-1][0]:7.2f}s  MCU: {buf[:n].hex(' ')}", flush=True)
            buf = buf[n:]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("scenario", choices=["lost", "lost1", "silent"])
    ap.add_argument("--port", default="/dev/ttyACM0")
    args = ap.parse_args()

    ser = serial.Serial(args.port, 38400, timeout=0.05)
    openocd("halt", "mww 0x20004004 0x5555AAAA", "reset run")
    time.sleep(0.3)
    ser.reset_input_buffer()
    t0 = time.monotonic()
    frames, ok = [], True
    print(f"update mode requested; scenario '{args.scenario}'")

    if args.scenario in ("lost", "lost1"):
        time.sleep(0.5)
        ser.write(frame(0xE1, [0x00, 0x02]))
        time.sleep(1.0)
        reply = ser.read(64)
        print(f"  header reply: {reply.hex(' ')}")
        ok &= reply == bytes.fromhex("2ee201011b")
        expect = bytes.fromhex("2ee201011b")
        if args.scenario == "lost1":
            ser.write(frame(0xE3, [0x00, 0x00] + [0x55] * 128))
            time.sleep(4.0)
            reply = ser.read(64)
            print(f"  block 0 reply: {reply.hex(' ')}  (application now erased)")
            ok &= reply == bytes.fromhex("2ee402000019")
            expect = bytes.fromhex("2ee402000019")
            ser.write(frame(0xE3, [0x00, 0x01] + [0x55] * 128)[:-2])
        else:
            ser.write(frame(0xE3, [0x00, 0x00] + [0x55] * 128)[:-2])
        t_quiet = time.monotonic() - t0
        collect(ser, t0 + t_quiet + REREQUEST_S * 2 + 3, t0, frames)
        got = [t for t, f in frames if f == expect]
        first = got[0] - t_quiet if got else None
        print(f"  re-request after {first:.1f} s (factory: {REREQUEST_S} s)" if got else "  NO re-request")
        ok &= bool(got) and near(first, REREQUEST_S)
    else:
        collect(ser, t0 + GIVE_UP_S + 30, t0, frames)
        prompts = [t for t, f in frames if f == bytes.fromhex("2ee201001c")]
        boot = [t for t, f in frames if f[1] == 0x7F]
        gaps = [b - a for a, b in zip(prompts, prompts[1:])]
        if gaps:
            print(f"  {len(prompts)} E2 00 prompts, mean gap {sum(gaps) / len(gaps):.2f} s (factory: {PROMPT_S} s)")
        print(f"  application booted at {boot[0]:.1f} s (factory: ~{GIVE_UP_S + 2:.0f} s)" if boot
              else "  application never booted")
        ok &= bool(gaps) and near(sum(gaps) / len(gaps), PROMPT_S)
        ok &= bool(boot) and near(boot[0], GIVE_UP_S + 2)

    state = openocd("halt", "reg pc", "mdw 0x20004004", "mdh 0x08002ffe", "resume")
    for line in state.splitlines():
        if "pc (/32)" in line or line.startswith(("0x2000", "0x0800")):
            print("  SWD: " + line.strip())
    print("PASS: matches the factory bootloader" if ok else "FAIL: differs from the factory bootloader")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
