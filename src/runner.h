#pragma once

#include "common.h"
#include "audio_system.h"
#include "data_win.h"
#include "event_table.h"
#include "file_system.h"
#include "ini.h"
#include "instance.h"
#include "renderer.h"
#include "runner_keyboard.h"
#include "spatial_grid.h"
#include "runner_gamepad.h"
#include "vm.h"

// Platform-specific boot log function
void Runner_platformBootLog(const char* message);

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
#define OTHER_ASYNC_SOCIAL  70
#define OTHER_ASYNC_SYSTEM  75

#define MAX_VIEWS 8

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
    bool enabled;
    int32_t viewX;
    int32_t viewY;
    int32_t viewWidth;
    int32_t viewHeight;
    int32_t portX;
    int32_t portY;
    int32_t portWidth;
    int32_t portHeight;
    uint32_t borderX;
    uint32_t borderY;
    int32_t speedX;
    int32_t speedY;
    int32_t objectId;
    float viewAngle;
} RuntimeView;

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
    float alpha;
} TileLayerState;

typedef struct {
    uint32_t start;
    uint32_t count;
} TileLayerCacheRow;

typedef struct {
    uint32_t tileX;
    uint32_t srcX;
    uint32_t srcY;
    int32_t tpagIndex;
    int32_t n3dsTileEntryIndex;
    bool mirror;
    bool flip;
} TileLayerCacheCell;

typedef struct {
    bool built;
    int32_t backgroundIndex;
    int32_t n3dsChunkCacheId;
    uint32_t tileWidth;
    uint32_t tileHeight;
    uint32_t tilesX;
    uint32_t tilesY;
    TileLayerCacheRow* rows;
    TileLayerCacheCell* cells;
} TileLayerRenderCache;

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

// A single entry in the depth-sorted draw list. Cached on Runner and rebuilt lazily based on Runner.drawableListStructureDirty / drawableListSortDirty.
// Filtering on instance->active/visible and runtimeLayer->visible happens at draw time so toggling those does not require invalidating the cache.
typedef enum { DRAWABLE_TILE, DRAWABLE_INSTANCE, DRAWABLE_LAYER } DrawableType;

typedef struct {
    DrawableType type;
    int32_t depth;
    union {
        Instance* instance;
        int32_t tileIndex;
        RuntimeLayer* runtimeLayer;
    };
} Drawable;

// stb_ds hashmap entry for ds_map: string key -> RValue
typedef struct {
    char* key;
    RValue value;
} DsMapEntry;

// ds_list: dynamic array of RValues
typedef struct {
    RValue* items; // stb_ds dynamic array of RValues
    bool freed;    // true when the slot is destroyed and available for reuse by ds_list_create (matches native GMS)
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

// Motion planning grid used by mp_grid_* builtins. Cell value 1 = blocked.
typedef struct {
    bool inUse;
    GMLReal left;
    GMLReal top;
    int32_t hcells;
    int32_t vcells;
    GMLReal cellWidth;
    GMLReal cellHeight;
    uint8_t* cells;
} MpGrid;

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
    RuntimeView views[MAX_VIEWS];
} SavedRoomState;

