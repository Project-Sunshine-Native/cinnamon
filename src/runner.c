#include "runner.h"
#include "vm.h"
#include "utils.h"
#include "json_writer.h"
#include "collision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
    vm->currentInstance = instance;
}

static void restoreVMInstanceContext(VMContext* vm, Instance* savedInstance) {
    vm->currentInstance = savedInstance;
}

static void executeCode(Runner* runner, Instance* instance, int32_t codeId) {
    // GameMaker does use codeIds less than 0, we'll just pretend we didn't hear them...
    if (0 > codeId) return;

    VMContext* vm = runner->vmContext;

    // Save instance context
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
        savedStackValues = safeMalloc((uint32_t) savedStackTop * sizeof(RValue));
        memcpy(savedStackValues, vm->stack.slots, (uint32_t) savedStackTop * sizeof(RValue));
    }

    // Set instance context
    setVMInstanceContext(vm, instance);

    // Execute
    RValue result = VM_executeCode(vm, codeId);
    RValue_free(&result);

    // Restore instance context
    restoreVMInstanceContext(vm, savedInstance);

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

const char* Runner_getEventName(int32_t eventType, int32_t eventSubtype) {
    switch (eventType) {
        case EVENT_CREATE:  return "Create";
        case EVENT_DESTROY: return "Destroy";
        case EVENT_ALARM:   return "Alarm";
        case EVENT_COLLISION: return "Collision";
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
                case OTHER_OUTSIDE_ROOM:    return "OutsideRoom";
                case OTHER_GAME_START:      return "GameStart";
                case OTHER_ROOM_START:      return "RoomStart";
                case OTHER_ROOM_END:        return "RoomEnd";
                case OTHER_END_OF_PATH:     return "EndOfPath";
                case OTHER_USER0 +  0:      return "UserEvent0";
                case OTHER_USER0 +  1:      return "UserEvent1";
                case OTHER_USER0 +  2:      return "UserEvent2";
                case OTHER_USER0 +  3:      return "UserEvent3";
                case OTHER_USER0 +  4:      return "UserEvent4";
                case OTHER_USER0 +  5:      return "UserEvent5";
                case OTHER_USER0 +  6:      return "UserEvent6";
                case OTHER_USER0 +  7:      return "UserEvent7";
                case OTHER_USER0 +  8:      return "UserEvent8";
                case OTHER_USER0 +  9:      return "UserEvent9";
                case OTHER_USER0 + 10:      return "UserEvent10";
                case OTHER_USER0 + 11:      return "UserEvent11";
                case OTHER_USER0 + 12:      return "UserEvent12";
                case OTHER_USER0 + 13:      return "UserEvent13";
                case OTHER_USER0 + 14:      return "UserEvent14";
                case OTHER_USER0 + 15:      return "UserEvent15";
                default:                    return "Other";
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
        const char* eventName = Runner_getEventName(eventType, eventSubtype);
        const char* objectName = runner->dataWin->objt.objects[instance->objectIndex].name;

        bool shouldTrace = shgeti(vm->eventsToBeTraced, "*") != -1 || shgeti(vm->eventsToBeTraced, eventName) != -1 || shgeti(vm->eventsToBeTraced, objectName) != -1;

        if (shouldTrace) {
            if (eventType == EVENT_ALARM) {
                fprintf(stderr, "Runner: [%s] %s %d (instanceId=%d)\n", objectName, eventName, eventSubtype, instance->instanceId);
            } else {
                fprintf(stderr, "Runner: [%s] %s (instanceId=%d)\n", objectName, eventName, instance->instanceId);
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
        if (inst->active) {
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
            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, 0.0f, 0.0f, 0.0f, 0.0f, xscale, yscale, 0.0f, 0xFFFFFF, bg->alpha);
        } else if (bg->tileX || bg->tileY) {
            Renderer_drawBackgroundTiled(runner->renderer, tpagIndex, bg->x, bg->y, bg->tileX, bg->tileY, roomW, roomH, bg->alpha);
        } else {
            // Single placement
            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, bg->x, bg->y, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFF, bg->alpha);
        }
    }
}

// ===[ Draw ]===

typedef enum { DRAWABLE_TILE, DRAWABLE_INSTANCE } DrawableType;

typedef struct {
    DrawableType type;
    int32_t depth;
    union {
        Instance* instance;
        int32_t tileIndex; // index into currentRoom->tiles
    };
} Drawable;

