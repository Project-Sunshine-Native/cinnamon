# DataWin Python Package

A comprehensive Python package for loading and parsing GameMaker **data.win** files (GameMaker Studio 1.4 and earlier).

## Features

- **Full Binary Parsing**: Parses all GameMaker data chunks (GEN8, OPTN, LANG, EXTN, SOND, AGRP, SPRT, BGND, PATH, SCPT, GLOB, SHDR, FONT, TMLN, OBJT, ROOM, TPAG, CODE, VARI, FUNC, STRG, TXTR, AUDO)
- **Flexible Parsing**: Choose which chunks to parse for performance optimization
- **Clean API**: Object-oriented access to all game data structures
- **Type Hints**: Full type annotations for IDE support
- **Path Computation**: Accurate smooth/linear path interpolation matching GM's algorithm

## Installation

### From Source

```bash
cd datawin_py
pip install -e .
```

### Development

```bash
pip install -e ".[dev]"
```

## Quick Start

### Basic Usage

```python
from datawin import DataWin

# Load a data.win file
dw = DataWin.load("path/to/data.win")

# Access game information
print(f"Game: {dw.gen8.name}")
print(f"Version: {dw.gen8.major}.{dw.gen8.minor}.{dw.gen8.release}")
print(f"Rooms: {len(dw.room.rooms)}")
print(f"Sprites: {len(dw.sprt.sprites)}")
print(f"Objects: {len(dw.objt.objects)}")

# Access specific elements
room = dw.get_room_by_name("Room1")
if room:
    print(f"Room size: {room.width}x{room.height}")
    print(f"Game objects in room: {len(room.game_objects)}")

sprite = dw.get_sprite_by_name("spr_player")
if sprite:
    print(f"Sprite size: {sprite.width}x{sprite.height}")
    print(f"Textures: {len(sprite.texture_offsets)}")
```

### Selective Parsing

Load only the chunks you need for faster parsing:

```python
from datawin import DataWin, DataWinParserOptions

options = DataWinParserOptions(
    parse_gen8=True,
    parse_room=True,
    parse_sprt=True,
    parse_objt=True,
    # Skip other chunks
    parse_code=False,
    parse_vari=False,
    parse_func=False,
    parse_audo=False,
    parse_txtr=False,
)

dw = DataWin.load("data.win", options)
```

### Progress Callback

Monitor loading progress:

```python
def progress(chunk_name: str, chunk_index: int, total_chunks: int) -> None:
    print(f"Loading {chunk_name}... ({chunk_index}/{total_chunks})")

options = DataWinParserOptions(
    progress_callback=progress
)

dw = DataWin.load("data.win", options)
```

## Data Structure Examples

### Accessing Rooms

```python
for room in dw.room.rooms:
    print(f"Room: {room.name}")
    print(f"  Size: {room.width}x{room.height}")
    print(f"  Speed: {room.speed}")
    print(f"  Background color: {hex(room.background_color)}")
    
    # Game objects in room
    for obj in room.game_objects:
        print(f"  - Instance {obj.instance_id}: {dw.objt.objects[obj.object_definition].name}")
        print(f"    Position: ({obj.x}, {obj.y})")
        print(f"    Scale: ({obj.scale_x}, {obj.scale_y})")
```

### Accessing Sprites

```python
for sprite in dw.sprt.sprites:
    print(f"Sprite: {sprite.name}")
    print(f"  Size: {sprite.width}x{sprite.height}")
    print(f"  Origin: ({sprite.origin_x}, {sprite.origin_y})")
    print(f"  Textures: {len(sprite.texture_offsets)}")
    
    # Collision masks
    if sprite.masks:
        print(f"  Collision masks: {len(sprite.masks)} frames")
```

### Accessing Game Objects

```python
for obj in dw.objt.objects:
    print(f"Object: {obj.name}")
    print(f"  Sprite: {dw.sprt.sprites[obj.sprite_id].name if obj.sprite_id >= 0 else 'None'}")
    print(f"  Depth: {obj.depth}")
    print(f"  Solid: {obj.solid}, Visible: {obj.visible}")
    
    # Events
    for event_type_idx, event_list in enumerate(obj.event_lists):
        if event_list.events:
            print(f"  Event type {event_type_idx}: {len(event_list.events)} events")
            for event in event_list.events:
                print(f"    - Subtype {event.event_subtype}: {len(event.actions)} actions")
```

### Working with Paths

```python
for path in dw.path.paths:
    print(f"Path: {path.name}")
    print(f"  Smooth: {path.is_smooth}, Closed: {path.is_closed}")
    print(f"  Points: {len(path.points)}")
    print(f"  Length: {path.length}")
    
    # Get position at 50% along path
    x, y, speed = path.get_position(0.5)
    print(f"  Position at t=0.5: ({x}, {y}, speed={speed})")
```

### Accessing Strings

```python
# Direct access
string = dw.get_string(0)
print(f"String 0: {string}")

# Browse all strings
for idx, s in enumerate(dw.strg.strings):
    if s:
        print(f"{idx}: {s}")
```

### Code and Bytecode

