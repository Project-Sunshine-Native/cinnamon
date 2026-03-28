#!/usr/bin/env python3
"""Debug script to check offset_map structure"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from datawin_py import DataWin

dw = DataWin.load("data.win")

print(f"Offset map type: {type(dw.tpag.offset_map)}")
print(f"Offset map size: {len(dw.tpag.offset_map)}")

# Get first sprite's texture offset
sprite = dw.sprt.sprites[0]
offset = sprite.texture_offsets[0]
print(f"\nSprite 0: {sprite.name}")
print(f"First texture offset: {offset}")
print(f"Type: {type(offset)}")

# Try to lookup
result = dw.tpag.offset_map.get(offset)
print(f"Lookup result: {result}")

# Try nearby offsets
print(f"\nTrying nearby offsets:")
for test_offset in range(offset - 10, offset + 10):
    result = dw.tpag.offset_map.get(test_offset)
    if result is not None:
        print(f"  {test_offset}: {result}")

# Check what keys are in the offset_map around this offset
print(f"\nKeys in offset_map near {offset}:")
count = 0
for key in sorted(dw.tpag.offset_map.keys()):
    if abs(key - offset) < 500:
        print(f"  {key}: {dw.tpag.offset_map[key]}")
        count += 1
        if count > 10:
            break
