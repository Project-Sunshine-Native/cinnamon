#include "runner.h"
#include "data_win.h"
#include "instance.h"
#include "renderer.h"
#include "vm.h"
#include "utils.h"
#include "json_writer.h"
#include "collision.h"

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_ds.h"

__attribute__((weak)) void Runner_platformBootLog(const char* message) {
    (void) message;
}

static void Runner_bootLog(const char* message) {
    Runner_platformBootLog(message);
}

static void Runner_bootLogf(const char* format, ...) {
    char buffer[192];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Runner_platformBootLog(buffer);
}

// ===[ Runtime Layer Teardown Helpers ]===
void Runner_freeRuntimeLayer(RuntimeLayer* runtimeLayer) {
    if (runtimeLayer->dynamicName != nullptr) {
        free(runtimeLayer->dynamicName);
        runtimeLayer->dynamicName = nullptr;
    }
    size_t elementCount = arrlenu(runtimeLayer->elements);
    repeat(elementCount, i) {
        RuntimeLayerElement* el = &runtimeLayer->elements[i];
        if (el->backgroundElement != nullptr) {
            free(el->backgroundElement);
            el->backgroundElement = nullptr;
        }
        if (el->spriteElement != nullptr) {
            free(el->spriteElement);
            el->spriteElement = nullptr;
        }
    }
    arrfree(runtimeLayer->elements);
    runtimeLayer->elements = nullptr;
}

static void freeRuntimeLayersArray(RuntimeLayer** runtimeLayerArray) {
    size_t count = arrlenu(*runtimeLayerArray);
    repeat(count, i) {
        Runner_freeRuntimeLayer(&(*runtimeLayerArray)[i]);
    }
    arrfree(*runtimeLayerArray);
    *runtimeLayerArray = nullptr;
}

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
    CodeLocals* savedCodeLocals = vm->currentCodeLocals;
    LocalSlotEntry* savedCodeLocalsSlotMap = vm->currentCodeLocalsSlotMap;
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
    vm->currentCodeLocals = savedCodeLocals;
    vm->currentCodeLocalsSlotMap = savedCodeLocalsSlotMap;
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
        case EVENT_PRECREATE:  return "PreCreate";
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

#ifndef DISABLE_VM_TRACING
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
#endif

    executeCode(runner, instance, codeId);

    vm->currentEventType = savedEventType;
    vm->currentEventSubtype = savedEventSubtype;
    vm->currentEventObjectIndex = savedEventObjectIndex;
}

void Runner_executeEvent(Runner* runner, Instance* instance, int32_t eventType, int32_t eventSubtype) {
    Runner_executeEventFromObject(runner, instance, instance->objectIndex, eventType, eventSubtype);
}

// Pair used for stable sorting: holds the instance pointer and its original array position.
typedef struct {
    Instance* inst;
    int32_t originalIndex;
} IndexedInstance;

// Comparator for per-object-type event dispatch (ascending objectIndex).
static int compareInstanceByObjectIndex(const void* a, const void* b) {
    const IndexedInstance* ia = (const IndexedInstance*) a;
    const IndexedInstance* ib = (const IndexedInstance*) b;
    // Primary: group by objectIndex ascending (lower object index executes first)
    if (ia->inst->objectIndex < ib->inst->objectIndex) return -1;
    if (ib->inst->objectIndex < ia->inst->objectIndex) return 1;
    // Secondary: preserve creation order within the same object type
    if (ia->originalIndex < ib->originalIndex) return -1;
    if (ib->originalIndex < ia->originalIndex) return 1;
    return 0;
}

void Runner_executeEventForAll(Runner* runner, int32_t eventType, int32_t eventSubtype) {
    // Dispatch events per-object-type, matching the native GMS 1.4 and 2.0 runners
    // The native runners iterate a prebuilt array of object indices (objects that have handlers for this event),
    // then for each object type iterate all its instances. We approximate this by sorting all active instances by
    // objectIndex (ascending), preserving creation order as the tiebreaker within the same object type
    int32_t count = (int32_t) arrlen(runner->instances);
    IndexedInstance* sorted = nullptr;
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (inst->active) {
            IndexedInstance ii = { .inst = inst, .originalIndex = i };
            arrput(sorted, ii);
        }
    }
    int32_t sortedCount = (int32_t) arrlen(sorted);
    if (sortedCount > 1) {
        qsort(sorted, sortedCount, sizeof(IndexedInstance), compareInstanceByObjectIndex);
    }
    repeat(sortedCount, i) {
        if (sorted[i].inst->active) {
            Runner_executeEvent(runner, sorted[i].inst, eventType, eventSubtype);
        }
    }
    arrfree(sorted);
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

typedef enum { DRAWABLE_TILE, DRAWABLE_INSTANCE, DRAWABLE_LAYER } DrawableType;