static int compareDrawableDepth(const void* a, const void* b) {
    const Drawable* da = (const Drawable*) a;
    const Drawable* db = (const Drawable*) b;
    // Higher depth draws first (behind), lower depth draws last (in front)
    if (da->depth > db->depth) return -1;
    if (db->depth > da->depth) return 1;
    // At same depth, tiles before instances (tiles are background)
    if (da->type < db->type) return -1;
    if (db->type < da->type) return 1;
    // At same depth and type, preserve original room order (higher index draws later = in front)
    if (da->type == DRAWABLE_TILE) {
        if (db->tileIndex > da->tileIndex) return -1;
        if (da->tileIndex > db->tileIndex) return 1;
    }
    return 0;
}

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
    // Collect active + visible instances for event dispatch
    Instance** drawList = nullptr;
    int32_t count = (int32_t) arrlen(runner->instances);
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (inst->active && inst->visible) {
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

    // DRAW_NORMAL: build a unified drawable list of tiles + instances, sorted by depth
    Drawable* drawables = nullptr;

    // Add visible instances
    repeat(drawCount, i) {
        Drawable d = { .type = DRAWABLE_INSTANCE, .depth = drawList[i]->depth, .instance = drawList[i] };
        arrput(drawables, d);
    }

    // Add tiles (skip hidden layers)
    Room* room = runner->currentRoom;
    repeat(room->tileCount, i) {
        RoomTile* tile = &room->tiles[i];
        // Check if this tile's layer is hidden
        ptrdiff_t layerIdx = hmgeti(runner->tileLayerMap, tile->tileDepth);
        if (layerIdx >= 0 && !runner->tileLayerMap[layerIdx].value.visible) continue;

        Drawable d = { .type = DRAWABLE_TILE, .depth = tile->tileDepth, .tileIndex = (int32_t) i };
        arrput(drawables, d);
    }

    // Sort all drawables by depth
    int32_t drawableCount = (int32_t) arrlen(drawables);
    if (drawableCount > 1) {
        qsort(drawables, drawableCount, sizeof(Drawable), compareDrawableDepth);
    }

    // Draw interleaved tiles and instances
    repeat(drawableCount, i) {
        Drawable* d = &drawables[i];
        if (d->type == DRAWABLE_TILE) {
            if (runner->renderer != nullptr) {
                RoomTile* tile = &room->tiles[d->tileIndex];
                float offsetX = 0.0f, offsetY = 0.0f;
                ptrdiff_t layerIdx = hmgeti(runner->tileLayerMap, tile->tileDepth);
                if (layerIdx >= 0) {
                    offsetX = runner->tileLayerMap[layerIdx].value.offsetX;
                    offsetY = runner->tileLayerMap[layerIdx].value.offsetY;
                }

                // Trace tile drawing if requested
                if (shlen(runner->vmContext->tilesToBeTraced) > 0) {
                    DataWin* dataWin = runner->dataWin;
                    const char* bgName = (tile->backgroundDefinition >= 0 && dataWin->bgnd.count > (uint32_t) tile->backgroundDefinition) ? dataWin->bgnd.backgrounds[tile->backgroundDefinition].name : "<none>";
                    const char* roomName = room->name;

                    bool shouldTrace = shgeti(runner->vmContext->tilesToBeTraced, "*") != -1 || shgeti(runner->vmContext->tilesToBeTraced, bgName) != -1 || shgeti(runner->vmContext->tilesToBeTraced, roomName) != -1;

                    if (shouldTrace) {
                        int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(dataWin, tile->backgroundDefinition);
                        if (tpagIndex >= 0) {
                            TexturePageItem* tpag = &dataWin->tpag.items[tpagIndex];
                            fprintf(stderr, "Runner: [%s] Drawing tile #%d bg=%s(%d) tpag(srcX=%d srcY=%d srcW=%d srcH=%d tgtX=%d tgtY=%d bndW=%d bndH=%d page=%d) tile(srcX=%d srcY=%d w=%u h=%u) at pos=(%d,%d) depth=%d\n", roomName, d->tileIndex, bgName, tile->backgroundDefinition, tpag->sourceX, tpag->sourceY, tpag->sourceWidth, tpag->sourceHeight, tpag->targetX, tpag->targetY, tpag->boundingWidth, tpag->boundingHeight, tpag->texturePageId, tile->sourceX, tile->sourceY, tile->width, tile->height, tile->x, tile->y, tile->tileDepth);

                            // Warn if tile source rect exceeds TPAG content bounds
                            if ((uint32_t) (tile->sourceX + tile->width) > (uint32_t) tpag->sourceWidth || (uint32_t) (tile->sourceY + tile->height) > (uint32_t) tpag->sourceHeight) {
                                fprintf(stderr, "Runner: [%s] WARNING: Tile #%d source rect (%d,%d %ux%u) exceeds TPAG content bounds (%dx%d)\n", roomName, d->tileIndex, tile->sourceX, tile->sourceY, tile->width, tile->height, tpag->sourceWidth, tpag->sourceHeight);
                            }
                        } else {
                            fprintf(stderr, "Runner: [%s] Drawing tile #%d bg=%s(%d) tpag=UNRESOLVED tile(srcX=%d srcY=%d w=%u h=%u) at pos=(%d,%d) depth=%d\n", roomName, d->tileIndex, bgName, tile->backgroundDefinition, tile->sourceX, tile->sourceY, tile->width, tile->height, tile->x, tile->y, tile->tileDepth);
                        }
                    }
                }

                Renderer_drawTile(runner->renderer, tile, offsetX, offsetY);
            }
        } else {
            Instance* inst = d->instance;
            int32_t codeId = findEventCodeIdAndOwner(runner->dataWin, inst->objectIndex, EVENT_DRAW, DRAW_NORMAL, nullptr);
            if (codeId >= 0) {
                Runner_executeEvent(runner, inst, EVENT_DRAW, DRAW_NORMAL);
            } else if (runner->renderer != nullptr) {
                Renderer_drawSelf(runner->renderer, inst);
            }
        }
    }

    arrfree(drawables);

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

    Instance* inst = Instance_create(instanceId, objectIndex, x, y);

    // Copy properties from object definition
    inst->spriteIndex = objDef->spriteId;
    inst->visible = objDef->visible;
    inst->solid = objDef->solid;
    inst->persistent = objDef->persistent;
    inst->depth = objDef->depth;
    inst->maskIndex = objDef->textureMaskId;

    arrput(runner->instances, inst);

    if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, objDef->name) != -1) {
        fprintf(stderr, "VM: Instance %s (%d) created at (%f, %f)\n", objDef->name, instanceId, x, y);
    }

    return inst;
}

