#pragma once

#include "data_win.h"
#include "file_system.h"
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

// stb_ds hashmap entry: depth -> tile layer state
typedef struct {
    int32_t key;
    TileLayerState value;
} TileLayerMapEntry;

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
} SavedRoomState;

typedef struct Runner {
    DataWin* dataWin;
    VMContext* vmContext;
    Renderer* renderer;
    FileSystem* fileSystem;
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
    TileLayerMapEntry* tileLayerMap; // stb_ds hashmap: depth -> tile layer state
    SavedRoomState* savedRoomStates; // array of size dataWin->room.count, for persistent room support
    float viewAngles[8]; // runtime-only view_angle per view (not stored in data.win)
    int32_t viewCurrent; // index of the view currently being drawn (for view_current)
} Runner;

const char* Runner_getEventName(int32_t eventType, int32_t eventSubtype);
Runner* Runner_create(DataWin* dataWin, VMContext* vm, FileSystem* fileSystem);
void Runner_initFirstRoom(Runner* runner);
void Runner_step(Runner* runner);
void Runner_executeEvent(Runner* runner, Instance* instance, int32_t eventType, int32_t eventSubtype);
void Runner_executeEventFromObject(Runner* runner, Instance* instance, int32_t startObjectIndex, int32_t eventType, int32_t eventSubtype);
void Runner_executeEventForAll(Runner* runner, int32_t eventType, int32_t eventSubtype);
void Runner_draw(Runner* runner);
void Runner_drawBackgrounds(Runner* runner, bool foreground);
void Runner_scrollBackgrounds(Runner* runner);
Instance* Runner_createInstance(Runner* runner, double x, double y, int32_t objectIndex);
void Runner_destroyInstance(Runner* runner, Instance* inst);
void Runner_cleanupDestroyedInstances(Runner* runner);
void Runner_dumpState(Runner* runner);
char* Runner_dumpStateJson(Runner* runner);
void Runner_free(Runner* runner);