```python
# Code entries (GML scripts/events)
for entry in dw.code.entries:
    print(f"Code: {entry.name}")
    print(f"  Length: {entry.length} bytes")
    print(f"  Arguments: {entry.arguments_count}")
    print(f"  Locals: {entry.locals_count}")
    print(f"  Bytecode offset: {entry.bytecode_absolute_offset}")

# Functions
for func in dw.func.functions:
    print(f"Function: {func.name}")
    print(f"  First address: {func.first_address}")
    print(f"  Occurrences: {func.occurrences}")

# Variables
for var in dw.vari.variables:
    print(f"Variable: {var.name}")
    print(f"  Type: {var.instance_type}, ID: {var.var_id}")
    print(f"  First address: {var.first_address}")
```

### Sounds and Audio

```python
for sound in dw.sond.sounds:
    print(f"Sound: {sound.name}")
    print(f"  Type: {sound.sound_type}")
    print(f"  File: {sound.file}")
    print(f"  Volume: {sound.volume}, Pitch: {sound.pitch}")
    print(f"  Audio group: {sound.audio_group}")
```

### Fonts

```python
for font in dw.font.fonts:
    print(f"Font: {font.name}")
    print(f"  Display name: {font.display_name}")
    print(f"  Size: {font.em_size}")
    print(f"  Bold: {font.bold}, Italic: {font.italic}")
    print(f"  Glyphs: {len(font.glyphs)}")
    
    # Font glyphs
    for glyph in font.glyphs:
        print(f"  Char '{chr(glyph.character)}': source({glyph.source_x}, {glyph.source_y})")
```

## Parser Options

`DataWinParserOptions` controls which chunks to parse:

```python
@dataclass
class DataWinParserOptions:
    parse_gen8: bool = True              # General game info
    parse_optn: bool = True              # Game options
    parse_lang: bool = True              # Languages
    parse_extn: bool = True              # Extensions
    parse_sond: bool = True              # Sounds
    parse_agrp: bool = True              # Audio groups
    parse_sprt: bool = True              # Sprites
    parse_bgnd: bool = True              # Backgrounds
    parse_path: bool = True              # Paths
    parse_scpt: bool = True              # Scripts
    parse_glob: bool = True              # Global variables
    parse_shdr: bool = True              # Shaders
    parse_font: bool = True              # Fonts
    parse_tmln: bool = True              # Timelines
    parse_objt: bool = True              # Objects
    parse_room: bool = True              # Rooms
    parse_tpag: bool = True              # Texture pages
    parse_code: bool = True              # Bytecode
    parse_vari: bool = True              # Variables
    parse_func: bool = True              # Functions
    parse_strg: bool = True              # Strings
    parse_txtr: bool = True              # Textures
    parse_audo: bool = True              # Audio
    skip_loading_precise_masks_for_non_precise_sprites: bool = False
    progress_callback: Optional[Callable] = None
```

## Architecture

### Modules

- **`binary_reader.py`**: Low-level binary reading (uint32, float, etc.)
- **`structures.py`**: Data class definitions for all chunk types
- **`parsers.py`**: Chunk-specific parsing functions
- **`datawin.py`**: Main `DataWin` class and public API

### Binary Format

The data.win file is structured as:
- `FORM` magic (4 bytes)
- Form length (4 bytes)
- Variable number of chunks, each:
  - Chunk name (4 bytes)
  - Chunk length (4 bytes)
  - Chunk data (variable)

## Performance Notes

- Parsing is single-threaded
- STRG (strings) chunk is loaded first as other chunks reference it
- By default all chunks are parsed; disable unused chunks for faster loading
- Bytecode is loaded but not validated/interpreted

## Limitations

- **GMS2+ not supported**: This parser targets GMS1.4 and earlier
- **YYC compiled games**: No bytecode available (parse_code will be empty)
- **Streaming textures**: Texture blob data must be extracted separately if needed
- **Path masks**: Sprite collision masks can optionally be skipped for large games

## Example: Extract Game Metadata

```python
from datawin import DataWin

dw = DataWin.load("data.win")

metadata = {
    "name": dw.gen8.name,
    "version": f"{dw.gen8.major}.{dw.gen8.minor}.{dw.gen8.release}.{dw.gen8.build}",
    "window": {
        "width": dw.gen8.default_window_width,
        "height": dw.gen8.default_window_height,
    },
    "steam_app_id": dw.gen8.steam_app_id,
    "stats": {
        "rooms": len(dw.room.rooms),
        "sprites": len(dw.sprt.sprites),
        "objects": len(dw.objt.objects),
        "sounds": len(dw.sond.sounds),
        "fonts": len(dw.font.fonts),
        "paths": len(dw.path.paths),
        "scripts": len(dw.scpt.scripts),
    }
}

import json
print(json.dumps(metadata, indent=2))
```

## License

MIT

## Contributing

Contributions welcome! Areas for enhancement:
- Audio data extraction
- Texture decompression
- Bytecode disassembly
- More documentation and examples

## References

- [Undoing Undertale by Kinnay](https://github.com/Kinnay/Undertale-Modding)
- GameMaker bytecode format documentation
- stb_ds hash map implementation
