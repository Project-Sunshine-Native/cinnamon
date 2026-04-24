"""Chunk parsers for data.win file"""

import math
from typing import List, Optional, Tuple, Callable, Any
from .binary_reader import BinaryReader
from .structures import *


OBJT_EVENT_TYPE_COUNT = 12


def read_string_ptr(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Optional[str]:
    """Read absolute file offset, resolve to STRG buffer, return string"""
    offset = reader.read_uint32()
    if offset == 0:
        return None
    strg_offset = offset - strg_buffer_base
    if strg_offset < 0 or strg_offset >= len(strg_buffer):
        return None
    # Find null terminator
    end = strg_buffer.find(b'\0', strg_offset)
    if end == -1:
        end = len(strg_buffer)
    return strg_buffer[strg_offset:end].decode('utf-8', errors='replace')


def read_pointer_table(reader: BinaryReader) -> Tuple[int, List[int]]:
    """Read pointer list header: count + absolute-offset pointers"""
    count = reader.read_uint32()
    if count == 0:
        return count, []
    ptrs = []
    for _ in range(count):
        ptrs.append(reader.read_uint32())
    return count, ptrs


def read_event_actions(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Tuple[int, List[EventAction]]:
    """Read PointerList of EventAction entries"""
    count, ptrs = read_pointer_table(reader)
    if count == 0:
        return 0, []

    actions = []
    for i in range(count):
        reader.seek(ptrs[i])
        action = EventAction()
        action.lib_id = reader.read_uint32()
        action.lib_action_id = reader.read_uint32()
        action.kind = reader.read_uint32()
        action.use_relative = reader.read_bool32()
        action.is_question = reader.read_bool32()
        action.use_apply_to = reader.read_bool32()
        action.exe_type = reader.read_uint32()
        action.action_name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        action.code_id = reader.read_int32()
        action.argument_count = reader.read_uint32()
        action.who = reader.read_int32()
        action.relative = reader.read_bool32()
        action.is_not = reader.read_bool32()
        actions.append(action)

    return count, actions


# ===[ PATH COMPUTATION ]===

def compute_path_internal(path: GamePath):
    """Compute internal representation for path following"""
    if not path.points:
        path.length = 0.0
        return

    internal_points = []

    if path.is_smooth:
        # Smooth curve computation
        if not path.is_closed:
            pt = path.points[0]
            internal_points.append(InternalPathPoint(x=pt.x, y=pt.y, speed=pt.speed, l=0.0))

        n = len(path.points) - 1 if path.is_closed else len(path.points) - 3

        for i in range(n + 1):
            p1 = path.points[i % len(path.points)]
            p2 = path.points[(i + 1) % len(path.points)]
            p3 = path.points[(i + 2) % len(path.points)]

            midpoint1_x = (p1.x + p2.x) / 2.0
            midpoint1_y = (p1.y + p2.y) / 2.0
            midpoint1_speed = (p1.speed + p2.speed) / 2.0

            midpoint2_x = (p2.x + p3.x) / 2.0
            midpoint2_y = (p2.y + p3.y) / 2.0
            midpoint2_speed = (p2.speed + p3.speed) / 2.0

            handle_piece(
                path.precision,
                midpoint1_x, midpoint1_y, midpoint1_speed,
                p2.x, p2.y, p2.speed,
                midpoint2_x, midpoint2_y, midpoint2_speed,
                internal_points
            )

        if not path.is_closed:
            pt = path.points[-1]
            internal_points.append(InternalPathPoint(x=pt.x, y=pt.y, speed=pt.speed, l=0.0))
        else:
            pt = internal_points[0]
            internal_points.append(InternalPathPoint(x=pt.x, y=pt.y, speed=pt.speed, l=0.0))
    else:
        # Linear path
        for pt in path.points:
            internal_points.append(InternalPathPoint(x=pt.x, y=pt.y, speed=pt.speed, l=0.0))
        if path.is_closed:
            pt = path.points[0]
            internal_points.append(InternalPathPoint(x=pt.x, y=pt.y, speed=pt.speed, l=0.0))

    # Compute length
    path.internal_points = internal_points
    path.length = 0.0

    if internal_points:
        internal_points[0].l = 0.0
        for i in range(1, len(internal_points)):
            dx = internal_points[i].x - internal_points[i - 1].x
            dy = internal_points[i].y - internal_points[i - 1].y
            path.length += math.sqrt(dx * dx + dy * dy)
            internal_points[i].l = path.length


def handle_piece(depth: int, x1: float, y1: float, s1: float, x2: float, y2: float, s2: float,
                 x3: float, y3: float, s3: float, points: List[InternalPathPoint]):
    """Recursive midpoint subdivision for smooth curves"""
    if depth == 0:
        return

    mx = (x1 + x2 + x2 + x3) / 4.0
    my = (y1 + y2 + y2 + y3) / 4.0
    ms = (s1 + s2 + s2 + s3) / 4.0

    if (x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1) > 16.0:
        handle_piece(depth - 1, x1, y1, s1, (x2 + x1) / 2.0, (y2 + y1) / 2.0, (s2 + s1) / 2.0, mx, my, ms, points)

    points.append(InternalPathPoint(x=mx, y=my, speed=ms, l=0.0))

    if (x2 - x3) * (x2 - x3) + (y2 - y3) * (y2 - y3) > 16.0:
        handle_piece(depth - 1, mx, my, ms, (x3 + x2) / 2.0, (y3 + y2) / 2.0, (s3 + s2) / 2.0, x3, y3, s3, points)


# ===[ CHUNK PARSERS ]===

def parse_gen8(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Gen8:
    g = Gen8()
    g.is_debugger_disabled = reader.read_uint8() != 0
    g.bytecode_version = reader.read_uint8()
    reader.skip(2)  # padding
    g.file_name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
    g.config = read_string_ptr(reader, strg_buffer, strg_buffer_base)
    g.last_obj = reader.read_uint32()
    g.last_tile = reader.read_uint32()
    g.game_id = reader.read_uint32()
    g.direct_play_guid = reader.read_bytes(16)
    g.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
    g.major = reader.read_uint32()
    g.minor = reader.read_uint32()
    g.release = reader.read_uint32()
    g.build = reader.read_uint32()
    g.default_window_width = reader.read_uint32()
    g.default_window_height = reader.read_uint32()
    g.info = reader.read_uint32()
    g.license_crc32 = reader.read_uint32()
    g.license_md5 = reader.read_bytes(16)
    g.timestamp = reader.read_uint64()
    g.display_name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
    g.active_targets = reader.read_uint64()
    g.function_classifications = reader.read_uint64()
    g.steam_app_id = reader.read_int32()
    g.debugger_port = reader.read_uint32()

    # Room order SimpleList
    room_order_count = reader.read_uint32()
    for _ in range(room_order_count):
        g.room_order.append(reader.read_int32())

    return g


def parse_optn(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Optn:
    o = Optn()

    marker = reader.read_int32()
    if marker != -2147483648:  # 0x80000000
        raise ValueError(f"OPTN: expected marker 0x80000000, got {hex(marker & 0xFFFFFFFF)}")

    shader_ext_version = reader.read_int32()

    o.info = reader.read_uint64()
    o.scale = reader.read_int32()
    o.window_color = reader.read_uint32()
    o.color_depth = reader.read_uint32()
    o.resolution = reader.read_uint32()
    o.frequency = reader.read_uint32()
    o.vertex_sync = reader.read_uint32()
    o.priority = reader.read_uint32()
    o.back_image = reader.read_uint32()
    o.front_image = reader.read_uint32()
    o.load_image = reader.read_uint32()
    o.load_alpha = reader.read_uint32()

    constant_count = reader.read_uint32()
    for _ in range(constant_count):
        const = OptnConstant()
        const.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        const.value = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        o.constants.append(const)

    return o


def parse_lang(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Lang:
    l = Lang()
    l.unknown1 = reader.read_uint32()
    language_count = reader.read_uint32()
    entry_count = reader.read_uint32()

    # Entry IDs
    for _ in range(entry_count):
        l.entry_ids.append(read_string_ptr(reader, strg_buffer, strg_buffer_base))

    # Languages
    for _ in range(language_count):
        lang = Language()
        lang.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        lang.region = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        for _ in range(entry_count):
            lang.entries.append(read_string_ptr(reader, strg_buffer, strg_buffer_base))
        l.languages.append(lang)

    return l


def parse_extn(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Extn:
    e = Extn()

    ext_count, ext_ptrs = read_pointer_table(reader)

    for i in range(ext_count):
        reader.seek(ext_ptrs[i])
        ext = Extension()
        ext.folder_name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        ext.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        ext.class_name = read_string_ptr(reader, strg_buffer, strg_buffer_base)

        # Files PointerList
        file_count, file_ptrs = read_pointer_table(reader)

        for j in range(file_count):
            reader.seek(file_ptrs[j])
            file = ExtensionFile()
            file.filename = read_string_ptr(reader, strg_buffer, strg_buffer_base)
            file.cleanup_script = read_string_ptr(reader, strg_buffer, strg_buffer_base)
            file.init_script = read_string_ptr(reader, strg_buffer, strg_buffer_base)
            file.kind = reader.read_uint32()

            # Functions PointerList
            func_count, func_ptrs = read_pointer_table(reader)

            for k in range(func_count):
                reader.seek(func_ptrs[k])
                func = ExtensionFunction()
                func.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
                func.function_id = reader.read_uint32()
                func.kind = reader.read_uint32()
                func.ret_type = reader.read_uint32()
                func.ext_name = read_string_ptr(reader, strg_buffer, strg_buffer_base)

                # Arguments SimpleList
                arg_count = reader.read_uint32()
                for _ in range(arg_count):
                    func.arguments.append(reader.read_uint32())

                file.functions.append(func)

            ext.files.append(file)

        e.extensions.append(ext)

    return e


def parse_sond(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Sond:
    s = Sond()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        reader.seek(ptrs[i])
        snd = Sound()
        snd.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        snd.flags = reader.read_uint32()
        snd.sound_type = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        snd.file = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        snd.effects = reader.read_uint32()
        snd.volume = reader.read_float32()
        snd.pitch = reader.read_float32()

        if (snd.flags & 0x64) == 0x64:
            snd.audio_group = reader.read_int32()
        else:
            preload = reader.read_int32()
            snd.audio_group = 0

        snd.audio_file = reader.read_int32()
        s.sounds.append(snd)

    return s


def parse_agrp(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Agrp:
    a = Agrp()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        reader.seek(ptrs[i])
        ag = AudioGroup()
        ag.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        a.audio_groups.append(ag)

    return a


def parse_sprt(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int, 
               skip_loading_precise_masks: bool = False) -> Sprt:
    s = Sprt()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        reader.seek(ptrs[i])
        spr = Sprite()
        spr.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        spr.width = reader.read_uint32()
        spr.height = reader.read_uint32()
        spr.margin_left = reader.read_int32()
        spr.margin_right = reader.read_int32()
        spr.margin_bottom = reader.read_int32()
        spr.margin_top = reader.read_int32()
        spr.transparent = reader.read_bool32()
        spr.smooth = reader.read_bool32()
        spr.preload = reader.read_bool32()
        spr.bbox_mode = reader.read_uint32()
        spr.sep_masks = reader.read_uint32()
        spr.origin_x = reader.read_int32()
        spr.origin_y = reader.read_int32()

        # Check for special type sprite
        check = reader.read_int32()
        if check == -1:
            raise ValueError(f"SPRT: GMS2 format sprites not supported: '{spr.name}'")

        # Texture offsets
        spr.texture_offsets = []
        for _ in range(check):
            spr.texture_offsets.append(reader.read_uint32())

        # Collision mask data
        mask_data_count = reader.read_uint32()
        if mask_data_count > 0 and spr.width > 0 and spr.height > 0:
            bytes_per_row = (spr.width + 7) // 8
            bytes_per_mask = bytes_per_row * spr.height

            if spr.sep_masks == 1 or not skip_loading_precise_masks:
                for _ in range(mask_data_count):
                    mask_data = reader.read_bytes(bytes_per_mask)
                    spr.masks.append(mask_data)
                    # Skip padding to 4-byte alignment
                    remainder = bytes_per_mask % 4
                    if remainder != 0:
                        reader.skip(4 - remainder)
            else:
                for _ in range(mask_data_count):
                    reader.skip(bytes_per_mask)
                    remainder = bytes_per_mask % 4
                    if remainder != 0:
                        reader.skip(4 - remainder)

        s.sprites.append(spr)

    return s


def parse_bgnd(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Bgnd:
    b = Bgnd()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        reader.seek(ptrs[i])
        bg = Background()
        bg.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        bg.transparent = reader.read_bool32()
        bg.smooth = reader.read_bool32()
        bg.preload = reader.read_bool32()
        bg.texture_offset = reader.read_uint32()
        b.backgrounds.append(bg)

    return b


def parse_path(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> PathChunk:
    p = PathChunk()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        reader.seek(ptrs[i])
        path = GamePath()
        path.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        path.is_smooth = reader.read_bool32()
        path.is_closed = reader.read_bool32()
        path.precision = reader.read_uint32()

        # Points SimpleList
        point_count = reader.read_uint32()
        for _ in range(point_count):
            pt = PathPoint()
            pt.x = reader.read_float32()
            pt.y = reader.read_float32()
            pt.speed = reader.read_float32()
            path.points.append(pt)

        # Precompute internal representation
        compute_path_internal(path)
        p.paths.append(path)

    return p


def parse_scpt(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Scpt:
    s = Scpt()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        reader.seek(ptrs[i])
        script = Script()
        script.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        script.code_id = reader.read_int32()
        s.scripts.append(script)

    return s


def parse_glob(reader: BinaryReader) -> Glob:
    g = Glob()

    count = reader.read_uint32()
    for _ in range(count):
        g.code_ids.append(reader.read_int32())

    return g


def parse_shdr(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Shdr:
    s = Shdr()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        reader.seek(ptrs[i])
        sh = Shader()
        sh.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        sh.shader_type = reader.read_uint32() & 0x7FFFFFFF
        sh.glsl_es_vertex = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        sh.glsl_es_fragment = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        sh.glsl_vertex = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        sh.glsl_fragment = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        sh.hlsl9_vertex = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        sh.hlsl9_fragment = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        sh.hlsl11_vertex_offset = reader.read_uint32()
        sh.hlsl11_pixel_offset = reader.read_uint32()

        # Vertex attributes SimpleList
        va_count = reader.read_uint32()
        for _ in range(va_count):
            sh.vertex_attributes.append(read_string_ptr(reader, strg_buffer, strg_buffer_base))

        sh.version = reader.read_int32()
        sh.pssl_vertex_offset = reader.read_uint32()
        sh.pssl_vertex_len = reader.read_uint32()
        sh.pssl_pixel_offset = reader.read_uint32()
        sh.pssl_pixel_len = reader.read_uint32()
        sh.cg_vita_vertex_offset = reader.read_uint32()
        sh.cg_vita_vertex_len = reader.read_uint32()
        sh.cg_vita_pixel_offset = reader.read_uint32()
        sh.cg_vita_pixel_len = reader.read_uint32()

        if sh.version >= 2:
            sh.cg_ps3_vertex_offset = reader.read_uint32()
            sh.cg_ps3_vertex_len = reader.read_uint32()
            sh.cg_ps3_pixel_offset = reader.read_uint32()
            sh.cg_ps3_pixel_len = reader.read_uint32()

        s.shaders.append(sh)

    return s


def parse_font(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> FontChunk:
    f = FontChunk()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        reader.seek(ptrs[i])
        font = Font()
        font.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        font.display_name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        font.em_size = reader.read_uint32()
        font.bold = reader.read_bool32()
        font.italic = reader.read_bool32()
        font.range_start = reader.read_uint16()
        font.charset = reader.read_uint8()
        font.anti_aliasing = reader.read_uint8()
        font.range_end = reader.read_uint32()
        font.texture_offset = reader.read_uint32()
        font.scale_x = reader.read_float32()
        font.scale_y = reader.read_float32()

        # Glyphs PointerList
        glyph_count, glyph_ptrs = read_pointer_table(reader)

        for j in range(glyph_count):
            reader.seek(glyph_ptrs[j])
            glyph = FontGlyph()
            glyph.character = reader.read_uint16()
            glyph.source_x = reader.read_uint16()
            glyph.source_y = reader.read_uint16()
            glyph.source_width = reader.read_uint16()
            glyph.source_height = reader.read_uint16()
            glyph.shift = reader.read_int16()
            glyph.offset = reader.read_int16()

            # Kerning SimpleListShort
            kerning_count = reader.read_uint16()
            for k in range(kerning_count):
                kern = KerningPair()
                kern.character = reader.read_int16()
                kern.shift_modifier = reader.read_int16()
                glyph.kerning.append(kern)

            font.glyphs.append(glyph)

        f.fonts.append(font)

    return f


def parse_tmln(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Tmln:
    t = Tmln()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        reader.seek(ptrs[i])
        tl = Timeline()
        tl.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        moment_count = reader.read_uint32()

        if moment_count > 0:
            # Pass 1: Read step + event pointer pairs
            event_ptrs = []
            for _ in range(moment_count):
                moment = TimelineMoment()
                moment.step = reader.read_uint32()
                event_ptrs.append(reader.read_uint32())
                tl.moments.append(moment)

            # Pass 2: Parse event action lists
            for j in range(moment_count):
                reader.seek(event_ptrs[j])
                action_count, actions = read_event_actions(reader, strg_buffer, strg_buffer_base)
                tl.moments[j].actions = actions

        t.timelines.append(tl)

    return t


def parse_objt(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Objt:
    o = Objt()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        reader.seek(ptrs[i])
        obj = GameObject()
        obj.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        obj.sprite_id = reader.read_int32()
        obj.visible = reader.read_bool32()
        obj.solid = reader.read_bool32()
        obj.depth = reader.read_int32()
        obj.persistent = reader.read_bool32()
        obj.parent_id = reader.read_int32()
        obj.texture_mask_id = reader.read_int32()
        obj.uses_physics = reader.read_bool32()
        obj.is_sensor = reader.read_bool32()
        obj.collision_shape = reader.read_uint32()
        obj.density = reader.read_float32()
        obj.restitution = reader.read_float32()
        obj.group = reader.read_uint32()
        obj.linear_damping = reader.read_float32()
        obj.angular_damping = reader.read_float32()
        phys_vert_count = reader.read_int32()
        obj.friction = reader.read_float32()
        obj.awake = reader.read_bool32()
        obj.kinematic = reader.read_bool32()

        # Physics vertices
        for _ in range(phys_vert_count):
            pv = PhysicsVertex()
            pv.x = reader.read_float32()
            pv.y = reader.read_float32()
            obj.physics_vertices.append(pv)

        # Events: PointerList of PointerList
        event_type_count, event_type_ptrs = read_pointer_table(reader)

        # Initialize event lists
        for et in range(OBJT_EVENT_TYPE_COUNT):
            obj.event_lists.append(ObjectEventList())

        for event_type in range(event_type_count):
            if event_type >= OBJT_EVENT_TYPE_COUNT:
                break

            reader.seek(event_type_ptrs[event_type])

            # Inner pointer list
            event_count, event_ptrs = read_pointer_table(reader)

            for j in range(event_count):
                reader.seek(event_ptrs[j])
                event = ObjectEvent()
                event.event_subtype = reader.read_uint32()
                action_count, actions = read_event_actions(reader, strg_buffer, strg_buffer_base)
                event.actions = actions
                obj.event_lists[event_type].events.append(event)

        o.objects.append(obj)

    return o


def parse_room(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> RoomChunk:
    rc = RoomChunk()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        reader.seek(ptrs[i])
        room = Room()
        room.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        room.caption = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        room.width = reader.read_uint32()
        room.height = reader.read_uint32()
        room.speed = reader.read_uint32()
        room.persistent = reader.read_bool32()
        room.background_color = reader.read_uint32()
        room.draw_background_color = reader.read_bool32()
        room.creation_code_id = reader.read_int32()
        room.flags = reader.read_uint32()
        backgrounds_ptr = reader.read_uint32()
        views_ptr = reader.read_uint32()
        game_objects_ptr = reader.read_uint32()
        tiles_ptr = reader.read_uint32()
        room.world = reader.read_bool32()
        room.top = reader.read_uint32()
        room.left = reader.read_uint32()
        room.right = reader.read_uint32()
        room.bottom = reader.read_uint32()
        room.gravity_x = reader.read_float32()
        room.gravity_y = reader.read_float32()
        room.meters_per_pixel = reader.read_float32()

        # Backgrounds  (always 8)
        reader.seek(backgrounds_ptr)
        bg_count, bg_ptrs = read_pointer_table(reader)
        for j in range(min(bg_count, 8)):
            reader.seek(bg_ptrs[j])
            bg = RoomBackground()
            bg.enabled = reader.read_bool32()
            bg.foreground = reader.read_bool32()
            bg.background_definition = reader.read_int32()
            bg.x = reader.read_int32()
            bg.y = reader.read_int32()
            bg.tile_x = reader.read_int32()
            bg.tile_y = reader.read_int32()
            bg.speed_x = reader.read_int32()
            bg.speed_y = reader.read_int32()
            bg.stretch = reader.read_bool32()
            room.backgrounds[j] = bg

        # Views (always 8)
        reader.seek(views_ptr)
        view_count, view_ptrs = read_pointer_table(reader)
        for j in range(min(view_count, 8)):
            reader.seek(view_ptrs[j])
            view = RoomView()
            view.enabled = reader.read_bool32()
            view.view_x = reader.read_int32()
            view.view_y = reader.read_int32()
            view.view_width = reader.read_int32()
            view.view_height = reader.read_int32()
            view.port_x = reader.read_int32()
            view.port_y = reader.read_int32()
            view.port_width = reader.read_int32()
            view.port_height = reader.read_int32()
            view.border_x = reader.read_uint32()
            view.border_y = reader.read_uint32()
            view.speed_x = reader.read_int32()
            view.speed_y = reader.read_int32()
            view.object_id = reader.read_int32()
            room.views[j] = view

        # Game Objects
        reader.seek(game_objects_ptr)
        obj_count, obj_ptrs = read_pointer_table(reader)
        for j in range(obj_count):
            reader.seek(obj_ptrs[j])
            go = RoomGameObject()
            go.x = reader.read_int32()
            go.y = reader.read_int32()
            go.object_definition = reader.read_int32()
            go.instance_id = reader.read_uint32()
            go.creation_code = reader.read_int32()
            go.scale_x = reader.read_float32()
            go.scale_y = reader.read_float32()
            go.color = reader.read_uint32()
            go.rotation = reader.read_float32()
            go.pre_create_code = reader.read_int32()
            room.game_objects.append(go)

        # Tiles
        reader.seek(tiles_ptr)
        tile_count, tile_ptrs = read_pointer_table(reader)
        for j in range(tile_count):
            reader.seek(tile_ptrs[j])
            tile = RoomTile()
            tile.x = reader.read_int32()
            tile.y = reader.read_int32()
            tile.background_definition = reader.read_int32()
            tile.source_x = reader.read_int32()
            tile.source_y = reader.read_int32()
            tile.width = reader.read_uint32()
            tile.height = reader.read_uint32()
            tile.tile_depth = reader.read_int32()
            tile.instance_id = reader.read_uint32()
            tile.scale_x = reader.read_float32()
            tile.scale_y = reader.read_float32()
            tile.color = reader.read_uint32()
            room.tiles.append(tile)

        rc.rooms.append(room)

    return rc


def parse_tpag(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Tpag:
    t = Tpag()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        reader.seek(ptrs[i])
        item = TexturePageItem()
        item.source_x = reader.read_uint16()
        item.source_y = reader.read_uint16()
        item.source_width = reader.read_uint16()
        item.source_height = reader.read_uint16()
        item.target_x = reader.read_uint16()
        item.target_y = reader.read_uint16()
        item.target_width = reader.read_uint16()
        item.target_height = reader.read_uint16()
        item.bounding_width = reader.read_uint16()
        item.bounding_height = reader.read_uint16()
        item.texture_page_id = reader.read_int16()
        t.items.append(item)

        # Build offset map
        t.offset_map[ptrs[i]] = i

    return t


def parse_code(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int, 
               chunk_length: int, chunk_data_start: int) -> Code:
    c = Code()

    if chunk_length == 0:
        # YYC-compiled, no bytecode
        return c

    code_count, code_ptrs = read_pointer_table(reader)

    for i in range(code_count):
        reader.seek(code_ptrs[i])
        entry = CodeEntry()
        entry.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        entry.length = reader.read_uint32()
        entry.locals_count = reader.read_uint16()
        entry.arguments_count = reader.read_uint16()

        # bytecodeRelAddr is relative to this field's position
        rel_addr_field_pos = reader.tell()
        bytecode_rel_addr = reader.read_int32()
        entry.bytecode_absolute_offset = rel_addr_field_pos + bytecode_rel_addr

        entry.offset = reader.read_uint32()
        c.entries.append(entry)

    # Compute bytecode blob range
    if c.entries:
        blob_start = c.entries[0].bytecode_absolute_offset
        for entry in c.entries:
            if entry.bytecode_absolute_offset < blob_start:
                blob_start = entry.bytecode_absolute_offset

        chunk_end = chunk_data_start + chunk_length
        blob_size = chunk_end - blob_start
        c.bytecode_buffer_base = blob_start
        c.bytecode_buffer = reader.read_bytes_at(blob_start, blob_size)

    return c


def parse_vari(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int, chunk_length: int) -> Vari:
    v = Vari()

    v.var_count1 = reader.read_uint32()
    v.var_count2 = reader.read_uint32()
    v.max_local_var_count = reader.read_uint32()

    # Variable entries are packed sequentially
    var_count = (chunk_length - 12) // 20

    for _ in range(var_count):
        var = Variable()
        var.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        var.instance_type = reader.read_int32()
        var.var_id = reader.read_int32()
        var.occurrences = reader.read_uint32()
        var.first_address = reader.read_uint32()
        v.variables.append(var)

    return v


def parse_func(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Func:
    f = Func()

    # Part 1: Functions SimpleList
    function_count = reader.read_uint32()
    for _ in range(function_count):
        func = Function()
        func.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
        func.occurrences = reader.read_uint32()
        func.first_address = reader.read_uint32()
        f.functions.append(func)

    # Part 2: Code Locals SimpleList
    code_locals_count = reader.read_uint32()
    for _ in range(code_locals_count):
        cl = CodeLocals()
        local_var_count = reader.read_uint32()
        cl.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)

        for _ in range(local_var_count):
            lv = LocalVar()
            lv.var_index = reader.read_uint32()
            lv.name = read_string_ptr(reader, strg_buffer, strg_buffer_base)
            cl.locals.append(lv)

        f.code_locals.append(cl)

    return f


def parse_strg(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Strg:
    s = Strg()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        # Pointer table points to length prefix
        # Actual string content starts 4 bytes after
        string_offset = ptrs[i] + 4 - strg_buffer_base
        if 0 <= string_offset < len(strg_buffer):
            end = strg_buffer.find(b'\0', string_offset)
            if end == -1:
                end = len(strg_buffer)
            s.strings.append(strg_buffer[string_offset:end].decode('utf-8', errors='replace'))
        else:
            s.strings.append(None)

    return s


def parse_txtr(reader: BinaryReader, file_size: int) -> Txtr:
    t = Txtr()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        texture = Texture()
        reader.seek(ptrs[i])
        texture.scaled = reader.read_uint32()
        texture.blob_offset = reader.read_uint32()
        texture.blob_data = None
        texture.loaded = False

        # Calculate blob size
        if texture.blob_offset == 0:
            texture.blob_size = 0
        else:
            next_offset = ptrs[i + 1] if i < count - 1 else 0

            if next_offset != 0 and next_offset > texture.blob_offset:
                texture.blob_size = next_offset - texture.blob_offset
            else:
                if file_size > texture.blob_offset:
                    texture.blob_size = file_size - texture.blob_offset
                    if texture.blob_size > 16 * 1024 * 1024:  # max 16MB
                        texture.blob_size = 16 * 1024 * 1024
                else:
                    texture.blob_size = 0

        t.textures.append(texture)

    return t


def parse_audo(reader: BinaryReader, strg_buffer: bytes, strg_buffer_base: int) -> Audo:
    a = Audo()

    count, ptrs = read_pointer_table(reader)

    for i in range(count):
        reader.seek(ptrs[i])
        entry = AudioEntry()
        entry.data_size = reader.read_uint32()
        entry.data_offset = reader.tell()
        a.entries.append(entry)

    return a
