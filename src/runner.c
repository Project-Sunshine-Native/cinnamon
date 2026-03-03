#include "runner.h"
#include "vm.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_ds.h"

// ===[ Helper: Find event action in object hierarchy ]===
// Walks the parent chain to find an event handler.
// Returns the EventAction's codeId, or -1 if not found.
static int32_t findEventCodeId(DataWin* dataWin, int32_t objectIndex, int32_t eventType, int32_t eventSubtype) {
    int32_t currentObj = objectIndex;
    int depth = 0;

    while (currentObj >= 0 && (uint32_t) currentObj < dataWin->objt.count && 32 > depth) {
        GameObject* obj = &dataWin->objt.objects[currentObj];

        if (OBJT_EVENT_TYPE_COUNT > eventType) {
            ObjectEventList* eventList = &obj->eventLists[eventType];
            for (uint32_t i = 0; eventList->eventCount > i; i++) {
                ObjectEvent* evt = &eventList->events[i];
                if ((int32_t) evt->eventSubtype == eventSubtype) {
                    // Found it - return the first action's codeId
                    if (evt->actionCount > 0 && evt->actions[0].codeId >= 0) {
                        return evt->actions[0].codeId;
                    }
                    return -1;
                }
            }
        }

        // Walk to parent
        currentObj = obj->parentId;
        depth++;
    }

    return -1;
}

// ===[ Event Execution ]===

static void setVMInstanceContext(VMContext* vm, Instance* instance) {
    vm->selfVars = instance->selfVars;
    vm->selfVarCount = instance->selfVarCount;
    vm->currentInstance = instance;
}

static void restoreVMInstanceContext(VMContext* vm, RValue* savedSelfVars, uint32_t savedSelfVarCount, Instance* savedInstance) {
    vm->selfVars = savedSelfVars;
    vm->selfVarCount = savedSelfVarCount;
    vm->currentInstance = savedInstance;
}

void Runner_executeEvent(Runner* runner, Instance* instance, int32_t eventType, int32_t eventSubtype) {
    int32_t codeId = findEventCodeId(runner->dataWin, instance->objectIndex, eventType, eventSubtype);
    if (0 > codeId) return;

    VMContext* vm = runner->vmContext;

    // Save VM context
    RValue* savedSelfVars = vm->selfVars;
    uint32_t savedSelfVarCount = vm->selfVarCount;
    Instance* savedInstance = (Instance*) vm->currentInstance;

    // Set instance context
    setVMInstanceContext(vm, instance);

    // Execute
    RValue result = VM_executeCode(vm, codeId);
    RValue_free(&result);

    // Restore
    restoreVMInstanceContext(vm, savedSelfVars, savedSelfVarCount, savedInstance);
}

void Runner_executeEventForAll(Runner* runner, int32_t eventType, int32_t eventSubtype) {
    // Iterate over a snapshot of the current instance count to avoid issues if instances are added
    int32_t count = (int32_t) arrlen(runner->instances);
    for (int32_t i = 0; count > i; i++) {
        Instance* inst = runner->instances[i];
        if (inst != nullptr && inst->active) {
            Runner_executeEvent(runner, inst, eventType, eventSubtype);
        }
    }
}

// ===[ Room Management ]===

static void executeCodeIfValid(Runner* runner, Instance* instance, int32_t codeId) {
    if (0 > codeId) return;
    if ((uint32_t) codeId >= runner->dataWin->code.count) return;

    VMContext* vm = runner->vmContext;
    RValue* savedSelfVars = vm->selfVars;
    uint32_t savedSelfVarCount = vm->selfVarCount;
    Instance* savedInstance = (Instance*) vm->currentInstance;

    setVMInstanceContext(vm, instance);

    RValue result = VM_executeCode(vm, codeId);
    RValue_free(&result);

    restoreVMInstanceContext(vm, savedSelfVars, savedSelfVarCount, savedInstance);
}