typedef struct {
    DrawableType type;
    int32_t depth;
    union {
        Instance* instance;
        int32_t tileIndex; // index into currentRoom->tiles
        RuntimeLayer* runtimeLayer;
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

// GMS2 tilemap cell bit layout (matches HTML5 Function_Layers.js TileIndex/Mirror/Flip/Rotate masks)
#define GMS2_TILE_INDEX_MASK  0x0007FFFF // bits 0..18
#define GMS2_TILE_MIRROR_MASK 0x10000000 // bit 28 (horizontal flip)
#define GMS2_TILE_FLIP_MASK   0x20000000 // bit 29 (vertical flip)
#define GMS2_TILE_ROTATE_MASK 0x40000000 // bit 30 (90 CW)

static void Runner_drawTileLayer(Runner* runner, RoomLayerTilesData* data, float layerOffsetX, float layerOffsetY) {
    if (data == nullptr || data->tileData == nullptr) return;
    if (0 > data->backgroundIndex) return;

    DataWin* dw = runner->dataWin;
    if ((uint32_t) data->backgroundIndex >= dw->bgnd.count) return;

    Background* tileset = &dw->bgnd.backgrounds[data->backgroundIndex];
    if (tileset->gms2TileWidth == 0 || tileset->gms2TileHeight == 0 || tileset->gms2TileColumns == 0) return;

    int32_t tpagIndex = DataWin_resolveTPAG(dw, tileset->textureOffset);
    if (0 > tpagIndex) return;

    uint32_t tileW = tileset->gms2TileWidth;
    uint32_t tileH = tileset->gms2TileHeight;
    uint32_t borderX = tileset->gms2OutputBorderX;
    uint32_t borderY = tileset->gms2OutputBorderY;
    uint32_t columns = tileset->gms2TileColumns;

    static bool rotateWarned = false;

    repeat(data->tilesY, ty) {
        repeat(data->tilesX, tx) {
            uint32_t cell = data->tileData[ty * data->tilesX + tx];
            uint32_t tileIndex = cell & GMS2_TILE_INDEX_MASK;
            if (tileIndex == 0) continue; // 0 = empty

            uint32_t col = tileIndex % columns;
            uint32_t row = tileIndex / columns;
            int32_t srcX = (int32_t) (col * (tileW + 2 * borderX) + borderX);
            int32_t srcY = (int32_t) (row * (tileH + 2 * borderY) + borderY);

            bool mirror = (cell & GMS2_TILE_MIRROR_MASK) != 0;
            bool flip = (cell & GMS2_TILE_FLIP_MASK) != 0;
            bool rotate = (cell & GMS2_TILE_ROTATE_MASK) != 0;

            if (rotate && !rotateWarned) {
                fprintf(stderr, "Runner: WARNING: GMS2 tile layer has rotated tiles; rotation not yet implemented, drawing unrotated\n");
                rotateWarned = true;
            }

            float xscale = mirror ? -1.0f : 1.0f;
            float yscale = flip ? -1.0f : 1.0f;

            // With negative scale the quad grows in the opposite direction, so shift the
            // destination by one tile to keep the origin at the top-left of the cell.
            float dstX = (float) (tx * tileW) + layerOffsetX + (mirror ? (float) tileW : 0.0f);
            float dstY = (float) (ty * tileH) + layerOffsetY + (flip ? (float) tileH : 0.0f);

            runner->renderer->vtable->drawSpritePart(runner->renderer, tpagIndex, srcX, srcY, (int32_t) tileW, (int32_t) tileH, dstX, dstY, xscale, yscale, 0xFFFFFF, 1.0f);
        }
    }
}

void Runner_draw(Runner* runner) {
    Room* room = runner->currentRoom;

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
    if (!DataWin_isVersionAtLeast(runner->dataWin, 2, 0, 0, 0))
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
    if (!DataWin_isVersionAtLeast(runner->dataWin, 2, 0, 0, 0)) {
        repeat(room->tileCount, i) {
            RoomTile* tile = &room->tiles[i];
            // Check if this tile's layer is hidden
            ptrdiff_t layerIdx = hmgeti(runner->tileLayerMap, tile->tileDepth);
            if (layerIdx >= 0 && !runner->tileLayerMap[layerIdx].value.visible) continue;

            Drawable d = { .type = DRAWABLE_TILE, .depth = tile->tileDepth, .tileIndex = (int32_t) i };
            arrput(drawables, d);
        }
    }

    if (DataWin_isVersionAtLeast(runner->dataWin, 2, 0, 0, 0)) {
        size_t runtimeLayersCount = arrlenu(runner->runtimeLayers);
        repeat(runtimeLayersCount, i) {
            RuntimeLayer* runtimeLayer = &runner->runtimeLayers[i];
            if (!runtimeLayer->visible) continue;
            Drawable d = { .type = DRAWABLE_LAYER, .depth = runtimeLayer->depth, .runtimeLayer = runtimeLayer };
            arrput(drawables, d);
        }
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

#ifndef DISABLE_VM_TRACING
                // Trace tile drawing if requested
                if (shlen(runner->vmContext->tilesToBeTraced) > 0) {
                    DataWin* dataWin = runner->dataWin;
                    const char* bgName = (tile->backgroundDefinition >= 0 && dataWin->bgnd.count > (uint32_t) tile->backgroundDefinition) ? dataWin->bgnd.backgrounds[tile->backgroundDefinition].name : "<none>";
                    const char* roomName = room->name;

                    bool shouldTrace = shgeti(runner->vmContext->tilesToBeTraced, "*") != -1 || shgeti(runner->vmContext->tilesToBeTraced, bgName) != -1 || shgeti(runner->vmContext->tilesToBeTraced, roomName) != -1;

                    if (shouldTrace) {
                        int32_t tpagIndex = Renderer_resolveObjectTPAGIndex(dataWin, tile);
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
#endif

                Renderer_drawTile(runner->renderer, tile, offsetX, offsetY);
            }
        } else if (d->type == DRAWABLE_INSTANCE) {
            Instance* inst = d->instance;
            int32_t codeId = findEventCodeIdAndOwner(runner->dataWin, inst->objectIndex, EVENT_DRAW, DRAW_NORMAL, nullptr);
            if (codeId >= 0) {
                Runner_executeEvent(runner, inst, EVENT_DRAW, DRAW_NORMAL);
            } else if (runner->renderer != nullptr) {
                Renderer_drawSelf(runner->renderer, inst);
            }
        } else if (d->type == DRAWABLE_LAYER)
        {
            RuntimeLayer* runtimeLayer = d->runtimeLayer;
            if (runtimeLayer == nullptr || !runtimeLayer->visible) continue;
            float layerOffsetX = runtimeLayer->xOffset;
            float layerOffsetY = runtimeLayer->yOffset;

            // Dynamic layers created via layer_create have no parsed RoomLayer, render their runtime elements instead (backgrounds, in the future sprites/tilemaps).
            if (runtimeLayer->dynamic) {
                if (runner->renderer == nullptr) continue;

                DataWin* dataWin = runner->dataWin;
                float roomW = (float) runner->currentRoom->width;
                float roomH = (float) runner->currentRoom->height;

                size_t elementCount = arrlenu(runtimeLayer->elements);
                repeat(elementCount, j) {
                    RuntimeLayerElement* layerElement = &runtimeLayer->elements[j];
                    if (layerElement->type == RuntimeLayerElementType_Background && layerElement->backgroundElement != nullptr) {
                        RuntimeBackgroundElement* bg = layerElement->backgroundElement;
                        if (!bg->visible) continue;
                        int32_t tpagIndex = Renderer_resolveSpriteTPAGIndex(dataWin, bg->spriteIndex);
                        if (0 > tpagIndex) continue;
                        if (bg->stretch) {
                            TexturePageItem* tpag = &dataWin->tpag.items[tpagIndex];
                            float xscale = roomW / (float) tpag->boundingWidth;
                            float yscale = roomH / (float) tpag->boundingHeight;
                            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, 0.0f, 0.0f, 0.0f, 0.0f, xscale, yscale, 0.0f, bg->blend, bg->alpha);
                        } else if (bg->htiled || bg->vtiled) {
                            Renderer_drawBackgroundTiled(runner->renderer, tpagIndex, layerOffsetX + bg->xOffset, layerOffsetY + bg->yOffset, bg->htiled, bg->vtiled, roomW, roomH, bg->alpha);
                        } else {
                            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, layerOffsetX + bg->xOffset, layerOffsetY + bg->yOffset, 0.0f, 0.0f, bg->xScale, bg->yScale, 0.0f, bg->blend, bg->alpha);
                        }
                    }
                }
                continue;
            }

            // Parsed layer: look up the RoomLayer by ID and render its data-driven content.
            RoomLayer* parsedLayer = Runner_findRoomLayerById(runner, (int32_t) runtimeLayer->id);
            if (parsedLayer == nullptr) continue;
            if (parsedLayer->type == RoomLayerType_Assets) {
                RoomLayerAssetsData* data = parsedLayer->assetsData;
                repeat(data->legacyTileCount, j) {
                    if (runner->renderer != nullptr) {
                        RoomTile* tile = &data->legacyTiles[j];
                        // Check if this tile's layer is hidden via tile_layer_hide()
                        ptrdiff_t layerIdx = hmgeti(runner->tileLayerMap, tile->tileDepth);
                        if (layerIdx >= 0 && !runner->tileLayerMap[layerIdx].value.visible) continue;
                        float offsetX = 0.0f, offsetY = 0.0f;
                        if (layerIdx >= 0) {
                            offsetX = runner->tileLayerMap[layerIdx].value.offsetX;
                            offsetY = runner->tileLayerMap[layerIdx].value.offsetY;
                        }

#ifndef DISABLE_VM_TRACING
                        // Trace tile drawing if requested
                        if (shlen(runner->vmContext->tilesToBeTraced) > 0) {
                            DataWin* dataWin = runner->dataWin;
                            const char* bgName = (tile->backgroundDefinition >= 0 && dataWin->bgnd.count > (uint32_t) tile->backgroundDefinition) ? dataWin->bgnd.backgrounds[tile->backgroundDefinition].name : "<none>";
                            const char* roomName = room->name;

                            bool shouldTrace = shgeti(runner->vmContext->tilesToBeTraced, "*") != -1 || shgeti(runner->vmContext->tilesToBeTraced, bgName) != -1 || shgeti(runner->vmContext->tilesToBeTraced, roomName) != -1;

                            if (shouldTrace) {
                                int32_t tpagIndex = Renderer_resolveObjectTPAGIndex(dataWin, tile);
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
#endif

                        Renderer_drawTile(runner->renderer, tile, offsetX, offsetY);
                    }
                }

                // Sprite elements are rendered from the runtime element list (not the parsed data) so that layer_sprite_destroy can remove them at runtime.
                size_t elementCount = arrlenu(runtimeLayer->elements);
                repeat(elementCount, j) {
                    if (runner->renderer == nullptr) break;
                    RuntimeLayerElement* el = &runtimeLayer->elements[j];
                    if (el->type != RuntimeLayerElementType_Sprite || el->spriteElement == nullptr) continue;
                    RuntimeSpriteElement* spr = el->spriteElement;
                    if (0 > spr->spriteIndex) continue;
                    Renderer_drawSpriteExt(
                        runner->renderer, spr->spriteIndex, (int32_t) spr->frameIndex,
                        spr->x, spr->y, spr->scaleX,
                        spr->scaleY, spr->rotation, spr->color,
                        1.0);
                }
            } else if(parsedLayer->type == RoomLayerType_Background) {
                if (runner->renderer == nullptr) return;
                    DataWin* dataWin = runner->dataWin;
                    float roomW = (float) runner->currentRoom->width;
                    float roomH = (float) runner->currentRoom->height;
                    RoomLayerBackgroundData* data = parsedLayer->backgroundData;

                        int32_t tpagIndex = Renderer_resolveSpriteTPAGIndex(dataWin, data->spriteIndex);
                        if (0 > tpagIndex) continue;

                        if (data->stretch) {
                            // Stretch to fill room dimensions
                            TexturePageItem* tpag = &dataWin->tpag.items[tpagIndex];
                            float xscale = roomW / (float) tpag->boundingWidth;
                            float yscale = roomH / (float) tpag->boundingHeight;
                            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, 0.0f, 0.0f, 0.0f, 0.0f, xscale, yscale, 0.0f, 0xFFFFFF, 1.0);
                        } else if (data->hTiled || data->vTiled) {
                            Renderer_drawBackgroundTiled(runner->renderer, tpagIndex, layerOffsetX, layerOffsetY, data->hTiled, data->vTiled, roomW, roomH, 1.0);
                        } else {
                            // Single placement
                            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, layerOffsetX, layerOffsetY, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFF, 1.0);
                        }
            } else if(parsedLayer->type == RoomLayerType_Instances) {
                // Instance depth is assigned from layers during room init (initRoom).
                // Nothing to do here - instances are drawn from the DRAWABLE_INSTANCE path.
            } else if(parsedLayer->type == RoomLayerType_Tiles) {
                if (runner->renderer == nullptr) continue;
                Runner_drawTileLayer(runner, parsedLayer->tilesData, layerOffsetX, layerOffsetY);
            }
        }
    }

    arrfree(drawables);

    fireDrawSubtype(runner, drawList, drawCount, DRAW_END);

    // Draw foreground backgrounds (in front of instances, behind GUI)
    Runner_drawBackgrounds(runner, true);

    fireDrawSubtype(runner, drawList, drawCount, DRAW_POST);

    arrfree(drawList);
}

