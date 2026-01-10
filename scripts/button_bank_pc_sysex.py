#!/usr/bin/env python3
"""Generate OpenDeck SysExConf messages to configure a button as
Bank Select (MSB/LSB) + Program Change.

This produces a .syx file containing:
- connection open
- set button MESSAGE_TYPE = BANK_SELECT_PROGRAM_CHANGE
- set button CHANNEL
- set button MIDI_ID (program number)
- set button VALUE (14-bit bank)
- connection close

It does NOT transmit MIDI; use any SysEx sender to send the generated .syx.

Protocol is implemented by libsysexconf and OpenDeck sys::Config.
Manufacturer ID: 00 53 43
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List


MANUFACTURER_ID = (0x00, 0x53, 0x43)

# libsysexconf/common.h
STATUS_REQUEST = 0x00

# lib::sysexconf::specialRequest_t
SPECIAL_CONN_CLOSE = 0x00
SPECIAL_CONN_OPEN = 0x01

# lib::sysexconf::wish_t and amount_t (standard messages)
WISH_GET = 0x00
WISH_SET = 0x01
AMOUNT_SINGLE = 0x00

# OpenDeck sys::Config::block_t
BLOCK_GLOBAL = 0
BLOCK_BUTTONS = 1

# OpenDeck sys::Config::Section::button_t
SECTION_BUTTON_TYPE = 0
SECTION_BUTTON_MESSAGE_TYPE = 1
SECTION_BUTTON_MIDI_ID = 2
SECTION_BUTTON_VALUE = 3
SECTION_BUTTON_CHANNEL = 4

# io::buttons::messageType_t in src/firmware/application/io/buttons/common.h
# BANK_SELECT_PROGRAM_CHANGE is currently 24 in this enum.
BUTTON_MSG_BANK_SELECT_PROGRAM_CHANGE = 24


def split14bit(value: int) -> tuple[int, int]:
    """Match lib::sysexconf::Split14Bit: returns (high, low), both 7-bit."""
    value &= 0x3FFF
    new_high = (value >> 8) & 0xFF
    new_low = value & 0xFF

    new_high = (new_high << 1) & 0x7F
    if (new_low >> 7) & 0x01:
        new_high |= 0x01
    else:
        new_high &= ~0x01

    new_low &= 0x7F
    return new_high, new_low


def sysex_special(wish_byte: int) -> List[int]:
    # SPECIAL_REQ_MSG_SIZE: F0 + 3xID + STATUS + PART + WISH + F7
    return [
        0xF0,
        *MANUFACTURER_ID,
        STATUS_REQUEST,
        0x00,  # PART
        wish_byte & 0x7F,
        0xF7,
    ]


def sysex_set_single(block: int, section: int, index: int, new_value: int, part: int = 0) -> List[int]:
    idx_h, idx_l = split14bit(index)
    val_h, val_l = split14bit(new_value)

    return [
        0xF0,
        *MANUFACTURER_ID,
        STATUS_REQUEST,
        part & 0x7F,
        WISH_SET,
        AMOUNT_SINGLE,
        block & 0x7F,
        section & 0x7F,
        idx_h,
        idx_l,
        val_h,
        val_l,
        0xF7,
    ]


@dataclass(frozen=True)
class BankProgram:
    bank: int
    program: int


def parse_bank_program(args: argparse.Namespace) -> BankProgram:
    if args.bank is not None:
        bank = args.bank
    else:
        # MSB/LSB mode
        bank = (args.msb << 7) | args.lsb

    return BankProgram(bank=bank, program=args.pc)


def validate(args: argparse.Namespace, bp: BankProgram) -> None:
    if not (0 <= args.button <= 0x3FFF):
        raise SystemExit("--button must be 0..16383")

    if not (1 <= args.channel <= 16):
        raise SystemExit("--channel must be 1..16")

    if not (0 <= bp.program <= 127):
        raise SystemExit("--pc must be 0..127")

    if not (0 <= bp.bank <= 0x3FFF):
        raise SystemExit("bank must be 0..16383 (14-bit)")


def flatten(chunks: Iterable[Iterable[int]]) -> bytes:
    out: List[int] = []
    for chunk in chunks:
        out.extend(chunk)
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate OpenDeck SysEx (.syx) to configure one button as Bank Select (MSB/LSB) + Program Change.",
    )

    parser.add_argument("--out", required=True, type=Path, help="Output .syx file")
    parser.add_argument("--button", required=True, type=int, help="Button index (0-based)")
    parser.add_argument("--channel", required=True, type=int, help="MIDI channel 1..16")

    bank_group = parser.add_mutually_exclusive_group(required=True)
    bank_group.add_argument("--bank", type=int, help="14-bit bank number 0..16383")
    bank_group.add_argument("--msb-lsb", action="store_true", help="Provide --msb and --lsb instead of --bank")

    parser.add_argument("--msb", type=int, default=0, help="Bank MSB 0..127 (used with --msb-lsb)")
    parser.add_argument("--lsb", type=int, default=0, help="Bank LSB 0..127 (used with --msb-lsb)")
    parser.add_argument("--pc", required=True, type=int, help="Program Change number 0..127")

    args = parser.parse_args()

    if args.msb_lsb:
        if not (0 <= args.msb <= 127 and 0 <= args.lsb <= 127):
            raise SystemExit("--msb/--lsb must be 0..127")

    bp = parse_bank_program(args)
    validate(args, bp)

    messages = [
        sysex_special(SPECIAL_CONN_OPEN),
        sysex_set_single(BLOCK_BUTTONS, SECTION_BUTTON_MESSAGE_TYPE, args.button, BUTTON_MSG_BANK_SELECT_PROGRAM_CHANGE),
        sysex_set_single(BLOCK_BUTTONS, SECTION_BUTTON_CHANNEL, args.button, args.channel),
        sysex_set_single(BLOCK_BUTTONS, SECTION_BUTTON_MIDI_ID, args.button, bp.program),
        sysex_set_single(BLOCK_BUTTONS, SECTION_BUTTON_VALUE, args.button, bp.bank),
        sysex_special(SPECIAL_CONN_CLOSE),
    ]

    data = flatten(messages)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)

    # Also print a quick summary.
    print(f"Wrote {len(data)} bytes to {args.out}")
    print(f"Button {args.button}: CH={args.channel}, BANK={bp.bank} (MSB={bp.bank >> 7}, LSB={bp.bank & 0x7F}), PC={bp.program}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
