"""Data structures for GameMaker data.win chunks"""

from dataclasses import dataclass, field
from typing import List, Dict, Tuple, Optional


# ===[ SIMPLE STRUCTURES ]===

@dataclass
class PathPoint:
    x: float = 0.0
    y: float = 0.0
    speed: float = 0.0


@dataclass
class InternalPathPoint:
    x: float = 0.0
    y: float = 0.0
    speed: float = 0.0
    l: float = 0.0  # cumulative length


@dataclass
class PhysicsVertex:
    x: float = 0.0
    y: float = 0.0


@dataclass
class KerningPair:
    character: int = 0
    shift_modifier: int = 0


# ===[ GEN8 - General Game Info ]===

@dataclass
class Gen8:
    is_debugger_disabled: bool = False
    bytecode_version: int = 0
    file_name: Optional[str] = None
    config: Optional[str] = None
    last_obj: int = 0
    last_tile: int = 0
    game_id: int = 0
    direct_play_guid: bytes = b''
    name: Optional[str] = None
    major: int = 0
    minor: int = 0
    release: int = 0
    build: int = 0
    default_window_width: int = 0
    default_window_height: int = 0
    info: int = 0
    license_crc32: int = 0
    license_md5: bytes = b''
    timestamp: int = 0
    display_name: Optional[str] = None
    active_targets: int = 0
    function_classifications: int = 0
    steam_app_id: int = 0
    debugger_port: int = 0
    room_order: List[int] = field(default_factory=list)


# ===[ OPTN - Options ]===

@dataclass
class OptnConstant:
    name: Optional[str] = None
    value: Optional[str] = None


@dataclass
class Optn:
    info: int = 0
    scale: int = 0
    window_color: int = 0
    color_depth: int = 0
    resolution: int = 0
    frequency: int = 0
    vertex_sync: int = 0
    priority: int = 0
    back_image: int = 0
    front_image: int = 0
    load_image: int = 0
    load_alpha: int = 0
    constants: List[OptnConstant] = field(default_factory=list)


# ===[ LANG - Languages ]===

@dataclass
class Language:
    name: Optional[str] = None
    region: Optional[str] = None
    entries: List[Optional[str]] = field(default_factory=list)


@dataclass
class Lang:
    unknown1: int = 0
    entry_ids: List[Optional[str]] = field(default_factory=list)
    languages: List[Language] = field(default_factory=list)


# ===[ EXTN - Extensions ]===

@dataclass
class ExtensionFunction:
    name: Optional[str] = None
    function_id: int = 0
    kind: int = 0
    ret_type: int = 0
    ext_name: Optional[str] = None
    arguments: List[int] = field(default_factory=list)


@dataclass
class ExtensionFile:
    filename: Optional[str] = None
    cleanup_script: Optional[str] = None
    init_script: Optional[str] = None
    kind: int = 0
    functions: List[ExtensionFunction] = field(default_factory=list)


@dataclass
class Extension:
    folder_name: Optional[str] = None
    name: Optional[str] = None
    class_name: Optional[str] = None
    files: List[ExtensionFile] = field(default_factory=list)


@dataclass
class Extn:
    extensions: List[Extension] = field(default_factory=list)


# ===[ SOND - Sounds ]===

@dataclass
class Sound:
    name: Optional[str] = None
    flags: int = 0
    sound_type: Optional[str] = None
    file: Optional[str] = None
    effects: int = 0
    volume: float = 0.0
    pitch: float = 0.0
    audio_group: int = 0
    audio_file: int = 0


@dataclass
class Sond:
    sounds: List[Sound] = field(default_factory=list)


# ===[ AGRP - Audio Groups ]===

@dataclass
class AudioGroup:
    name: Optional[str] = None


@dataclass
class Agrp:
    audio_groups: List[AudioGroup] = field(default_factory=list)


# ===[ SPRT - Sprites ]===

