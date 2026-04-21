#pragma once

#include "common.h"
#include "audio_system.h"
#include "data_win.h"
#include "file_system.h"
#include "ini.h"
#include "instance.h"
#include "renderer.h"
#include "runner_keyboard.h"
#include "vm.h"

// ===[ Event Type Constants ]===
#define EVENT_CREATE     0
#define EVENT_DESTROY    1
#define EVENT_ALARM      2
#define EVENT_STEP       3
#define EVENT_COLLISION  4
#define EVENT_KEYBOARD   5
#define EVENT_OTHER      7
#define EVENT_DRAW       8
#define EVENT_KEYPRESS   9
#define EVENT_KEYRELEASE 10
#define EVENT_PRECREATE  14

// ===[ Step Sub-event Constants ]===
#define STEP_NORMAL 0
#define STEP_BEGIN  1
#define STEP_END    2

// ===[ Draw Sub-event Constants ]===
#define DRAW_NORMAL    0
#define DRAW_GUI       64
#define DRAW_BEGIN     72
#define DRAW_END       73
#define DRAW_GUI_BEGIN 74
#define DRAW_GUI_END   75
#define DRAW_PRE       76
#define DRAW_POST      77

// ===[ Other Sub-event Constants ]===
#define OTHER_OUTSIDE_ROOM  0
#define OTHER_GAME_START    2
#define OTHER_ROOM_START    4
#define OTHER_ROOM_END      5
#define OTHER_ANIMATION_END 7
#define OTHER_END_OF_PATH   8
#define OTHER_USER0         10

// ===[ Operating System Types ]===
// See GameMaker-HTML5's Globals.js
typedef enum {
    OS_UNKNOWN = -1,
    OS_WINDOWS,
    OS_MACOSX,
    OS_PSP,
    OS_IOS,
    OS_ANDROID,
    OS_SYMBIAN,
    OS_LINUX,
    OS_WINPHONE,
    OS_TIZEN,
    OS_WIN8NATIVE,
    OS_WIIU,
    OS_3DS,
    OS_PSVITA,
    OS_BB10,
    OS_PS4,
    OS_XBOXONE,
    OS_PS3,
    OS_XBOX360,
    OS_UWP,
    OS_AMAZON,
    OS_SWITCH,

    OS_LLVM_WIN32 = 65536,
    OS_LLVM_MACOSX,
    OS_LLVM_PSP,
    OS_LLVM_IOS,
    OS_LLVM_ANDROID,
    OS_LLVM_SYMBIAN,
    OS_LLVM_LINUX,
    OS_LLVM_WINPHONE
} YoYoOperatingSystem;

typedef struct {
    bool visible;
    bool foreground;
    int32_t backgroundIndex;  // BGND resource index (mutable at runtime)
    float x, y;               // float for sub-pixel scrolling accumulation
    bool tileX, tileY;
    float speedX, speedY;
    bool stretch;
    float alpha;
} RuntimeBackground;

typedef struct {
    bool visible;
    float offsetX;
    float offsetY;
} TileLayerState;

// Mutable background element on a dynamically-created layer (layer_background_create).
// For parsed room layers, RoomLayerBackgroundData is used directly and this struct is unused.
typedef struct {
    int32_t spriteIndex; // SPRT index (-1 = none)
    bool visible;
    bool htiled;
    bool vtiled;
    bool stretch;
    float xScale;
    float yScale;
    uint32_t blend; // BGR
    float alpha;
    float xOffset; // element-local offset (in addition to layer offset)
    float yOffset;
} RuntimeBackgroundElement;

// Mutable sprite element on an Assets layer. Populated from RoomLayerAssetsData.sprites at room init, can be removed at runtime via layer_sprite_destroy (used by language variant selection).
typedef struct {
    int32_t spriteIndex; // SPRT index (-1 = none/destroyed)
    int32_t x;
    int32_t y;
    float scaleX;
    float scaleY;
    uint32_t color; // BGR + alpha
    float animationSpeed;
    uint32_t animationSpeedType;
    float frameIndex;
    float rotation;
} RuntimeSpriteElement;

// Values match GML layerelementtype_* enum so layer_get_element_type can return them as-is.
typedef enum {
    RuntimeLayerElementType_Background = 1,
    RuntimeLayerElementType_Sprite = 4,
} RuntimeLayerElementType;

typedef struct {
    uint32_t id;
    RuntimeLayerElementType type;
    RuntimeBackgroundElement* backgroundElement; // owned; nullptr if type != Background
    RuntimeSpriteElement* spriteElement; // owned; nullptr if type != Sprite
} RuntimeLayerElement;

