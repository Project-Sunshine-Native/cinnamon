#!/usr/bin/env python3
"""
DataWin Texture Extractor

Extracts texture pages from GameMaker data.win files and cuts them up
into individual sprite frames using sprite definitions.

Usage:
    python processor.py <path_to_data.win> [output_dir]
"""

import sys
import io
import struct
import subprocess
import shutil
from pathlib import Path
import traceback
from typing import Optional, Tuple, List

# Try to import required packages
try:
    from PIL import Image
except ImportError:
    print("ERROR: PIL/Pillow not found. Install with: pip install Pillow")
    sys.exit(1)

try:
    import zlib
except ImportError:
    print("ERROR: zlib module not found")
    sys.exit(1)

# Add datawin_py to path
sys.path.insert(0, str(Path(__file__).parent))
from datawin_py import DataWin, DataWinParserOptions


class TextureExtractor:
    """Extracts and processes textures from GameMaker data.win files"""

    def __init__(self, data_win_path: str, output_dir: str = "gfx"):
        """Initialize the extractor
        
        Args:
            data_win_path: Path to data.win file
            output_dir: Base output directory for extracted sprites
        """
        self.data_win_path = Path(data_win_path)
        self.output_dir = Path(output_dir)
        self.dw: Optional[DataWin] = None
        self.texture_pages: List[Optional[Image.Image]] = []

    def load_data_win(self):
        """Load the data.win file"""
        print(f"Loading data.win from {self.data_win_path}...")
        self.dw = DataWin.load(str(self.data_win_path))
        print(f"Loaded: {self.dw.gen8.name} v{self.dw.gen8.major}.{self.dw.gen8.minor}")
        print(f"  Sprites: {len(self.dw.sprt.sprites)}")
        print(f"  Backgrounds: {len(self.dw.bgnd.backgrounds)}")
        print(f"  Textures: {len(self.dw.txtr.textures)}")

    def extract_texture_pages(self):
        """Extract and decode texture pages from TXTR chunk"""
        print(f"\nExtracting {len(self.dw.txtr.textures)} texture pages...")
        
        # We need to read from the file to get blob data
        with open(self.data_win_path, 'rb') as f:
            for i, texture in enumerate(self.dw.txtr.textures):
                print(f"  Processing page {i}...", end=" ", flush=True)
                
                if texture.blob_offset == 0:
                    print("(external texture, skipped)")
                    self.texture_pages.append(None)
                    continue
                
                try:
                    # Read texture blob from file
                    f.seek(texture.blob_offset)
                    blob_data = f.read(texture.blob_size)
                    
                    # Decompress PNG data
                    img = Image.open(io.BytesIO(blob_data))
                    self.texture_pages.append(img)
                    print(f"OK: {img.width}x{img.height}")
                except Exception as e:
                    print(f"FAILED: {e}")
                    self.texture_pages.append(None)

    def extract_sprites(self):
        """Extract sprites by cutting up texture pages"""
        if not self.texture_pages:
            print("No texture pages loaded!")
            return
        
        print(f"\nExtracting sprites from texture pages...")
        
        # Ensure output directory exists
        self.output_dir.mkdir(parents=True, exist_ok=True)
        
        sprite_count = 0
        
        for sprite_idx, sprite in enumerate(self.dw.sprt.sprites):
            if not sprite.name:
                print(f"  Sprite {sprite_idx}: (unnamed, skipped)")
                continue
            
            print(f"  Sprite {sprite_idx}: {sprite.name}")
            
            # Create sprite directory
            sprite_dir = self.output_dir / f"{sprite.name}"
            sprite_dir.mkdir(parents=True, exist_ok=True)
            
            # Extract each texture frame for this sprite
            frame_count = len(sprite.texture_offsets)
            png_files = []
            
            for frame_idx, file_offset in enumerate(sprite.texture_offsets):
                try:
                    # Resolve file offset to TPAG index using the offset map
                    tpag_idx = self.dw.tpag.offset_map.get(file_offset)
                    if tpag_idx is None:
                        print(f"    Frame {frame_idx}: Could not resolve offset {file_offset}")
                        continue
                    
                    # Bounds check
                    if tpag_idx >= len(self.dw.tpag.items):
                        print(f"    Frame {frame_idx}: TPAG index {tpag_idx} out of range ({len(self.dw.tpag.items)} items)")
                        continue
                    
                    # Get texture page item info
                    tpag_item = self.dw.tpag.items[tpag_idx]
                    texture_page_id = tpag_item.texture_page_id
                    
                    if not (0 <= texture_page_id < len(self.texture_pages)):
                        print(f"    Frame {frame_idx}: Texture page ID {texture_page_id} out of range")
                        continue
                    
                    page_img = self.texture_pages[texture_page_id]
                    if page_img is None:
                        print(f"    Frame {frame_idx}: Texture page {texture_page_id} not loaded")
                        continue
                    
                    # Extract the rectangle from the texture page
                    # TPAG stores source rectangles
                    src_x = tpag_item.source_x
                    src_y = tpag_item.source_y
                    src_w = tpag_item.source_width
                    src_h = tpag_item.source_height
                    
                    # Full logical sprite size (includes transparent borders)
                    tgt_w = tpag_item.bounding_width
                    tgt_h = tpag_item.bounding_height
                    
                    # Create blank image with bounding size
                    frame_img = Image.new('RGBA', (tgt_w, tgt_h), (0, 0, 0, 0))
                    
                    # Crop from source
                    src_box = (src_x, src_y, min(src_x + src_w, page_img.width), min(src_y + src_h, page_img.height))
                    crop = page_img.crop(src_box)
                    
                    # Paste into target (at target position)
                    tgt_x = tpag_item.target_x
                    tgt_y = tpag_item.target_y
                    frame_img.paste(crop, (tgt_x, tgt_y), crop if crop.mode == 'RGBA' else None)
                    
                    # Save frame
                    frame_path = sprite_dir / f"{sprite.name}_{frame_idx}.png"
                    frame_img.save(str(frame_path), 'PNG')
                    png_files.append(frame_path.name)
                    print(f"    \033[32mOK Frame {frame_idx}: {frame_path.name}\033[0m")
                
                except Exception as e:
                    import traceback
                    print(f"    \033[31mFAILED Frame {frame_idx}: {e}\033[0m")
                    traceback.print_exc()
            
            # Generate .t3s file
            if png_files:
                self.generate_t3s_file(sprite_dir, sprite.name, png_files)
                sprite_count += 1
        
        print(f"\n\033[32mOK Extracted {sprite_count} sprites\033[0m")

    def extract_audio(self):
        """Extract all sounds to romfs/audio/<name>.pcm via ffmpeg (44.1 kHz, 16-bit, stereo)"""
        sounds = self.dw.sond.sounds
        entries = self.dw.audo.entries
        data_win_dir = self.data_win_path.parent
        project_root = Path.cwd()
        sfx_root = project_root / "sfx"

        def resolve_external_audio(sound_file: str) -> Optional[Path]:
            if not sound_file:
                return None

            sf = Path(sound_file)
            base = sf.name

            candidates = [
                sf,
                data_win_dir / sound_file,
                data_win_dir / base,
                data_win_dir / "mus" / base,
                data_win_dir / "audio" / base,
                project_root / sound_file,
                project_root / base,
                project_root / "mus" / base,
                project_root / "audio" / base,
                sfx_root / sound_file,
                sfx_root / base,
                sfx_root / "mus" / base,
                sfx_root / "audio" / base,
            ]

            for c in candidates:
                if c.exists() and c.is_file():
                    return c
            return None

        if not sounds:
            print("No sounds found.")
            return

        print(f"\nExtracting {len(sounds)} audio entries...")

        audio_dir = Path("romfs/audio")
        audio_dir.mkdir(parents=True, exist_ok=True)

        success_count = 0

        with open(self.data_win_path, 'rb') as f:
            for sound in sounds:
                name = sound.name
                if not name:
                    continue

                print(f"  {name}...", end=" ", flush=True)

                try:
                    out_path = audio_dir / f"{name}.pcm"

                    # Prefer original external file path (e.g., mus/*.ogg) when available.
                    # Some games map many music resources to a shared AUDO entry placeholder.
                    ext_src = resolve_external_audio(sound.file)
                    if ext_src is not None:
                        result = subprocess.run(
                            [
                                "ffmpeg", "-y",
                                "-i", str(ext_src),
                                "-f", "s16le",
                                "-ar", "44100",
                                "-ac", "2",
                                str(out_path),
                            ],
                            capture_output=True,
                        )
                    else:
                        audio_idx = sound.audio_file
                        if audio_idx < 0 or audio_idx >= len(entries):
                            print(f"    \033[31mFAILED: audio_file index {audio_idx} out of range\033[0m")
                            continue

                        entry = entries[audio_idx]
                        if entry.data_size == 0:
                            print(f"    \033[31mFAILED: empty audio entry\033[0m")
                            continue

                        # Heuristic warning: tiny 'mus_*' entries are often placeholders and
                        # can cause multiple tracks to decode to the same clip.
                        if name.startswith("mus_") and entry.data_size < 128 * 1024:
                            print(f"    \033[33mWARN: using small AUDO entry ({entry.data_size} B) for music; external file not found\033[0m")

                        f.seek(entry.data_offset)
                        raw_data = f.read(entry.data_size)

                        result = subprocess.run(
                            [
                                "ffmpeg", "-y",
                                "-i", "pipe:0",
                                "-f", "s16le",
                                "-ar", "44100",
                                "-ac", "2",
                                str(out_path),
                            ],
                            input=raw_data,
                            capture_output=True,
                        )

                    if result.returncode != 0:
                        print(f"    \033[31mFAILED: ffmpeg error: {result.stderr.decode(errors='replace').splitlines()[-1]}\033[0m")
                    else:
                        # Also emit file-stem alias when it differs from the SOND name.
                        if sound.file:
                            file_stem = Path(sound.file).stem
                            if file_stem and file_stem != name:
                                alias_path = audio_dir / f"{file_stem}.pcm"
                                if alias_path != out_path:
                                    shutil.copyfile(out_path, alias_path)
                        print(f"    \033[32mOK\033[0m")
                        success_count += 1

                except Exception as e:
                    print(f"    \033[31mFAILED: {e}\033[0m  ")
                    traceback.print_exc()
        print(f"\n\033[32mOK Extracted {success_count} audio files to romfs/audio/\033[0m")

    def extract_backgrounds(self):
        """Extract background images from texture pages"""
        if not self.texture_pages:
            print("No texture pages loaded!")
            return

        print(f"\nExtracting backgrounds from texture pages...")
        self.output_dir.mkdir(parents=True, exist_ok=True)

        background_count = 0

        for bg_idx, background in enumerate(self.dw.bgnd.backgrounds):
            if not background.name:
                print(f"  Background {bg_idx}: (unnamed, skipped)")
                continue

            print(f"  Background {bg_idx}: {background.name}")

            bg_dir = self.output_dir / f"{background.name}"
            bg_dir.mkdir(parents=True, exist_ok=True)

            try:
                tpag_idx = self.dw.tpag.offset_map.get(background.texture_offset)
                if tpag_idx is None:
                    print(f"    Could not resolve offset {background.texture_offset}")
                    continue

                if tpag_idx >= len(self.dw.tpag.items):
                    print(f"    TPAG index {tpag_idx} out of range ({len(self.dw.tpag.items)} items)")
                    continue

                tpag_item = self.dw.tpag.items[tpag_idx]
                texture_page_id = tpag_item.texture_page_id

                if not (0 <= texture_page_id < len(self.texture_pages)):
                    print(f"    Texture page ID {texture_page_id} out of range")
                    continue

                page_img = self.texture_pages[texture_page_id]
                if page_img is None:
                    print(f"    Texture page {texture_page_id} not loaded")
                    continue

                src_x = tpag_item.source_x
                src_y = tpag_item.source_y
                src_w = tpag_item.source_width
                src_h = tpag_item.source_height

                tgt_w = tpag_item.bounding_width or src_w
                tgt_h = tpag_item.bounding_height or src_h

                frame_img = Image.new('RGBA', (tgt_w, tgt_h), (0, 0, 0, 0))
                src_box = (
                    src_x,
                    src_y,
                    min(src_x + src_w, page_img.width),
                    min(src_y + src_h, page_img.height),
                )
                crop = page_img.crop(src_box)

                tgt_x = tpag_item.target_x
                tgt_y = tpag_item.target_y
                frame_img.paste(crop, (tgt_x, tgt_y), crop if crop.mode == 'RGBA' else None)

                frame_path = bg_dir / f"{background.name}_0.png"
                frame_img.save(str(frame_path), 'PNG')
                self.generate_t3s_file(bg_dir, background.name, [frame_path.name])
                print(f"    \033[32mOK Saved {frame_path.name}\033[0m")
                background_count += 1

            except Exception as e:
                import traceback
                print(f"    FAILED: {e}")
                traceback.print_exc()

        print(f"\n\033[32mOK Extracted {background_count} backgrounds\033[0m")

    def generate_t3s_file(self, sprite_dir: Path, sprite_name: str, png_files: List[str]):
        """Generate a .t3s file for a sprite
        
        Args:
            sprite_dir: Directory containing the sprite PNGs
            sprite_name: Name of the sprite
            png_files: List of PNG filenames (in order)
        """
        t3s_path = sprite_dir / f"{sprite_name}.t3s"
        
        # Generate .t3s format
        # Format: -f <format> -z <compression>
        # Then list of PNG files

        lines = [
            "-f etc1a4",
            "-z auto",
        ]

        if len(png_files) > 1:
            lines.insert(0, "--atlas")  # Use atlas mode if multiple frames
        
        for png_file in png_files:
            lines.append(png_file)
        
        # Write .t3s file
        with open(t3s_path, 'w') as f:
            f.write('\n'.join(lines) + '\n')
        
        print(f"    \033[32mOK Generated {t3s_path.name}\033[0m")

    def run(self):
        """Run the complete extraction process"""
        try:
            self.load_data_win()
            self.extract_texture_pages()
            self.extract_sprites()
            self.extract_backgrounds()
            self.extract_audio()
            print(f"\n\033[32mOK Complete! Sprites saved to {self.output_dir}/\033[0m")
        except Exception as e:
            print(f"\033[31mFAILED: {e}\033[0m")
            import traceback
            traceback.print_exc()
            sys.exit(1)


def main():
    if len(sys.argv) < 2:
        print("Usage: python processor.py <path_to_data.win> [output_dir]")
        print("\nExtracts sprites and textures from GameMaker data.win files for undertale 3ds")
        print("Example: python processor.py data.win gfx/")
        sys.exit(1)
    
    data_win_path = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "gfx"
    
    if not Path(data_win_path).exists():
        print(f"Error: {data_win_path} not found")
        sys.exit(1)
    
    extractor = TextureExtractor(data_win_path, output_dir)
    extractor.run()


if __name__ == "__main__":
    main()