@dataclass
class Sprite:
    name: Optional[str] = None
    width: int = 0
    height: int = 0
    margin_left: int = 0
    margin_right: int = 0
    margin_bottom: int = 0
    margin_top: int = 0
    transparent: bool = False
    smooth: bool = False
    preload: bool = False
    bbox_mode: int = 0
    sep_masks: int = 0
    origin_x: int = 0
    origin_y: int = 0
    texture_offsets: List[int] = field(default_factory=list)
    masks: List[Optional[bytes]] = field(default_factory=list)


@dataclass
class Sprt:
    sprites: List[Sprite] = field(default_factory=list)


# ===[ BGND - Backgrounds ]===

@dataclass
class Background:
    name: Optional[str] = None
    transparent: bool = False
    smooth: bool = False
    preload: bool = False
    texture_offset: int = 0


@dataclass
class Bgnd:
    backgrounds: List[Background] = field(default_factory=list)


# ===[ PATH - Paths ]===

@dataclass
class GamePath:
    name: Optional[str] = None
    is_smooth: bool = False
    is_closed: bool = False
    precision: int = 0
    points: List[PathPoint] = field(default_factory=list)
    internal_points: List[InternalPathPoint] = field(default_factory=list)
    length: float = 0.0

    def get_position(self, t: float) -> Tuple[float, float, float]:
        """Get interpolated position at t in [0, 1]"""
        if not self.internal_points:
            return (0.0, 0.0, 0.0)

        if len(self.internal_points) == 1 or self.length == 0.0 or t <= 0.0:
            pt = self.internal_points[0]
            return (pt.x, pt.y, pt.speed)

        if t >= 1.0:
            pt = self.internal_points[-1]
            return (pt.x, pt.y, pt.speed)

        # Find interval
        l = self.length * t
        pos = 0
        while pos < len(self.internal_points) - 2 and l >= self.internal_points[pos + 1].l:
            pos += 1

        node = self.internal_points[pos]
        next_node = self.internal_points[pos + 1]
        l_rem = l - node.l
        w = next_node.l - node.l

        if w != 0.0:
            x = node.x + l_rem * (next_node.x - node.x) / w
            y = node.y + l_rem * (next_node.y - node.y) / w
            speed = node.speed + l_rem * (next_node.speed - node.speed) / w
        else:
            x, y, speed = node.x, node.y, node.speed

        return (x, y, speed)


@dataclass
class PathChunk:
    paths: List[GamePath] = field(default_factory=list)


# ===[ SCPT - Scripts ]===

@dataclass
class Script:
    name: Optional[str] = None
    code_id: int = 0


@dataclass
class Scpt:
    scripts: List[Script] = field(default_factory=list)


# ===[ GLOB - Global Variables ]===

@dataclass
class Glob:
    code_ids: List[int] = field(default_factory=list)


# ===[ SHDR - Shaders ]===

@dataclass
class Shader:
    name: Optional[str] = None
    shader_type: int = 0
    glsl_es_vertex: Optional[str] = None
    glsl_es_fragment: Optional[str] = None
    glsl_vertex: Optional[str] = None
    glsl_fragment: Optional[str] = None
    hlsl9_vertex: Optional[str] = None
    hlsl9_fragment: Optional[str] = None
    hlsl11_vertex_offset: int = 0
    hlsl11_pixel_offset: int = 0
    vertex_attributes: List[Optional[str]] = field(default_factory=list)
    version: int = 0
    pssl_vertex_offset: int = 0
    pssl_vertex_len: int = 0
    pssl_pixel_offset: int = 0
    pssl_pixel_len: int = 0
    cg_vita_vertex_offset: int = 0
    cg_vita_vertex_len: int = 0
    cg_vita_pixel_offset: int = 0
    cg_vita_pixel_len: int = 0
    cg_ps3_vertex_offset: int = 0
    cg_ps3_vertex_len: int = 0
    cg_ps3_pixel_offset: int = 0
    cg_ps3_pixel_len: int = 0


