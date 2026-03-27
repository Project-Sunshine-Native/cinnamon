QUICKSTART.md

# DataWin Python Package - Quick Start

## Installation

### Option 1: Install from Local Source
```bash
cd datawin_py
pip install -e .
```

### Option 2: Use Directly (No Install)
```python
import sys
from pathlib import Path
sys.path.insert(0, str(Path("datawin_py")))

from datawin_py import DataWin
```

## Basic Usage (3 lines!)

```python
from datawin_py import DataWin

dw = DataWin.load("data.win")
print(dw)  # <DataWin game=MyGame version=1.0.0.0 rooms=5 sprites=12 objects=8>
```

## Common Tasks

### 1. Get Game Info
```python
dw = DataWin.load("data.win")
print(f"Game: {dw.gen8.name}")
print(f"Version: {dw.gen8.major}.{dw.gen8.minor}.{dw.gen8.release}")
print(f"Window: {dw.gen8.default_window_width}x{dw.gen8.default_window_height}")
```

### 2. List All Rooms
```python
for room in dw.room.rooms:
    print(f"- {room.name} ({room.width}x{room.height})")
```

### 3. Find a Room by Name
```python
room = dw.get_room_by_name("Level_1")
if room:
    print(f"Found room: {room.name}")
    print(f"Objects in room: {len(room.game_objects)}")
```

### 4. Get Sprite Details
```python
sprite = dw.get_sprite_by_name("spr_player")
if sprite:
    print(f"Sprite size: {sprite.width}x{sprite.height}")
    print(f"Frames: {len(sprite.texture_offsets)}")
    print(f"Origin: ({sprite.origin_x}, {sprite.origin_y})")
```

### 5. List Game Objects and Their Sprites
```python
for obj in dw.objt.objects:
    if obj.sprite_id >= 0:
        sprite = dw.sprt.sprites[obj.sprite_id]
        print(f"{obj.name} uses {sprite.name}")
```

### 6. Find All Objects in a Room
```python
room = dw.get_room_by_name("Level_1")
for obj_inst in room.game_objects:
    obj_def = dw.objt.objects[obj_inst.object_definition]
    print(f"{obj_def.name} at ({obj_inst.x}, {obj_inst.y})")
```

### 7. Get Path Position
```python
path = dw.path.paths[0]
# Get position at 25%, 50%, 75% along the path
for t in [0.25, 0.5, 0.75]:
    x, y, speed = path.get_position(t)
    print(f"t={t}: ({x:.1f}, {y:.1f})")
```

### 8. Parse Only Specific Chunks (Faster!)
```python
from datawin_py import DataWin, DataWinParserOptions

options = DataWinParserOptions(
    parse_gen8=True,
    parse_room=True,
    parse_sprt=True,
    parse_objt=False,  # Don't parse objects if you don't need them
    parse_code=False,  # Skip bytecode
    parse_audo=False,  # Skip audio
)

dw = DataWin.load("data.win", options)
```

### 9. Export Game Metadata as JSON
```python
import json

metadata = {
    "name": dw.gen8.name,
    "version": f"{dw.gen8.major}.{dw.gen8.minor}",
    "rooms": len(dw.room.rooms),
    "sprites": len(dw.sprt.sprites),
    "objects": len(dw.objt.objects),
}

print(json.dumps(metadata, indent=2))
```

### 10. Get All Strings
```python
# Strings are stored in the STRG chunk
strings = [s for s in dw.strg.strings if s]
for idx, s in enumerate(strings[:10]):  # First 10
    print(f"{idx}: {s}")
```

## Available Attributes

### Game Info (dw.gen8)
- `name` - Game name
- `major`, `minor`, `release`, `build` - Version
- `default_window_width`, `default_window_height` - Window size
- `steam_app_id` - Steam ID
- `timestamp` - Build date

### Collections
- `dw.room.rooms` - List of Room objects
- `dw.sprt.sprites` - List of Sprite objects
- `dw.objt.objects` - List of GameObject objects
- `dw.sond.sounds` - List of Sound objects
- `dw.font.fonts` - List of Font objects
- `dw.path.paths` - List of GamePath objects
- `dw.strg.strings` - List of strings

