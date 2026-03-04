#include "runner.h"
#include "vm.h"
#include "utils.h"
#include "json_writer.h"

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

    VMContext* vm = runner->vmContext;

    // Save instance context
    RValue* savedSelfVars = vm->selfVars;
    uint32_t savedSelfVarCount = vm->selfVarCount;
    Instance* savedInstance = (Instance*) vm->currentInstance;

    // Save full VM execution state, because VM_executeCode overwrites all of these.
    // This is necessary for nested execution (e.g., instance_create triggering a Create
    // event while another event's executeLoop is still on the call stack).
    uint8_t* savedBytecodeBase = vm->bytecodeBase;
    uint32_t savedIP = vm->ip;
    uint32_t savedCodeEnd = vm->codeEnd;
    const char* savedCodeName = vm->currentCodeName;
    RValue* savedLocalVars = vm->localVars;
    uint32_t savedLocalVarCount = vm->localVarCount;
    ArrayMapEntry* savedLocalArrayMap = vm->localArrayMap;
    int32_t savedStackTop = vm->stack.top;

    // Save stack values (VM_executeCode resets stack.top to 0, which would let
    // the nested execution overwrite the caller's stack slot values)
    RValue* savedStackValues = nullptr;
    if (savedStackTop > 0) {
        savedStackValues = malloc((uint32_t) savedStackTop * sizeof(RValue));
        memcpy(savedStackValues, vm->stack.slots, (uint32_t) savedStackTop * sizeof(RValue));
    }

    // Set instance context
    setVMInstanceContext(vm, instance);

    // Execute
    RValue result = VM_executeCode(vm, codeId);
    RValue_free(&result);

    // Restore instance context
    restoreVMInstanceContext(vm, savedSelfVars, savedSelfVarCount, savedInstance);

    // Restore VM execution state
    vm->bytecodeBase = savedBytecodeBase;
    vm->ip = savedIP;
    vm->codeEnd = savedCodeEnd;
    vm->currentCodeName = savedCodeName;
    vm->localVars = savedLocalVars;
    vm->localVarCount = savedLocalVarCount;
    vm->localArrayMap = savedLocalArrayMap;
    vm->stack.top = savedStackTop;

    // Restore stack values
    if (savedStackTop > 0) {
        memcpy(vm->stack.slots, savedStackValues, (uint32_t) savedStackTop * sizeof(RValue));
        free(savedStackValues);
    }
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
        case EVENT_DRAW:
            switch (eventSubtype) {
                case DRAW_NORMAL:    return "Draw";
                case DRAW_GUI:       return "DrawGUI";
                case DRAW_BEGIN:     return "DrawBegin";
                case DRAW_END:       return "DrawEnd";
                case DRAW_GUI_BEGIN: return "DrawGUIBegin";
                case DRAW_GUI_END:   return "DrawGUIEnd";
                case DRAW_PRE:       return "DrawPre";
                case DRAW_POST:      return "DrawPost";
                default:             return "Draw";
            }
        case EVENT_KEYBOARD:   return "Keyboard";
        case EVENT_OTHER:
            switch (eventSubtype) {
                case OTHER_GAME_START: return "GameStart";
                case OTHER_ROOM_START: return "RoomStart";
                case OTHER_ROOM_END:   return "RoomEnd";
                default:               return "Other";
            }
        case EVENT_KEYPRESS:   return "KeyPress";
        case EVENT_KEYRELEASE: return "KeyRelease";
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

// ===[ Background Scrolling & Drawing ]===

void Runner_scrollBackgrounds(Runner* runner) {
    repeat(8, i) {
        RuntimeBackground* bg = &runner->backgrounds[i];
        if (!bg->visible) continue;
        bg->x += bg->speedX;
        bg->y += bg->speedY;
    }
}

