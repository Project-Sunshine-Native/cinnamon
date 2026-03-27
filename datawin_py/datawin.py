"""Main DataWin class for loading and accessing game data"""

from dataclasses import dataclass, field
from typing import Optional, Callable, Any
from .binary_reader import BinaryReader
from .structures import *
from . import parsers


@dataclass
class DataWinParserOptions:
    """Options for controlling which chunks to parse"""
    parse_gen8: bool = True
    parse_optn: bool = True
    parse_lang: bool = True
    parse_extn: bool = True
    parse_sond: bool = True
    parse_agrp: bool = True
    parse_sprt: bool = True
    parse_bgnd: bool = True
    parse_path: bool = True
    parse_scpt: bool = True
    parse_glob: bool = True
    parse_shdr: bool = True
    parse_font: bool = True
    parse_tmln: bool = True
    parse_objt: bool = True
    parse_room: bool = True
    parse_tpag: bool = True
    parse_code: bool = True
    parse_vari: bool = True
    parse_func: bool = True
    parse_strg: bool = True
    parse_txtr: bool = True
    parse_audo: bool = True
    skip_loading_precise_masks_for_non_precise_sprites: bool = False
    progress_callback: Optional[Callable[[str, int, int, Any], None]] = None


class DataWin:
    """Parsed GameMaker data.win file"""

    def __init__(self):
        self.gen8 = Gen8()
        self.optn = Optn()
        self.lang = Lang()
        self.extn = Extn()
        self.sond = Sond()
        self.agrp = Agrp()
        self.sprt = Sprt()
        self.bgnd = Bgnd()
        self.path = PathChunk()
        self.scpt = Scpt()
        self.glob = Glob()
        self.shdr = Shdr()
        self.font = FontChunk()
        self.tmln = Tmln()
        self.objt = Objt()
        self.room = RoomChunk()
        self.tpag = Tpag()
        self.code = Code()
        self.vari = Vari()
        self.func = Func()
        self.strg = Strg()
        self.txtr = Txtr()
        self.audo = Audo()

        self.strg_buffer = b''
        self.strg_buffer_base = 0
        self.file = None
        self.file_size = 0

    @staticmethod
    def load(file_path: str, options: Optional[DataWinParserOptions] = None) -> 'DataWin':
        """Load and parse a data.win file"""
        if options is None:
            options = DataWinParserOptions()

        dw = DataWin()

        try:
            with open(file_path, 'rb') as file:
                file.seek(0, 2)
                dw.file_size = file.tell()
                file.seek(0)

                if dw.file_size <= 0:
                    raise ValueError(f"Invalid file size: {dw.file_size}")

                reader = BinaryReader(file, dw.file_size)

                # Validate FORM header
                form_magic = reader.read_bytes(4)
                if form_magic != b'FORM':
                    raise ValueError(f"Invalid file: expected FORM magic, got '{form_magic.decode('utf-8', errors='replace')}'")

                form_length = reader.read_uint32()

                # Pass 1: Find and load STRG only
                if options.parse_strg:
                    reader.seek(8)
                    total_chunks = 0

                    while reader.tell() < dw.file_size:
                        if reader.tell() + 8 > dw.file_size:
                            break

                        chunk_name = reader.read_bytes(4).decode('utf-8', errors='replace')
                        chunk_length = reader.read_uint32()
                        chunk_data_start = reader.tell()

                        if chunk_name == 'STRG':
                            dw.strg_buffer_base = chunk_data_start
                            dw.strg_buffer = reader.read_bytes(chunk_length)
                        else:
                            reader.skip(chunk_length)

                        total_chunks += 1

                # Pass 2: Parse all chunks
                reader.seek(8)
                chunk_index = 0

                while reader.tell() < dw.file_size:
                    if reader.tell() + 8 > dw.file_size:
                        break

                    chunk_name = reader.read_bytes(4).decode('utf-8', errors='replace')
                    chunk_length = reader.read_uint32()
                    chunk_data_start = reader.tell()
                    chunk_end = chunk_data_start + chunk_length

                    if options.progress_callback:
                        options.progress_callback(chunk_name, chunk_index, total_chunks if options.parse_strg else 0)

                    # Parse chunks based on options
                    if chunk_name == 'GEN8' and options.parse_gen8:
                        dw.gen8 = parsers.parse_gen8(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'OPTN' and options.parse_optn:
                        dw.optn = parsers.parse_optn(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'LANG' and options.parse_lang:
                        dw.lang = parsers.parse_lang(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'EXTN' and options.parse_extn:
                        dw.extn = parsers.parse_extn(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'SOND' and options.parse_sond:
                        dw.sond = parsers.parse_sond(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'AGRP' and options.parse_agrp:
                        dw.agrp = parsers.parse_agrp(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'SPRT' and options.parse_sprt:
                        dw.sprt = parsers.parse_sprt(
                            reader,
                            dw.strg_buffer,
                            dw.strg_buffer_base,
                            options.skip_loading_precise_masks_for_non_precise_sprites
                        )
                    elif chunk_name == 'BGND' and options.parse_bgnd:
                        dw.bgnd = parsers.parse_bgnd(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'PATH' and options.parse_path:
                        dw.path = parsers.parse_path(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'SCPT' and options.parse_scpt:
                        dw.scpt = parsers.parse_scpt(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'GLOB' and options.parse_glob:
                        dw.glob = parsers.parse_glob(reader)
                    elif chunk_name == 'SHDR' and options.parse_shdr:
                        dw.shdr = parsers.parse_shdr(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'FONT' and options.parse_font:
                        dw.font = parsers.parse_font(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'TMLN' and options.parse_tmln:
                        dw.tmln = parsers.parse_tmln(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'OBJT' and options.parse_objt:
                        dw.objt = parsers.parse_objt(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'ROOM' and options.parse_room:
                        dw.room = parsers.parse_room(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'TPAG' and options.parse_tpag:
                        dw.tpag = parsers.parse_tpag(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'CODE' and options.parse_code:
                        dw.code = parsers.parse_code(reader, dw.strg_buffer, dw.strg_buffer_base, chunk_length, chunk_data_start)
                    elif chunk_name == 'VARI' and options.parse_vari:
                        dw.vari = parsers.parse_vari(reader, dw.strg_buffer, dw.strg_buffer_base, chunk_length)
                    elif chunk_name == 'FUNC' and options.parse_func:
                        dw.func = parsers.parse_func(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'STRG' and options.parse_strg:
                        dw.strg = parsers.parse_strg(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'TXTR' and options.parse_txtr:
                        dw.txtr = parsers.parse_txtr(reader, dw.file_size)
                    elif chunk_name == 'AUDO' and options.parse_audo:
                        dw.audo = parsers.parse_audo(reader, dw.strg_buffer, dw.strg_buffer_base)
                    elif chunk_name == 'DAFL':
                        # Empty chunk
                        pass
                    else:
                        print(f"Unknown chunk: {chunk_name} (length {chunk_length} at offset {chunk_data_start - 8:08X})")

                    # Seek to chunk end
                    reader.seek(chunk_end)
                    chunk_index += 1

        except Exception as e:
            raise RuntimeError(f"Failed to load data.win: {e}") from e

        return dw

    def get_string(self, index: int) -> Optional[str]:
        """Get a string by index from STRG chunk"""
        if 0 <= index < len(self.strg.strings):
            return self.strg.strings[index]
        return None

    def get_sprite(self, index: int) -> Optional[Sprite]:
        """Get a sprite by index"""
        if 0 <= index < len(self.sprt.sprites):
            return self.sprt.sprites[index]
        return None

    def get_room(self, index: int) -> Optional[Room]:
        """Get a room by index"""
        if 0 <= index < len(self.room.rooms):
            return self.room.rooms[index]
        return None

    def get_object(self, index: int) -> Optional[GameObject]:
        """Get a game object by index"""
        if 0 <= index < len(self.objt.objects):
            return self.objt.objects[index]
        return None

    def get_room_by_name(self, name: str) -> Optional[Room]:
        """Get a room by name"""
        for room in self.room.rooms:
            if room.name == name:
                return room
        return None

    def get_sprite_by_name(self, name: str) -> Optional[Sprite]:
        """Get a sprite by name"""
        for sprite in self.sprt.sprites:
            if sprite.name == name:
                return sprite
        return None

    def get_object_by_name(self, name: str) -> Optional[GameObject]:
        """Get a game object by name"""
        for obj in self.objt.objects:
            if obj.name == name:
                return obj
        return None

    def __repr__(self) -> str:
        return (
            f"<DataWin game={self.gen8.name or '?'} "
            f"version={self.gen8.major}.{self.gen8.minor}.{self.gen8.release}.{self.gen8.build} "
            f"rooms={len(self.room.rooms)} sprites={len(self.sprt.sprites)} objects={len(self.objt.objects)}>"
        )