// ===[ Room Management ]===

static void initRoom(Runner* runner, int32_t roomIndex) {
    DataWin* dataWin = runner->dataWin;
    require(roomIndex >= 0 && dataWin->room.count > (uint32_t) roomIndex);

    Room* room = &dataWin->room.rooms[roomIndex];
    SavedRoomState* savedState = &runner->savedRoomStates[roomIndex];

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

    // If this is a persistent room that was previously visited, restore saved state
    if (room->persistent && savedState->initialized) {
        // Restore backgrounds from saved state
        memcpy(runner->backgrounds, savedState->backgrounds, sizeof(runner->backgrounds));
        runner->backgroundColor = savedState->backgroundColor;
        runner->drawBackgroundColor = savedState->drawBackgroundColor;

        // Restore tile layer map
        hmfree(runner->tileLayerMap);
        runner->tileLayerMap = savedState->tileLayerMap;
        savedState->tileLayerMap = nullptr;

        // Keep only persistent instances (which travel between rooms), free non-persistent
        // ones from the previous room. When the old room was also persistent, Runner_step
        // already separated them; when it was NOT persistent, they're still here.
        Instance** keptInstances = nullptr;
        int32_t oldCount = (int32_t) arrlen(runner->instances);
        repeat(oldCount, i) {
            Instance* inst = runner->instances[i];
            if (inst->persistent) {
                arrput(keptInstances, inst);
            } else {
                Instance_free(inst);
            }
        }
        arrfree(runner->instances);
        runner->instances = keptInstances;

        // Add back the saved room instances
        int32_t savedCount = (int32_t) arrlen(savedState->instances);
        repeat(savedCount, i) {
            arrput(runner->instances, savedState->instances[i]);
        }
        arrfree(savedState->instances);
        savedState->instances = nullptr;

        // No Create events, no preCreateCode, no creationCode, no room creation code
        fprintf(stderr, "Runner: Room restored (persistent): %s (room %d) with %d instances\n", room->name, roomIndex, (int) arrlen(runner->instances));
        return;
    }

    // === Normal room initialization (first visit, or non-persistent room) ===

    // Reset tile layer state for the new room
    hmfree(runner->tileLayerMap);
    runner->tileLayerMap = nullptr;

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
        dst->alpha = 1.0f;
    }

    // Handle persistent instances: keep persistent ones, free non-persistent
    Instance** keptInstances = nullptr;
    int32_t oldCount = (int32_t) arrlen(runner->instances);
    repeat(oldCount, i) {
        Instance* inst = runner->instances[i];
        if (inst->persistent) {
            arrput(keptInstances, inst);
        } else {
            Instance_free(inst);
        }
    }
    arrfree(runner->instances);
    runner->instances = keptInstances;

    // Two-pass instance creation (matches HTML5 runner behavior):
    // Pass 1: Create all instance objects so they exist for cross-references
    // Pass 2: Fire preCreateCode, CREATE events, and creationCode
    // This ensures that when an instance's Create event reads another instance
    // (e.g. obj_mainchara reading obj_markerA.x), the target already exists.

    // Pass 1: Create all instances without firing events
    repeat(room->gameObjectCount, i) {
        RoomGameObject* roomObj = &room->gameObjects[i];

        // Check if a persistent instance with this ID already exists
        bool alreadyExists = false;
        repeat(arrlen(runner->instances), j) {
            if (runner->instances[j]->instanceId == roomObj->instanceID) {
                alreadyExists = true;
                break;
            }
        }
        if (alreadyExists) continue;

        Instance* inst = createAndInitInstance(runner, roomObj->instanceID, roomObj->objectDefinition, (double) roomObj->x, (double) roomObj->y);
        inst->imageXscale = (double) roomObj->scaleX;
        inst->imageYscale = (double) roomObj->scaleY;
        inst->imageAngle = (double) roomObj->rotation;
    }

    // Pass 2: Fire events for newly created instances (in room definition order)
    repeat(room->gameObjectCount, i) {
        RoomGameObject* roomObj = &room->gameObjects[i];

        // Find the instance we created (skip persistent ones that were kept)
        Instance* inst = nullptr;
        repeat(arrlen(runner->instances), j) {
            if (runner->instances[j]->instanceId == roomObj->instanceID) {
                inst = runner->instances[j];
                break;
            }
        }
        if (inst == nullptr) continue;

        // Skip instances that already had their Create event fired (persistent carry-overs)
        if (inst->createEventFired) continue;
        inst->createEventFired = true;

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

    // Mark this room as initialized for persistent room support
    savedState->initialized = true;

    fprintf(stderr, "Runner: Room loaded: %s (room %d) with %d instances\n", room->name, roomIndex, (int) arrlen(runner->instances));
}

// ===[ Public API ]===

Runner* Runner_create(DataWin* dataWin, VMContext* vm, FileSystem* fileSystem) {
    Runner* runner = safeCalloc(1, sizeof(Runner));
    runner->dataWin = dataWin;
    runner->vmContext = vm;
    runner->fileSystem = fileSystem;
    runner->frameCount = 0;
    runner->instances = nullptr;
    runner->pendingRoom = -1;
    runner->gameStartFired = false;
    runner->currentRoomIndex = -1;
    runner->currentRoomOrderPosition = -1;
    runner->nextInstanceId = dataWin->gen8.lastObj + 1;
    runner->keyboard = RunnerKeyboard_create();
    runner->savedRoomStates = safeCalloc(dataWin->room.count, sizeof(SavedRoomState));

    // Link runner to VM context
    vm->runner = (struct Runner*) runner;

    return runner;
}