void Runner_drawBackgrounds(Runner* runner, bool foreground) {
    if (runner->renderer == nullptr) return;
    DataWin* dataWin = runner->dataWin;
    float roomW = (float) runner->currentRoom->width;
    float roomH = (float) runner->currentRoom->height;

    repeat(8, i) {
        RuntimeBackground* bg = &runner->backgrounds[i];
        if (!bg->visible || bg->foreground != foreground) continue;
        if (0 > bg->backgroundIndex) continue;

        int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(dataWin, bg->backgroundIndex);
        if (0 > tpagIndex) continue;

        if (bg->stretch) {
            // Stretch to fill room dimensions
            TexturePageItem* tpag = &dataWin->tpag.items[tpagIndex];
            float xscale = roomW / (float) tpag->boundingWidth;
            float yscale = roomH / (float) tpag->boundingHeight;
            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, 0.0f, 0.0f, 0.0f, 0.0f, xscale, yscale, 0.0f, 0xFFFFFF, 1.0f);
        } else if (bg->tileX || bg->tileY) {
            Renderer_drawBackgroundTiled(runner->renderer, tpagIndex, bg->x, bg->y, bg->tileX, bg->tileY, roomW, roomH);
        } else {
            // Single placement
            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, bg->x, bg->y, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFF, 1.0f);
        }
    }
}

// ===[ Draw ]===

static int compareInstanceDepth(const void* a, const void* b) {
    Instance* instA = *(Instance**) a;
    Instance* instB = *(Instance**) b;
    // Higher depth draws first (behind), lower depth draws last (in front)
    if (instA->depth > instB->depth) return -1;
    if (instB->depth > instA->depth) return 1;
    return 0;
}

static void fireDrawSubtype(Runner* runner, Instance** drawList, int32_t drawCount, int32_t subtype) {
    repeat(drawCount, i) {
        Runner_executeEvent(runner, drawList[i], EVENT_DRAW, subtype);
    }
}