void Runner_drawGUI(Runner* runner) {
    Instance** drawList = nullptr;
    int32_t count = (int32_t) arrlen(runner->instances);
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (inst->active && inst->visible) {
            arrput(drawList, inst);
        }
    }

    int32_t drawCount = (int32_t) arrlen(drawList);
    if (drawCount > 1) {
        qsort(drawList, drawCount, sizeof(Instance*), compareInstanceDepth);
    }

    fireDrawSubtype(runner, drawList, drawCount, DRAW_GUI_BEGIN);
    fireDrawSubtype(runner, drawList, drawCount, DRAW_GUI);
    fireDrawSubtype(runner, drawList, drawCount, DRAW_GUI_END);

    arrfree(drawList);
}

// ===[ Instance Creation Helper ]===

static bool isObjectDisabled(Runner* runner, int32_t objectIndex) {
    if (runner->disabledObjects == nullptr) return false;
    const char* name = runner->dataWin->objt.objects[objectIndex].name;
    return shgeti(runner->disabledObjects, name) != -1;
}

static Instance* createAndInitInstance(Runner* runner, int32_t instanceId, int32_t objectIndex, GMLReal x, GMLReal y) {
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

    hmput(runner->instancesToId, instanceId, inst);
    arrput(runner->instances, inst);

#ifndef DISABLE_VM_TRACING
    if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, objDef->name) != -1) {
        fprintf(stderr, "VM: Instance %s (%d) created at (%f, %f)\n", objDef->name, instanceId, x, y);
    }