// Runtime-mutable state for a GMS2 room layer. Parsed layers are populated at room load from RoomLayer and share IDs with the parsed data.
// Dynamic layers are created via layer_create and carry their own name + element list; they don't correspond to any RoomLayer.
typedef struct {
    uint32_t id;
    int32_t depth;
    bool visible;
    float xOffset;
    float yOffset;
    float hSpeed;
    float vSpeed;
    bool dynamic; // true = created at runtime via layer_create
    char* dynamicName; // owned; only populated for dynamic layers
    RuntimeLayerElement* elements; // stb_ds array; only populated for dynamic layers
} RuntimeLayer;

// stb_ds hashmap entry: depth -> tile layer state
typedef struct {
    int32_t key;
    TileLayerState value;
} TileLayerMapEntry;

// stb_ds hashmap entry for ds_map: string key -> RValue
typedef struct {
    char* key;
    RValue value;
} DsMapEntry;

// ds_list: dynamic array of RValues
typedef struct {
    RValue* items; // stb_ds dynamic array of RValues
} DsList;

// ===[ GML Buffer System ]===

// Buffer type constants (matching GML)
#define GML_BUFFER_FIXED 0
#define GML_BUFFER_GROW  1
#define GML_BUFFER_WRAP  2
#define GML_BUFFER_FAST  3

// Buffer data type constants (matching GML)
#define GML_BUFTYPE_U8      1
#define GML_BUFTYPE_S8      2
#define GML_BUFTYPE_U16     3
#define GML_BUFTYPE_S16     4
#define GML_BUFTYPE_U32     5
#define GML_BUFTYPE_S32     6
#define GML_BUFTYPE_F16     7
#define GML_BUFTYPE_F32     8
#define GML_BUFTYPE_F64     9
#define GML_BUFTYPE_BOOL   10
#define GML_BUFTYPE_STRING 11
#define GML_BUFTYPE_U64    12
#define GML_BUFTYPE_TEXT   13

// Buffer seek mode constants (matching GML)
#define GML_BUFFER_SEEK_START    0
#define GML_BUFFER_SEEK_RELATIVE 1
#define GML_BUFFER_SEEK_END      2

typedef struct {
    uint8_t* data;       // raw byte storage
    int32_t size;        // allocated size in bytes
    int32_t position;    // current read/write cursor
    int32_t usedSize;    // high-water mark for grow buffers
    int32_t alignment;   // byte alignment for read/write operations
    int32_t type;        // GML_BUFFER_FIXED, _GROW, _WRAP, _FAST
    bool isValid;        // false after buffer_delete (tombstone)
} GmlBuffer;

// Open text file handle for GML file_text_* functions
#define MAX_OPEN_TEXT_FILES 32
typedef struct {
    char* content; // full file content (for read mode)
    char* writeBuffer; // accumulated text (for write mode)
    char* filePath; // relative path (for write mode, to flush on close)
    int32_t readPos; // current byte position in content (read mode)
    int32_t contentLen; // length of content string
    bool isWriteMode;
    bool isOpen;
} OpenTextFile;

// Saved state for persistent rooms. When leaving a persistent room, instance state
// and visual properties are saved here. When returning, they are restored instead
// of re-creating from the room definition.
typedef struct {
    bool initialized;
    Instance** instances; // stb_ds array of saved Instance*
    RuntimeBackground backgrounds[8];
    uint32_t backgroundColor;
    bool drawBackgroundColor;
    TileLayerMapEntry* tileLayerMap; // stb_ds hashmap: depth -> tile layer state
    RuntimeLayer* runtimeLayers; // stb_ds array, index-parallel to currentRoom->layers
} SavedRoomState;

