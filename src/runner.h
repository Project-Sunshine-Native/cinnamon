#pragma once

#include "data_win.h"
#include "instance.h"
#include "vm.h"

// ===[ Event Type Constants ]===
#define EVENT_CREATE  0
#define EVENT_DESTROY 1
#define EVENT_ALARM   2
#define EVENT_STEP    3
#define EVENT_OTHER   7
#define EVENT_DRAW    8

// ===[ Step Sub-event Constants ]===
#define STEP_NORMAL 0
#define STEP_BEGIN  1
#define STEP_END    2

// ===[ Other Sub-event Constants ]===
#define OTHER_GAME_START 2
#define OTHER_ROOM_START 4
#define OTHER_ROOM_END   5

typedef struct Runner {
    DataWin* dataWin;
    VMContext* vmContext;
    Room* currentRoom;
    int32_t currentRoomIndex;
    int32_t currentRoomOrderPosition;
    Instance** instances; // stb_ds array of Instance*
    int32_t pendingRoom;  // -1 = none
    bool gameStartFired;
    int frameCount;
    uint32_t nextInstanceId;
} Runner;

Runner* Runner_create(DataWin* dataWin, VMContext* vm);
void Runner_initFirstRoom(Runner* runner);
void Runner_step(Runner* runner);
void Runner_executeEvent(Runner* runner, Instance* instance, int32_t eventType, int32_t eventSubtype);
void Runner_executeEventFromObject(Runner* runner, Instance* instance, int32_t startObjectIndex, int32_t eventType, int32_t eventSubtype);
void Runner_executeEventForAll(Runner* runner, int32_t eventType, int32_t eventSubtype);
Instance* Runner_createInstance(Runner* runner, double x, double y, int32_t objectIndex);
void Runner_destroyInstance(Runner* runner, Instance* inst);
void Runner_cleanupDestroyedInstances(Runner* runner);
void Runner_free(Runner* runner);