#endif

    return inst;
}

// ===[ Room Management ]===

static void initRoom(Runner* runner, int32_t roomIndex) {
    DataWin* dataWin = runner->dataWin;
    require(roomIndex >= 0 && dataWin->room.count > (uint32_t) roomIndex);

    Room* room = &dataWin->room.rooms[roomIndex];
    Runner_bootLogf("runner: initRoom begin %s (%d)", room->name, roomIndex);

    // Lazy-room load: if the payload wasn't loaded, read it from the data.win file now before anything touches the room's game objects/tiles/layers.
    if (!room->payloadLoaded) {
        Runner_bootLog("runner: loading room payload");
        DataWin_loadRoomPayload(dataWin, roomIndex);
        Runner_bootLog("runner: room payload loaded");
    }

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

        // Restore runtime layers
        freeRuntimeLayersArray(&runner->runtimeLayers);
        runner->runtimeLayers = savedState->runtimeLayers;
        savedState->runtimeLayers = nullptr;

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
                hmdel(runner->instancesToId, inst->instanceId);
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
        Runner_bootLogf("runner: initRoom restored persistent (%d instances)", (int) arrlen(runner->instances));
        return;
    }

    // === Normal room initialization (first visit, or non-persistent room) ===

    // Reset tile layer state for the new room
    hmfree(runner->tileLayerMap);
    runner->tileLayerMap = nullptr;

    // Populate runtime layers from parsed room layers (GMS2+ only; empty for GMS1.x).
    // Dynamic layers created via layer_create are appended to this array later.
    freeRuntimeLayersArray(&runner->runtimeLayers);
    uint32_t maxLayerId = 0;
    repeat(room->layerCount, i) {
        RoomLayer* layerSource = &room->layers[i];
        RuntimeLayer runtimeLayer = {
            .id = layerSource->id,
            .depth = layerSource->depth,
            .visible = layerSource->visible,
            .xOffset = layerSource->xOffset,
            .yOffset = layerSource->yOffset,
            .hSpeed = layerSource->hSpeed,
            .vSpeed = layerSource->vSpeed,
            .dynamic = false,
            .dynamicName = nullptr,
            .elements = nullptr,
        };
        arrput(runner->runtimeLayers, runtimeLayer);
        if (layerSource->id > maxLayerId) maxLayerId = layerSource->id;
    }
    // Watermark: ensure runtime-allocated IDs (layers + elements) stay above parsed IDs.
    if (maxLayerId >= runner->nextLayerId) runner->nextLayerId = maxLayerId + 1;

    // Populate runtime sprite elements for Assets layers, so they can be queried and destroyed via layer_sprite_get_sprite/layer_sprite_destroy
    repeat(room->layerCount, i) {
        RoomLayer* layerSource = &room->layers[i];
        if (layerSource->type != RoomLayerType_Assets || layerSource->assetsData == nullptr) continue;
        RoomLayerAssetsData* assets = layerSource->assetsData;
        RuntimeLayer* runtimeLayer = &runner->runtimeLayers[i];
        repeat(assets->spriteCount, j) {
            SpriteInstance* src = &assets->sprites[j];
            RuntimeSpriteElement* spriteElement = safeMalloc(sizeof(RuntimeSpriteElement));
            spriteElement->spriteIndex = src->spriteIndex;
            spriteElement->x = src->x;
            spriteElement->y = src->y;
            spriteElement->scaleX = src->scaleX;
            spriteElement->scaleY = src->scaleY;
            spriteElement->color = src->color;
            spriteElement->animationSpeed = src->animationSpeed;
            spriteElement->animationSpeedType = src->animationSpeedType;
            spriteElement->frameIndex = src->frameIndex;
            spriteElement->rotation = src->rotation;
            RuntimeLayerElement el = {
                .id = Runner_getNextLayerId(runner),
                .type = RuntimeLayerElementType_Sprite,
                .backgroundElement = nullptr,
                .spriteElement = spriteElement,
            };
            arrput(runtimeLayer->elements, el);
        }
    }

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
            hmdel(runner->instancesToId, inst->instanceId);
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
        if (isObjectDisabled(runner, roomObj->objectDefinition)) continue;

        Instance* inst = createAndInitInstance(runner, roomObj->instanceID, roomObj->objectDefinition, (GMLReal) roomObj->x, (GMLReal) roomObj->y);
        inst->imageXscale = (float) roomObj->scaleX;
        inst->imageYscale = (float) roomObj->scaleY;
        inst->imageAngle = (float) roomObj->rotation;
        inst->imageSpeed = roomObj->imageSpeed;
        inst->imageIndex = (float) roomObj->imageIndex;
    }

    // In GMS2, instances get their depth from their room layer, not the object definition.
    // This must happen before firing Create events so scripts like scr_depth() read the layer depth.
    if (DataWin_isVersionAtLeast(runner->dataWin, 2, 0, 0, 0)) {
        repeat(room->layerCount, li) {
            RoomLayer* layer = &room->layers[li];
            if (layer->type != RoomLayerType_Instances || layer->instancesData == nullptr) continue;
            RoomLayerInstancesData* layerData = layer->instancesData;
            repeat(layerData->instanceCount, ii) {
                Instance* inst = hmget(runner->instancesToId, layerData->instanceIds[ii]);
                if (inst != nullptr) {
                    inst->depth = layer->depth;
                }
            }
        }
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
        if ((i % 32u) == 0u || i + 1u == room->gameObjectCount) {
            Runner_bootLogf("runner: create pass %u/%u (inst=%d obj=%d)", i + 1u, room->gameObjectCount, inst->instanceId, inst->objectIndex);
        }

        Runner_executeEvent(runner, inst, EVENT_PRECREATE, 0);
        executeCode(runner, inst, roomObj->preCreateCode);
        Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
        executeCode(runner, inst, roomObj->creationCode);
    }

    // Run room creation code
    if (room->creationCodeId >= 0 && dataWin->code.count > (uint32_t) room->creationCodeId) {
        // Room creation code runs in global context (no specific instance)
        Runner_bootLogf("runner: room creation code begin id=%d", room->creationCodeId);
        RValue result = VM_executeCode(runner->vmContext, room->creationCodeId);
        RValue_free(&result);
        Runner_bootLog("runner: room creation code end");
    }

    // Mark this room as initialized for persistent room support
    savedState->initialized = true;

    fprintf(stderr, "Runner: Room loaded: %s (room %d) with %d instances\n", room->name, roomIndex, (int) arrlen(runner->instances));
    Runner_bootLogf("runner: initRoom end (%d instances)", (int) arrlen(runner->instances));
}