@dataclass
class Shdr:
    shaders: List[Shader] = field(default_factory=list)


# ===[ FONT - Fonts ]===

@dataclass
class FontGlyph:
    character: int = 0
    source_x: int = 0
    source_y: int = 0
    source_width: int = 0
    source_height: int = 0
    shift: int = 0
    offset: int = 0
    kerning: List[KerningPair] = field(default_factory=list)


@dataclass
class Font:
    name: Optional[str] = None
    display_name: Optional[str] = None
    em_size: int = 0
    bold: bool = False
    italic: bool = False
    range_start: int = 0
    charset: int = 0
    anti_aliasing: int = 0
    range_end: int = 0
    texture_offset: int = 0
    scale_x: float = 0.0
    scale_y: float = 0.0
    glyphs: List[FontGlyph] = field(default_factory=list)


@dataclass
class FontChunk:
    fonts: List[Font] = field(default_factory=list)


# ===[ TMLN - Timelines ]===

@dataclass
class EventAction:
    lib_id: int = 0
    lib_action_id: int = 0
    kind: int = 0
    use_relative: bool = False
    is_question: bool = False
    use_apply_to: bool = False
    exe_type: int = 0
    action_name: Optional[str] = None
    code_id: int = 0
    argument_count: int = 0
    who: int = 0
    relative: bool = False
    is_not: bool = False


@dataclass
class TimelineMoment:
    step: int = 0
    actions: List[EventAction] = field(default_factory=list)


@dataclass
class Timeline:
    name: Optional[str] = None
    moments: List[TimelineMoment] = field(default_factory=list)


@dataclass
class Tmln:
    timelines: List[Timeline] = field(default_factory=list)


# ===[ OBJT - Objects ]===

@dataclass
class ObjectEvent:
    event_subtype: int = 0
    actions: List[EventAction] = field(default_factory=list)


@dataclass
class ObjectEventList:
    events: List[ObjectEvent] = field(default_factory=list)


@dataclass
class GameObject:
    name: Optional[str] = None
    sprite_id: int = 0
    visible: bool = False
    solid: bool = False
    depth: int = 0
    persistent: bool = False
    parent_id: int = 0
    texture_mask_id: int = 0
    uses_physics: bool = False
    is_sensor: bool = False
    collision_shape: int = 0
    density: float = 0.0
    restitution: float = 0.0
    group: int = 0
    linear_damping: float = 0.0
    angular_damping: float = 0.0
    friction: float = 0.0
    awake: bool = False
    kinematic: bool = False
    physics_vertices: List[PhysicsVertex] = field(default_factory=list)
    event_lists: List[ObjectEventList] = field(default_factory=list)


@dataclass
class Objt:
    objects: List[GameObject] = field(default_factory=list)


# ===[ ROOM - Rooms ]===

@dataclass
class RoomBackground:
    enabled: bool = False
    foreground: bool = False
    background_definition: int = 0
    x: int = 0
    y: int = 0
    tile_x: int = 0
    tile_y: int = 0
    speed_x: int = 0
    speed_y: int = 0
    stretch: bool = False


@dataclass
class RoomView:
    enabled: bool = False
    view_x: int = 0
    view_y: int = 0
    view_width: int = 0
    view_height: int = 0
    port_x: int = 0
    port_y: int = 0
    port_width: int = 0
    port_height: int = 0
    border_x: int = 0
    border_y: int = 0
    speed_x: int = 0
    speed_y: int = 0
    object_id: int = 0


@dataclass
class RoomGameObject:
    x: int = 0
    y: int = 0
    object_definition: int = 0
    instance_id: int = 0
    creation_code: int = 0
    scale_x: float = 0.0
    scale_y: float = 0.0
    color: int = 0
    rotation: float = 0.0
    pre_create_code: int = 0