typedef struct {
    Instance* instance;
    int32_t codeId;
    int32_t ownerObjectIndex;
} N3DSResolvedDrawEvent;

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
    // Per-object instance lists: for each object index, a stb_ds array of Instance*.
    // An instance appears in its own object's list AND in every ancestor object's list (descendant-inclusive).
    // This lets collision dispatch iterate only the instances of a target object (and its descendants) instead of scanning all instances in the room.
    // Must be kept in sync with any instance creation, change, or deletion.
    Instance*** instancesByObject;
    // Same as instancesByObject but each instance only appears in the bucket of its EXACT objectIndex (no ancestors). Used by event dispatch so we don't double-fire when both a child and its parent declare the same event.
    Instance*** instancesByExactObject;
    // Precomputed (eventType, eventSubtype) -> dense slot remap. Built once at Runner_create, never mutated.
    EventSlotMap eventSlotMap;
    // Precomputed per-object and per-slot CSR tables of resolved event handlers. Replaces the per-dispatch parent-chain walk in findEventCodeIdAndOwner.
    ResolvedEventTable eventTable;
    // Precomputed assets map.
    struct { char* key; int32_t value; }* assetsByName;
    // For each event type, the deduplicated list of object indices that respond to ANY subtype of that event (including via inheritance). Derived from the event table; used by collision dispatch to skip non-collision objects in the outer loop.
    // Length = OBJT_EVENT_TYPE_COUNT.
    int32_t** objectsWithAnyEventOfType;
    // Reusable scratch array for Runner_executeEventForAll. Pre-grown to avoid stb_ds arrput overhead and repeated allocations on the per-frame dispatch path. Owned via stb_ds; truncated at the start of each call.
    Instance** eventDispatchInstances;
    // LIFO arena used to snapshot per-object instance lists before iteration.
    // Any loop that might fire user code iterates a copy so that in-flight mutations (instance_change swap-remove, spawns, destroys) don't corrupt it.
    // Each call pushes its snapshot (append) and pops on normal loop exit; nesting is safe because pushes/pops are LIFO and outer ranges stay untouched under newer pushes.
    Instance** instanceSnapshots;
    SpatialGrid* spatialGrid;
    uint32_t collisionQueryCounter;
    int32_t pendingRoom;  // -1 = none
    bool gameStartFired;
    int frameCount;
    uint32_t nextInstanceId;
    RunnerKeyboardState* keyboard;
    RuntimeView views[MAX_VIEWS];
    RunnerGamepadState* gamepads;
    RuntimeBackground backgrounds[8];
    uint32_t backgroundColor;      // runtime-mutable (BGR format)
    bool drawBackgroundColor;
    bool shouldExit;
    bool debugMode;
    void* nativeWindow;
    void (*setWindowTitle)(void* window, const char* title);
    bool (*windowHasFocus)(void* window);
    TileLayerMapEntry* tileLayerMap; // stb_ds hashmap: depth -> tile layer state
    RuntimeLayer* runtimeLayers; // stb_ds array, index-parallel to currentRoom->layers for parsed entries; dynamic entries appended
    TileLayerRenderCache* tileLayerCaches; // array parallel to currentRoom->layers for parsed tile layers
    uint32_t tileLayerCacheCount;
    uint32_t nextLayerId;        // counter for IDs of layers/elements created at runtime
    SavedRoomState* savedRoomStates; // array of size dataWin->room.count, for persistent room support
    int32_t viewCurrent; // index of the view currently being drawn (for view_current)
    bool drawViewBoundsValid;
    int32_t drawViewX;
    int32_t drawViewY;
    int32_t drawViewWidth;
    int32_t drawViewHeight;
    struct { char* key; int value; }* disabledObjects; // stb_ds string hashmap, nullptr = no filtering
    struct { int key; Instance* value; }* instancesById;
    bool forceDrawDepth;
    // Depth-sorted unified list of all drawables (instances + tiles + runtime layers) for the current room.
    // Active/visible filtering happens at draw time, so toggling those flags does not invalidate the cache.
    //
    // Two-tier invalidation:
    //   structureDirty - the SET of entries changed (instance/layer create or destroy, room change). Full rebuild.
    //   sortDirty      - the entries are the same but .depth values may have shifted. Refresh depths and only re-sort if order broke. Cheap when small depth shifts don't cross neighbors (typical depth=-y games).
    Drawable* cachedDrawables; // stb_ds array
    bool drawableListStructureDirty;
    bool drawableListSortDirty;
    // Dummy instance to serve as "self" during GLOB script execution
    // In bytecode version 17+, global init scripts store method values on "self" via Pop.v.v
    // The real runner uses a persistent YYObjectBase for this, the YYObjectBase is a "parent" of Instance
    // For now, we'll use a dummy Instance with objectIndex = -1 as a hack
    Instance* globalScopeInstance;
    // Struct instances created by @@NewGMLObject@@. Reuses Instance with objectIndex=-1.
    // Tracked separately so event/step/draw iteration over runner->instances stays clean.
    Instance** structInstances;
    int32_t forcedDepth;

    // ===[ Builtin function state ]===
    DsMapEntry** dsMapPool; // stb_ds array of stb_ds hashmaps
    DsList* dsListPool; // stb_ds array of DsList
    GmlBuffer* gmlBufferPool; // stb_ds array of GmlBuffer
    MpGrid* mpGridPool; // stb_ds array of motion-planning grids

    // Motion planning potential field settings
    GMLReal mpPotMaxrot;
    GMLReal mpPotStep;
    GMLReal mpPotAhead;
    bool mpPotOnSpot;

    // Legacy audio_play_music / audio_stop_music tracking
    int32_t lastMusicInstance;
    int32_t* musicInstanceStack; // stb_ds array used to restore the previous legacy music track after nested play/stop flows

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

    // Async map ID
    int32_t asyncLoadMapId;

    // Used by the "os_type" built-in
    YoYoOperatingSystem osType;

    // GUI layer size (display_set_gui_size). 0 = auto-match the current view's port size.
    int32_t guiWidth;
    int32_t guiHeight;

    // Per-frame draw breakdown for lightweight platform HUD diagnostics.
    uint32_t frameDrawBackgrounds;
    uint32_t frameDrawTiles;
    uint32_t frameDrawInstances;
    uint32_t frameDrawLayerElements;
    double frameDrawBackgroundMs;
    double frameDrawTilesMs;
    double frameDrawInstancesMs;
    double frameDrawLayerMs;
    double frameDrawGuiMs;
    double frameDrawGuiEventMs;
    double frameDrawBattleReplayMs;
    double frameStepPrepMs;
    double frameStepEventMs;
    double frameStepMotionMs;
    double frameStepCollisionMs;
    double frameStepFinalizeMs;
    double frameStepTopObjectMs;
    uint32_t frameStepTopObjectCalls;
    int32_t frameStepTopObjectIndex;
    double* frameStepObjectMsByObject;
    uint32_t* frameStepObjectCallsByObject;
    bool n3dsDrawBattleStateValid;
    bool n3dsDrawBattleActive;
    bool n3dsDrawDodgingBullets;
    bool n3dsDrawAsrielBattle;
    bool n3dsDrawHasTopEnemyDialogue;
    bool n3dsBattleReplayListsValid;
    bool n3dsTopScreenGUIListValid;
    Instance** n3dsBattleFieldInstances;
    Instance** n3dsBattleUIInstances;
    Instance** n3dsTopScreenGUIInstances;
    // Bottom-screen text/dialogue/inventory UI state (separate from battle)
    bool n3dsDrawTextUIStateValid;
    bool n3dsDrawTextUIActive;
    bool n3dsTextUIListValid;
    Instance** n3dsTextUIInstances;
    bool n3dsTopScreenGUIResponderListsValid[3];
    N3DSResolvedDrawEvent* n3dsTopScreenGUIResponderEvents[3];

    // GMS legacy (pre 2022.1) collision behavior: AABB overlap treats touching edges as overlap.
    bool collisionCompatibilityMode;
} Runner;

