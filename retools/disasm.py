#!/usr/bin/env python3
"""Disassemble instructions at a virtual address in a PE binary.

Decodes x86/x64 machine code into assembly starting at the given VA.
Output format:  0xADDRESS: [RAW_BYTES]  MNEMONIC  OPERANDS

Usage:
    python retools/disasm.py <binary> <va> [-n COUNT] [-b]

Examples:
    python retools/disasm.py binary.exe 0x401000
    python retools/disasm.py binary.exe 0x401000 -n 50 -b
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import Binary


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("binary", help="Path to PE binary (.exe / .dll)")
    p.add_argument("va", help="Start virtual address in hex (e.g. 0x401000)")
    p.add_argument("-n", "--count", type=int, default=30,
                   help="Number of instructions to decode (default: 30)")
    p.add_argument("-b", "--bytes", action="store_true",
                   help="Show raw hex bytes alongside each instruction")
    args = p.parse_args()

    print("hint: use decompiler with --types kb.h for richer output", file=sys.stderr)

    b = Binary(args.binary)
    for insn in b.disasm(int(args.va, 16), args.count):
        if args.bytes:
            raw = " ".join(f"{x:02X}" for x in insn.bytes)
            print(f"0x{insn.address:08X}: {raw:30s} {insn.mnemonic:8s} {insn.op_str}")
        else:
            print(f"0x{insn.address:08X}: {insn.mnemonic:8s} {insn.op_str}")


if __name__ == "__main__":
    main()