// Cleans up the runner state, used when freeing the Runner or when restarting the Runner
static void cleanupState(Runner* runner) {
    // Free all instances
    repeat(arrlen(runner->instances), i) {
        hmdel(runner->instancesToId, runner->instances[i]->instanceId);
        Instance_free(runner->instances[i]);
    }
    arrfree(runner->instances);
    runner->instances = nullptr;

    // Free saved room states
    if (runner->savedRoomStates != nullptr) {
        repeat(runner->dataWin->room.count, i) {
            SavedRoomState* state = &runner->savedRoomStates[i];
            int32_t savedCount = (int32_t) arrlen(state->instances);
            repeat(savedCount, j) {
                hmdel(runner->instancesToId, state->instances[j]->instanceId);
                Instance_free(state->instances[j]);
            }
            arrfree(state->instances);
            hmfree(state->tileLayerMap);
            freeRuntimeLayersArray(&state->runtimeLayers);
        }
        free(runner->savedRoomStates);
    }
    runner->savedRoomStates = nullptr;

    hmfree(runner->instancesToId);
    runner->instancesToId = nullptr;
    hmfree(runner->tileLayerMap);
    runner->tileLayerMap = nullptr;
    freeRuntimeLayersArray(&runner->runtimeLayers);
    shfree(runner->disabledObjects);
    runner->disabledObjects = nullptr;

    // Free ds_map pool
    repeat((int32_t) arrlen(runner->dsMapPool), i) {
        DsMapEntry* map = runner->dsMapPool[i];
        if (map != nullptr) {
            repeat(shlen(map), j) {
                free(map[j].key);
                RValue_free(&map[j].value);
            }
            shfree(map);
        }
    }
    arrfree(runner->dsMapPool);
    runner->dsMapPool = nullptr;

    // Free ds_list pool
    repeat((int32_t) arrlen(runner->dsListPool), i) {
        DsList* list = &runner->dsListPool[i];
        repeat(arrlen(list->items), j) {
            RValue_free(&list->items[j]);
        }
        arrfree(list->items);
    }
    arrfree(runner->dsListPool);
    runner->dsListPool = nullptr;

    // Free INI state
    if (runner->currentIni != nullptr) {
        Ini_free(runner->currentIni);
        runner->currentIni = nullptr;
    }
    free(runner->currentIniPath);
    runner->currentIniPath = nullptr;
    if (runner->cachedIni != nullptr) {
        Ini_free(runner->cachedIni);
        runner->cachedIni = nullptr;
    }
    free(runner->cachedIniPath);
    runner->cachedIniPath = nullptr;

    // Free open text files
    repeat(MAX_OPEN_TEXT_FILES, i) {
        OpenTextFile* file = &runner->openTextFiles[i];
        if (file->isOpen) {
            free(file->content);
            free(file->writeBuffer);
            free(file->filePath);
            *file = (OpenTextFile) {0};
        }
    }
}

// ===[ Public API ]===

void Runner_reset(Runner* runner) {
    // This actually sets the default runner values, used for initialization and restarting
    cleanupState(runner);

    // Reset VM state
    VM_reset(runner->vmContext);

    runner->pendingRoom = -1;
    runner->gameStartFired = false;
    runner->currentRoomIndex = -1;
    runner->currentRoomOrderPosition = -1;
    runner->nextInstanceId = runner->dataWin->gen8.lastObj + 1;
    runner->savedRoomStates = safeCalloc(runner->dataWin->room.count, sizeof(SavedRoomState));
    runner->nextLayerId = 1;
    if (runner->audioSystem != nullptr) {
        runner->audioSystem->vtable->stopAll(runner->audioSystem);
    }

    // Create the instance used for "self" in GLOB scripts
    Instance_free(runner->globalScopeInstance);
    runner->globalScopeInstance = Instance_create(0, -1, 0, 0);

    // Reset builtin function state
    runner->mpPotMaxrot = 30.0;
    runner->mpPotStep = 10.0;
    runner->mpPotAhead = 3.0;
    runner->mpPotOnSpot = true;
    runner->lastMusicInstance = -1;
}

Runner* Runner_create(DataWin* dataWin, VMContext* vm, Renderer* renderer, FileSystem* fileSystem, AudioSystem* audioSystem) {
    requireNotNull(dataWin);
    requireNotNull(vm);
    // renderer and audioSystem may be NULL on platforms (e.g. 3DS) that set them after creation
    requireNotNull(fileSystem);

    Runner* runner = safeCalloc(1, sizeof(Runner));
    runner->dataWin = dataWin;
    runner->vmContext = vm;
    runner->renderer = renderer;
    runner->fileSystem = fileSystem;
    runner->audioSystem = audioSystem;
    runner->frameCount = 0;
    runner->osType = OS_WINDOWS;
    runner->keyboard = RunnerKeyboard_create();

    Runner_reset(runner);

    // Link runner to VM context
    vm->runner = (struct Runner*) runner;

    if (renderer != nullptr) {
        renderer->vtable->init(renderer, dataWin);
    }
    if (audioSystem != nullptr) {
        audioSystem->vtable->init(audioSystem, dataWin, fileSystem);
    }

    return runner;
}

Instance* Runner_createInstance(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex) {
    if (isObjectDisabled(runner, objectIndex)) return nullptr;
    Instance* inst = createAndInitInstance(runner, runner->nextInstanceId++, objectIndex, x, y);
    inst->createEventFired = true;
    Runner_executeEvent(runner, inst, EVENT_PRECREATE, 0);
    Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
    return inst;
}

Instance* Runner_copyInstance(Runner* runner, Instance* source, bool performEvent) {
    requireNotNull(source);
    if (isObjectDisabled(runner, source->objectIndex)) return nullptr;

    Instance* inst = createAndInitInstance(runner, runner->nextInstanceId++, source->objectIndex, source->x, source->y);
    Instance_copyFields(inst, source);
    inst->createEventFired = true;
    if (performEvent) {
        Runner_executeEvent(runner, inst, EVENT_PRECREATE, 0);
        Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
    }
    return inst;
}

