#!/usr/bin/env python3
"""
Example: Load and explore a GameMaker data.win file
"""

import sys
import json
from pathlib import Path

# Add parent directory to path if running from this directory
sys.path.insert(0, str(Path(__file__).parent.parent))

from datawin_py import DataWin, DataWinParserOptions


def print_header(text: str, char: str = "=") -> None:
    """Print a formatted header"""
    print(f"\n{char * 60}")
    print(f" {text}")
    print(f"{char * 60}\n")


def example_basic_loading(file_path: str) -> None:
    """Example 1: Load and print basic game info"""
    print_header("Example 1: Basic Loading", "=")
    
    dw = DataWin.load(file_path)
    print(f"Game Name: {dw.gen8.name}")
    print(f"Version: {dw.gen8.major}.{dw.gen8.minor}.{dw.gen8.release}.{dw.gen8.build}")
    print(f"Display Name: {dw.gen8.display_name}")
    print(f"Window: {dw.gen8.default_window_width}x{dw.gen8.default_window_height}")
    print(f"Steam App ID: {dw.gen8.steam_app_id}")
    print()
    print(f"Statistics:")
    print(f"  Rooms: {len(dw.room.rooms)}")
    print(f"  Sprites: {len(dw.sprt.sprites)}")
    print(f"  Objects: {len(dw.objt.objects)}")
    print(f"  Sounds: {len(dw.sond.sounds)}")
    print(f"  Fonts: {len(dw.font.fonts)}")
    print(f"  Paths: {len(dw.path.paths)}")
    print(f"  Scripts: {len(dw.scpt.scripts)}")
    print(f"  Strings: {len(dw.strg.strings)}")


def example_selective_parsing(file_path: str) -> None:
    """Example 2: Only parse specific chunks for performance"""
    print_header("Example 2: Selective Parsing (Rooms & Sprites only)", "=")
    
    options = DataWinParserOptions(
        parse_gen8=True,
        parse_room=True,
        parse_sprt=True,
        parse_objt=True,
        # Skip everything else
        parse_code=False,
        parse_vari=False,
        parse_func=False,
        parse_audo=False,
        parse_txtr=False,
        parse_sond=False,
        parse_extn=False,
        parse_lang=False,
        parse_optn=False,
    )
    
    dw = DataWin.load(file_path, options)
    print(f"Loaded {len(dw.room.rooms)} rooms and {len(dw.sprt.sprites)} sprites")


def example_rooms(file_path: str) -> None:
    """Example 3: Explore rooms and their contents"""
    print_header("Example 3: Rooms", "=")
    
    dw = DataWin.load(file_path)
    
    for i, room in enumerate(dw.room.rooms[:5]):  # First 5 rooms
        print(f"Room {i}: {room.name}")
        print(f"  Size: {room.width}x{room.height}")
        print(f"  Speed: {room.speed}")
        print(f"  Background Color: {hex(room.background_color)}")
        print(f"  Game Objects: {len(room.game_objects)}")
        print(f"  Tiles: {len(room.tiles)}")
        print(f"  Views: {len([v for v in room.views if v.enabled])}")
        
        # Show first few game objects
        for j, obj_inst in enumerate(room.game_objects[:3]):
            obj_def = dw.objt.objects[obj_inst.object_definition]
            print(f"    - {obj_def.name} @ ({obj_inst.x}, {obj_inst.y})")
        
        if len(room.game_objects) > 3:
            print(f"    ... and {len(room.game_objects) - 3} more")
        print()


def example_sprites(file_path: str) -> None:
    """Example 4: Explore sprites"""
    print_header("Example 4: Sprites", "=")
    
    dw = DataWin.load(file_path)
    
    for i, sprite in enumerate(dw.sprt.sprites[:10]):  # First 10 sprites
        print(f"Sprite {i}: {sprite.name}")
        print(f"  Size: {sprite.width}x{sprite.height}")
        print(f"  Origin: ({sprite.origin_x}, {sprite.origin_y})")
        print(f"  Textures: {len(sprite.texture_offsets)}")
        print(f"  Transparent: {sprite.transparent}, Smooth: {sprite.smooth}")
        if sprite.masks:
            print(f"  Collision Masks: {len(sprite.masks)} frames")
        print()