typedef struct {
    uint32_t liveInstances;
    uint32_t liveStructInstances;
    uint32_t dsMapSlots;
    uint32_t dsMapLive;
    uint32_t dsListSlots;
    uint32_t dsListLive;
    uint32_t dsListItems;
} RunnerTelemetry;

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
void Runner_resetFrameDrawStats(Runner* runner);
void Runner_drawBackgrounds(Runner* runner, bool foreground);
void Runner_computeViewDisplayScale(Runner* runner, int32_t gameW, int32_t gameH, float* outScaleX, float* outScaleY);
void Runner_drawViews(Runner* runner, int32_t gameW, int32_t gameH, float displayScaleX, float displayScaleY, bool debugShowCollisionMasks);
void Runner_scrollBackgrounds(Runner* runner);
Instance* Runner_createInstance(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex);
Instance* Runner_createInstanceWithDepth(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex, int32_t depth);
Instance* Runner_createInstanceWithLayer(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex, int32_t layerId);
Instance* Runner_copyInstance(Runner* runner, Instance* source, bool performEvent);
void Runner_destroyInstance(Runner* runner, Instance* inst);
void Runner_cleanupDestroyedInstances(Runner* runner);
// Add inst to the per-object lists of its object and every ancestor.
void Runner_addInstanceToObjectLists(Runner* runner, Instance* inst);
// Remove inst from the per-object lists of its object and every ancestor, preserving creation order (stable remove).
void Runner_removeInstanceFromObjectLists(Runner* runner, Instance* inst);
// Reset every per-object list to length 0 without releasing the backing arrays.
void Runner_clearAllObjectLists(Runner* runner);

// Push a snapshot of instancesByObject[targetObjIndex] onto runner->instanceSnapshots. Returns the base offset where this snapshot begins.
// The length is arrlen(runner->instanceSnapshots) - base.
// Invalid indices or empty buckets push zero entries (base == current arena length).
// Pair with Runner_popInstanceSnapshot(runner, base) when done.
int32_t Runner_pushInstancesOfObject(Runner* runner, int32_t targetObjIndex);
// Push a snapshot matching "target", which GML can pass in several forms: an object index (push the descendant-inclusive bucket), INSTANCE_ALL (push every instance in the room), or an instance ID >= 100000 (push that single instance if it exists).
// Returns base offset for pairing with Runner_popInstanceSnapshot.
int32_t Runner_pushInstancesForTarget(Runner* runner, int32_t target);
// Truncate the snapshot arena back to "base", releasing everything pushed after it.
void Runner_popInstanceSnapshot(Runner* runner, int32_t base);

void Runner_dumpState(Runner* runner);
char* Runner_dumpStateJson(Runner* runner);
void Runner_free(Runner* runner);
RuntimeLayer* Runner_findRuntimeLayerById(Runner* runner, int32_t id);
RoomLayer* Runner_findRoomLayerById(Runner* runner, int32_t id);
RuntimeLayerElement* Runner_findLayerElementById(Runner* runner, int32_t elementId, RuntimeLayer** outLayer);
uint32_t Runner_getNextLayerId(Runner* runner);
void Runner_drawTileLayer(Runner* runner, uint32_t layerIndex, RoomLayerTilesData* data, float layerOffsetX, float layerOffsetY, float alpha);
void Runner_freeRuntimeLayer(RuntimeLayer* runtimeLayer);
RunnerTelemetry Runner_collectTelemetry(Runner* runner);