void Runner_destroyInstance(MAYBE_UNUSED Runner* runner, Instance* inst) {
    GameObject* gameObject = &runner->dataWin->objt.objects[inst->objectIndex];
    Runner_executeEvent(runner, inst, EVENT_DESTROY, 0);
    // A destroyed instance must ALWAYS be not active
    // If a destroyed instance is active, then well, something went VERY wrong
    inst->active = false;
    inst->destroyed = true;

#ifndef DISABLE_VM_TRACING
    if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, gameObject->name) != -1) {
        fprintf(stderr, "VM: Instance %s (%d) destroyed\n", gameObject->name, inst->instanceId);
    }
#endif
}

RuntimeLayer* Runner_findRuntimeLayerById(Runner* runner, int32_t id) {
    size_t count = arrlenu(runner->runtimeLayers);
    repeat(count, i) {
        if ((int32_t) runner->runtimeLayers[i].id == id)
            return &runner->runtimeLayers[i];
    }
    return nullptr;
}

RoomLayer* Runner_findRoomLayerById(Runner* runner, int32_t id) {
    if (runner->currentRoom == nullptr) return nullptr;
    repeat(runner->currentRoom->layerCount, i) {
        if ((int32_t) runner->currentRoom->layers[i].id == id) return &runner->currentRoom->layers[i];
    }
    return nullptr;
}

RuntimeLayerElement* Runner_findLayerElementById(Runner* runner, int32_t elementId, RuntimeLayer** outLayer) {
    size_t layerCount = arrlenu(runner->runtimeLayers);
    repeat(layerCount, i) {
        RuntimeLayer* runtimeLayer = &runner->runtimeLayers[i];
        size_t elementCount = arrlenu(runtimeLayer->elements);
        repeat(elementCount, j) {
            if ((int32_t) runtimeLayer->elements[j].id == elementId) {
                if (outLayer != nullptr)
                    *outLayer = runtimeLayer;
                
                return &runtimeLayer->elements[j];
            }
        }
    }
    if (outLayer != nullptr) *outLayer = nullptr;
    return nullptr;
}

uint32_t Runner_getNextLayerId(Runner* runner) {
    return runner->nextLayerId++;
}

void Runner_cleanupDestroyedInstances(Runner* runner) {
    int32_t count = (int32_t) arrlen(runner->instances);
    int32_t writeIdx = 0;
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->destroyed) {
            runner->instances[writeIdx++] = inst;
        } else {
            hmdel(runner->instancesToId, inst->instanceId);
            Instance_free(inst);
        }
    }
    arrsetlen(runner->instances, writeIdx);
}

void Runner_initFirstRoom(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    require(dataWin->gen8.roomOrderCount > 0);

    int32_t firstRoomIndex = dataWin->gen8.roomOrder[0];
    Runner_bootLogf("runner: initFirstRoom begin (roomIndex=%d)", firstRoomIndex);

    // Run global init scripts with the global scope instance as "self"
    // In GMS 2.3+ (BC17), GLOB scripts store function declarations on "self" via Pop.v.v
    runner->vmContext->currentInstance = runner->globalScopeInstance;
    repeat(dataWin->glob.count, i) {
        int32_t codeId = dataWin->glob.codeIds[i];
        if (codeId >= 0 && dataWin->code.count > (uint32_t) codeId) {
            fprintf(stderr, "Runner: Executing global init script: %s\n", dataWin->code.entries[codeId].name);
            Runner_bootLogf("runner: global init begin %u/%u %s", i + 1u, dataWin->glob.count, dataWin->code.entries[codeId].name);
            RValue result = VM_executeCode(runner->vmContext, codeId);
            RValue_free(&result);
            Runner_bootLogf("runner: global init end %u/%u", i + 1u, dataWin->glob.count);
        }
    }
    runner->vmContext->currentInstance = nullptr;
    Runner_bootLog("runner: global init complete");

    // Initialize the first room
    Runner_bootLog("runner: init first room begin");
    initRoom(runner, firstRoomIndex);
    Runner_bootLog("runner: init first room end");

    // Fire Game Start for all instances
    Runner_bootLog("runner: game start begin");
    Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_GAME_START);
    runner->gameStartFired = true;
    Runner_bootLog("runner: game start end");

    // Fire Room Start for all instances
    Runner_bootLog("runner: room start begin");
    Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_START);
    Runner_bootLog("runner: room start end");
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

#ifndef DISABLE_VM_TRACING
    if (codeId >= 0 && shlen(vm->eventsToBeTraced) != -1) {
        const char* selfName = runner->dataWin->objt.objects[self->objectIndex].name;
        const char* targetName = runner->dataWin->objt.objects[targetObjectIndex].name;
        bool shouldTrace = shgeti(vm->eventsToBeTraced, "*") != -1 || shgeti(vm->eventsToBeTraced, "Collision") != -1 || shgeti(vm->eventsToBeTraced, selfName) != -1;
        if (shouldTrace) {
            fprintf(stderr, "Runner: [%s] Collision with %s (instanceId=%d, otherId=%d)\n", selfName, targetName, self->instanceId, other->instanceId);
        }
    }