Instance* Runner_createInstance(Runner* runner, double x, double y, int32_t objectIndex) {
    Instance* inst = createAndInitInstance(runner, runner->nextInstanceId++, objectIndex, x, y);
    inst->createEventFired = true;
    Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
    return inst;
}

void Runner_destroyInstance(Runner* runner, Instance* inst) {
    (void) runner;
    GameObject* gameObject = &runner->dataWin->objt.objects[inst->objectIndex];
    Runner_executeEvent(runner, inst, EVENT_DESTROY, 0);
    inst->active = false;

    if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, gameObject->name) != -1) {
        fprintf(stderr, "VM: Instance %s (%d) destroyed\n", gameObject->name, inst->instanceId);
    }
}

void Runner_cleanupDestroyedInstances(Runner* runner) {
    int32_t count = (int32_t) arrlen(runner->instances);
    int32_t writeIdx = 0;
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (inst->active) {
            runner->instances[writeIdx++] = inst;
        } else {
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

// ===[ Collision Event Dispatch ]===

static void executeCollisionEvent(Runner* runner, Instance* self, Instance* other, int32_t targetObjectIndex) {
    VMContext* vm = runner->vmContext;

    // Save event context
    int32_t savedEventType = vm->currentEventType;
    int32_t savedEventSubtype = vm->currentEventSubtype;
    int32_t savedEventObjectIndex = vm->currentEventObjectIndex;
    struct Instance* savedOtherInstance = vm->otherInstance;

    // Set collision event context
    vm->currentEventType = EVENT_COLLISION;
    vm->currentEventSubtype = targetObjectIndex;
    vm->otherInstance = other;

    int32_t ownerObjectIndex = -1;
    int32_t codeId = findEventCodeIdAndOwner(runner->dataWin, self->objectIndex, EVENT_COLLISION, targetObjectIndex, &ownerObjectIndex);

    vm->currentEventObjectIndex = ownerObjectIndex;

    if (codeId >= 0 && shlen(vm->eventsToBeTraced) != -1) {
        const char* selfName = runner->dataWin->objt.objects[self->objectIndex].name;
        const char* targetName = runner->dataWin->objt.objects[targetObjectIndex].name;
        bool shouldTrace = shgeti(vm->eventsToBeTraced, "*") != -1 || shgeti(vm->eventsToBeTraced, "Collision") != -1 || shgeti(vm->eventsToBeTraced, selfName) != -1;
        if (shouldTrace) {
            fprintf(stderr, "Runner: [%s] Collision with %s (instanceId=%d, otherId=%d)\n", selfName, targetName, self->instanceId, other->instanceId);
        }
    }

    executeCode(runner, self, codeId);

    // Restore event context
    vm->currentEventType = savedEventType;
    vm->currentEventSubtype = savedEventSubtype;
    vm->currentEventObjectIndex = savedEventObjectIndex;
    vm->otherInstance = savedOtherInstance;
}

static void dispatchCollisionEvents(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    int32_t count = (int32_t) arrlen(runner->instances);

    repeat(count, i) {
        Instance* self = runner->instances[i];
        if (!self->active) continue;

        // Walk the parent chain to find all collision event handlers for this object
        int32_t currentObj = self->objectIndex;
        int depth = 0;
        while (currentObj >= 0 && dataWin->objt.count > (uint32_t) currentObj && 32 > depth) {
            GameObject* obj = &dataWin->objt.objects[currentObj];

            if (OBJT_EVENT_TYPE_COUNT > EVENT_COLLISION) {
                ObjectEventList* eventList = &obj->eventLists[EVENT_COLLISION];
                repeat(eventList->eventCount, evtIdx) {
                    ObjectEvent* evt = &eventList->events[evtIdx];
                    int32_t targetObjIndex = (int32_t) evt->eventSubtype;

                    if (evt->actionCount == 0 || 0 > evt->actions[0].codeId) continue;

                    // Check all instances of the target object
                    repeat(count, j) {
                        Instance* other = runner->instances[j];
                        if (!other->active) continue;
                        if (other == self) continue;
                        if (!VM_isObjectOrDescendant(dataWin, other->objectIndex, targetObjIndex)) continue;

                        // Compute bboxes
                        InstanceBBox bboxSelf = Collision_computeBBox(dataWin, self);
                        InstanceBBox bboxOther = Collision_computeBBox(dataWin, other);
                        if (!bboxSelf.valid || !bboxOther.valid) continue;

                        // AABB overlap test
                        if (bboxSelf.left >= bboxOther.right || bboxOther.left >= bboxSelf.right ||
                            bboxSelf.top >= bboxOther.bottom || bboxOther.top >= bboxSelf.bottom) continue;

                        // Precise collision check if either sprite needs it
                        Sprite* sprSelf = Collision_getSprite(dataWin, self);
                        Sprite* sprOther = Collision_getSprite(dataWin, other);
                        bool needsPrecise = (sprSelf != nullptr && sprSelf->sepMasks == 1) || (sprOther != nullptr && sprOther->sepMasks == 1);

                        if (needsPrecise) {
                            if (!Collision_instancesOverlapPrecise(dataWin, self, other, bboxSelf, bboxOther)) continue;
                        }

                        // Collision detected! If either instance is solid, restore both to xprevious/yprevious
                        if (self->solid || other->solid) {
                            self->x = self->xprevious;
                            self->y = self->yprevious;
                            other->x = other->xprevious;
                            other->y = other->yprevious;
                        }

                        executeCollisionEvent(runner, self, other, targetObjIndex);
                    }
                }
            }

            currentObj = obj->parentId;
            depth++;
        }
    }
}

// ===[ View Following + Clamping ]===
// Single-axis follow with border-based scrolling, room clamping, and speed limit.
static int32_t followAxis(int32_t viewPos, int32_t viewSize, int32_t targetPos, uint32_t border, int32_t speed, int32_t roomSize) {
    int32_t pos = viewPos;

    // Border-based scrolling
    if (2 * (int32_t) border >= viewSize) {
        pos = targetPos - viewSize / 2;
    } else if (targetPos - (int32_t) border < viewPos) {
        pos = targetPos - (int32_t) border;
    } else if (targetPos + (int32_t) border > viewPos + viewSize) {
        pos = targetPos + (int32_t) border - viewSize;
    }

    // Clamp to room bounds
    if (0 > pos) pos = 0;
    if (pos + viewSize > roomSize) pos = roomSize - viewSize;

    // Speed limit
    if (speed >= 0) {
        if (pos < viewPos && viewPos - pos > speed) pos = viewPos - speed;
        if (pos > viewPos && pos - viewPos > speed) pos = viewPos + speed;
    }

    return pos;
}

static void updateViews(Runner* runner) {
    Room* room = runner->currentRoom;
    if (!(room->flags & 1)) return;

    for (int32_t vi = 0; 8 > vi; vi++) {
        RoomView* view = &room->views[vi];
        if (!view->enabled || 0 > view->objectId) continue;

        // Find first active instance of the target object
        Instance* target = nullptr;
        int32_t count = (int32_t) arrlen(runner->instances);
        for (int32_t i = 0; count > i; i++) {
            Instance* inst = runner->instances[i];
            if (inst->active && inst->objectIndex == view->objectId) { target = inst; break; }
        }
        if (target == nullptr) continue;

        int32_t ix = (int32_t) floor(target->x);
        int32_t iy = (int32_t) floor(target->y);

        view->viewX = followAxis(view->viewX, view->viewWidth, ix, view->borderX, view->speedX, (int32_t) room->width);
        view->viewY = followAxis(view->viewY, view->viewHeight, iy, view->borderY, view->speedY, (int32_t) room->height);
    }
}

static void dispatchOutsideRoomEvents(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    int32_t roomWidth = (int32_t) runner->currentRoom->width;
    int32_t roomHeight = (int32_t) runner->currentRoom->height;
    int32_t count = (int32_t) arrlen(runner->instances);

    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;

        // Early-out: skip instances whose object has no Outside Room event
        if (0 > findEventCodeIdAndOwner(dataWin, inst->objectIndex, EVENT_OTHER, OTHER_OUTSIDE_ROOM, nullptr)) continue;

        // Compute bounding box
        bool outside;
        InstanceBBox bbox = Collision_computeBBox(dataWin, inst);
        if (bbox.valid) {
            outside = (0 > bbox.right || bbox.left > roomWidth || 0 > bbox.bottom || bbox.top > roomHeight);
        } else {
            // No sprite/mask: use raw position as a point
            outside = (0 > inst->x || inst->x > roomWidth || 0 > inst->y || inst->y > roomHeight);
        }

        // Fire event only on inside-to-outside transition (edge-triggered)
        if (outside && !inst->outsideRoom) {
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_OUTSIDE_ROOM);
            if (runner->pendingRoom >= 0) break;
        }

        inst->outsideRoom = outside;
    }
}

