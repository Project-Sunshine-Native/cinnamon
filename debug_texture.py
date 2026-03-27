#!/usr/bin/env python3
"""Debug script to understand texture/sprite/TPAG relationships"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from datawin_py import DataWin

dw = DataWin.load("data.win")

print(f"TPAG Items: {len(dw.tpag.items)}")
print(f"Sprites: {len(dw.sprt.sprites)}")
print(f"Textures: {len(dw.txtr.textures)}")

# Look at first few sprites
print("\n=== First 5 Sprites ===")
for i in range(min(5, len(dw.sprt.sprites))):
    sprite = dw.sprt.sprites[i]
    print(f"\nSprite {i}: {sprite.name}")
    print(f"  Size: {sprite.width}x{sprite.height}")
    print(f"  Origin: ({sprite.origin_x}, {sprite.origin_y})")
    print(f"  Texture offsets: {sprite.texture_offsets}")
    print(f"  Texture offset count: {len(sprite.texture_offsets)}")
    
    # Check if these are valid TPAG indices
    for j, offset in enumerate(sprite.texture_offsets[:3]):
        if offset < len(dw.tpag.items):
            item = dw.tpag.items[offset]
            print(f"    Frame {j}: TPAG[{offset}] -> page {item.texture_page_id}, "
                  f"src({item.source_x},{item.source_y})-{item.source_width}x{item.source_height}")
        else:
            print(f"    Frame {j}: offset {offset} > TPAG size {len(dw.tpag.items)}")

# Check TPAG offset map
print(f"\n=== TPAG Offset Map Size: {len(dw.tpag.offset_map)} ===")
print(f"First few entries: {list(dw.tpag.offset_map.items())[:5]}")

# Check texture page items
print(f"\n=== First 5 TPAG Items ===")
for i in range(min(5, len(dw.tpag.items))):
    item = dw.tpag.items[i]
    print(f"TPAG[{i}]: page={item.texture_page_id}, "
          f"src({item.source_x},{item.source_y})-{item.source_width}x{item.source_height}, "
          f"tgt({item.target_x},{item.target_y})-{item.target_width}x{item.target_height}")