#endif

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

    repeat(8, vi) {
        RoomView* view = &room->views[vi];
        if (!view->enabled) continue;

        if (view->objectId >= 0) {
            // Find first active instance of the target object
            Instance* target = nullptr;
            int32_t count = (int32_t) arrlen(runner->instances);
            repeat(count, i) {
                Instance* inst = runner->instances[i];
                if (inst->active && VM_isObjectOrDescendant(runner->dataWin, inst->objectIndex, view->objectId)) { target = inst; break; };
            }

            if (target != nullptr) {
                int32_t ix = (int32_t) GMLReal_floor(target->x);
                int32_t iy = (int32_t) GMLReal_floor(target->y);
                view->viewX = followAxis(view->viewX, view->viewWidth, ix, view->borderX, view->speedX, (int32_t) room->width);
                view->viewY = followAxis(view->viewY, view->viewHeight, iy, view->borderY, view->speedY, (int32_t) room->height);
                continue;
            }
        }

        // Always clamp to room bounds
        if (view->viewX + view->viewWidth > (int32_t) room->width) view->viewX = (int32_t) room->width - view->viewWidth;
        if (view->viewY + view->viewHeight > (int32_t) room->height) view->viewY = (int32_t) room->height - view->viewHeight;
        if (0 > view->viewX) view->viewX = 0;
        if (0 > view->viewY) view->viewY = 0;
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

    GMLReal orient = inst->pathOrientation * M_PI / 180.0;

    // Get current position's speed factor
    PathPositionResult cur = GamePath_getPosition(path, inst->pathPosition);
    GMLReal sp = cur.speed / (100.0 * inst->pathScale);

    // Advance position (compute in higher precision, truncate to float on store - matches native runner)
    inst->pathPosition = (float) (inst->pathPosition + inst->pathSpeed * sp / path->length);

    // Handle end actions if position out of [0,1]
    PathPositionResult pos0 = GamePath_getPosition(path, 0.0);
    if (inst->pathPosition >= 1.0f || 0.0f >= inst->pathPosition) {
        atPathEnd = (inst->pathSpeed == 0.0f) ? false : true;

        switch (inst->pathEndAction) {
            // stop moving
            case 0: {
                if (inst->pathSpeed >= 0.0f) {
                    if (inst->pathSpeed != 0.0f) {
                        inst->pathPosition = 1.0f;
                        inst->pathIndex = -1;
                    }
                } else {
                    inst->pathPosition = 0.0f;
                    inst->pathIndex = -1;
                }
                break;
            }
            // continue from start position (restart)
            case 1: {
                if (0.0f > inst->pathPosition) {
                    inst->pathPosition += 1.0f;
                } else {
                    inst->pathPosition -= 1.0f;
                }
                break;
            }
            // continue from current position
            case 2: {
                PathPositionResult pos1 = GamePath_getPosition(path, 1.0);
                GMLReal xx = pos1.x - pos0.x;
                GMLReal yy = pos1.y - pos0.y;
                GMLReal xdif = inst->pathScale * (xx * GMLReal_cos(orient) + yy * GMLReal_sin(orient));
                GMLReal ydif = inst->pathScale * (yy * GMLReal_cos(orient) - xx * GMLReal_sin(orient));

                if (0.0f > inst->pathPosition) {
                    inst->pathXStart -= (float) xdif;
                    inst->pathYStart -= (float) ydif;
                    inst->pathPosition += 1.0f;
                } else {
                    inst->pathXStart += (float) xdif;
                    inst->pathYStart += (float) ydif;
                    inst->pathPosition -= 1.0f;
                }
                break;
            }
            // reverse
            case 3: {
                if (0.0f > inst->pathPosition) {
                    inst->pathPosition = -inst->pathPosition;
                    inst->pathSpeed = (float) GMLReal_fabs(inst->pathSpeed);
                } else {
                    inst->pathPosition = 2.0f - inst->pathPosition;
                    inst->pathSpeed = (float) -GMLReal_fabs(inst->pathSpeed);
                }
                break;
            }
            // default: stop
            default: {
                inst->pathPosition = 1.0f;
                inst->pathIndex = -1;
                break;
            }
        }
    }

    // Find the new position in the room
    PathPositionResult newPos = GamePath_getPosition(path, inst->pathPosition);
    GMLReal xx = newPos.x - pos0.x; // relative
    GMLReal yy = newPos.y - pos0.y;

    GMLReal newx = inst->pathXStart + inst->pathScale * (xx * GMLReal_cos(orient) + yy * GMLReal_sin(orient));
    GMLReal newy = inst->pathYStart + inst->pathScale * (yy * GMLReal_cos(orient) - xx * GMLReal_sin(orient));

    // Trick to set the direction: set hspeed/vspeed to delta, which updates direction
    inst->hspeed = (float) (newx - inst->x);
    inst->vspeed = (float) (newy - inst->y);
    Instance_computeSpeedFromComponents(inst);

    // Normal speed should not be used
    inst->speed = 0.0f;
    inst->hspeed = 0.0f;
    inst->vspeed = 0.0f;

    // Set the new position
    inst->x = (float) newx;
    inst->y = (float) newy;

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

    // Advance image_index by image_speed for all active instances
    int32_t animCount = (int32_t) arrlen(runner->instances);
    repeat(animCount, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;
        if (0 > inst->spriteIndex) continue;

        inst->imageIndex += inst->imageSpeed;

        // Wrap image_index (matches HTML5 runner: manual subtract/add instead of using fmod)
        Sprite* sprite = &runner->dataWin->sprt.sprites[inst->spriteIndex];
        float frameCount = (float) sprite->textureCount;
        if (inst->imageIndex >= frameCount) {
            inst->imageIndex -= frameCount;
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_ANIMATION_END);
        } else if (0.0f > inst->imageIndex) {
            inst->imageIndex += frameCount;
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_ANIMATION_END);
        }
    }

    // Scroll backgrounds
    Runner_scrollBackgrounds(runner);

    // Advance GMS2 layer parallax (hspeed/vspeed per frame)
    size_t layerCount = arrlenu(runner->runtimeLayers);
    repeat(layerCount, i) {
        RuntimeLayer* rl = &runner->runtimeLayers[i];
        rl->xOffset += rl->hSpeed;
        rl->yOffset += rl->vSpeed;
    }

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
#ifndef DISABLE_VM_TRACING
                if (shgeti(runner->vmContext->alarmsToBeTraced, "*") != -1 || shgeti(runner->vmContext->alarmsToBeTraced, object->name) != -1) {
                    fprintf(stderr, "VM: [%s] Ticking down Alarm[%d] (instanceId=%d), current tick is %d\n", object->name, alarmIdx, inst->instanceId, inst->alarm[alarmIdx]);
                }