// ===[ Path Adaptation ]===
// Advances path position and updates instance x/y (HTML5: yyInstance.js Adapt_Path, lines 2755-2881)
// Returns true if end of path was reached (and pathSpeed != 0), to fire OTHER_END_OF_PATH event.
static bool adaptPath(Runner* runner, Instance* inst) {
    if (0 > inst->pathIndex) return false;

    DataWin* dataWin = runner->dataWin;
    if ((uint32_t) inst->pathIndex >= dataWin->path.count) return false;

    GamePath* path = &dataWin->path.paths[inst->pathIndex];
    if (0.0 >= path->length) return false;

    bool atPathEnd = false;

    double orient = inst->pathOrientation * M_PI / 180.0;

    // Get current position's speed factor
    PathPositionResult cur = GamePath_getPosition(path, inst->pathPosition);
    double sp = cur.speed / (100.0 * inst->pathScale);

    // Advance position
    inst->pathPosition = inst->pathPosition + inst->pathSpeed * sp / path->length;

    // Handle end actions if position out of [0,1]
    PathPositionResult pos0 = GamePath_getPosition(path, 0.0);
    if (inst->pathPosition >= 1.0 || 0.0 >= inst->pathPosition) {
        atPathEnd = (inst->pathSpeed == 0.0) ? false : true;

        switch (inst->pathEndAction) {
            // stop moving
            case 0: {
                if (inst->pathSpeed != 0.0) {
                    inst->pathPosition = 1.0;
                    inst->pathIndex = -1;
                }
                break;
            }
            // continue from start position (restart)
            case 1: {
                if (0.0 > inst->pathPosition) {
                    inst->pathPosition += 1.0;
                } else {
                    inst->pathPosition -= 1.0;
                }
                break;
            }
            // continue from current position
            case 2: {
                PathPositionResult pos1 = GamePath_getPosition(path, 1.0);
                double xx = pos1.x - pos0.x;
                double yy = pos1.y - pos0.y;
                double xdif = inst->pathScale * (xx * cos(orient) + yy * sin(orient));
                double ydif = inst->pathScale * (yy * cos(orient) - xx * sin(orient));

                if (0.0 > inst->pathPosition) {
                    inst->pathXStart -= xdif;
                    inst->pathYStart -= ydif;
                    inst->pathPosition += 1.0;
                } else {
                    inst->pathXStart += xdif;
                    inst->pathYStart += ydif;
                    inst->pathPosition -= 1.0;
                }
                break;
            }
            // reverse
            case 3: {
                if (0.0 > inst->pathPosition) {
                    inst->pathPosition = -inst->pathPosition;
                    inst->pathSpeed = fabs(inst->pathSpeed);
                } else {
                    inst->pathPosition = 2.0 - inst->pathPosition;
                    inst->pathSpeed = -fabs(inst->pathSpeed);
                }
                break;
            }
            // default: stop
            default: {
                inst->pathPosition = 1.0;
                inst->pathIndex = -1;
                break;
            }
        }
    }

    // Find the new position in the room
    PathPositionResult newPos = GamePath_getPosition(path, inst->pathPosition);
    double xx = newPos.x - pos0.x; // relative
    double yy = newPos.y - pos0.y;

    double newx = inst->pathXStart + inst->pathScale * (xx * cos(orient) + yy * sin(orient));
    double newy = inst->pathYStart + inst->pathScale * (yy * cos(orient) - xx * sin(orient));

    // Trick to set the direction: set hspeed/vspeed to delta, which updates direction
    inst->hspeed = newx - inst->x;
    inst->vspeed = newy - inst->y;
    Instance_computeSpeedFromComponents(inst);

    // Normal speed should not be used
    inst->speed = 0.0;
    inst->hspeed = 0.0;
    inst->vspeed = 0.0;

    // Set the new position
    inst->x = newx;
    inst->y = newy;

    return atPathEnd;
}