void Runner_draw(Runner* runner) {
    // Collect active + visible instances
    Instance** drawList = nullptr;
    int32_t count = (int32_t) arrlen(runner->instances);
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (inst != nullptr && inst->active && inst->visible) {
            arrput(drawList, inst);
        }
    }

    // Sort by depth descending (higher depth first)
    int32_t drawCount = (int32_t) arrlen(drawList);
    if (drawCount > 1) {
        qsort(drawList, drawCount, sizeof(Instance*), compareInstanceDepth);
    }

    // Draw non-foreground backgrounds (behind everything)
    Runner_drawBackgrounds(runner, false);

    // Fire draw subtypes in correct GameMaker order
    fireDrawSubtype(runner, drawList, drawCount, DRAW_PRE);
    fireDrawSubtype(runner, drawList, drawCount, DRAW_BEGIN);

    // DRAW_NORMAL: instances with Draw events execute them, others get default sprite draw
    repeat(drawCount, i) {
        Instance* inst = drawList[i];
        int32_t codeId = findEventCodeIdAndOwner(runner->dataWin, inst->objectIndex, EVENT_DRAW, DRAW_NORMAL, nullptr);
        if (codeId >= 0) {
            Runner_executeEvent(runner, inst, EVENT_DRAW, DRAW_NORMAL);
        } else if (runner->renderer != nullptr) {
            Renderer_drawSelf(runner->renderer, inst);
        }
    }

    fireDrawSubtype(runner, drawList, drawCount, DRAW_END);

    // Draw foreground backgrounds (in front of instances, behind GUI)
    Runner_drawBackgrounds(runner, true);

    fireDrawSubtype(runner, drawList, drawCount, DRAW_POST);
    fireDrawSubtype(runner, drawList, drawCount, DRAW_GUI_BEGIN);
    fireDrawSubtype(runner, drawList, drawCount, DRAW_GUI);
    fireDrawSubtype(runner, drawList, drawCount, DRAW_GUI_END);

    arrfree(drawList);
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

    if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, objDef->name) != -1) {
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

    // Copy room background definitions into mutable runtime state
    runner->backgroundColor = room->backgroundColor;
    runner->drawBackgroundColor = room->drawBackgroundColor;
    repeat(8, i) {
        RoomBackground* src = &room->backgrounds[i];
        RuntimeBackground* dst = &runner->backgrounds[i];
        dst->visible = src->enabled;
        dst->foreground = src->foreground;
        dst->backgroundIndex = src->backgroundDefinition;
        dst->x = (float) src->x;
        dst->y = (float) src->y;
        dst->tileX = (bool) src->tileX;
        dst->tileY = (bool) src->tileY;
        dst->speedX = (float) src->speedX;
        dst->speedY = (float) src->speedY;
        dst->stretch = src->stretch;
    }

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
    runner->keyboard = RunnerKeyboard_create();

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
    // Scroll backgrounds
    Runner_scrollBackgrounds(runner);

    // Execute Begin Step for all instances
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_BEGIN);

    // Dispatch keyboard events
    RunnerKeyboardState* kb = runner->keyboard;
    for (int32_t key = 0; GML_KEY_COUNT > key; key++) {
        if (kb->keyPressed[key]) {
            Runner_executeEventForAll(runner, EVENT_KEYPRESS, key);
        }
    }
    for (int32_t key = 0; GML_KEY_COUNT > key; key++) {
        if (kb->keyDown[key]) {
            Runner_executeEventForAll(runner, EVENT_KEYBOARD, key);
        }
    }
    for (int32_t key = 0; GML_KEY_COUNT > key; key++) {
        if (kb->keyReleased[key]) {
            Runner_executeEventForAll(runner, EVENT_KEYRELEASE, key);
        }
    }

    // Process alarms for all instances
    int32_t alarmCount = (int32_t) arrlen(runner->instances);
    repeat(alarmCount, i) {
        Instance* inst = runner->instances[i];
        if (inst == nullptr || !inst->active) continue;

        GameObject* object = &runner->dataWin->objt.objects[inst->objectIndex];

        repeat(GML_ALARM_COUNT, alarmIdx) {
            if (inst->alarm[alarmIdx] > 0) {
                if (shgeti(runner->vmContext->alarmsToBeTraced, "*") != -1 || shgeti(runner->vmContext->alarmsToBeTraced, object->name) != -1) {
                    printf("VM: [%s] Ticking down Alarm[%d] (instanceId=%d), current tick is %d\n", object->name, alarmIdx, inst->instanceId, inst->alarm[alarmIdx]);
                }

                inst->alarm[alarmIdx]--;
                if (inst->alarm[alarmIdx] == 0) {
                    inst->alarm[alarmIdx] = -1;

                    if (shgeti(runner->vmContext->alarmsToBeTraced, "*") != -1 || shgeti(runner->vmContext->alarmsToBeTraced, object->name) != -1) {
                        printf("VM: [%s] Firing Alarm[%d] (instanceId=%d)\n", object->name, alarmIdx, inst->instanceId);
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

// ===[ State Dump ]===

void Runner_dumpState(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    VMContext* vm = runner->vmContext;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);

    printf("=== Frame %d State Dump ===\n", runner->frameCount);
    printf("Room: %s (index %d)\n", runner->currentRoom->name, runner->currentRoomIndex);
    printf("Instance count: %d\n", instanceCount);

    repeat(instanceCount, i) {
        Instance* inst = runner->instances[i];
        if (inst == nullptr || !inst->active) continue;

        const char* objName = (inst->objectIndex >= 0 && dataWin->objt.count > (uint32_t) inst->objectIndex) ? dataWin->objt.objects[inst->objectIndex].name : "<unknown>";

        const char* spriteName = "<none>";
        if (inst->spriteIndex >= 0 && dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
            spriteName = dataWin->sprt.sprites[inst->spriteIndex].name;
        }

        printf("\n--- Instance #%d (%s, objectIndex=%d) ---\n", inst->instanceId, objName, inst->objectIndex);
        printf("  Position: (%g, %g)\n", inst->x, inst->y);
        printf("  Depth: %d\n", inst->depth);
        printf("  Sprite: %s (index %d), imageIndex=%g, imageSpeed=%g\n", spriteName, inst->spriteIndex, inst->imageIndex, inst->imageSpeed);
        printf("  Scale: (%g, %g), Angle: %g, Alpha: %g, Blend: 0x%06X\n", inst->imageXscale, inst->imageYscale, inst->imageAngle, inst->imageAlpha, inst->imageBlend);
        printf("  Visible: %s, Active: %s, Solid: %s, Persistent: %s\n", inst->visible ? "true" : "false", inst->active ? "true" : "false", inst->solid ? "true" : "false", inst->persistent ? "true" : "false");

        // Active alarms
        bool hasAlarm = false;
        repeat(GML_ALARM_COUNT, alarmIdx) {
            if (inst->alarm[alarmIdx] >= 0) {
                if (!hasAlarm) { printf("  Alarms:"); hasAlarm = true; }
                printf(" [%d]=%d", alarmIdx, inst->alarm[alarmIdx]);
            }
        }
        if (hasAlarm) printf("\n");

        // Self variables (non-array)
        bool hasSelfVars = false;
        repeat(dataWin->vari.variableCount, varIdx) {
            Variable* var = &dataWin->vari.variables[varIdx];
            if (var->instanceType != INSTANCE_SELF || var->varID < 0) continue;
            if ((uint32_t) var->varID >= inst->selfVarCount) continue;
            RValue val = inst->selfVars[var->varID];
            if (val.type == RVALUE_UNDEFINED) continue;

            if (!hasSelfVars) { printf("  Self Variables:\n"); hasSelfVars = true; }
            char* valStr = RValue_toStringFancy(val);
            printf("    %s = %s\n", var->name, valStr);
            free(valStr);
        }

        // Self arrays
        int64_t selfArrayLen = hmlen(inst->selfArrayMap);
        if (selfArrayLen > 0) {
            printf("  Self Arrays:\n");
            repeat(selfArrayLen, arrIdx) {
                int64_t key = inst->selfArrayMap[arrIdx].key;
                RValue val = inst->selfArrayMap[arrIdx].value;
                int32_t varID = (int32_t) (key >> 32);
                int32_t arrayIndex = (int32_t) (key & 0xFFFFFFFF);

                // Find variable name by scanning VARI entries
                const char* varName = "<unknown>";
                repeat(dataWin->vari.variableCount, varIdx) {
                    Variable* var = &dataWin->vari.variables[varIdx];
                    if (var->varID == varID && var->instanceType == INSTANCE_SELF) {
                        varName = var->name;
                        break;
                    }
                }

                char* valStr = RValue_toStringFancy(val);
                printf("    %s[%d] = %s\n", varName, arrayIndex, valStr);
                free(valStr);
            }
        }
    }

    // Global variables (non-array)
    printf("\n=== Global Variables ===\n");
    repeat(dataWin->vari.variableCount, varIdx) {
        Variable* var = &dataWin->vari.variables[varIdx];
        if (var->instanceType != INSTANCE_GLOBAL || var->varID < 0) continue;
        if ((uint32_t) var->varID >= vm->globalVarCount) continue;
        RValue val = vm->globalVars[var->varID];
        if (val.type == RVALUE_UNDEFINED) continue;

        char* valStr = RValue_toStringFancy(val);
        printf("  %s = %s\n", var->name, valStr);
        free(valStr);
    }

    // Global arrays
    int64_t globalArrayLen = hmlen(vm->globalArrayMap);
    if (globalArrayLen > 0) {
        repeat(globalArrayLen, arrIdx) {
            int64_t key = vm->globalArrayMap[arrIdx].key;
            RValue val = vm->globalArrayMap[arrIdx].value;
            int32_t varID = (int32_t) (key >> 32);
            int32_t arrayIndex = (int32_t) (key & 0xFFFFFFFF);

            const char* varName = "<unknown>";
            repeat(dataWin->vari.variableCount, varIdx) {
                Variable* var = &dataWin->vari.variables[varIdx];
                if (var->varID == varID && var->instanceType == INSTANCE_GLOBAL) {
                    varName = var->name;
                    break;
                }
            }

            char* valStr = RValue_toStringFancy(val);
            printf("  %s[%d] = %s\n", varName, arrayIndex, valStr);
            free(valStr);
        }
    }

    printf("\n=== End Frame %d State Dump ===\n", runner->frameCount);
}

// ===[ JSON State Dump ]===

static void writeRValueJson(JsonWriter* w, RValue val) {
    switch (val.type) {
        case RVALUE_REAL:
            JsonWriter_double(w, val.real);
            break;
        case RVALUE_INT32:
            JsonWriter_int(w, val.int32);
            break;
        case RVALUE_INT64:
            JsonWriter_int(w, val.int64);
            break;
        case RVALUE_STRING:
            JsonWriter_string(w, val.string);
            break;
        case RVALUE_BOOL:
            JsonWriter_bool(w, val.int32 != 0);
            break;
        case RVALUE_UNDEFINED:
            JsonWriter_null(w);
            break;
        case RVALUE_ARRAY_REF: {
            char buf[64];
            snprintf(buf, sizeof(buf), "<array_ref:%d>", val.int32);
            JsonWriter_string(w, buf);
            break;
        }
    }
}

char* Runner_dumpStateJson(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    VMContext* vm = runner->vmContext;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);

    JsonWriter w = JsonWriter_create();

    JsonWriter_beginObject(&w);

    JsonWriter_propertyInt(&w, "frame", runner->frameCount);

    // Room info
    JsonWriter_key(&w, "room");
    JsonWriter_beginObject(&w);
    JsonWriter_propertyString(&w, "name", runner->currentRoom->name);
    JsonWriter_propertyInt(&w, "index", runner->currentRoomIndex);
    JsonWriter_endObject(&w);

    // Instances
    JsonWriter_key(&w, "instances");
    JsonWriter_beginArray(&w);

    repeat(instanceCount, i) {
        Instance* inst = runner->instances[i];
        if (inst == nullptr || !inst->active) continue;

        const char* objName = (inst->objectIndex >= 0 && dataWin->objt.count > (uint32_t) inst->objectIndex) ? dataWin->objt.objects[inst->objectIndex].name : nullptr;

        const char* spriteName = nullptr;
        if (inst->spriteIndex >= 0 && dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
            spriteName = dataWin->sprt.sprites[inst->spriteIndex].name;
        }

        JsonWriter_beginObject(&w);

        JsonWriter_propertyInt(&w, "instanceId", inst->instanceId);
        JsonWriter_propertyString(&w, "objectName", objName);
        JsonWriter_propertyInt(&w, "objectIndex", inst->objectIndex);
        JsonWriter_propertyDouble(&w, "x", inst->x);
        JsonWriter_propertyDouble(&w, "y", inst->y);
        JsonWriter_propertyInt(&w, "depth", inst->depth);

        // Sprite
        JsonWriter_key(&w, "sprite");
        JsonWriter_beginObject(&w);
        JsonWriter_propertyString(&w, "name", spriteName);
        JsonWriter_propertyInt(&w, "index", inst->spriteIndex);
        JsonWriter_propertyDouble(&w, "imageIndex", inst->imageIndex);
        JsonWriter_propertyDouble(&w, "imageSpeed", inst->imageSpeed);
        JsonWriter_endObject(&w);

        // Scale
        JsonWriter_key(&w, "scale");
        JsonWriter_beginObject(&w);
        JsonWriter_propertyDouble(&w, "x", inst->imageXscale);
        JsonWriter_propertyDouble(&w, "y", inst->imageYscale);
        JsonWriter_endObject(&w);

        JsonWriter_propertyDouble(&w, "angle", inst->imageAngle);
        JsonWriter_propertyDouble(&w, "alpha", inst->imageAlpha);
        JsonWriter_propertyInt(&w, "blend", inst->imageBlend);
        JsonWriter_propertyBool(&w, "visible", inst->visible);
        JsonWriter_propertyBool(&w, "active", inst->active);
        JsonWriter_propertyBool(&w, "solid", inst->solid);
        JsonWriter_propertyBool(&w, "persistent", inst->persistent);

        // Alarms
        JsonWriter_key(&w, "alarms");
        JsonWriter_beginObject(&w);
        repeat(GML_ALARM_COUNT, alarmIdx) {
            if (inst->alarm[alarmIdx] >= 0) {
                char alarmKey[4];
                snprintf(alarmKey, sizeof(alarmKey), "%d", alarmIdx);
                JsonWriter_propertyInt(&w, alarmKey, inst->alarm[alarmIdx]);
            }
        }
        JsonWriter_endObject(&w);

        // Self variables (non-array)
        JsonWriter_key(&w, "selfVariables");
        JsonWriter_beginObject(&w);
        repeat(dataWin->vari.variableCount, varIdx) {
            Variable* var = &dataWin->vari.variables[varIdx];
            if (var->instanceType != INSTANCE_SELF || var->varID < 0) continue;
            if ((uint32_t) var->varID >= inst->selfVarCount) continue;
            RValue val = inst->selfVars[var->varID];
            if (val.type == RVALUE_UNDEFINED) continue;

            JsonWriter_key(&w, var->name);
            writeRValueJson(&w, val);
        }
        JsonWriter_endObject(&w);

        // Self arrays
        JsonWriter_key(&w, "selfArrays");
        JsonWriter_beginObject(&w);
        int64_t selfArrayLen = hmlen(inst->selfArrayMap);
        if (selfArrayLen > 0) {
            repeat(selfArrayLen, arrIdx) {
                int64_t key = inst->selfArrayMap[arrIdx].key;
                RValue val = inst->selfArrayMap[arrIdx].value;
                int32_t varID = (int32_t) (key >> 32);
                int32_t arrayIndex = (int32_t) (key & 0xFFFFFFFF);

                // Find variable name
                const char* varName = nullptr;
                repeat(dataWin->vari.variableCount, varIdx) {
                    Variable* var = &dataWin->vari.variables[varIdx];
                    if (var->varID == varID && var->instanceType == INSTANCE_SELF) {
                        varName = var->name;
                        break;
                    }
                }

                if (varName == nullptr) continue;

                // Check if we already started this variable's object
                // We write arrays as "varName": {"0": val, "1": val, ...}
                // Since selfArrayMap entries may be interleaved, we build per-variable
                // For simplicity, write each entry as varName[index] flattened
                char compositeKey[256];
                snprintf(compositeKey, sizeof(compositeKey), "%s[%d]", varName, arrayIndex);
                JsonWriter_key(&w, compositeKey);
                writeRValueJson(&w, val);
            }
        }
        JsonWriter_endObject(&w);

        JsonWriter_endObject(&w);
    }

    JsonWriter_endArray(&w);

    // Global variables (non-array)
    JsonWriter_key(&w, "globalVariables");
    JsonWriter_beginObject(&w);
    repeat(dataWin->vari.variableCount, varIdx) {
        Variable* var = &dataWin->vari.variables[varIdx];
        if (var->instanceType != INSTANCE_GLOBAL || var->varID < 0) continue;
        if ((uint32_t) var->varID >= vm->globalVarCount) continue;
        RValue val = vm->globalVars[var->varID];
        if (val.type == RVALUE_UNDEFINED) continue;

        JsonWriter_key(&w, var->name);
        writeRValueJson(&w, val);
    }
    JsonWriter_endObject(&w);

    // Global arrays
    JsonWriter_key(&w, "globalArrays");
    JsonWriter_beginObject(&w);
    int64_t globalArrayLen = hmlen(vm->globalArrayMap);
    if (globalArrayLen > 0) {
        repeat(globalArrayLen, arrIdx) {
            int64_t key = vm->globalArrayMap[arrIdx].key;
            RValue val = vm->globalArrayMap[arrIdx].value;
            int32_t varID = (int32_t) (key >> 32);
            int32_t arrayIndex = (int32_t) (key & 0xFFFFFFFF);

            const char* varName = nullptr;
            repeat(dataWin->vari.variableCount, varIdx) {
                Variable* var = &dataWin->vari.variables[varIdx];
                if (var->varID == varID && var->instanceType == INSTANCE_GLOBAL) {
                    varName = var->name;
                    break;
                }
            }

            if (varName == nullptr) continue;

            char compositeKey[256];
            snprintf(compositeKey, sizeof(compositeKey), "%s[%d]", varName, arrayIndex);
            JsonWriter_key(&w, compositeKey);
            writeRValueJson(&w, val);
        }
    }
    JsonWriter_endObject(&w);

    JsonWriter_endObject(&w);

    char* result = JsonWriter_copyOutput(&w);
    JsonWriter_free(&w);
    return result;
}

void Runner_free(Runner* runner) {
    if (runner == nullptr) return;

    // Free all instances
    repeat(arrlen(runner->instances), i) {
        Instance_free(runner->instances[i]);
    }
    arrfree(runner->instances);

    RunnerKeyboard_free(runner->keyboard);
    free(runner);
}