### Helper Methods
- `get_room(index)` - Get room by index
- `get_sprite(index)` - Get sprite by index
- `get_object(index)` - Get object by index
- `get_string(index)` - Get string by index
- `get_room_by_name(name)` - Find room by name
- `get_sprite_by_name(name)` - Find sprite by name
- `get_object_by_name(name)` - Find object by name

## Data Structure Examples

### Room Object
```python
room = dw.room.rooms[0]
room.name              # Room name
room.width, room.height  # Room dimensions
room.speed               # Game speed
room.game_objects        # List of game object instances
room.tiles               # List of tile instances
room.backgrounds         # List of background slots
room.views               # List of view slots
```

### Sprite Object
```python
sprite = dw.sprt.sprites[0]
sprite.name              # Sprite name
sprite.width, sprite.height  # Frame size
sprite.origin_x, sprite.origin_y  # Origin point
sprite.texture_offsets   # List of texture page indices
sprite.masks             # Collision masks (if loaded)
```

### GameObject Object
```python
obj = dw.objt.objects[0]
obj.name                 # Object name
obj.sprite_id            # Index into sprites
obj.depth                # Depth/layer
obj.visible, obj.solid   # Flags
obj.event_lists          # Events by type
obj.physics_vertices     # Physics shape
```

### Room Game Object Instance
```python
inst = room.game_objects[0]
inst.x, inst.y           # Position
inst.object_definition   # Index into objects
inst.instance_id         # Unique ID
inst.scale_x, inst.scale_y  # Scale
inst.rotation            # Rotation angle
inst.creation_code       # Creation code index
```

## Error Handling

```python
try:
    dw = DataWin.load("data.win")
except RuntimeError as e:
    print(f"Failed to load: {e}")

# Get optional elements safely
sprite = dw.get_sprite(obj.sprite_id) if obj.sprite_id >= 0 else None
if sprite:
    print(sprite.name)
```

## Performance Tips

1. **Skip Unused Chunks** - Use DataWinParserOptions
2. **Skip Sprite Masks** - Set `skip_loading_precise_masks_for_non_precise_sprites=True`
3. **Progressive Loading** - Use progress callbacks for UI feedback
4. **Lazy Loading** - Load strings/textures on demand if needed

## Example: Complete Game Analyzer

```python
from datawin_py import DataWin

dw = DataWin.load("data.win")

print(f"=== {dw.gen8.name} ===")
print(f"Version: {dw.gen8.major}.{dw.gen8.minor}")
print(f"Window: {dw.gen8.default_window_width}x{dw.gen8.default_window_height}\n")

print("ROOMS:")
for room in dw.room.rooms[:5]:
    print(f"  {room.name}: {room.width}x{room.height} ({len(room.game_objects)} objects)")

print("\nSPRITES:")
for sprite in dw.sprt.sprites[:5]:
    print(f"  {sprite.name}: {sprite.width}x{sprite.height} ({len(sprite.texture_offsets)} frames)")

print("\nOBJECTS:")
for obj in dw.objt.objects[:5]:
    spr_name = dw.sprt.sprites[obj.sprite_id].name if obj.sprite_id >= 0 else "None"
    print(f"  {obj.name}: sprite={spr_name}, depth={obj.depth}")

print("\nSOUNDS:")
for sound in dw.sond.sounds[:5]:
    print(f"  {sound.name}: {sound.file}")
```

## Troubleshooting

### Import Error
```python
# Make sure you're in the right directory or install the package
import sys
from pathlib import Path
sys.path.insert(0, "path/to/cinnamon")  # Add to path
```

### File Not Found
```python
from pathlib import Path
if Path("data.win").exists():
    dw = DataWin.load("data.win")
else:
    print("data.win not found")
```

### Invalid File Error
```python
# Make sure you're loading a real GameMaker data.win file
# Not all .win files are GameMaker format (e.g., Windows config files)
```

## Next Steps

1. Read full [README.md](README.md)
2. Run example: `python example.py data.win`
3. Explore [implementation details](IMPLEMENTATION.md)
4. Check [API documentation](README.md#available-attributes)

---

Happy modding! 🎮
