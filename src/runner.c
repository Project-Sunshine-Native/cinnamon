#include "runner.h"
#include "vm.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_ds.h"

// ===[ Helper: Find event action in object hierarchy ]===
// Walks the parent chain starting from objectIndex to find an event handler.
// Returns the EventAction's codeId, or -1 if not found.
// If outOwnerObjectIndex is non-null, it is set to the objectIndex that owns the found event (or -1 if not found).
static int32_t findEventCodeIdAndOwner(DataWin* dataWin, int32_t objectIndex, int32_t eventType, int32_t eventSubtype, int32_t* outOwnerObjectIndex) {
    int32_t currentObj = objectIndex;
    int depth = 0;

    while (currentObj >= 0 && (uint32_t) currentObj < dataWin->objt.count && 32 > depth) {
        GameObject* obj = &dataWin->objt.objects[currentObj];

        if (OBJT_EVENT_TYPE_COUNT > eventType) {
            ObjectEventList* eventList = &obj->eventLists[eventType];
            repeat(eventList->eventCount, i) {
                ObjectEvent* evt = &eventList->events[i];
                if ((int32_t) evt->eventSubtype == eventSubtype) {
                    // Found it - return the first action's codeId
                    if (evt->actionCount > 0 && evt->actions[0].codeId >= 0) {
                        if (outOwnerObjectIndex != nullptr) *outOwnerObjectIndex = currentObj;
                        return evt->actions[0].codeId;
                    }
                    if (outOwnerObjectIndex != nullptr) *outOwnerObjectIndex = -1;
                    return -1;
                }
            }
        }

        // Walk to parent
        currentObj = obj->parentId;
        depth++;
    }

    if (outOwnerObjectIndex != nullptr) *outOwnerObjectIndex = -1;
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

static void executeCode(Runner* runner, Instance* instance, int32_t codeId) {
    // GameMaker does use codeIds less than 0, we'll just pretend we didn't hear them...
    if (0 > codeId) return;

    // Save VM context
    VMContext* vm = runner->vmContext;
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

static const char* getEventName(int32_t eventType, int32_t eventSubtype) {
    switch (eventType) {
        case EVENT_CREATE:  return "Create";
        case EVENT_DESTROY: return "Destroy";
        case EVENT_ALARM:   return "Alarm";
        case EVENT_STEP:
            switch (eventSubtype) {
                case STEP_BEGIN:  return "BeginStep";
                case STEP_NORMAL: return "NormalStep";
                case STEP_END:    return "EndStep";
                default:          return "Step";
            }
        case EVENT_DRAW: return "Draw";
        case EVENT_OTHER:
            switch (eventSubtype) {
                case OTHER_GAME_START: return "GameStart";
                case OTHER_ROOM_START: return "RoomStart";
                case OTHER_ROOM_END:   return "RoomEnd";
                default:               return "Other";
            }
        default: return "Unknown";
    }
}

void Runner_executeEventFromObject(Runner* runner, Instance* instance, int32_t startObjectIndex, int32_t eventType, int32_t eventSubtype) {
    int32_t ownerObjectIndex = -1;
    int32_t codeId = findEventCodeIdAndOwner(runner->dataWin, startObjectIndex, eventType, eventSubtype, &ownerObjectIndex);

    VMContext* vm = runner->vmContext;
    int32_t savedEventType = vm->currentEventType;
    int32_t savedEventSubtype = vm->currentEventSubtype;
    int32_t savedEventObjectIndex = vm->currentEventObjectIndex;

    vm->currentEventType = eventType;
    vm->currentEventSubtype = eventSubtype;
    vm->currentEventObjectIndex = ownerObjectIndex;

    if (codeId >= 0 && shlen(vm->eventsToBeTraced) != -1) {
        const char* eventName = getEventName(eventType, eventSubtype);
        const char* objectName = runner->dataWin->objt.objects[instance->objectIndex].name;

        bool shouldTrace = shgeti(vm->eventsToBeTraced, "*") != -1 || shgeti(vm->eventsToBeTraced, eventName) != -1 || shgeti(vm->eventsToBeTraced, objectName) != -1;

        if (shouldTrace) {
            if (eventType == EVENT_ALARM) {
                printf("Runner: [%s] %s %d (instanceId=%d)\n", objectName, eventName, eventSubtype, instance->instanceId);
            } else {
                printf("Runner: [%s] %s (instanceId=%d)\n", objectName, eventName, instance->instanceId);
            }
        }
    }

    executeCode(runner, instance, codeId);

    vm->currentEventType = savedEventType;
    vm->currentEventSubtype = savedEventSubtype;
    vm->currentEventObjectIndex = savedEventObjectIndex;
}

void Runner_executeEvent(Runner* runner, Instance* instance, int32_t eventType, int32_t eventSubtype) {
    Runner_executeEventFromObject(runner, instance, instance->objectIndex, eventType, eventSubtype);
}

void Runner_executeEventForAll(Runner* runner, int32_t eventType, int32_t eventSubtype) {
    // Iterate over a snapshot of the current instance count to avoid issues if instances are added
    int32_t count = (int32_t) arrlen(runner->instances);
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (inst != nullptr && inst->active) {
            Runner_executeEvent(runner, inst, eventType, eventSubtype);
        }
    }
}

// ===[ Instance Creation Helper ]===

static Instance* createAndInitInstance(Runner* runner, int32_t instanceId, int32_t objectIndex, double x, double y) {
    DataWin* dataWin = runner->dataWin;
    require(objectIndex >= 0 && dataWin->objt.count > (uint32_t) objectIndex);

    GameObject* objDef = &dataWin->objt.objects[objectIndex];
    uint32_t selfVarCount = runner->vmContext->selfVarCount;

    Instance* inst = Instance_create(instanceId, objectIndex, x, y, selfVarCount);

    // Copy properties from object definition
    inst->spriteIndex = objDef->spriteId;
    inst->visible = objDef->visible;
    inst->solid = objDef->solid;
    inst->persistent = objDef->persistent;
    inst->depth = objDef->depth;

    arrput(runner->instances, inst);

    if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != 1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, objDef->name) != 1) {
        printf("VM: Instance %s (%d) created at (%f, %f)\n", objDef->name, instanceId, x, y);
    }

    return inst;
}

