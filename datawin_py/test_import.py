#!/usr/bin/env python3
"""
Quick test to verify package imports and basic functionality
"""

import sys
from pathlib import Path

# Add parent directory to path
sys.path.insert(0, str(Path(__file__).parent.parent))

# Test importing
try:
    from datawin_py import DataWin, DataWinParserOptions
    from datawin_py.binary_reader import BinaryReader
    from datawin_py.structures import (
        GamePath, Room, Sprite, GameObject, Sound, 
        Font, PathPoint, EventAction
    )
    from datawin_py import parsers
    print("✓ All imports successful")
except ImportError as e:
    print(f"✗ Import failed: {e}")
    sys.exit(1)

# Test basic instantiation
try:
    options = DataWinParserOptions()
    print("✓ DataWinParserOptions created")
    
    dw = DataWin()
    print("✓ DataWin instance created")
    
    path = GamePath(name="test_path", is_smooth=True)
    print(f"✓ GamePath created: {path.name}")
    
    room = Room(name="test_room", width=800, height=600)
    print(f"✓ Room created: {room.name} ({room.width}x{room.height})")
    
    sprite = Sprite(name="test_sprite", width=32, height=32)
    print(f"✓ Sprite created: {sprite.name} ({sprite.width}x{sprite.height})")
    
except Exception as e:
    print(f"✗ Instantiation failed: {e}")
    sys.exit(1)

# Test path interpolation
try:
    path = GamePath(
        name="linear_path",
        is_smooth=False,
        is_closed=False,
        precision=4,
        points=[
            PathPoint(x=0.0, y=0.0, speed=1.0),
            PathPoint(x=100.0, y=100.0, speed=1.0),
        ]
    )
    parsers.compute_path_internal(path)
    
    if len(path.internal_points) > 0:
        x, y, speed = path.get_position(0.5)
        print(f"✓ Path interpolation works: position at t=0.5 = ({x:.1f}, {y:.1f})")
    else:
        print("✗ Path computation failed")
        sys.exit(1)
        
except Exception as e:
    print(f"✗ Path interpolation test failed: {e}")
    import traceback
    traceback.print_exc()
    sys.exit(1)

print("\n✓ All tests passed!")
print("\nUsage: python -m datawin_py")
print("Or: from datawin_py import DataWin; dw = DataWin.load('data.win')")