typedef struct Runner {
    DataWin* dataWin;
    VMContext* vmContext;
    Renderer* renderer;
    FileSystem* fileSystem;
    AudioSystem* audioSystem;
    Room* currentRoom;
    int32_t currentRoomIndex;
    int32_t currentRoomOrderPosition;
    Instance** instances; // stb_ds array of Instance*
    int32_t pendingRoom;  // -1 = none
    bool gameStartFired;
    int frameCount;
    uint32_t nextInstanceId;
    RunnerKeyboardState* keyboard;
    RuntimeBackground backgrounds[8];
    uint32_t backgroundColor;      // runtime-mutable (BGR format)
    bool drawBackgroundColor;
    bool shouldExit;
    bool debugMode;

    // ===[ 3DS-specific fields ]===
    bool drawSpriteDecimationEnabled; // Skip alternate sprite draw calls to improve performance
    int drawSpriteDecimationPhase;    // Tracks which phase of decimation we're in
    void* nativeWindow;
    void (*setWindowTitle)(void* window, const char* title);
    TileLayerMapEntry* tileLayerMap; // stb_ds hashmap: depth -> tile layer state
    RuntimeLayer* runtimeLayers; // stb_ds array, index-parallel to currentRoom->layers for parsed entries; dynamic entries appended
    uint32_t nextLayerId;        // counter for IDs of layers/elements created at runtime
    SavedRoomState* savedRoomStates; // array of size dataWin->room.count, for persistent room support
    float viewAngles[8]; // runtime-only view_angle per view (not stored in data.win)
    int32_t viewCurrent; // index of the view currently being drawn (for view_current)
    struct { char* key; int value; }* disabledObjects; // stb_ds string hashmap, nullptr = no filtering
    struct { int key; Instance* value; }* instancesToId;
    bool forceDrawDepth;
    bool disableAsrielBackgroundEffects;
    // In bytecode version 17+, global init scripts store method values on "self" via Pop.v.v
    // The real runner uses a persistent YYObjectBase for this, the YYObjectBase is a "parent" of Instance
    // For now, we'll use a dummy Instance with objectIndex = -1 as a hack
    Instance* globalScopeInstance;
    int32_t forcedDepth;

    // ===[ Builtin function state ]===
    DsMapEntry** dsMapPool; // stb_ds array of stb_ds hashmaps
    DsList* dsListPool; // stb_ds array of DsList
    GmlBuffer* gmlBufferPool; // stb_ds array of GmlBuffer

    // Motion planning potential field settings
    GMLReal mpPotMaxrot;
    GMLReal mpPotStep;
    GMLReal mpPotAhead;
    bool mpPotOnSpot;

    // Legacy audio_play_music / audio_stop_music tracking
    int32_t lastMusicInstance;

    // INI file state
    IniFile* currentIni;
    char* currentIniPath;
    bool currentIniDirty;
    // Some games (like Undertale) open and close the same INI file EVERY SINGLE FRAME!
    // While on modern devices this isn't a huge deal, this WILL cause issues on devices that have less than stellar file systems (like the PlayStation 2)
    // To avoid unnecessary disk reads, we cache the last-closed INI and reuse it on reopen
    IniFile* cachedIni; // Cache of last-closed INI (for fast reopen)
    char* cachedIniPath;

    // Text file handles for file_text_* functions
    OpenTextFile openTextFiles[MAX_OPEN_TEXT_FILES];

    // Used by the "os_type" built-in
    YoYoOperatingSystem osType;

    // GUI layer size (display_set_gui_size). 0 = auto-match the current view's port size.
    int32_t guiWidth;
    int32_t guiHeight;
} Runner;

const char* Runner_getEventName(int32_t eventType, int32_t eventSubtype);
void Runner_reset(Runner* runner);
Runner* Runner_create(DataWin* dataWin, VMContext* vm, Renderer* renderer, FileSystem* fileSystem, AudioSystem* audioSystem);
void Runner_initFirstRoom(Runner* runner);
void Runner_step(Runner* runner);
void Runner_executeEvent(Runner* runner, Instance* instance, int32_t eventType, int32_t eventSubtype);
void Runner_executeEventFromObject(Runner* runner, Instance* instance, int32_t startObjectIndex, int32_t eventType, int32_t eventSubtype);
void Runner_executeEventForAll(Runner* runner, int32_t eventType, int32_t eventSubtype);
void Runner_draw(Runner* runner);
void Runner_drawGUI(Runner* runner);
void Runner_drawBackgrounds(Runner* runner, bool foreground);
void Runner_scrollBackgrounds(Runner* runner);
Instance* Runner_createInstance(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex);
Instance* Runner_copyInstance(Runner* runner, Instance* source, bool performEvent);
void Runner_destroyInstance(Runner* runner, Instance* inst);
void Runner_cleanupDestroyedInstances(Runner* runner);
void Runner_dumpState(Runner* runner);
char* Runner_dumpStateJson(Runner* runner);
void Runner_free(Runner* runner);
RuntimeLayer* Runner_findRuntimeLayerById(Runner* runner, int32_t id);
RoomLayer* Runner_findRoomLayerById(Runner* runner, int32_t id);
RuntimeLayerElement* Runner_findLayerElementById(Runner* runner, int32_t elementId, RuntimeLayer** outLayer);
uint32_t Runner_getNextLayerId(Runner* runner);
void Runner_freeRuntimeLayer(RuntimeLayer* runtimeLayer);