// ===[ Room Management ]===

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
    repeat(oldCount, i) {
        Instance* inst = runner->instances[i];
        if (inst != nullptr && inst->persistent) {
            arrput(keptInstances, inst);
        } else if (inst != nullptr) {
            Instance_free(inst);
        }
    }
    arrfree(runner->instances);
    runner->instances = keptInstances;

    // Create new instances from room definition
    repeat(room->gameObjectCount, i) {
        RoomGameObject* roomObj = &room->gameObjects[i];

        // Check if a persistent instance with this ID already exists
        bool alreadyExists = false;
        repeat(arrlen(runner->instances), j) {
            if (runner->instances[j] != nullptr && runner->instances[j]->instanceId == roomObj->instanceID) {
                alreadyExists = true;
                break;
            }
        }
        if (alreadyExists) continue;

        Instance* inst = createAndInitInstance(runner, roomObj->instanceID, roomObj->objectDefinition, (double) roomObj->x, (double) roomObj->y);
        inst->imageXscale = (double) roomObj->scaleX;
        inst->imageYscale = (double) roomObj->scaleY;
        inst->imageAngle = (double) roomObj->rotation;

        executeCode(runner, inst, roomObj->preCreateCode);
        Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
        executeCode(runner, inst, roomObj->creationCode);
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
    runner->nextInstanceId = dataWin->gen8.lastObj + 1;

    // Link runner to VM context
    vm->runner = (struct Runner*) runner;

    return runner;
}

Instance* Runner_createInstance(Runner* runner, double x, double y, int32_t objectIndex) {
    Instance* inst = createAndInitInstance(runner, runner->nextInstanceId++, objectIndex, x, y);
    Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
    return inst;
}

void Runner_destroyInstance(Runner* runner, Instance* inst) {
    (void) runner;
    GameObject* gameObject = &runner->dataWin->objt.objects[inst->objectIndex];
    Runner_executeEvent(runner, inst, EVENT_DESTROY, 0);
    inst->active = false;

    if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, gameObject->name) != -1) {
        printf("VM: Instance %s (%d) destroyed\n", gameObject->name, inst->instanceId);
    }
}

void Runner_cleanupDestroyedInstances(Runner* runner) {
    int32_t count = (int32_t) arrlen(runner->instances);
    int32_t writeIdx = 0;
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (inst != nullptr && inst->active) {
            runner->instances[writeIdx++] = inst;
        } else if (inst != nullptr) {
            Instance_free(inst);
        }
    }
    arrsetlen(runner->instances, writeIdx);
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

    // Process alarms for all instances
    int32_t alarmCount = (int32_t) arrlen(runner->instances);
    repeat(alarmCount, i) {
        Instance* inst = runner->instances[i];
        if (inst == nullptr || !inst->active) continue;

        GameObject* object = &runner->dataWin->objt.objects[inst->objectIndex];

        repeat(GML_ALARM_COUNT, alarmIdx) {
            if (inst->alarm[alarmIdx] > 0) {
                if (shgeti(runner->vmContext->alarmsToBeTraced, "*") != -1 || shgeti(runner->vmContext->alarmsToBeTraced, object->name) != -1) {
                    printf("VM: Ticking down Alarm[%d] for %s (%d), current tick is %d\n", alarmIdx, object->name, inst->instanceId, inst->alarm[alarmIdx]);
                }

                inst->alarm[alarmIdx]--;
                if (inst->alarm[alarmIdx] == 0) {
                    inst->alarm[alarmIdx] = -1;

                    if (shgeti(runner->vmContext->alarmsToBeTraced, "*") != -1 || shgeti(runner->vmContext->alarmsToBeTraced, object->name) != -1) {
                        printf("VM: Firing Alarm[%d] for %s (%d)\n", alarmIdx, object->name, inst->instanceId);
                    }

                    Runner_executeEvent(runner, inst, EVENT_ALARM, alarmIdx);
                }
            }
        }
    }

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

    Runner_cleanupDestroyedInstances(runner);

    runner->frameCount++;
}

void Runner_free(Runner* runner) {
    if (runner == nullptr) return;

    // Free all instances
    repeat(arrlen(runner->instances), i) {
        Instance_free(runner->instances[i]);
    }
    arrfree(runner->instances);

    free(runner);
}