def example_objects(file_path: str) -> None:
    """Example 5: Explore game objects"""
    print_header("Example 5: Game Objects", "=")
    
    dw = DataWin.load(file_path)
    
    for i, obj in enumerate(dw.objt.objects[:10]):  # First 10 objects
        print(f"Object {i}: {obj.name}")
        
        if obj.sprite_id >= 0 and obj.sprite_id < len(dw.sprt.sprites):
            sprite = dw.sprt.sprites[obj.sprite_id]
            print(f"  Sprite: {sprite.name}")
        
        print(f"  Depth: {obj.depth}")
        print(f"  Visible: {obj.visible}, Solid: {obj.solid}")
        print(f"  Physics: {obj.uses_physics}, Sensor: {obj.is_sensor}")
        
        # Event counts
        total_events = sum(len(el.events) for el in obj.event_lists)
        if total_events > 0:
            print(f"  Events: {total_events} total")
            for et_idx, el in enumerate(obj.event_lists):
                if el.events:
                    print(f"    Event Type {et_idx}: {len(el.events)}")
        print()


def example_paths(file_path: str) -> None:
    """Example 6: Explore paths"""
    print_header("Example 6: Paths", "=")
    
    dw = DataWin.load(file_path)
    
    for i, path in enumerate(dw.path.paths[:5]):  # First 5 paths
        print(f"Path {i}: {path.name}")
        print(f"  Smooth: {path.is_smooth}, Closed: {path.is_closed}")
        print(f"  Precision: {path.precision}")
        print(f"  Points: {len(path.points)}")
        print(f"  Total Length: {path.length:.2f}")
        
        # Show sample positions
        if path.points:
            for t in [0.0, 0.25, 0.5, 0.75, 1.0]:
                x, y, speed = path.get_position(t)
                print(f"    t={t}: ({x:.1f}, {y:.1f}, speed={speed:.2f})")
        print()


def example_progress_callback(file_path: str) -> None:
    """Example 7: Loading with progress callback"""
    print_header("Example 7: Progress Callback", "=")
    
    def on_progress(chunk_name: str, chunk_idx: int, total: int) -> None:
        if total > 0:
            pct = (chunk_idx / total) * 100
            print(f"  Loading {chunk_name:6s} ({chunk_idx:3d}/{total}) [{pct:5.1f}%]")
    
    options = DataWinParserOptions(progress_callback=on_progress)
    dw = DataWin.load(file_path, options)
    print(f"\nLoading complete!")


def example_metadata_export(file_path: str) -> None:
    """Example 8: Export game metadata as JSON"""
    print_header("Example 8: Metadata Export", "=")
    
    dw = DataWin.load(file_path)
    
    metadata = {
        "game": {
            "name": dw.gen8.name,
            "display_name": dw.gen8.display_name,
            "version": {
                "major": dw.gen8.major,
                "minor": dw.gen8.minor,
                "release": dw.gen8.release,
                "build": dw.gen8.build,
            }
        },
        "window": {
            "width": dw.gen8.default_window_width,
            "height": dw.gen8.default_window_height,
        },
        "steam_app_id": dw.gen8.steam_app_id,
        "statistics": {
            "rooms": len(dw.room.rooms),
            "sprites": len(dw.sprt.sprites),
            "objects": len(dw.objt.objects),
            "sounds": len(dw.sond.sounds),
            "fonts": len(dw.font.fonts),
            "paths": len(dw.path.paths),
            "scripts": len(dw.scpt.scripts),
            "strings": len(dw.strg.strings),
            "shaders": len(dw.shdr.shaders),
            "timelines": len(dw.tmln.timelines),
        },
        "resources": {
            "room_names": [r.name for r in dw.room.rooms],
            "sprite_names": [s.name for s in dw.sprt.sprites],
            "object_names": [o.name for o in dw.objt.objects],
        }
    }
    
    print(json.dumps(metadata, indent=2))


def main():
    if len(sys.argv) < 2:
        print("Usage: python example.py <path_to_data.win>")
        print("\nThis script demonstrates various DataWin parsing features.")
        sys.exit(1)
    
    file_path = sys.argv[1]
    
    if not Path(file_path).exists():
        print(f"Error: File '{file_path}' not found")
        sys.exit(1)
    
    try:
        # Run examples
        example_basic_loading(file_path)
        example_selective_parsing(file_path)
        example_rooms(file_path)
        example_sprites(file_path)
        example_objects(file_path)
        example_paths(file_path)
        example_progress_callback(file_path)
        example_metadata_export(file_path)
        
        print_header("Examples Complete!", "=")
        
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
