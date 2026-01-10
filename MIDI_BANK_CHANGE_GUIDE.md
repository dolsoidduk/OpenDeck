# MIDI Bank Change Guide

This guide explains how to configure **Bank Select (MSB/LSB)** + **Program Change (PC)** in OpenDeck.

## What OpenDeck sends

When you set a button (or other control) to the message type **Bank Select + Program Change**, OpenDeck expands that single action into **three MIDI messages**, sent in this order:

1. **Control Change** CC#0  (Bank Select **MSB**)
2. **Control Change** CC#32 (Bank Select **LSB**)
3. **Program Change** (PC)

This matches the standard MIDI “Bank Select then Program Change” workflow used by many synths and plugins.

## OpenDeck configuration fields

For message type **BANK_SELECT_PROGRAM_CHANGE**:

- **CHANNEL**: MIDI channel (1–16)
- **MIDI_ID**: Program Change number (0–127)
- **VALUE**: 14-bit bank number (0–16383)

OpenDeck derives the Bank Select bytes from VALUE:

- MSB = (VALUE >> 7) & 0x7F
- LSB = VALUE & 0x7F

Equivalently, if you already know MSB and LSB, you can compute VALUE as:

- VALUE = (MSB << 7) | LSB  =  MSB × 128 + LSB

## Examples

### Example 1: MSB=0, LSB=0, PC=10

- MESSAGE_TYPE = BANK_SELECT_PROGRAM_CHANGE
- CHANNEL = 1
- VALUE = (0 << 7) | 0 = 0
- MIDI_ID = 10

Result:

- CC#0  = 0
- CC#32 = 0
- PC    = 10

### Example 2: MSB=3, LSB=7, PC=12

- MESSAGE_TYPE = BANK_SELECT_PROGRAM_CHANGE
- CHANNEL = 1
- VALUE = (3 << 7) | 7 = 391
- MIDI_ID = 12

Result:

- CC#0  = 3
- CC#32 = 7
- PC    = 12

### Example 3: “Bank 5, program 42” style devices

Some gear documents banks as a single number (0–16383) instead of MSB/LSB.

If your device says:

- Bank = 5
- Program = 42

Then set:

- VALUE = 5
- MIDI_ID = 42

OpenDeck will automatically split Bank 5 into MSB/LSB.

## Common pitfalls

- **Program numbering (0–127 vs 1–128):** MIDI Program Change values are 0–127, but some UIs/manuals display programs as 1–128. If your target device is “off by one,” try subtracting 1 from the program number.
- **Bank Select required before PC:** Many devices require receiving CC#0/CC#32 before the Program Change to select the right bank. OpenDeck always sends Bank Select first.
- **Devices using only MSB or only LSB:** Some devices only honor CC#0 or CC#32. If that’s the case, set the unused part to 0 (by choosing a VALUE where MSB or LSB is 0), or consult the device’s MIDI implementation chart.

## SysEx로 버튼 설정하기(Configurator 없이)

OpenDeck는 SysExConf 프로토콜로 버튼 설정값을 쓸 수 있습니다. 레포에 포함된 스크립트로 “버튼 1개를 BANK_SELECT_PROGRAM_CHANGE로 설정”하는 `.syx` 파일을 생성할 수 있습니다.

- 스크립트: `scripts/button_bank_pc_sysex.py`

예시(버튼 0번, 채널 1, MSB=3/LSB=7, PC=12):

`python3 scripts/button_bank_pc_sysex.py --out bank_pc.syx --button 0 --channel 1 --msb-lsb --msb 3 --lsb 7 --pc 12`

생성된 `bank_pc.syx`를 MIDI SysEx 송신 툴로 OpenDeck에 전송하면 설정이 적용됩니다.