void Runner_step(Runner* runner) {
    // Save xprevious/yprevious and path_positionprevious for all active instances
    int32_t prevCount = (int32_t) arrlen(runner->instances);
    repeat(prevCount, i) {
        Instance* inst = runner->instances[i];
        if (inst->active) {
            inst->xprevious = inst->x;
            inst->yprevious = inst->y;
            inst->pathPositionPrevious = inst->pathPosition;
        }
    }

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
        if (!inst->active) continue;

        GameObject* object = &runner->dataWin->objt.objects[inst->objectIndex];

        repeat(GML_ALARM_COUNT, alarmIdx) {
            if (inst->alarm[alarmIdx] > 0) {
                if (shgeti(runner->vmContext->alarmsToBeTraced, "*") != -1 || shgeti(runner->vmContext->alarmsToBeTraced, object->name) != -1) {
                    fprintf(stderr, "VM: [%s] Ticking down Alarm[%d] (instanceId=%d), current tick is %d\n", object->name, alarmIdx, inst->instanceId, inst->alarm[alarmIdx]);
                }

                inst->alarm[alarmIdx]--;
                if (inst->alarm[alarmIdx] == 0) {
                    inst->alarm[alarmIdx] = -1;

                    if (shgeti(runner->vmContext->alarmsToBeTraced, "*") != -1 || shgeti(runner->vmContext->alarmsToBeTraced, object->name) != -1) {
                        fprintf(stderr, "VM: [%s] Firing Alarm[%d] (instanceId=%d)\n", object->name, alarmIdx, inst->instanceId);
                    }

                    Runner_executeEvent(runner, inst, EVENT_ALARM, alarmIdx);
                }
            }
        }
    }

    // Execute Normal Step for all instances
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_NORMAL);

    // Apply motion: friction, gravity, then x += hspeed, y += vspeed
    int32_t motionCount = (int32_t) arrlen(runner->instances);
    repeat(motionCount, mi) {
        Instance* inst = runner->instances[mi];
        if (!inst->active) continue;

        // Friction: reduce speed toward zero (HTML5: AdaptSpeed)
        if (inst->friction != 0.0) {
            double ns = (inst->speed > 0.0) ? inst->speed - inst->friction : inst->speed + inst->friction;
            if ((inst->speed > 0.0 && ns < 0.0) || (inst->speed < 0.0 && ns > 0.0)) {
                inst->speed = 0.0;
            } else if (inst->speed != 0.0) {
                inst->speed = ns;
            }
            Instance_computeComponentsFromSpeed(inst);
        }

        // Gravity: add velocity in gravity_direction (HTML5: AddTo_Speed)
        if (inst->gravity != 0.0) {
            double gravDirRad = inst->gravityDirection * (M_PI / 180.0);
            inst->hspeed += inst->gravity * clampFloat(cos(gravDirRad));
            inst->vspeed -= inst->gravity * clampFloat(sin(gravDirRad));
            Instance_computeSpeedFromComponents(inst);
        }

        // Path adaptation (HTML5: Adapt_Path, runs after friction/gravity, before x+=hspeed)
        if (adaptPath(runner, inst)) {
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_END_OF_PATH);
        }

        // Apply movement
        if (inst->hspeed != 0.0 || inst->vspeed != 0.0) {
            inst->x += inst->hspeed;
            inst->y += inst->vspeed;
        }
    }

    // Dispatch outside room events
    dispatchOutsideRoomEvents(runner);

    // Dispatch collision events
    dispatchCollisionEvents(runner);

    // Execute End Step for all instances
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_END);

    // Update view following and clamping
    updateViews(runner);

    // Advance image_index by image_speed for all active instances
    int32_t animCount = (int32_t) arrlen(runner->instances);
    repeat(animCount, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;
        if (0 > inst->spriteIndex) continue;

        inst->imageIndex += inst->imageSpeed;

        // Wrap image_index (matches HTML5 runner: manual subtract/add instead of using fmod)
        Sprite* sprite = &runner->dataWin->sprt.sprites[inst->spriteIndex];
        double frameCount = (double) sprite->textureCount;
        if (inst->imageIndex >= frameCount) {
            inst->imageIndex -= frameCount;
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_ANIMATION_END);
        } else if (0.0 > inst->imageIndex) {
            inst->imageIndex += frameCount;
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_ANIMATION_END);
        }
    }

    // Handle room transition
    if (runner->pendingRoom >= 0) {
        int32_t oldRoomIndex = runner->currentRoomIndex;
        Room* oldRoom = runner->currentRoom;
        const char* oldRoomName = oldRoom->name;

        // Fire Room End for all instances
        Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_END);

        int32_t newRoomIndex = runner->pendingRoom;
        require(runner->dataWin->room.count > (uint32_t) newRoomIndex);
        const char* newRoomName = runner->dataWin->room.rooms[newRoomIndex].name;

        fprintf(stderr, "Room changed: %s (room %d) -> %s (room %d)\n", oldRoomName, oldRoomIndex, newRoomName, newRoomIndex);

        // If the old room is persistent, save its instance and visual state
        if (oldRoom->persistent) {
            SavedRoomState* state = &runner->savedRoomStates[oldRoomIndex];

            // Free any previously saved instances (from an earlier visit)
            int32_t prevSavedCount = (int32_t) arrlen(state->instances);
            repeat(prevSavedCount, i) {
                Instance_free(state->instances[i]);
            }
            arrfree(state->instances);
            state->instances = nullptr;
            hmfree(state->tileLayerMap);
            state->tileLayerMap = nullptr;

            // Separate persistent instances (travel with player) from room instances (saved)
            Instance** keptInstances = nullptr;
            int32_t count = (int32_t) arrlen(runner->instances);
            repeat(count, i) {
                Instance* inst = runner->instances[i];
                if (inst->persistent) {
                    arrput(keptInstances, inst);
                } else if (inst->active) {
                    arrput(state->instances, inst);
                } else {
                    Instance_free(inst);
                }
            }
            arrfree(runner->instances);
            runner->instances = keptInstances;

            // Save room visual state
            memcpy(state->backgrounds, runner->backgrounds, sizeof(runner->backgrounds));
            state->backgroundColor = runner->backgroundColor;
            state->drawBackgroundColor = runner->drawBackgroundColor;

            // Transfer tile layer map ownership to saved state
            state->tileLayerMap = runner->tileLayerMap;
            runner->tileLayerMap = nullptr;

            state->initialized = true;
        }

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
        if (!inst->active) continue;

        GameObject* gameObject = nullptr;
        const char* objName = "<unknown>";
        if (inst->objectIndex >= 0 && dataWin->objt.count > (uint32_t) inst->objectIndex) {
            gameObject = &dataWin->objt.objects[inst->objectIndex];
            objName = gameObject->name;
        }

        const char* spriteName = "<none>";
        if (inst->spriteIndex >= 0 && dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
            spriteName = dataWin->sprt.sprites[inst->spriteIndex].name;
        }

        const char* parentName = "<none>";
        if (gameObject != nullptr && gameObject->parentId >= 0 && dataWin->objt.count > (uint32_t) gameObject->parentId) {
            parentName = dataWin->objt.objects[gameObject->parentId].name;
        }

        printf("\n--- Instance #%d (%s, objectIndex=%d) ---\n", inst->instanceId, objName, inst->objectIndex);
        printf("  Position: (%g, %g)\n", inst->x, inst->y);
        printf("  Depth: %d\n", inst->depth);
        printf("  Sprite: %s (index %d), imageIndex=%g, imageSpeed=%g\n", spriteName, inst->spriteIndex, inst->imageIndex, inst->imageSpeed);
        printf("  Scale: (%g, %g), Angle: %g, Alpha: %g, Blend: 0x%06X\n", inst->imageXscale, inst->imageYscale, inst->imageAngle, inst->imageAlpha, inst->imageBlend);
        printf("  Visible: %s, Active: %s, Solid: %s, Persistent: %s\n", inst->visible ? "true" : "false", inst->active ? "true" : "false", inst->solid ? "true" : "false", inst->persistent ? "true" : "false");
        printf("  Parent: %s (parentId=%d)\n", parentName, gameObject != nullptr ? gameObject->parentId : -1);

        // Active alarms
        bool hasAlarm = false;
        repeat(GML_ALARM_COUNT, alarmIdx) {
            if (inst->alarm[alarmIdx] >= 0) {
                if (!hasAlarm) { printf("  Alarms:"); hasAlarm = true; }
                printf(" [%d]=%d", alarmIdx, inst->alarm[alarmIdx]);
            }
        }
        if (hasAlarm) printf("\n");

        // Self variables (non-array, sparse hashmap)
        bool hasSelfVars = false;
        repeat(hmlen(inst->selfVars), svIdx) {
            int32_t varID = inst->selfVars[svIdx].key;
            RValue val = inst->selfVars[svIdx].value;
            if (val.type == RVALUE_UNDEFINED) continue;

            // Resolve variable name from VARI chunk
            const char* varName = "?";
            repeat(dataWin->vari.variableCount, varIdx) {
                Variable* var = &dataWin->vari.variables[varIdx];
                if (var->instanceType == INSTANCE_SELF && var->varID == varID) {
                    varName = var->name;
                    break;
                }
            }

            if (!hasSelfVars) { printf("  Self Variables:\n"); hasSelfVars = true; }
            char* valStr = RValue_toStringFancy(val);
            printf("    %s = %s\n", varName, valStr);
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
        if (!inst->active) continue;

        const char* objName = (inst->objectIndex >= 0 && dataWin->objt.count > (uint32_t) inst->objectIndex) ? dataWin->objt.objects[inst->objectIndex].name : nullptr;

        const char* spriteName = nullptr;
        if (inst->spriteIndex >= 0 && dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
            spriteName = dataWin->sprt.sprites[inst->spriteIndex].name;
        }

        JsonWriter_beginObject(&w);

        JsonWriter_propertyInt(&w, "instanceId", inst->instanceId);
        JsonWriter_propertyString(&w, "objectName", objName);
        JsonWriter_propertyInt(&w, "objectIndex", inst->objectIndex);

        // Parent object
        const char* parentName = nullptr;
        int32_t parentId = -1;
        if (inst->objectIndex >= 0 && dataWin->objt.count > (uint32_t) inst->objectIndex) {
            parentId = dataWin->objt.objects[inst->objectIndex].parentId;
            if (parentId >= 0 && dataWin->objt.count > (uint32_t) parentId) {
                parentName = dataWin->objt.objects[parentId].name;
            }
        }
        JsonWriter_propertyString(&w, "parentObjectName", parentName);
        JsonWriter_propertyInt(&w, "parentObjectIndex", parentId);

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

        // Self variables (non-array, sparse hashmap)
        JsonWriter_key(&w, "selfVariables");
        JsonWriter_beginObject(&w);
        repeat(hmlen(inst->selfVars), svIdx) {
            int32_t varID = inst->selfVars[svIdx].key;
            RValue val = inst->selfVars[svIdx].value;
            if (val.type == RVALUE_UNDEFINED) continue;

            // Resolve variable name from VARI chunk
            const char* varName = "?";
            repeat(dataWin->vari.variableCount, varIdx) {
                Variable* var = &dataWin->vari.variables[varIdx];
                if (var->instanceType == INSTANCE_SELF && var->varID == varID) {
                    varName = var->name;
                    break;
                }
            }

            JsonWriter_key(&w, varName);
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

    // Tiles
    Room* dumpRoom = runner->currentRoom;
    JsonWriter_key(&w, "tiles");
    JsonWriter_beginArray(&w);
    repeat(dumpRoom->tileCount, tileIdx) {
        RoomTile* tile = &dumpRoom->tiles[tileIdx];
        const char* bgName = (tile->backgroundDefinition >= 0 && dataWin->bgnd.count > (uint32_t) tile->backgroundDefinition) ? dataWin->bgnd.backgrounds[tile->backgroundDefinition].name : nullptr;

        JsonWriter_beginObject(&w);
        JsonWriter_propertyInt(&w, "index", tileIdx);
        JsonWriter_propertyInt(&w, "x", tile->x);
        JsonWriter_propertyInt(&w, "y", tile->y);
        JsonWriter_propertyInt(&w, "backgroundIndex", tile->backgroundDefinition);
        if (bgName != nullptr) {
            JsonWriter_propertyString(&w, "backgroundName", bgName);
        } else {
            JsonWriter_propertyNull(&w, "backgroundName");
        }
        JsonWriter_propertyInt(&w, "sourceX", tile->sourceX);
        JsonWriter_propertyInt(&w, "sourceY", tile->sourceY);
        JsonWriter_propertyInt(&w, "width", tile->width);
        JsonWriter_propertyInt(&w, "height", tile->height);
        JsonWriter_propertyInt(&w, "depth", tile->tileDepth);
        JsonWriter_propertyInt(&w, "instanceID", tile->instanceID);
        JsonWriter_propertyDouble(&w, "scaleX", tile->scaleX);
        JsonWriter_propertyDouble(&w, "scaleY", tile->scaleY);
        JsonWriter_propertyInt(&w, "color", tile->color);

        ptrdiff_t layerIdx = hmgeti(runner->tileLayerMap, tile->tileDepth);
        bool visible = (layerIdx >= 0) ? runner->tileLayerMap[layerIdx].value.visible : true;
        JsonWriter_propertyBool(&w, "visible", visible);
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

    // Free saved room states
    if (runner->savedRoomStates != nullptr) {
        repeat(runner->dataWin->room.count, i) {
            SavedRoomState* state = &runner->savedRoomStates[i];
            int32_t savedCount = (int32_t) arrlen(state->instances);
            repeat(savedCount, j) {
                Instance_free(state->instances[j]);
            }
            arrfree(state->instances);
            hmfree(state->tileLayerMap);
        }
        free(runner->savedRoomStates);
    }

    hmfree(runner->tileLayerMap);
    RunnerKeyboard_free(runner->keyboard);
    free(runner);
}