@dataclass
class RoomTile:
    x: int = 0
    y: int = 0
    background_definition: int = 0
    source_x: int = 0
    source_y: int = 0
    width: int = 0
    height: int = 0
    tile_depth: int = 0
    instance_id: int = 0
    scale_x: float = 0.0
    scale_y: float = 0.0
    color: int = 0


@dataclass
class Room:
    name: Optional[str] = None
    caption: Optional[str] = None
    width: int = 0
    height: int = 0
    speed: int = 0
    persistent: bool = False
    background_color: int = 0
    draw_background_color: bool = False
    creation_code_id: int = 0
    flags: int = 0
    world: bool = False
    top: int = 0
    left: int = 0
    right: int = 0
    bottom: int = 0
    gravity_x: float = 0.0
    gravity_y: float = 0.0
    meters_per_pixel: float = 0.0
    backgrounds: List[RoomBackground] = field(default_factory=lambda: [RoomBackground() for _ in range(8)])
    views: List[RoomView] = field(default_factory=lambda: [RoomView() for _ in range(8)])
    game_objects: List[RoomGameObject] = field(default_factory=list)
    tiles: List[RoomTile] = field(default_factory=list)


@dataclass
class RoomChunk:
    rooms: List[Room] = field(default_factory=list)


# ===[ TPAG - Texture Pages ]===

@dataclass
class TexturePageItem:
    source_x: int = 0
    source_y: int = 0
    source_width: int = 0
    source_height: int = 0
    target_x: int = 0
    target_y: int = 0
    target_width: int = 0
    target_height: int = 0
    bounding_width: int = 0
    bounding_height: int = 0
    texture_page_id: int = 0


@dataclass
class Tpag:
    items: List[TexturePageItem] = field(default_factory=list)
    offset_map: Dict[int, int] = field(default_factory=dict)


# ===[ CODE - Bytecode ]===

@dataclass
class CodeEntry:
    name: Optional[str] = None
    length: int = 0
    locals_count: int = 0
    arguments_count: int = 0
    bytecode_absolute_offset: int = 0
    offset: int = 0


@dataclass
class Code:
    entries: List[CodeEntry] = field(default_factory=list)
    bytecode_buffer: bytes = b''
    bytecode_buffer_base: int = 0


# ===[ VARI - Variables ]===

@dataclass
class Variable:
    name: Optional[str] = None
    instance_type: int = 0
    var_id: int = 0
    occurrences: int = 0
    first_address: int = 0


@dataclass
class Vari:
    var_count1: int = 0
    var_count2: int = 0
    max_local_var_count: int = 0
    variables: List[Variable] = field(default_factory=list)


# ===[ FUNC - Functions ]===

@dataclass
class LocalVar:
    var_index: int = 0
    name: Optional[str] = None


@dataclass
class CodeLocals:
    name: Optional[str] = None
    locals: List[LocalVar] = field(default_factory=list)


@dataclass
class Function:
    name: Optional[str] = None
    occurrences: int = 0
    first_address: int = 0


@dataclass
class Func:
    functions: List[Function] = field(default_factory=list)
    code_locals: List[CodeLocals] = field(default_factory=list)


# ===[ STRG - Strings ]===

@dataclass
class Strg:
    strings: List[Optional[str]] = field(default_factory=list)


# ===[ TXTR - Textures ]===

@dataclass
class Texture:
    scaled: int = 0
    blob_offset: int = 0
    blob_size: int = 0
    blob_data: Optional[bytes] = None
    loaded: bool = False


@dataclass
class Txtr:
    textures: List[Texture] = field(default_factory=list)


# ===[ AUDO - Audio ]===

@dataclass
class AudioEntry:
    data_size: int = 0
    data_offset: int = 0
    data: Optional[bytes] = None


@dataclass
class Audo:
    entries: List[AudioEntry] = field(default_factory=list)