static void initRoom(Runner* runner, int32_t roomIndex) {
    DataWin* dataWin = runner->dataWin;
    require(roomIndex >= 0 && dataWin->room.count > (uint32_t) roomIndex);

    Room* room = &dataWin->room.rooms[roomIndex];
    runner->currentRoom = room;
    runner->currentRoomIndex = roomIndex;

    // Find position in room order
    runner->currentRoomOrderPosition = -1;
    repeat(dataWin->gen8.roomOrderCount, i) {
        if (dataWin->gen8.roomOrder[i] == roomIndex) {
            runner->currentRoomOrderPosition = (int32_t) i;
            break;
        }
    }

    // Handle persistent instances: keep persistent ones, free non-persistent
    Instance** keptInstances = nullptr;
    int32_t oldCount = (int32_t) arrlen(runner->instances);
    for (int32_t i = 0; oldCount > i; i++) {
        Instance* inst = runner->instances[i];
        if (inst != nullptr && inst->persistent) {
            arrput(keptInstances, inst);
        } else if (inst != nullptr) {
            Instance_free(inst);
        }
    }
    arrfree(runner->instances);
    runner->instances = keptInstances;

    // Get self var count from VM context
    uint32_t selfVarCount = runner->vmContext->selfVarCount;

    // Create new instances from room definition
    repeat(room->gameObjectCount, i) {
        RoomGameObject* roomObj = &room->gameObjects[i];
        require(roomObj->objectDefinition >= 0 && dataWin->objt.count > (uint32_t) roomObj->objectDefinition);

        // Check if a persistent instance with this ID already exists
        bool alreadyExists = false;
        for (int32_t j = 0; (int32_t) arrlen(runner->instances) > j; j++) {
            if (runner->instances[j] != nullptr && runner->instances[j]->instanceId == roomObj->instanceID) {
                alreadyExists = true;
                break;
            }
        }
        if (alreadyExists) continue;

        GameObject* objDef = &dataWin->objt.objects[roomObj->objectDefinition];

        Instance* inst = Instance_create(
            roomObj->instanceID,
            roomObj->objectDefinition,
            (double) roomObj->x,
            (double) roomObj->y,
            selfVarCount
        );

        // Copy properties from object definition
        inst->spriteIndex = objDef->spriteId;
        inst->visible = objDef->visible;
        inst->solid = objDef->solid;
        inst->persistent = objDef->persistent;
        inst->depth = objDef->depth;

        // Copy properties from room game object
        inst->imageXscale = (double) roomObj->scaleX;
        inst->imageYscale = (double) roomObj->scaleY;
        inst->imageAngle = (double) roomObj->rotation;

        arrput(runner->instances, inst);

        // Run PreCreate code
        executeCodeIfValid(runner, inst, roomObj->preCreateCode);

        // Run Create event
        Runner_executeEvent(runner, inst, EVENT_CREATE, 0);

        // Run instance creation code
        executeCodeIfValid(runner, inst, roomObj->creationCode);
    }

    // Run room creation code
    if (room->creationCodeId >= 0 && dataWin->code.count > (uint32_t) room->creationCodeId) {
        // Room creation code runs in global context (no specific instance)
        RValue result = VM_executeCode(runner->vmContext, room->creationCodeId);
        RValue_free(&result);
    }

    fprintf(stderr, "Runner: Room loaded: %s (room %d) with %d instances\n", room->name, roomIndex, (int) arrlen(runner->instances));
}

// ===[ Public API ]===

Runner* Runner_create(DataWin* dataWin, VMContext* vm) {
    Runner* runner = calloc(1, sizeof(Runner));
    runner->dataWin = dataWin;
    runner->vmContext = vm;
    runner->frameCount = 0;
    runner->instances = nullptr;
    runner->pendingRoom = -1;
    runner->gameStartFired = false;
    runner->currentRoomIndex = -1;
    runner->currentRoomOrderPosition = -1;

    // Link runner to VM context
    vm->runner = (struct Runner*) runner;

    return runner;
}

void Runner_initFirstRoom(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    require(dataWin->gen8.roomOrderCount > 0);

    int32_t firstRoomIndex = dataWin->gen8.roomOrder[0];

    // Run global init scripts first
    repeat(dataWin->glob.count, i) {
        int32_t codeId = dataWin->glob.codeIds[i];
        if (codeId >= 0 && dataWin->code.count > (uint32_t) codeId) {
            fprintf(stderr, "Runner: Executing global init script: %s\n", dataWin->code.entries[codeId].name);
            RValue result = VM_executeCode(runner->vmContext, codeId);
            RValue_free(&result);
        }
    }

    // Initialize the first room
    initRoom(runner, firstRoomIndex);

    // Fire Game Start for all instances
    Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_GAME_START);
    runner->gameStartFired = true;

    // Fire Room Start for all instances
    Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_START);
}

void Runner_step(Runner* runner) {
    // Execute Begin Step for all instances
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_BEGIN);

    // Execute Normal Step for all instances
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_NORMAL);

    // Execute End Step for all instances
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_END);

    // Handle room transition
    if (runner->pendingRoom >= 0) {
        int32_t oldRoomIndex = runner->currentRoomIndex;
        const char* oldRoomName = runner->currentRoom->name;

        // Fire Room End for all instances
        Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_END);

        int32_t newRoomIndex = runner->pendingRoom;
        require(runner->dataWin->room.count > (uint32_t) newRoomIndex);
        const char* newRoomName = runner->dataWin->room.rooms[newRoomIndex].name;

        fprintf(stderr, "Room changed: %s (room %d) -> %s (room %d)\n", oldRoomName, oldRoomIndex, newRoomName, newRoomIndex);

        // Load new room
        initRoom(runner, newRoomIndex);

        // Fire Room Start for all instances
        Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_START);

        runner->pendingRoom = -1;
    }

    runner->frameCount++;
}

void Runner_free(Runner* runner) {
    if (runner == nullptr) return;

    // Free all instances
    for (int32_t i = 0; (int32_t) arrlen(runner->instances) > i; i++) {
        Instance_free(runner->instances[i]);
    }
    arrfree(runner->instances);

    free(runner);
}