#endif

                inst->alarm[alarmIdx]--;
                if (inst->alarm[alarmIdx] == 0) {
                    inst->alarm[alarmIdx] = -1;

#ifndef DISABLE_VM_TRACING
                    if (shgeti(runner->vmContext->alarmsToBeTraced, "*") != -1 || shgeti(runner->vmContext->alarmsToBeTraced, object->name) != -1) {
                        fprintf(stderr, "VM: [%s] Firing Alarm[%d] (instanceId=%d)\n", object->name, alarmIdx, inst->instanceId);
                    }
#endif

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
        if (inst->friction != 0.0f) {
            float ns = (inst->speed > 0.0f) ? inst->speed - inst->friction : inst->speed + inst->friction;
            if ((inst->speed > 0.0f && ns < 0.0f) || (inst->speed < 0.0f && ns > 0.0f)) {
                inst->speed = 0.0f;
            } else if (inst->speed != 0.0f) {
                inst->speed = ns;
            }
            Instance_computeComponentsFromSpeed(inst);
        }

        // Gravity: add velocity in gravity_direction (HTML5: AddTo_Speed)
        if (inst->gravity != 0.0f) {
            GMLReal gravDirRad = inst->gravityDirection * (M_PI / 180.0);
            inst->hspeed += (float) (inst->gravity * clampFloat(GMLReal_cos(gravDirRad)));
            inst->vspeed -= (float) (inst->gravity * clampFloat(GMLReal_sin(gravDirRad)));
            Instance_computeSpeedFromComponents(inst);
        }

        // Path adaptation (HTML5: Adapt_Path, runs after friction/gravity, before x+=hspeed)
        if (adaptPath(runner, inst)) {
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_END_OF_PATH);
        }

        // Apply movement
        if (inst->hspeed != 0.0f || inst->vspeed != 0.0f) {
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

    // Handle game restart
    if (runner->pendingRoom == ROOM_RESTARTGAME) {
        // See you soon!
        // Free the currently-loaded non-eager room before reset so lazyLoadRooms stays steady-state.
        if (runner->dataWin->lazyLoadRooms && runner->currentRoom != nullptr && !runner->currentRoom->eagerlyLoaded) {
            DataWin_freeRoomPayload(runner->currentRoom);
        }
        Runner_reset(runner);
        Runner_initFirstRoom(runner);
        runner->frameCount++;
        return;
    }

    // Handle room transition
    if (runner->pendingRoom >= 0) {
        int32_t oldRoomIndex = runner->currentRoomIndex;
        Room* oldRoom = runner->currentRoom;
        const char* oldRoomName = oldRoom->name;

        // Fire Room End for all instances
        Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_END);

        int32_t newRoomIndex = runner->pendingRoom;
        runner->pendingRoom = -1;
        require(runner->dataWin->room.count > (uint32_t) newRoomIndex);
        const char* newRoomName = runner->dataWin->room.rooms[newRoomIndex].name;

        fprintf(stderr, "Room changed: %s (room %d) -> %s (room %d)\n", oldRoomName, oldRoomIndex, newRoomName, newRoomIndex);

        // If the old room is persistent, save its instance and visual state
        if (oldRoom->persistent) {
            SavedRoomState* state = &runner->savedRoomStates[oldRoomIndex];

            // Free any previously saved instances (from an earlier visit)
            int32_t prevSavedCount = (int32_t) arrlen(state->instances);
            repeat(prevSavedCount, i) {
                hmdel(runner->instancesToId, state->instances[i]->instanceId);
                Instance_free(state->instances[i]);
            }
            arrfree(state->instances);
            state->instances = nullptr;
            hmfree(state->tileLayerMap);
            state->tileLayerMap = nullptr;
            freeRuntimeLayersArray(&state->runtimeLayers);

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
                    hmdel(runner->instancesToId, inst->instanceId);
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

            // Transfer runtime layer ownership to saved state
            state->runtimeLayers = runner->runtimeLayers;
            runner->runtimeLayers = nullptr;

            state->initialized = true;
        }

        // Free the outgoing room's payload under lazyLoadRooms, unless it's eagerly pinned or we're restarting the same room (initRoom would just re-load it).
        if (runner->dataWin->lazyLoadRooms && !oldRoom->eagerlyLoaded && newRoomIndex != oldRoomIndex) {
            DataWin_freeRoomPayload(oldRoom);
        }

        // Load new room
        initRoom(runner, newRoomIndex);

        // Fire Room Start for all instances
        Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_START);
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

        // Self variables
        bool hasSelfVars = false;
        bool hasSelfArrays = false;
        repeat(hmlen(inst->selfVars), svIdx) {
            int32_t varID = inst->selfVars[svIdx].key;
            RValue val = inst->selfVars[svIdx].value;
            if (val.type == RVALUE_UNDEFINED) continue;

            const char* varName = "?";
            repeat(dataWin->vari.variableCount, varIdx) {
                Variable* var = &dataWin->vari.variables[varIdx];
                if (var->instanceType == INSTANCE_SELF && var->varID == varID) {
                    varName = var->name;
                    break;
                }
            }

            if (val.type == RVALUE_ARRAY && val.array != nullptr) {
                if (!hasSelfArrays) { printf("  Self Arrays:\n"); hasSelfArrays = true; }
                repeat(val.array->length, ai) {
                    RValue inner = val.array->data[ai];
                    if (inner.type == RVALUE_UNDEFINED) continue;
                    char* innerStr = RValue_toStringFancy(inner);
                    printf("    %s[%d] = %s\n", varName, (int) ai, innerStr);
                    free(innerStr);
                }
            } else {
                if (!hasSelfVars) { printf("  Self Variables:\n"); hasSelfVars = true; }
                char* valStr = RValue_toStringFancy(val);
                printf("    %s = %s\n", varName, valStr);
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

    // Global arrays: scan globalVars slots for RVALUE_ARRAY entries
    repeat(dataWin->vari.variableCount, varIdx) {
        Variable* var = &dataWin->vari.variables[varIdx];
        if (var->instanceType != INSTANCE_GLOBAL || var->varID < 0) continue;
        if ((uint32_t) var->varID >= vm->globalVarCount) continue;
        RValue val = vm->globalVars[var->varID];
        if (val.type != RVALUE_ARRAY || val.array == nullptr) continue;
        repeat(val.array->length, ai) {
            RValue inner = val.array->data[ai];
            if (inner.type == RVALUE_UNDEFINED) continue;
            char* innerStr = RValue_toStringFancy(inner);
            printf("  %s[%d] = %s\n", var->name, (int) ai, innerStr);
            free(innerStr);
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
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:
            JsonWriter_int(w, val.int64);
            break;
#endif
        case RVALUE_STRING:
            JsonWriter_string(w, val.string);
            break;
        case RVALUE_BOOL:
            JsonWriter_bool(w, val.int32 != 0);
            break;
        case RVALUE_UNDEFINED:
            JsonWriter_null(w);
            break;
        case RVALUE_ARRAY: {
            // Render arrays as a JSON array. Skips RVALUE_UNDEFINED entries (they read as 0/null anyway).
            JsonWriter_beginArray(w);
            if (val.array != nullptr) {
                repeat(val.array->length, ai) {
                    writeRValueJson(w, val.array->data[ai]);
                }
            }
            JsonWriter_endArray(w);
            break;
        }
#if IS_BC17_OR_HIGHER_ENABLED
        case RVALUE_METHOD: {
            char buf[64];
            snprintf(buf, sizeof(buf), "<method:%d>", val.method->codeIndex);
            JsonWriter_string(w, buf);
            break;
        }
#endif
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
    JsonWriter_endObject(&w);

    char* result = JsonWriter_copyOutput(&w);
    JsonWriter_free(&w);
    return result;
}

void Runner_free(Runner* runner) {
    if (runner == nullptr) return;

    cleanupState(runner);

    RunnerKeyboard_free(runner->keyboard);
    Instance_free(runner->globalScopeInstance);
    free(runner);
}
