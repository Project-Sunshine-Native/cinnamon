#include "runner.h"
#include "data_win.h"
#include "instance.h"
#include "renderer.h"
#include "vm.h"
#include "utils.h"
#include "json_writer.h"
#include "collision.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef __3DS__
#include <3ds.h>
#include "n3ds/n3ds_platform_config.h"
#endif

#ifndef N3DS_ENABLE_TILE_LAYER_CHUNK_CACHE
#define N3DS_ENABLE_TILE_LAYER_CHUNK_CACHE 0
#endif

#include "debug_overlay.h"
// #include "stb_ds.h"

#ifdef __3DS__
void N3DSRenderer_beginBottomScreenGUIEx(Renderer* renderer, int32_t guiW, int32_t guiH, float scaleX, float scaleY, float offsetX, float offsetY);
void N3DSRenderer_beginBottomScreenGUI(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_endBottomScreenGUI(Renderer* renderer);
void N3DSRenderer_beginBottomScreenGUI2x(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_endBottomScreenGUI2x(Renderer* renderer);
void N3DSRenderer_beginTopScreenGUI(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_endTopScreenGUI(Renderer* renderer);
void N3DSRenderer_beginTopScreenGUI2x(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_endTopScreenGUI2x(Renderer* renderer);
bool N3DSRenderer_isTopScreenGUIActive(Renderer* renderer);
bool N3DSRenderer_isTopScreenBattleViewActive(Renderer* renderer);
void N3DSRenderer_setTopScreenBattleViewActive(Renderer* renderer, bool active);
int32_t N3DSRenderer_findTileEntryIndex(Renderer* renderer, int32_t backgroundIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH);
bool N3DSRenderer_drawCachedTileEntry(Renderer* renderer, int32_t tileEntryIndex, float drawX, float drawY, float xscale, float yscale, uint32_t color, float alpha);
int32_t N3DSRenderer_createTileLayerChunkCache(Renderer* renderer, int32_t roomWidth, int32_t roomHeight, const TileLayerRenderCache* cache);
bool N3DSRenderer_drawTileLayerChunkCache(Renderer* renderer, int32_t cacheId, float layerOffsetX, float layerOffsetY, float alpha);
void N3DSRenderer_destroyTileLayerChunkCache(Renderer* renderer, int32_t cacheId);
#endif

#define RoomLayerType_Path 0
#define RoomLayerType_Background 1
#define RoomLayerType_Instances 2
#define RoomLayerType_Assets 3
#define RoomLayerType_Tiles 4
#define RoomLayerType_Effect 6
#define RoomLayerType_Path2 7

static uint32_t gRunnerDrawTraceCount = 0;
static int32_t gRunnerLastLoggedRoomIndex = INT32_MIN;
static int32_t gRunnerLastLoggedPendingRoom = INT32_MIN;
static int32_t gRunnerLastLoggedDrawableCount = INT32_MIN;
static int32_t gRunnerLastLoggedInstanceCount = INT32_MIN;

static double Runner_nowMs(void) {
#ifdef __3DS__
    return (double) svcGetSystemTick() * 1000.0 / (double) SYSCLOCK_ARM11;
#else
    return (double) clock() * 1000.0 / (double) CLOCKS_PER_SEC;
#endif
}

#ifdef __3DS__
static bool Runner_isOld3DSLike(void) {
    static bool initialized = false;
    static bool old3DSLike = true;
    if (!initialized) {
        bool isNew3DS = false;
        if (R_SUCCEEDED(APT_CheckNew3DS(&isNew3DS))) {
            old3DSLike = !isNew3DS;
        } else {
            old3DSLike = true;
        }
#if N3DS_FORCE_OLD3DS_MODE
        old3DSLike = true;
#endif
        initialized = true;
    }
    return old3DSLike;
}
#endif

static void Runner_logTiming(const char* phase, double startMs, double totalStartMs) {
    double nowMs = Runner_nowMs();
    fprintf(
        stderr,
        "Runner load: %-28s phase=%8.2f ms total=%8.2f ms\n",
        phase != NULL ? phase : "<null>",
        nowMs - startMs,
        nowMs - totalStartMs
    );
}

static int32_t findEventCodeIdAndOwner(Runner* runner, int32_t objectIndex, int32_t eventType, int32_t eventSubtype, int32_t* outOwnerObjectIndex);
static void Runner_executeResolvedEvent(Runner* runner, Instance* instance, int32_t eventType, int32_t eventSubtype, int32_t codeId, int32_t ownerObjectIndex);

#ifdef __3DS__
//bottom screen battlefield offset factor
static const float k3DSBottomBattleFieldScale = 1.0f;
static const float k3DSBottomBattleFieldYOffset = 0.0f;
static const float k3DSTopBattleEnemyInstanceYOffset = 112.0f;

static bool Runner_stringContainsToken(const char* haystack, const char* needle) {
    return haystack != NULL && needle != NULL && strstr(haystack, needle) != NULL;
}

static bool Runner_is3DSValidObjectIndex(Runner* runner, int32_t objectIndex);
static bool Runner_objectMatches3DSNameInHierarchy(Runner* runner, int32_t objectIndex, const char* name);
static bool Runner_objectContains3DSTokenInHierarchy(Runner* runner, int32_t objectIndex, const char* needle);
static bool Runner_is3DSLiveBattleBorderObject(Runner* runner, Instance* inst);
static bool Runner_shouldHideOn3DSTopScreen(Runner* runner, Instance* inst);
static bool Runner_is3DSBattleBackdropInstance(Runner* runner, Instance* inst);
static bool Runner_shouldOffset3DSTopBattleInstance(Runner* runner, Instance* inst);
static bool Runner_is3DSBattleFieldObject(Runner* runner, Instance* inst);
static bool Runner_is3DSAsrielBattle(Runner* runner);
static bool Runner_is3DSUndyneBattle(Runner* runner);

static RValue Runner_get3DSGlobal(Runner* runner, const char* name) {
    if (runner == NULL || runner->vmContext == NULL || name == NULL) return RValue_makeUndefined();

    ptrdiff_t idx = shgeti(runner->vmContext->globalVarNameMap, (char*) name);
    if (idx < 0) return RValue_makeUndefined();

    int32_t varID = runner->vmContext->globalVarNameMap[idx].value;
    if ((uint32_t) varID >= runner->vmContext->globalVarCount) return RValue_makeUndefined();

    RValue value = runner->vmContext->globalVars[varID];
    value.ownsReference = false;
    return value;
}

static double Runner_get3DSGlobalReal(Runner* runner, const char* name) {
    return (double) RValue_toReal(Runner_get3DSGlobal(runner, name));
}

static double Runner_get3DSGlobalArrayReal(Runner* runner, const char* name, int32_t index) {
    RValue arr = Runner_get3DSGlobal(runner, name);
    if (arr.type != RVALUE_ARRAY || arr.array == NULL) return 0.0;
    if (index < 0 || index >= GMLArray_length1D(arr.array)) return 0.0;

    RValue* slot = GMLArray_slot(arr.array, index);
    if (slot == NULL) return 0.0;
    return (double) RValue_toReal(*slot);
}

static double Runner_get3DSInstanceRealVar(Runner* runner, Instance* inst, const char* name) {
    if (runner == NULL || runner->vmContext == NULL || inst == NULL || name == NULL) return 0.0;

    ptrdiff_t slot = shgeti(runner->vmContext->selfVarNameMap, (char*) name);
    if (slot < 0) return 0.0;

    int32_t varID = runner->vmContext->selfVarNameMap[slot].value;
    RValue value = Instance_getSelfVar(inst, varID);
    return (double) RValue_toReal(value);
}

static bool Runner_is3DSDodgingBullets(Runner* runner) {
    return Runner_get3DSGlobalReal(runner, "turntimer") > 0.0;
}

static void Runner_prepare3DSDrawBattleState(Runner* runner) {
    if (runner == NULL || runner->n3dsDrawBattleStateValid) return;

    runner->n3dsDrawHasTopEnemyDialogue = false;
    runner->n3dsDrawAsrielBattle = false;
    bool hasLiveBattleBorder = false;
    bool hasActiveBattleController = false;
    if (runner->instances != NULL) {
        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst == NULL || inst->destroyed || !inst->active) continue;
            if (Runner_is3DSLiveBattleBorderObject(runner, inst)) {
                if (inst->visible) hasLiveBattleBorder = true;
            }
            if (inst->visible &&
                Runner_is3DSValidObjectIndex(runner, inst->objectIndex) &&
                !Runner_is3DSBattleBackdropInstance(runner, inst)) {
                if (Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "asriel")) {
                    runner->n3dsDrawAsrielBattle = true;
                } else if (inst->spriteIndex >= 0 && (uint32_t) inst->spriteIndex < runner->dataWin->sprt.count) {
                    const char* spriteName = runner->dataWin->sprt.sprites[inst->spriteIndex].name;
                    if (Runner_stringContainsToken(spriteName, "asriel")) {
                        runner->n3dsDrawAsrielBattle = true;
                    }
                }
            }
            if (Runner_is3DSValidObjectIndex(runner, inst->objectIndex) &&
                Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_battlecontroller") &&
                ((int32_t) Runner_get3DSInstanceRealVar(runner, inst, "drawrect") == 1 ||
                 (int32_t) Runner_get3DSInstanceRealVar(runner, inst, "drawbinfo") == 1)) {
                hasActiveBattleController = true;
            }
        }
    }

    runner->n3dsDrawBattleActive = hasLiveBattleBorder && hasActiveBattleController;
    runner->n3dsDrawDodgingBullets = runner->n3dsDrawBattleActive && (Runner_get3DSGlobalReal(runner, "turntimer") > 0.0);
    runner->n3dsDrawBattleStateValid = true;
}

static bool Runner_is3DSLiveBattleBorderObject(Runner* runner, Instance* inst) {
    if (runner == NULL || inst == NULL) return false;
    if (!Runner_is3DSValidObjectIndex(runner, inst->objectIndex)) return false;

    return Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_uborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_dborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_lborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_rborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_blackborderer");
}

static bool Runner_is3DSBattleActive(Runner* runner) {
    if (runner == NULL) return false;
    if (runner->n3dsDrawBattleStateValid) return runner->n3dsDrawBattleActive;
    Runner_prepare3DSDrawBattleState(runner);
    return runner->n3dsDrawBattleActive;
}

static bool Runner_is3DSValidObjectIndex(Runner* runner, int32_t objectIndex) {
    return runner != NULL &&
        runner->dataWin != NULL &&
        objectIndex >= 0 &&
        (uint32_t) objectIndex < runner->dataWin->objt.count;
}

static const char* Runner_get3DSObjectName(Runner* runner, int32_t objectIndex) {
    if (!Runner_is3DSValidObjectIndex(runner, objectIndex)) return NULL;
    return runner->dataWin->objt.objects[objectIndex].name;
}

static bool Runner_objectMatches3DSNameInHierarchy(Runner* runner, int32_t objectIndex, const char* name) {
    int32_t depth = 0;
    while (Runner_is3DSValidObjectIndex(runner, objectIndex) && depth < 64) {
        const char* objectName = Runner_get3DSObjectName(runner, objectIndex);
        if (objectName != NULL && strcmp(objectName, name) == 0) return true;
        objectIndex = runner->dataWin->objt.objects[objectIndex].parentId;
        depth++;
    }
    return false;
}

static bool Runner_objectContains3DSTokenInHierarchy(Runner* runner, int32_t objectIndex, const char* token) {
    int32_t depth = 0;
    while (Runner_is3DSValidObjectIndex(runner, objectIndex) && depth < 64) {
        const char* objectName = Runner_get3DSObjectName(runner, objectIndex);
        if (Runner_stringContainsToken(objectName, token)) return true;
        objectIndex = runner->dataWin->objt.objects[objectIndex].parentId;
        depth++;
    }
    return false;
}

static bool Runner_is3DSInstanceInsideBattleField(Runner* runner, Instance* inst) {
    if (runner == NULL || inst == NULL) return false;

    double left = Runner_get3DSGlobalArrayReal(runner, "idealborder", 0);
    double right = Runner_get3DSGlobalArrayReal(runner, "idealborder", 1);
    double top = Runner_get3DSGlobalArrayReal(runner, "idealborder", 2);
    double bottom = Runner_get3DSGlobalArrayReal(runner, "idealborder", 3);
    if (right <= left || bottom <= top) return false;

    double marginX = 24.0;
    double marginY = 24.0;
    double expandedLeft = left - marginX;
    double expandedRight = right + marginX;
    double expandedTop = top - marginY;
    double expandedBottom = bottom + marginY;

    InstanceBBox bbox = Collision_computeBBox(runner->dataWin, inst);
    if (bbox.valid) {
        double overlapLeft = fmax((double) bbox.left, expandedLeft);
        double overlapRight = fmin((double) bbox.right, expandedRight);
        double overlapTop = fmax((double) bbox.top, expandedTop);
        double overlapBottom = fmin((double) bbox.bottom, expandedBottom);
        if ((overlapRight - overlapLeft) > 4.0 && (overlapBottom - overlapTop) > 4.0) {
            return true;
        }
    }

    double x = inst->x;
    double y = inst->y;
    return x >= expandedLeft && x <= expandedRight &&
        y >= expandedTop && y <= expandedBottom;
}

static bool Runner_shouldCull3DSBottomBattleFieldInstance(Runner* runner, Instance* inst) {
    if (runner == NULL || inst == NULL) return false;
    return false;
}

static bool Runner_is3DSTopEnemyDialogueObjectIndex(Runner* runner, int32_t objectIndex) {
    if (!Runner_is3DSValidObjectIndex(runner, objectIndex)) return false;

    return Runner_objectContains3DSTokenInHierarchy(runner, objectIndex, "blcon") ||
        Runner_objectMatches3DSNameInHierarchy(runner, objectIndex, "obj_blconsm") ||
        Runner_objectMatches3DSNameInHierarchy(runner, objectIndex, "obj_blconwdflowey") ||
        Runner_objectMatches3DSNameInHierarchy(runner, objectIndex, "obj_blconwideslave");
}

static bool Runner_is3DSTopEnemyDialogueObject(Runner* runner, Instance* inst) {
    if (runner == NULL || inst == NULL) return false;
    return Runner_is3DSTopEnemyDialogueObjectIndex(runner, inst->objectIndex);
}

static bool Runner_is3DSBattleWriterObjectIndex(Runner* runner, int32_t objectIndex) {
    if (!Runner_is3DSValidObjectIndex(runner, objectIndex)) return false;

    return Runner_objectMatches3DSNameInHierarchy(runner, objectIndex, "obj_writer") ||
        Runner_objectMatches3DSNameInHierarchy(runner, objectIndex, "obj_writer_quiz") ||
        Runner_objectMatches3DSNameInHierarchy(runner, objectIndex, "obj_healwriter") ||
        Runner_objectMatches3DSNameInHierarchy(runner, objectIndex, "OBJ_WRITER") ||
        Runner_objectMatches3DSNameInHierarchy(runner, objectIndex, "OBJ_NOMSCWRITER");
}

static bool Runner_has3DSTopEnemyDialogue(Runner* runner, Drawable* drawables, int32_t drawableCount) {
    if (runner == NULL || drawables == NULL || drawableCount <= 0) return false;

    repeat(drawableCount, i) {
        Drawable* d = &drawables[i];
        if (d->type != DRAWABLE_INSTANCE) continue;

        Instance* inst = d->instance;
        if (inst == NULL || !inst->active || !inst->visible) continue;
        if (Runner_is3DSTopEnemyDialogueObject(runner, inst)) return true;
    }

    return false;
}

static bool Runner_is3DSBattleBackdropInstance(Runner* runner, Instance* inst) {
    if (runner == NULL || inst == NULL) return false;

    if (Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_battlebg") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "battlebg") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "gridfriend")) {
        return true;
    }

    if (inst->spriteIndex < 0 || (uint32_t) inst->spriteIndex >= runner->dataWin->sprt.count) {
        return false;
    }

    Sprite* sprite = &runner->dataWin->sprt.sprites[inst->spriteIndex];
    const char* spriteName = sprite->name;
    if (Runner_stringContainsToken(spriteName, "battlebg") ||
        Runner_stringContainsToken(spriteName, "gridfriend")) {
        return true;
    }

    // Treat room-sized battle backdrops as non-enemy scenery so the enemy-only
    // top-screen offset doesn't drag the whole scene downward.
    return sprite->width >= 320 || sprite->height >= 240;
}

static bool Runner_shouldOffset3DSTopBattleInstance(Runner* runner, Instance* inst) {
    if (runner == NULL || inst == NULL) return false;
    Runner_prepare3DSDrawBattleState(runner);
    if (!runner->n3dsDrawBattleActive) return false;
    if (Runner_is3DSAsrielBattle(runner)) return false;
    if (Runner_shouldHideOn3DSTopScreen(runner, inst)) return false;
    if (Runner_is3DSBattleBackdropInstance(runner, inst)) return false;
    if (Runner_is3DSTopEnemyDialogueObject(runner, inst)) return false;
    if (Runner_is3DSBattleWriterObjectIndex(runner, inst->objectIndex)) return false;
    return true;
}

static float Runner_get3DSTopBattleInstanceYOffset(Runner* runner, Instance* inst) {
    if (inst == NULL) return 0.0f;
    if (Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "asgoreb") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "asgorespear")) {
        return -200.0f;
    }
    return k3DSTopBattleEnemyInstanceYOffset;
}

static bool Runner_is3DSBattleUIObject(Runner* runner, Instance* inst) {
    if (runner == NULL || inst == NULL) return false;
    if (!Runner_is3DSValidObjectIndex(runner, inst->objectIndex)) return false;
    Runner_prepare3DSDrawBattleState(runner);
    if (!runner->n3dsDrawBattleActive) return false;
    if (Runner_is3DSTopEnemyDialogueObjectIndex(runner, inst->objectIndex)) return false;
    if (runner->n3dsDrawHasTopEnemyDialogue && Runner_is3DSBattleWriterObjectIndex(runner, inst->objectIndex)) return false;

    const char* objectName = Runner_get3DSObjectName(runner, inst->objectIndex);
    if (objectName == NULL) return false;

    if (
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_battlecontroller") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_blackborderer") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_uborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_dborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_lborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_rborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_fightbt") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_itembt") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_sparebt") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_talkbt") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_anybt") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_hpname") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_dbulletcontroller") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_sinefi") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_tensionbar") ||
        Runner_is3DSBattleWriterObjectIndex(runner, inst->objectIndex)) {
        return true;
    }

    if (Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "border") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "bullet") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "heart") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "soul")) {
        return true;
    }

    if (inst->spriteIndex >= 0 && (uint32_t) inst->spriteIndex < runner->dataWin->sprt.count) {
        const char* spriteName = runner->dataWin->sprt.sprites[inst->spriteIndex].name;
        if (Runner_stringContainsToken(spriteName, "bullet") ||
            Runner_stringContainsToken(spriteName, "heart") ||
            Runner_stringContainsToken(spriteName, "soul")) {
            return true;
        }
    }

    if (Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "blt") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_asgore_spearswipe") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_asgore_spearswipegen") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_asgorebulparent") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_asgoreattackgen") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_asgore_firehit") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_asgorefakespear") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "menubone") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "bonestab") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "bonewall") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "boneplat") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "boneloop") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "sans_bonebul") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "discoball") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "legline") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "sidedam") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "woshspiral") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "blackbox_rewind") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "answernode") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "target") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "eyeflash")) {
        return true;
    }

    if (runner->n3dsDrawDodgingBullets && Runner_is3DSInstanceInsideBattleField(runner, inst)) {
        return true;
    }

    return false;
}

static bool Runner_is3DSBattleFieldObject(Runner* runner, Instance* inst) {
    if (runner == NULL || inst == NULL) return false;
    if (!Runner_is3DSValidObjectIndex(runner, inst->objectIndex)) return false;
    Runner_prepare3DSDrawBattleState(runner);
    if (!runner->n3dsDrawBattleActive) return false;
    bool isDodgingBullets = runner->n3dsDrawDodgingBullets;

    if ((isDodgingBullets &&
            Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_battlecontroller")) ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_blackborderer") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_uborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_dborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_lborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_rborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_dbulletcontroller") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_sinefi")) {
        return true;
    }

    if (Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "border") ||
        Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "bullet") ||
        (isDodgingBullets && (
            Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "heart") ||
            Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "soul")))) {
        return true;
    }

    if (inst->spriteIndex >= 0 && (uint32_t) inst->spriteIndex < runner->dataWin->sprt.count) {
        const char* spriteName = runner->dataWin->sprt.sprites[inst->spriteIndex].name;
        if (Runner_stringContainsToken(spriteName, "bullet") ||
            (isDodgingBullets && (
                Runner_stringContainsToken(spriteName, "heart") ||
                Runner_stringContainsToken(spriteName, "soul")))) {
            return true;
        }
    }

    if (isDodgingBullets && Runner_is3DSInstanceInsideBattleField(runner, inst)) {
        return true;
    }

    return false;
}

static bool Runner_is3DSBattleFieldBackLayerObject(Runner* runner, Instance* inst) {
    if (runner == NULL || inst == NULL) return false;
    if (!Runner_is3DSValidObjectIndex(runner, inst->objectIndex)) return false;

    if (Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_battlecontroller") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_blackborderer") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_uborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_dborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_lborder") ||
        Runner_objectMatches3DSNameInHierarchy(runner, inst->objectIndex, "obj_rborder")) {
        return true;
    }

    if (Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "border")) {
        return true;
    }

    if (inst->spriteIndex >= 0 && (uint32_t) inst->spriteIndex < runner->dataWin->sprt.count) {
        const char* spriteName = runner->dataWin->sprt.sprites[inst->spriteIndex].name;
        if (Runner_stringContainsToken(spriteName, "border")) {
            return true;
        }
    }

    return false;
}

static bool Runner_is3DSAsrielBattle(Runner* runner) {
    if (runner == NULL) return false;
    Runner_prepare3DSDrawBattleState(runner);
    return runner->n3dsDrawBattleActive && runner->n3dsDrawAsrielBattle;
}

static bool Runner_is3DSUndyneBattle(Runner* runner) {
    if (runner == NULL) return false;
    Runner_prepare3DSDrawBattleState(runner);
    if (!runner->n3dsDrawBattleActive) return false;
    if (runner->instances == NULL) return false;

    int32_t instanceCount = (int32_t) arrlen(runner->instances);
    repeat(instanceCount, i) {
        Instance* inst = runner->instances[i];
        if (inst == NULL || inst->destroyed || !inst->active || !inst->visible) continue;
        if (!Runner_is3DSValidObjectIndex(runner, inst->objectIndex)) continue;
        if (Runner_is3DSBattleBackdropInstance(runner, inst)) continue;

        if (Runner_objectContains3DSTokenInHierarchy(runner, inst->objectIndex, "undyne")) {
            return true;
        }

        if (inst->spriteIndex >= 0 && (uint32_t) inst->spriteIndex < runner->dataWin->sprt.count) {
            const char* spriteName = runner->dataWin->sprt.sprites[inst->spriteIndex].name;
            if (Runner_stringContainsToken(spriteName, "undyne")) {
                return true;
            }
        }
    }

    return false;
}

static void Runner_prepare3DSBattleReplayLists(Runner* runner, Drawable* drawables, int32_t drawableCount) {
    if (runner == NULL || runner->n3dsBattleReplayListsValid) return;

    arrsetlen(runner->n3dsBattleFieldInstances, 0);
    arrsetlen(runner->n3dsBattleUIInstances, 0);
    Runner_prepare3DSDrawBattleState(runner);
    if (!runner->n3dsDrawBattleActive) {
        runner->n3dsBattleReplayListsValid = true;
        return;
    }
    bool hasTopEnemyDialogue = Runner_has3DSTopEnemyDialogue(runner, drawables, drawableCount);
    runner->n3dsDrawHasTopEnemyDialogue = hasTopEnemyDialogue;

    repeat(drawableCount, i) {
        Drawable* d = &drawables[i];
        if (d->type != DRAWABLE_INSTANCE) continue;

        Instance* inst = d->instance;
        if (inst == NULL || !inst->active || !inst->visible) continue;

        if (Runner_is3DSBattleFieldObject(runner, inst) &&
            Runner_is3DSBattleFieldBackLayerObject(runner, inst)) {
            arrput(runner->n3dsBattleFieldInstances, inst);
        }
    }

    repeat(drawableCount, i) {
        Drawable* d = &drawables[i];
        if (d->type != DRAWABLE_INSTANCE) continue;

        Instance* inst = d->instance;
        if (inst == NULL || !inst->active || !inst->visible) continue;

        bool isBattleFieldObject = Runner_is3DSBattleFieldObject(runner, inst);
        bool isBattleUIObject = Runner_is3DSBattleUIObject(runner, inst);
        if (isBattleFieldObject && !Runner_is3DSBattleFieldBackLayerObject(runner, inst)) {
            arrput(runner->n3dsBattleFieldInstances, inst);
        }
        if (hasTopEnemyDialogue && Runner_is3DSBattleWriterObjectIndex(runner, inst->objectIndex)) {
            isBattleUIObject = false;
        }
        if (isBattleUIObject && !isBattleFieldObject) arrput(runner->n3dsBattleUIInstances, inst);
    }

    runner->n3dsBattleReplayListsValid = true;
}

static int32_t Runner_get3DSGUIResponderCacheIndex(int32_t subtype) {
    switch (subtype) {
        case DRAW_GUI_BEGIN: return 0;
        case DRAW_GUI: return 1;
        case DRAW_GUI_END: return 2;
        default: return -1;
    }
}

static void Runner_prepare3DSTopScreenGUIInstanceList(Runner* runner, Drawable* drawables, int32_t drawableCount) {
    if (runner == NULL || runner->n3dsTopScreenGUIListValid) return;

    arrsetlen(runner->n3dsTopScreenGUIInstances, 0);
    Runner_prepare3DSDrawBattleState(runner);
    runner->n3dsDrawHasTopEnemyDialogue = Runner_has3DSTopEnemyDialogue(runner, drawables, drawableCount);

    repeat(drawableCount, i) {
        Drawable* d = &drawables[i];
        if (d->type != DRAWABLE_INSTANCE) continue;

        Instance* inst = d->instance;
        if (inst == NULL || !inst->active || !inst->visible) continue;
        if (runner->n3dsDrawBattleActive && Runner_shouldHideOn3DSTopScreen(runner, inst)) continue;
        arrput(runner->n3dsTopScreenGUIInstances, inst);
    }

    runner->n3dsTopScreenGUIListValid = true;
}

static N3DSResolvedDrawEvent* Runner_prepare3DSTopScreenGUIResponderList(Runner* runner, Drawable* drawables, int32_t drawableCount, int32_t subtype, int32_t slot) {
    int32_t cacheIndex = Runner_get3DSGUIResponderCacheIndex(subtype);
    if (runner == NULL || cacheIndex < 0 || slot < 0) return nullptr;

    if (!runner->n3dsTopScreenGUIResponderListsValid[cacheIndex]) {
        Runner_prepare3DSTopScreenGUIInstanceList(runner, drawables, drawableCount);
        bool hasTopEnemyDialogue = Runner_has3DSTopEnemyDialogue(runner, drawables, drawableCount);
        arrsetlen(runner->n3dsTopScreenGUIResponderEvents[cacheIndex], 0);

        int32_t instanceCount = (int32_t) arrlen(runner->n3dsTopScreenGUIInstances);
        repeat(instanceCount, i) {
            Instance* inst = runner->n3dsTopScreenGUIInstances[i];
            if (inst == NULL || !inst->active || !inst->visible) continue;
            if (runner->n3dsDrawBattleActive &&
                !Runner_is3DSTopEnemyDialogueObject(runner, inst) &&
                !(hasTopEnemyDialogue && Runner_is3DSBattleWriterObjectIndex(runner, inst->objectIndex))) {
                continue;
            }

            int32_t ownerObjectIndex = -1;
            int32_t codeId = ResolvedEventTable_lookup(&runner->eventTable, inst->objectIndex, slot, &ownerObjectIndex);
            if (0 > codeId) continue;

            N3DSResolvedDrawEvent resolvedEvent = {
                .instance = inst,
                .codeId = codeId,
                .ownerObjectIndex = ownerObjectIndex,
            };
            arrput(runner->n3dsTopScreenGUIResponderEvents[cacheIndex], resolvedEvent);
        }

        runner->n3dsTopScreenGUIResponderListsValid[cacheIndex] = true;
    }

    return runner->n3dsTopScreenGUIResponderEvents[cacheIndex];
}

static void Runner_draw3DSBottomBattleUI(Runner* runner, Drawable* drawables, int32_t drawableCount, int32_t subtype, bool drawBattleFieldOnly, float scale, float yOffset) {
    if (runner == NULL || runner->renderer == NULL) return;
#ifdef N3DS_DISABLE_BOTTOM_SCREEN
    (void) drawables;
    (void) drawableCount;
    (void) subtype;
    (void) drawBattleFieldOnly;
    (void) scale;
    (void) yOffset;
    return;
#endif
    int32_t slot = EventSlotMap_lookup(&runner->eventSlotMap, EVENT_DRAW, subtype);
    if (subtype != DRAW_NORMAL && slot < 0) return;
    if (Runner_is3DSAsrielBattle(runner)) return;
    Runner_prepare3DSBattleReplayLists(runner, drawables, drawableCount);
    if (!runner->n3dsDrawBattleActive) return;
    Instance** instances = drawBattleFieldOnly ? runner->n3dsBattleFieldInstances : runner->n3dsBattleUIInstances;
    int32_t instanceCount = (int32_t) arrlen(instances);
    if (instanceCount <= 0) return;

    int32_t guiW = runner->guiWidth > 0 ? runner->guiWidth : (int32_t) runner->dataWin->gen8.defaultWindowWidth;
    int32_t guiH = runner->guiHeight > 0 ? runner->guiHeight : (int32_t) runner->dataWin->gen8.defaultWindowHeight;
    if (guiW <= 0) guiW = 320;
    if (guiH <= 0) guiH = 240;

    if (scale == 1.0f && yOffset == 0.0f) {
        N3DSRenderer_beginBottomScreenGUI(runner->renderer, guiW, guiH);
    } else {
        N3DSRenderer_beginBottomScreenGUIEx(runner->renderer, guiW, guiH, scale, scale, 0.0f, yOffset);
    }
    repeat(instanceCount, i) {
        Instance* inst = instances[i];
        if (inst == NULL || !inst->active || !inst->visible) continue;
        if (drawBattleFieldOnly && subtype == DRAW_NORMAL && Runner_shouldCull3DSBottomBattleFieldInstance(runner, inst)) {
            continue;
        }
        int32_t ownerObjectIndex = -1;
        int32_t codeId = slot >= 0
            ? ResolvedEventTable_lookup(&runner->eventTable, inst->objectIndex, slot, &ownerObjectIndex)
            : -1;
        if (codeId >= 0) {
            Runner_executeResolvedEvent(runner, inst, EVENT_DRAW, subtype, codeId, ownerObjectIndex);
        } else {
            if (subtype == DRAW_NORMAL) {
                Renderer_drawSelf(runner->renderer, inst);
            }
        }
    }
    N3DSRenderer_endBottomScreenGUI(runner->renderer);
}

static bool Runner_shouldHideOn3DSTopScreen(Runner* runner, Instance* inst) {
    if (runner == NULL || runner->renderer == NULL || inst == NULL) return false;
    if (Runner_is3DSAsrielBattle(runner)) return false;
    return Runner_is3DSBattleUIObject(runner, inst);
}
#endif

// ===[ Runtime Layer Teardown Helpers ]===
void Runner_freeRuntimeLayer(RuntimeLayer* runtimeLayer) {
    if (runtimeLayer->dynamicName != nullptr) {
        free(runtimeLayer->dynamicName);
        runtimeLayer->dynamicName = nullptr;
    }
    size_t elementCount = arrlenu(runtimeLayer->elements);
    for (size_t i = 0; i < elementCount; i++) {
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
// Resolves the handler for (objectIndex, eventType, eventSubtype) via the precomputed ResolvedEventTable.
// Returns the CODE chunk handler id, or -1 if the object does not respond.
// If outOwnerObjectIndex is non-null, it is set to the resolved owner objectIndex (-1 if not found).
static int32_t findEventCodeIdAndOwner(Runner* runner, int32_t objectIndex, int32_t eventType, int32_t eventSubtype, int32_t* outOwnerObjectIndex) {
    int32_t slot = EventSlotMap_lookup(&runner->eventSlotMap, eventType, eventSubtype);
    if (0 > slot) {
        if (outOwnerObjectIndex != nullptr) *outOwnerObjectIndex = -1;
        return -1;
    }
    return ResolvedEventTable_lookup(&runner->eventTable, objectIndex, slot, outOwnerObjectIndex);
}

// ===[ Per-Object Instance Lists ]===
// Each instance lives in the list of its own object and every ancestor object (descendant-inclusive).
// This mirrors the native runner and lets collision dispatch iterate only the candidate instances for a target object, instead of scanning the whole room per collision event.
// The difference is that the native runner uses a linked list, while we move things manually with memmove.

void Runner_addInstanceToObjectLists(Runner* runner, Instance* inst) {
    DataWin* dataWin = runner->dataWin;
    int32_t currentObj = inst->objectIndex;
    int32_t depth = 0;
    while (currentObj >= 0 && dataWin->objt.count > (uint32_t) currentObj && 32 > depth) {
        arrput(runner->instancesByObject[currentObj], inst);
        currentObj = dataWin->objt.objects[currentObj].parentId;
        depth++;
    }
    if (inst->objectIndex >= 0 && dataWin->objt.count > (uint32_t) inst->objectIndex) {
        arrput(runner->instancesByExactObject[inst->objectIndex], inst);
    }
    SpatialGrid_markInstanceAsDirty(runner->spatialGrid, inst);
}

// Stable remove of inst from list, preserving creation order. Returns true if removed.
static bool removeInstanceFromList(Instance*** listPtr, Instance* inst) {
    Instance** list = *listPtr;
    int32_t n = (int32_t) arrlen(list);
    repeat(n, i) {
        if (list[i] == inst) {
            if (n - 1 > i) memmove(&list[i], &list[i + 1], (size_t) (n - 1 - i) * sizeof(Instance*));
            arrsetlen(*listPtr, n - 1);
            return true;
        }
    }
    return false;
}

void Runner_removeInstanceFromObjectLists(Runner* runner, Instance* inst) {
    DataWin* dataWin = runner->dataWin;
    int32_t currentObj = inst->objectIndex;
    int32_t depth = 0;
    while (currentObj >= 0 && dataWin->objt.count > (uint32_t) currentObj && 32 > depth) {
        removeInstanceFromList(&runner->instancesByObject[currentObj], inst);
        currentObj = dataWin->objt.objects[currentObj].parentId;
        depth++;
    }
    if (inst->objectIndex >= 0 && dataWin->objt.count > (uint32_t) inst->objectIndex) {
        removeInstanceFromList(&runner->instancesByExactObject[inst->objectIndex], inst);
    }
    SpatialGrid_markInstanceAsDirty(runner->spatialGrid, inst);
}

void Runner_clearAllObjectLists(Runner* runner) {
    if (runner->instancesByObject == nullptr) return;
    uint32_t count = runner->dataWin->objt.count;
    repeat(count, i) {
        arrsetlen(runner->instancesByObject[i], 0);
        if (runner->instancesByExactObject != nullptr) {
            arrsetlen(runner->instancesByExactObject[i], 0);
        }
    }
}

int32_t Runner_pushInstancesOfObject(Runner* runner, int32_t targetObjIndex) {
    int32_t base = (int32_t) arrlen(runner->instanceSnapshots);

    if (0 > targetObjIndex || (uint32_t) targetObjIndex >= runner->dataWin->objt.count)
        return base;

    Instance** source = runner->instancesByObject[targetObjIndex];
    int32_t sourceCount = (int32_t) arrlen(source);

    if (0 >= sourceCount)
        return base;

    arrsetlen(runner->instanceSnapshots, base + sourceCount);
    memcpy(&runner->instanceSnapshots[base], source, (size_t) sourceCount * sizeof(Instance*));
    return base;
}

void Runner_popInstanceSnapshot(Runner* runner, int32_t base) {
    arrsetlen(runner->instanceSnapshots, base);
}

int32_t Runner_pushInstancesForTarget(Runner* runner, int32_t target) {
    int32_t base = (int32_t) arrlen(runner->instanceSnapshots);
    if (target >= 0 && 100000 > target) {
        return Runner_pushInstancesOfObject(runner, target);
    }
    if (target == INSTANCE_ALL) {
        int32_t total = (int32_t) arrlen(runner->instances);
        if (0 >= total)
            return base;
        arrsetlen(runner->instanceSnapshots, base + total);
        memcpy(&runner->instanceSnapshots[base], runner->instances, (size_t) total * sizeof(Instance*));
        return base;
    }
    if (target >= 100000) {
        Instance* inst = hmget(runner->instancesById, target);
        if (inst != nullptr) arrput(runner->instanceSnapshots, inst);
        return base;
    }
    return base;
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
    IntIntHashMap* savedCodeLocalsSlotMap = vm->currentCodeLocalsSlotMap;
    int32_t savedCodeIndex = vm->currentCodeIndex;
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
    vm->currentCodeLocalsSlotMap = savedCodeLocalsSlotMap;
    vm->currentCodeIndex = savedCodeIndex;
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

// Executes an already-resolved event handler (see findEventCodeIdAndOwner) and verified codeId >= 0.
static void Runner_executeResolvedEvent(Runner* runner, Instance* instance, int32_t eventType, int32_t eventSubtype, int32_t codeId, int32_t ownerObjectIndex) {
    VMContext* vm = runner->vmContext;
    int32_t savedEventType = vm->currentEventType;
    int32_t savedEventSubtype = vm->currentEventSubtype;
    int32_t savedEventObjectIndex = vm->currentEventObjectIndex;

    vm->currentEventType = eventType;
    vm->currentEventSubtype = eventSubtype;
    vm->currentEventObjectIndex = ownerObjectIndex;

#ifdef ENABLE_VM_TRACING
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

void Runner_executeEventFromObject(Runner* runner, Instance* instance, int32_t startObjectIndex, int32_t eventType, int32_t eventSubtype) {
    int32_t ownerObjectIndex = -1;
    int32_t codeId = findEventCodeIdAndOwner(runner, startObjectIndex, eventType, eventSubtype, &ownerObjectIndex);
    // Fast path: If the codeId is invalid, let's bail out fast
    // This way can avoid the need of loading and saving the current state variables
    if (0 > codeId)
        return;
    Runner_executeResolvedEvent(runner, instance, eventType, eventSubtype, codeId, ownerObjectIndex);
}

void Runner_executeEvent(Runner* runner, Instance* instance, int32_t eventType, int32_t eventSubtype) {
    Runner_executeEventFromObject(runner, instance, instance->objectIndex, eventType, eventSubtype);
}

// Events that GMS 2.3+ routes through the per-object obj_has_event table instead of Perform_Event_All.
// Anything else (BC16 ALWAYS; BC17 non-perObject) goes through Runner_executeEventForAll
static bool eventUsesBC17PerObjectDispatch(int32_t eventType) {
    return eventType == EVENT_STEP || eventType == EVENT_ALARM || eventType == EVENT_KEYBOARD || eventType == EVENT_KEYPRESS || eventType == EVENT_KEYRELEASE;
}

void Runner_executeEventForAll(Runner* runner, int32_t eventType, int32_t eventSubtype) {
    int32_t slot = EventSlotMap_lookup(&runner->eventSlotMap, eventType, eventSubtype);
    if (slot == -1) return;
    bool profileStepNormal = (eventType == EVENT_STEP && eventSubtype == STEP_NORMAL);

    // We always snapshot the iteration list before dispatching so instances spawned during this phase do NOT fire the current event.
    Instance** scratch = runner->eventDispatchInstances;
    arrsetlen(scratch, 0);

    // On GMS 2.x, the native runner dispatches events in the eventUsesPerObjectDispatch set per-object. Route those through executeEventPerObject to match.
    if (DataWin_isVersionAtLeast(runner->dataWin, 2, 0, 0, 0) && eventUsesBC17PerObjectDispatch(eventType)) {
        ResolvedEventTable* table = &runner->eventTable;
        uint32_t entryCount;
        SlotResponderEntry* entries = ResolvedEventTable_slotEntries(table, slot, &entryCount);
        if (entryCount == 0) return;

        repeat(entryCount, i) {
            int32_t concreteObj = entries[i].concreteObjectId;
            Instance** bucket = runner->instancesByExactObject[concreteObj];
            int32_t bucketCount = (int32_t) arrlen(bucket);
            if (bucketCount == 0) continue;

            int32_t ownerObjectIndex = -1;
            int32_t codeId = ResolvedEventTable_lookup(table, concreteObj, slot, &ownerObjectIndex);
            if (codeId < 0) continue;

            int32_t snapshotBase = (int32_t) arrlen(runner->instanceSnapshots);
            arrsetlen(runner->instanceSnapshots, snapshotBase + bucketCount);
            memcpy(&runner->instanceSnapshots[snapshotBase], bucket, (size_t) bucketCount * sizeof(Instance*));

            repeat(bucketCount, j) {
                Instance* inst = runner->instanceSnapshots[snapshotBase + j];
                if (!inst->active) continue;
                double execStartMs = profileStepNormal ? Runner_nowMs() : 0.0;
                Runner_executeResolvedEvent(runner, inst, eventType, eventSubtype, codeId, ownerObjectIndex);
                if (profileStepNormal && inst->objectIndex >= 0 && (uint32_t) inst->objectIndex < runner->dataWin->objt.count) {
                    runner->frameStepObjectMsByObject[inst->objectIndex] += Runner_nowMs() - execStartMs;
                    runner->frameStepObjectCallsByObject[inst->objectIndex]++;
                }
            }

            arrsetlen(runner->instanceSnapshots, snapshotBase);
        }
        return;
    }

    int32_t count = (int32_t) arrlen(runner->instances);
    if (count == 0) return;
    arrsetlen(scratch, count);
    memcpy(scratch, runner->instances, (size_t) count * sizeof(Instance*));
    runner->eventDispatchInstances = scratch;

    repeat(count, i) {
        Instance* inst = scratch[i];
        if (!inst->active) continue;
        // Skip non-responders without entering Runner_executeEvent. ResolvedEventTable_lookup is a tiny CSR scan; non-responders bail in a few compares and avoid the VM state save/restore overhead inside Runner_executeEventFromObject.
        int32_t ownerObjectIndex = -1;
        int32_t codeId = ResolvedEventTable_lookup(&runner->eventTable, inst->objectIndex, slot, &ownerObjectIndex);
        if (0 > codeId) continue;
        double execStartMs = profileStepNormal ? Runner_nowMs() : 0.0;
        Runner_executeResolvedEvent(runner, inst, eventType, eventSubtype, codeId, ownerObjectIndex);
        if (profileStepNormal && inst->objectIndex >= 0 && (uint32_t) inst->objectIndex < runner->dataWin->objt.count) {
            runner->frameStepObjectMsByObject[inst->objectIndex] += Runner_nowMs() - execStartMs;
            runner->frameStepObjectCallsByObject[inst->objectIndex]++;
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
    double startMs = Runner_nowMs();
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
        runner->frameDrawBackgrounds++;
    }
    runner->frameDrawBackgroundMs += Runner_nowMs() - startMs;
}

// ===[ Draw ]===

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
    // At same depth, newer instances (higher instanceId) draw FIRST (behind), older draw LAST (front).
    if (da->type == DRAWABLE_INSTANCE && db->type == DRAWABLE_INSTANCE) {
        if (db->instance->instanceId > da->instance->instanceId) return 1;
        if (da->instance->instanceId > db->instance->instanceId) return -1;
    }
    return 0;
}

static void fireDrawSubtype(Runner* runner, Drawable* drawables, int32_t drawableCount, int32_t subtype) {
    int32_t slot = EventSlotMap_lookup(&runner->eventSlotMap, EVENT_DRAW, subtype);
    if (slot == -1) return;

#ifdef __3DS__
    if ((subtype == DRAW_GUI_BEGIN || subtype == DRAW_GUI || subtype == DRAW_GUI_END) && runner != NULL) {
        Runner_prepare3DSDrawBattleState(runner);
        if (runner->n3dsDrawBattleActive) {
            int32_t guiW = runner->guiWidth > 0 ? runner->guiWidth : (int32_t) runner->dataWin->gen8.defaultWindowWidth;
            int32_t guiH = runner->guiHeight > 0 ? runner->guiHeight : (int32_t) runner->dataWin->gen8.defaultWindowHeight;
            if (guiW <= 0) guiW = 320;
            if (guiH <= 0) guiH = 240;
            bool useTopEnemyDialogue2x = Runner_has3DSTopEnemyDialogue(runner, drawables, drawableCount);
            runner->n3dsDrawHasTopEnemyDialogue = useTopEnemyDialogue2x;
            if (useTopEnemyDialogue2x) N3DSRenderer_beginTopScreenGUI2x(runner->renderer, guiW, guiH);
            else N3DSRenderer_beginTopScreenGUI(runner->renderer, guiW, guiH);

            N3DSResolvedDrawEvent* responderEvents = Runner_prepare3DSTopScreenGUIResponderList(runner, drawables, drawableCount, subtype, slot);
            int32_t responderCount = (int32_t) arrlen(responderEvents);
            repeat(responderCount, i) {
                N3DSResolvedDrawEvent* resolvedEvent = &responderEvents[i];
                Instance* inst = resolvedEvent->instance;
                if (inst == NULL || !inst->active || !inst->visible) continue;
                Runner_executeResolvedEvent(runner, inst, EVENT_DRAW, subtype, resolvedEvent->codeId, resolvedEvent->ownerObjectIndex);
            }
            if (useTopEnemyDialogue2x) N3DSRenderer_endTopScreenGUI2x(runner->renderer);
            else N3DSRenderer_endTopScreenGUI(runner->renderer);
            return;
        }
    }
#endif

    repeat(drawableCount, i) {
        Drawable* d = &drawables[i];
        if (d->type != DRAWABLE_INSTANCE)
            continue;

        Instance* inst = d->instance;
        if (!inst->active || !inst->visible)
            continue;
#ifdef __3DS__
        if ((subtype == DRAW_GUI_BEGIN || subtype == DRAW_GUI || subtype == DRAW_GUI_END) &&
            Runner_shouldHideOn3DSTopScreen(runner, inst)) {
            continue;
        }
#endif

        int32_t ownerObjectIndex = -1;
        int32_t codeId = ResolvedEventTable_lookup(&runner->eventTable, inst->objectIndex, slot, &ownerObjectIndex);
        if (0 > codeId) continue;
        Runner_executeResolvedEvent(runner, inst, EVENT_DRAW, subtype, codeId, ownerObjectIndex);
    }
}

// GMS2 tilemap cell bit layout (matches HTML5 Function_Layers.js TileIndex/Mirror/Flip/Rotate masks)
#define GMS2_TILE_INDEX_MASK  0x0007FFFF // bits 0..18
#define GMS2_TILE_MIRROR_MASK 0x10000000 // bit 28 (horizontal flip)
#define GMS2_TILE_FLIP_MASK   0x20000000 // bit 29 (vertical flip)
#define GMS2_TILE_ROTATE_MASK 0x40000000 // bit 30 (90 CW)

static void Runner_getCurrentViewBounds(Runner* runner, float* outLeft, float* outTop, float* outRight, float* outBottom) {
    Room* room = runner->currentRoom;
    float left = 0.0f;
    float top = 0.0f;
    float right = room != nullptr ? (float) room->width : 0.0f;
    float bottom = room != nullptr ? (float) room->height : 0.0f;

    if (runner != NULL && runner->drawViewBoundsValid) {
        left = (float) runner->drawViewX;
        top = (float) runner->drawViewY;
        right = left + (float) runner->drawViewWidth;
        bottom = top + (float) runner->drawViewHeight;
    } else if (room != nullptr && (room->flags & 1) != 0) {
        int32_t viewIndex = runner->viewCurrent;
        if (viewIndex >= 0 && viewIndex < MAX_VIEWS) {
            RuntimeView* view = &runner->views[viewIndex];
            if (view->enabled) {
                left = (float) view->viewX;
                top = (float) view->viewY;
                right = left + (float) view->viewWidth;
                bottom = top + (float) view->viewHeight;
            }
        }
    }

    *outLeft = left;
    *outTop = top;
    *outRight = right;
    *outBottom = bottom;
}

static bool Runner_rectIntersectsCurrentView(Runner* runner, float x, float y, float w, float h) {
    float viewLeft, viewTop, viewRight, viewBottom;
    Runner_getCurrentViewBounds(runner, &viewLeft, &viewTop, &viewRight, &viewBottom);
    return !(x + w <= viewLeft || y + h <= viewTop || x >= viewRight || y >= viewBottom);
}

static bool Runner_tileIntersectsCurrentView(Runner* runner, RoomTile* tile, float offsetX, float offsetY) {
    float width = (float) tile->width * fabsf(tile->scaleX);
    float height = (float) tile->height * fabsf(tile->scaleY);
    if (width < 1.0f) width = (float) tile->width;
    if (height < 1.0f) height = (float) tile->height;
    return Runner_rectIntersectsCurrentView(runner, (float) tile->x + offsetX, (float) tile->y + offsetY, width, height);
}

static bool Runner_instanceIntersectsCurrentView(Runner* runner, Instance* inst) {
    if (runner == NULL || inst == NULL) return false;

    InstanceBBox bbox = Collision_computeBBox(runner->dataWin, inst);
    if (!bbox.valid) {
        return true;
    }

    return Runner_rectIntersectsCurrentView(
        runner,
        (float) bbox.left,
        (float) bbox.top,
        (float) (bbox.right - bbox.left),
        (float) (bbox.bottom - bbox.top)
    );
}

static void Runner_freeTileLayerCaches(Runner* runner) {
    if (runner == NULL || runner->tileLayerCaches == NULL) return;

    repeat(runner->tileLayerCacheCount, i) {
#ifdef __3DS__
        if (runner->renderer != NULL && runner->tileLayerCaches[i].n3dsChunkCacheId >= 0) {
            N3DSRenderer_destroyTileLayerChunkCache(runner->renderer, runner->tileLayerCaches[i].n3dsChunkCacheId);
            runner->tileLayerCaches[i].n3dsChunkCacheId = -1;
        }
#endif
        free(runner->tileLayerCaches[i].rows);
        free(runner->tileLayerCaches[i].cells);
        runner->tileLayerCaches[i].rows = NULL;
        runner->tileLayerCaches[i].cells = NULL;
        runner->tileLayerCaches[i].built = false;
    }

    free(runner->tileLayerCaches);
    runner->tileLayerCaches = NULL;
    runner->tileLayerCacheCount = 0;
}

static void Runner_buildTileLayerCaches(Runner* runner) {
    if (runner == NULL || runner->currentRoom == NULL) return;

    Runner_freeTileLayerCaches(runner);

    Room* room = runner->currentRoom;
    if (room->layerCount == 0) return;

#ifdef __3DS__
    if (runner->osType == OS_3DS && Runner_isOld3DSLike()) {
        return;
    }
#endif

    runner->tileLayerCaches = safeCalloc(room->layerCount, sizeof(TileLayerRenderCache));
    runner->tileLayerCacheCount = room->layerCount;

    repeat(room->layerCount, i) {
        RoomLayer* layer = &room->layers[i];
        if (layer->type != RoomLayerType_Tiles || layer->tilesData == NULL) continue;

        RoomLayerTilesData* data = layer->tilesData;
        TileLayerRenderCache* cache = &runner->tileLayerCaches[i];
        cache->backgroundIndex = data->backgroundIndex;
        cache->n3dsChunkCacheId = -1;
        cache->tilesX = data->tilesX;
        cache->tilesY = data->tilesY;

        if (data->tileData == NULL || data->tilesX == 0 || data->tilesY == 0 || data->backgroundIndex < 0 ||
            (uint32_t) data->backgroundIndex >= runner->dataWin->bgnd.count) {
            continue;
        }

        Background* tileset = &runner->dataWin->bgnd.backgrounds[data->backgroundIndex];
        if (tileset->gms2TileWidth == 0 || tileset->gms2TileHeight == 0 || tileset->gms2TileColumns == 0) {
            continue;
        }

        cache->tileWidth = tileset->gms2TileWidth;
        cache->tileHeight = tileset->gms2TileHeight;
        cache->rows = safeCalloc(data->tilesY, sizeof(TileLayerCacheRow));

        uint32_t borderX = tileset->gms2OutputBorderX;
        uint32_t borderY = tileset->gms2OutputBorderY;
        uint32_t columns = tileset->gms2TileColumns;
        uint32_t totalNonEmpty = 0;
        repeat(data->tilesY, ty) {
            cache->rows[ty].start = totalNonEmpty;
            repeat(data->tilesX, tx) {
                uint32_t cell = data->tileData[ty * data->tilesX + tx];
                uint32_t tileIndex = cell & GMS2_TILE_INDEX_MASK;
                if (tileIndex == 0) continue;

                uint32_t tileSlot = tileIndex - 1u;
                uint32_t col = tileSlot % columns;
                uint32_t row = tileSlot / columns;

                TileLayerCacheCell cachedCell = {
                    .tileX = tx,
                    .srcX = col * (cache->tileWidth + 2 * borderX) + borderX,
                    .srcY = row * (cache->tileHeight + 2 * borderY) + borderY,
                    .tpagIndex = tileset->tpagIndex,
                    .n3dsTileEntryIndex = -1,
                    .mirror = (cell & GMS2_TILE_MIRROR_MASK) != 0,
                    .flip = (cell & GMS2_TILE_FLIP_MASK) != 0,
                };
#ifdef __3DS__
                if (runner->renderer != NULL) {
                    cachedCell.n3dsTileEntryIndex = N3DSRenderer_findTileEntryIndex(
                        runner->renderer,
                        data->backgroundIndex,
                        (int32_t) cachedCell.srcX,
                        (int32_t) cachedCell.srcY,
                        (int32_t) cache->tileWidth,
                        (int32_t) cache->tileHeight
                    );
                }
#endif
                arrput(cache->cells, cachedCell);
                totalNonEmpty++;
            }
            cache->rows[ty].count = totalNonEmpty - cache->rows[ty].start;
        }

        cache->built = true;
    }
}

static uint32_t Runner_findFirstVisibleTileCell(const TileLayerCacheCell* cells, uint32_t count, uint32_t startX) {
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (cells[mid].tileX < startX) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

void Runner_drawTileLayer(Runner* runner, uint32_t layerIndex, RoomLayerTilesData* data, float layerOffsetX, float layerOffsetY, float alpha) {
    double startMs = Runner_nowMs();
    if (data == nullptr || data->tileData == nullptr) return;
    if (0 > data->backgroundIndex) return;
    if (layerIndex >= runner->tileLayerCacheCount || runner->tileLayerCaches == NULL) return;

    DataWin* dw = runner->dataWin;
    if ((uint32_t) data->backgroundIndex >= dw->bgnd.count) return;

    Background* tileset = &dw->bgnd.backgrounds[data->backgroundIndex];
    if (tileset->gms2TileWidth == 0 || tileset->gms2TileHeight == 0 || tileset->gms2TileColumns == 0) return;

    int32_t tpagIndex = tileset->tpagIndex;
    if (0 > tpagIndex) return;

    uint32_t tileW = tileset->gms2TileWidth;
    uint32_t tileH = tileset->gms2TileHeight;

    static bool rotateWarned = false;
    TileLayerRenderCache* cache = NULL;
    bool useCachedPath = layerIndex < runner->tileLayerCacheCount &&
        runner->tileLayerCaches != NULL;
    if (useCachedPath) {
        cache = &runner->tileLayerCaches[layerIndex];
        useCachedPath = cache->built && cache->cells != NULL && cache->rows != NULL;
    }

#if defined(__3DS__) && N3DS_ENABLE_TILE_LAYER_CHUNK_CACHE
    if (useCachedPath && runner->renderer != NULL && runner->osType == OS_3DS) {
        if (cache->n3dsChunkCacheId < 0 && runner->currentRoom != NULL) {
            cache->n3dsChunkCacheId = N3DSRenderer_createTileLayerChunkCache(
                runner->renderer,
                runner->currentRoom->width,
                runner->currentRoom->height,
                cache
            );
        }
        if (cache->n3dsChunkCacheId >= 0 &&
            N3DSRenderer_drawTileLayerChunkCache(runner->renderer, cache->n3dsChunkCacheId, layerOffsetX, layerOffsetY, alpha)) {
            runner->frameDrawTilesMs += Runner_nowMs() - startMs;
            return;
        }
    }
#endif

    float viewLeft, viewTop, viewRight, viewBottom;
    Runner_getCurrentViewBounds(runner, &viewLeft, &viewTop, &viewRight, &viewBottom);
    int32_t startX = (int32_t) floorf((viewLeft - layerOffsetX) / (float) tileW);
    int32_t startY = (int32_t) floorf((viewTop - layerOffsetY) / (float) tileH);
    int32_t endX = (int32_t) ceilf((viewRight - layerOffsetX) / (float) tileW);
    int32_t endY = (int32_t) ceilf((viewBottom - layerOffsetY) / (float) tileH);
    if (startX < 0) startX = 0;
    if (startY < 0) startY = 0;
    if (endX > (int32_t) data->tilesX) endX = (int32_t) data->tilesX;
    if (endY > (int32_t) data->tilesY) endY = (int32_t) data->tilesY;
    if (startX >= endX || startY >= endY) return;

    uint32_t borderX = tileset->gms2OutputBorderX;
    uint32_t borderY = tileset->gms2OutputBorderY;
    uint32_t columns = tileset->gms2TileColumns;

    for (int32_t ty = startY; ty < endY; ty++) {
        if (useCachedPath) {
            TileLayerCacheRow* rowCache = &cache->rows[ty];
            if (rowCache->count == 0) continue;

            const TileLayerCacheCell* rowCells = &cache->cells[rowCache->start];
            uint32_t startCell = Runner_findFirstVisibleTileCell(rowCells, rowCache->count, (uint32_t) startX);
            for (uint32_t ci = startCell; ci < rowCache->count; ci++) {
                const TileLayerCacheCell* cachedCell = &rowCells[ci];
                if ((int32_t) cachedCell->tileX >= endX) break;

                uint32_t cell = data->tileData[ty * data->tilesX + cachedCell->tileX];
                bool rotate = (cell & GMS2_TILE_ROTATE_MASK) != 0;

                if (rotate && !rotateWarned) {
                    fprintf(stderr, "Runner: WARNING: GMS2 tile layer has rotated tiles; rotation not yet implemented, drawing unrotated\n");
                    rotateWarned = true;
                }

                float xscale = cachedCell->mirror ? -1.0f : 1.0f;
                float yscale = cachedCell->flip ? -1.0f : 1.0f;

                float dstX = (float) (cachedCell->tileX * tileW) + layerOffsetX + (cachedCell->mirror ? (float) tileW : 0.0f);
                float dstY = (float) (ty * tileH) + layerOffsetY + (cachedCell->flip ? (float) tileH : 0.0f);
                float tileAlpha = alpha;
                if (tileAlpha < 0.0f) tileAlpha = 0.0f;
                if (tileAlpha > 1.0f) tileAlpha = 1.0f;
                uint32_t alphaByte = (uint32_t) lroundf(tileAlpha * 255.0f);
#ifdef __3DS__
                if (runner->renderer != NULL && cachedCell->n3dsTileEntryIndex >= 0) {
                    if (N3DSRenderer_drawCachedTileEntry(
                            runner->renderer,
                            cachedCell->n3dsTileEntryIndex,
                            dstX,
                            dstY,
                            xscale,
                            yscale,
                            0x00FFFFFFu,
                            tileAlpha
                        )) {
                        runner->frameDrawTiles++;
                        continue;
                    }
                }
#endif
                RoomTile tile = {
                    .x = (int32_t) lroundf(dstX),
                    .y = (int32_t) lroundf(dstY),
                    .useSpriteDefinition = false,
                    .backgroundDefinition = data->backgroundIndex,
                    .sourceX = (int32_t) cachedCell->srcX,
                    .sourceY = (int32_t) cachedCell->srcY,
                    .width = tileW,
                    .height = tileH,
                    .tileDepth = 0,
                    .instanceID = 0,
                    .scaleX = xscale,
                    .scaleY = yscale,
                    .color = (alphaByte << 24) | 0x00FFFFFFu,
                };
                Renderer_drawTile(runner->renderer, &tile, 0.0f, 0.0f);
                runner->frameDrawTiles++;
            }
            continue;
        }

        for (int32_t tx = startX; tx < endX; tx++) {
            uint32_t cell = data->tileData[ty * data->tilesX + tx];
            uint32_t tileIndex = cell & GMS2_TILE_INDEX_MASK;
            if (tileIndex == 0) continue;

            bool rotate = (cell & GMS2_TILE_ROTATE_MASK) != 0;
            if (rotate && !rotateWarned) {
                fprintf(stderr, "Runner: WARNING: GMS2 tile layer has rotated tiles; rotation not yet implemented, drawing unrotated\n");
                rotateWarned = true;
            }

            uint32_t tileSlot = tileIndex - 1u;
            uint32_t col = tileSlot % columns;
            uint32_t row = tileSlot / columns;
            float xscale = (cell & GMS2_TILE_MIRROR_MASK) != 0 ? -1.0f : 1.0f;
            float yscale = (cell & GMS2_TILE_FLIP_MASK) != 0 ? -1.0f : 1.0f;
            float dstX = (float) (tx * (int32_t) tileW) + layerOffsetX + (xscale < 0.0f ? (float) tileW : 0.0f);
            float dstY = (float) (ty * (int32_t) tileH) + layerOffsetY + (yscale < 0.0f ? (float) tileH : 0.0f);
            float tileAlpha = alpha;
            if (tileAlpha < 0.0f) tileAlpha = 0.0f;
            if (tileAlpha > 1.0f) tileAlpha = 1.0f;
            uint32_t alphaByte = (uint32_t) lroundf(tileAlpha * 255.0f);

            RoomTile tile = {
                .x = (int32_t) lroundf(dstX),
                .y = (int32_t) lroundf(dstY),
                .useSpriteDefinition = false,
                .backgroundDefinition = data->backgroundIndex,
                .sourceX = (int32_t) (col * (tileW + 2 * borderX) + borderX),
                .sourceY = (int32_t) (row * (tileH + 2 * borderY) + borderY),
                .width = tileW,
                .height = tileH,
                .tileDepth = 0,
                .instanceID = 0,
                .scaleX = xscale,
                .scaleY = yscale,
                .color = (alphaByte << 24) | 0x00FFFFFFu,
            };
            Renderer_drawTile(runner->renderer, &tile, 0.0f, 0.0f);
            runner->frameDrawTiles++;
        }
    }
    runner->frameDrawTilesMs += Runner_nowMs() - startMs;
}

void Runner_resetFrameDrawStats(Runner* runner) {
    if (runner == NULL) return;
    runner->frameDrawBackgrounds = 0;
    runner->frameDrawTiles = 0;
    runner->frameDrawInstances = 0;
    runner->frameDrawLayerElements = 0;
    runner->frameDrawBackgroundMs = 0.0;
    runner->frameDrawTilesMs = 0.0;
    runner->frameDrawInstancesMs = 0.0;
    runner->frameDrawLayerMs = 0.0;
    runner->frameDrawGuiMs = 0.0;
    runner->frameDrawGuiEventMs = 0.0;
    runner->frameDrawBattleReplayMs = 0.0;
}

// Returns true if "drawables" is already in compareDrawableDepth order. Used by the sort-dirty path to skip qsort when small depth perturbations didn't actually cross any neighbor.
static bool isDrawableArraySorted(Drawable* drawables, int32_t count) {
    for (int32_t i = 1; count > i; i++) {
        if (compareDrawableDepth(&drawables[i - 1], &drawables[i]) > 0) return false;
    }
    return true;
}

// Refreshes each entry's cached .depth from the live instance/runtime-layer pointer. Tile entries never change depth mid-room so they're left alone.
static void refreshDrawableDepths(Drawable* drawables, int32_t count) {
    for (int32_t i = 0; count > i; i++) {
        Drawable* d = &drawables[i];
        if (d->type == DRAWABLE_INSTANCE) {
            d->depth = d->instance->depth;
        } else if (d->type == DRAWABLE_LAYER) {
            d->depth = d->runtimeLayer->depth;
        }
    }
}

// Rebuilds runner->cachedDrawables when invalidated. Two-tier strategy:
//   structureDirty - the SET of entries changed (instance/layer create or destroy, room change). Drop the cache and re-add every instance/tile/runtime-layer, then qsort.
//   sortDirty only - the entries are the same but .depth values may have shifted. Refresh depths from the live sources and only qsort if the order actually broke.
static void rebuildDrawableCacheIfDirty(Runner* runner) {
    if (runner->drawableListStructureDirty) {
        arrsetlen(runner->cachedDrawables, 0);
        Room* room = runner->currentRoom;
        if (room == nullptr) {
            runner->drawableListStructureDirty = false;
            runner->drawableListSortDirty = false;
            return;
        }

        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            Drawable d = { .type = DRAWABLE_INSTANCE, .depth = inst->depth };
            d.instance = inst;
            arrput(runner->cachedDrawables, d);
        }

        if (!DataWin_isVersionAtLeast(runner->dataWin, 2, 0, 0, 0)) {
            repeat(room->tileCount, i) {
                RoomTile* tile = &room->tiles[i];
                Drawable d = { .type = DRAWABLE_TILE, .depth = tile->tileDepth };
                d.tileIndex = (int32_t) i;
                arrput(runner->cachedDrawables, d);
            }
        } else {
            size_t runtimeLayersCount = arrlenu(runner->runtimeLayers);
            repeat(runtimeLayersCount, i) {
                RuntimeLayer* runtimeLayer = &runner->runtimeLayers[i];
                Drawable d = { .type = DRAWABLE_LAYER, .depth = runtimeLayer->depth };
                d.runtimeLayer = runtimeLayer;
                arrput(runner->cachedDrawables, d);
            }
        }

        int32_t count = (int32_t) arrlen(runner->cachedDrawables);
        if (count > 1) {
            qsort(runner->cachedDrawables, count, sizeof(Drawable), compareDrawableDepth);
        }
        runner->drawableListStructureDirty = false;
        runner->drawableListSortDirty = false;
        return;
    }

    if (runner->drawableListSortDirty) {
        int32_t count = (int32_t) arrlen(runner->cachedDrawables);
        refreshDrawableDepths(runner->cachedDrawables, count);
        if (count > 1 && !isDrawableArraySorted(runner->cachedDrawables, count)) {
            qsort(runner->cachedDrawables, count, sizeof(Drawable), compareDrawableDepth);
        }
        runner->drawableListSortDirty = false;
    }
}

void Runner_draw(Runner* runner) {
    Room* room = runner->currentRoom;

    rebuildDrawableCacheIfDirty(runner);
    int32_t drawableCount = (int32_t) arrlen(runner->cachedDrawables);
    Drawable* drawables = runner->cachedDrawables;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);

    bool traceDraw = false;

    if (traceDraw) {
        char buffer[320];
        snprintf(
            buffer,
            sizeof(buffer),
            "runner: draw room=%s idx=%d pendingRoom=%d instances=%d drawables=%d bgColor=%06X drawBg=%d",
            room != nullptr && room->name != nullptr ? room->name : "<null>",
            runner->currentRoomIndex,
            runner->pendingRoom,
            instanceCount,
            drawableCount,
            runner->backgroundColor & 0xFFFFFFu,
            runner->drawBackgroundColor ? 1 : 0
        );
        Runner_platformBootLog(buffer);

        if (drawableCount == 0) {
            Runner_platformBootLog("runner: no drawables in current room");
        }

        gRunnerLastLoggedRoomIndex = runner->currentRoomIndex;
        gRunnerLastLoggedPendingRoom = runner->pendingRoom;
        gRunnerLastLoggedDrawableCount = drawableCount;
        gRunnerLastLoggedInstanceCount = instanceCount;
        gRunnerDrawTraceCount++;

        int32_t traceInstanceCount = instanceCount;
        if (traceInstanceCount > 4) traceInstanceCount = 4;
        // repeat(traceInstanceCount, ti) {
        //     Instance* inst = runner->instances[ti];
        //     if (inst == nullptr) continue;
        //     char instBuffer[256];
        //     const char* objectName =
        //         (inst->objectIndex >= 0 && (uint32_t) inst->objectIndex < runner->dataWin->objt.count &&
        //          runner->dataWin->objt.objects[inst->objectIndex].name != nullptr)
        //         ? runner->dataWin->objt.objects[inst->objectIndex].name
        //         : "<null>";
        //     snprintf(
        //         instBuffer,
        //         sizeof(instBuffer),
        //         "runner: inst[%d] id=%u obj=%s sprite=%d visible=%d active=%d depth=%d x=%.1f y=%.1f alarm0=%d alarm1=%d",
        //         ti,
        //         inst->instanceId,
        //         objectName,
        //         inst->spriteIndex,
        //         inst->visible ? 1 : 0,
        //         inst->active ? 1 : 0,
        //         inst->depth,
        //         inst->x,
        //         inst->y,
        //         inst->alarm[0],
        //         inst->alarm[1]
        //     );
        //     Runner_bootLog(instBuffer);
        // }
    }

    // Draw non-foreground backgrounds (behind everything)
    if (!DataWin_isVersionAtLeast(runner->dataWin, 2, 0, 0, 0))
        Runner_drawBackgrounds(runner, false);

// Fire draw subtypes in correct GameMaker order. fireDrawSubtype walks the cache and filters inline.
    fireDrawSubtype(runner, drawables, drawableCount, DRAW_PRE);
    fireDrawSubtype(runner, drawables, drawableCount, DRAW_BEGIN);

#ifdef __3DS__
    Runner_prepare3DSDrawBattleState(runner);
    bool n3dsBattleActiveForDraw = runner->n3dsDrawBattleActive;
    int32_t n3dsTopScreenDrawInstanceCursor = 0;
    int32_t n3dsTopScreenDrawInstanceCount = 0;
    if (n3dsBattleActiveForDraw) {
        Runner_prepare3DSTopScreenGUIInstanceList(runner, drawables, drawableCount);
        n3dsTopScreenDrawInstanceCount = (int32_t) arrlen(runner->n3dsTopScreenGUIInstances);
    }
#endif
    int32_t drawNormalSlot = EventSlotMap_lookup(&runner->eventSlotMap, EVENT_DRAW, DRAW_NORMAL);

    // Draw interleaved tiles and instances
    repeat(drawableCount, i) {
        Drawable* d = &drawables[i];
        if (d->type == DRAWABLE_TILE) {
            if (runner->renderer != nullptr) {
                double drawStartMs = Runner_nowMs();
                RoomTile* tile = &room->tiles[d->tileIndex];
                // Skip tiles whose layer was hidden via tile_layer_hide(). Filtered here (not in the cache) so toggling layer visibility doesn't invalidate.
                ptrdiff_t layerIdx = hmgeti(runner->tileLayerMap, tile->tileDepth);
                if (layerIdx >= 0 && !runner->tileLayerMap[layerIdx].value.visible) continue;
                float offsetX = 0.0f, offsetY = 0.0f;
                if (layerIdx >= 0) {
                    offsetX = runner->tileLayerMap[layerIdx].value.offsetX;
                    offsetY = runner->tileLayerMap[layerIdx].value.offsetY;
                }
                if (!Runner_tileIntersectsCurrentView(runner, tile, offsetX, offsetY)) continue;

#ifdef ENABLE_VM_TRACING
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
                runner->frameDrawTiles++;
                runner->frameDrawTilesMs += Runner_nowMs() - drawStartMs;
            }
        } else if (d->type == DRAWABLE_INSTANCE) {
            double drawStartMs = Runner_nowMs();
            Instance* inst = d->instance;
            // Filter inactive/invisible instances at draw time so the cache doesn't need invalidation when those flags toggle.
            if (!inst->active || !inst->visible) continue;
#ifdef __3DS__
            if (n3dsBattleActiveForDraw) {
                if (n3dsTopScreenDrawInstanceCursor >= n3dsTopScreenDrawInstanceCount ||
                    runner->n3dsTopScreenGUIInstances[n3dsTopScreenDrawInstanceCursor] != inst) {
                    continue;
                }
                n3dsTopScreenDrawInstanceCursor++;
            }
#endif
            int32_t ownerObjectIndex = -1;
            int32_t codeId = drawNormalSlot >= 0
                ? ResolvedEventTable_lookup(&runner->eventTable, inst->objectIndex, drawNormalSlot, &ownerObjectIndex)
                : -1;
            if (codeId < 0 && !Runner_instanceIntersectsCurrentView(runner, inst)) continue;
            bool offsetTopBattleEnemy = false;
            double savedY = 0.0;
#ifdef __3DS__
            if (runner->osType == OS_3DS && n3dsBattleActiveForDraw &&
                Runner_shouldOffset3DSTopBattleInstance(runner, inst)) {
                offsetTopBattleEnemy = true;
                savedY = inst->y;
                inst->y += Runner_get3DSTopBattleInstanceYOffset(runner, inst);
            }
#endif
            if (codeId >= 0) {
                Runner_executeResolvedEvent(runner, inst, EVENT_DRAW, DRAW_NORMAL, codeId, ownerObjectIndex);
            } else if (runner->renderer != nullptr) {
                Renderer_drawSelf(runner->renderer, inst);
            }
#ifdef __3DS__
            if (offsetTopBattleEnemy) {
                inst->y = savedY;
            }
#endif
            runner->frameDrawInstances++;
            runner->frameDrawInstancesMs += Runner_nowMs() - drawStartMs;
        } else if (d->type == DRAWABLE_LAYER)
        {
            double layerStartMs = Runner_nowMs();
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
                            if (tpag->boundingWidth == 0 || tpag->boundingHeight == 0) continue;
                            float xscale = roomW / (float) tpag->boundingWidth;
                            float yscale = roomH / (float) tpag->boundingHeight;
                            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, 0.0f, 0.0f, 0.0f, 0.0f, xscale, yscale, 0.0f, bg->blend, bg->alpha);
                        } else if (bg->htiled || bg->vtiled) {
                            Renderer_drawBackgroundTiled(runner->renderer, tpagIndex, layerOffsetX + bg->xOffset, layerOffsetY + bg->yOffset, bg->htiled, bg->vtiled, roomW, roomH, bg->alpha);
                        } else {
                            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, layerOffsetX + bg->xOffset, layerOffsetY + bg->yOffset, 0.0f, 0.0f, bg->xScale, bg->yScale, 0.0f, bg->blend, bg->alpha);
                        }
                        runner->frameDrawLayerElements++;
                    }
                }
                runner->frameDrawLayerMs += Runner_nowMs() - layerStartMs;
                continue;
            }

            // Parsed layer: look up the RoomLayer by ID and render its data-driven content.
            RoomLayer* parsedLayer = Runner_findRoomLayerById(runner, (int32_t) runtimeLayer->id);
            if (parsedLayer == nullptr) continue;
            if (traceDraw) {
                // char layerBuffer[256];
                // uint32_t runtimeElementCount = (uint32_t) arrlenu(runtimeLayer->elements);
                // uint32_t assetsSpriteCount = (parsedLayer->assetsData != nullptr) ? parsedLayer->assetsData->spriteCount : 0u;
                // uint32_t assetsTileCount = (parsedLayer->assetsData != nullptr) ? parsedLayer->assetsData->legacyTileCount : 0u;
                // uint32_t layerInstanceCount = (parsedLayer->instancesData != nullptr) ? parsedLayer->instancesData->instanceCount : 0u;
                // snprintf(
                //     layerBuffer,
                //     sizeof(layerBuffer),
                //     "runner: layer id=%u type=%u depth=%d visible=%d runtimeEls=%u assetsSprites=%u assetsTiles=%u layerInstances=%u",
                //     parsedLayer->id,
                //     parsedLayer->type,
                //     parsedLayer->depth,
                //     parsedLayer->visible ? 1 : 0,
                //     runtimeElementCount,
                //     assetsSpriteCount,
                //     assetsTileCount,
                //     layerInstanceCount
                // );
                // Runner_bootLog(layerBuffer);
            }
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
                        if (!Runner_tileIntersectsCurrentView(runner, tile, offsetX, offsetY)) continue;

#ifdef ENABLE_VM_TRACING
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
                        runner->frameDrawTiles++;
                        runner->frameDrawTilesMs += Runner_nowMs() - layerStartMs;
                        layerStartMs = Runner_nowMs();
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
                    runner->frameDrawLayerElements++;
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
                            if (tpag->boundingWidth == 0 || tpag->boundingHeight == 0) continue;
                            float xscale = roomW / (float) tpag->boundingWidth;
                            float yscale = roomH / (float) tpag->boundingHeight;
                            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, 0.0f, 0.0f, 0.0f, 0.0f, xscale, yscale, 0.0f, 0xFFFFFF, 1.0);
                        } else if (data->hTiled || data->vTiled) {
                            Renderer_drawBackgroundTiled(runner->renderer, tpagIndex, layerOffsetX, layerOffsetY, data->hTiled, data->vTiled, roomW, roomH, 1.0);
                        } else {
                            // Single placement
                            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, layerOffsetX, layerOffsetY, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFF, 1.0);
                        }
                        runner->frameDrawBackgroundMs += Runner_nowMs() - layerStartMs;
                        layerStartMs = Runner_nowMs();
            } else if(parsedLayer->type == RoomLayerType_Instances) {
                // Instance depth is assigned from layers during room init (initRoom).
                // Nothing to do here - instances are drawn from the DRAWABLE_INSTANCE path.
            } else if(parsedLayer->type == RoomLayerType_Tiles) {
                if (runner->renderer == nullptr) continue;
                float tileAlpha = 1.0f;
                ptrdiff_t tileLayerIdx = hmgeti(runner->tileLayerMap, parsedLayer->depth);
                if (tileLayerIdx >= 0) {
                    tileAlpha = runner->tileLayerMap[tileLayerIdx].value.alpha;
                }
                Runner_drawTileLayer(runner, (uint32_t) i, parsedLayer->tilesData, layerOffsetX, layerOffsetY, tileAlpha);
            }
            runner->frameDrawLayerMs += Runner_nowMs() - layerStartMs;
        }
    }

    fireDrawSubtype(runner, drawables, drawableCount, DRAW_END);

    // Draw foreground backgrounds (in front of instances, behind GUI)
    Runner_drawBackgrounds(runner, true);

    fireDrawSubtype(runner, drawables, drawableCount, DRAW_POST);
#ifdef __3DS__
    if (runner->renderer != NULL) {
        if (Runner_is3DSBattleActive(runner)) {
        bool isDodgingBullets = Runner_is3DSDodgingBullets(runner);
        bool isAsrielBattle = Runner_is3DSAsrielBattle(runner);
        bool isUndyneBattle = Runner_is3DSUndyneBattle(runner);
        float battleFieldScale = isDodgingBullets ? ((isUndyneBattle || isAsrielBattle) ? 1.0f : k3DSBottomBattleFieldScale) : 1.0f;
        float battleFieldYOffset = isDodgingBullets ? (isAsrielBattle ? 0.0f : k3DSBottomBattleFieldYOffset) : 0.0f;
        Runner_draw3DSBottomBattleUI(runner, drawables, drawableCount, DRAW_NORMAL, true, battleFieldScale, battleFieldYOffset);
        Runner_draw3DSBottomBattleUI(runner, drawables, drawableCount, DRAW_NORMAL, false, 1.0f, 0.0f);
        }
    }
#endif
    if (traceDraw) {
        gRunnerDrawTraceCount++;
    }
#ifdef __3DS__
    runner->n3dsTopScreenGUIListValid = false;
    repeat(3, i) runner->n3dsTopScreenGUIResponderListsValid[i] = false;
#endif
}

void Runner_drawGUI(Runner* runner) {
    rebuildDrawableCacheIfDirty(runner);
    Drawable* drawables = runner->cachedDrawables;
    int32_t drawableCount = (int32_t) arrlen(drawables);
    double phaseStartMs = Runner_nowMs();

    fireDrawSubtype(runner, drawables, drawableCount, DRAW_GUI_BEGIN);
    runner->frameDrawGuiEventMs += Runner_nowMs() - phaseStartMs;
#ifdef __3DS__
    if (runner->renderer != NULL) {
        Runner_prepare3DSDrawBattleState(runner);
        if (runner->n3dsDrawBattleActive) {
        double battleReplayStartMs = Runner_nowMs();
        bool isDodgingBullets = runner->n3dsDrawDodgingBullets;
        bool isAsrielBattle = Runner_is3DSAsrielBattle(runner);
        bool isUndyneBattle = Runner_is3DSUndyneBattle(runner);
        float battleFieldScale = isDodgingBullets ? ((isUndyneBattle || isAsrielBattle) ? 1.0f : k3DSBottomBattleFieldScale) : 1.0f;
        float battleFieldYOffset = isDodgingBullets ? (isAsrielBattle ? 0.0f : k3DSBottomBattleFieldYOffset) : 0.0f;
        Runner_draw3DSBottomBattleUI(runner, drawables, drawableCount, DRAW_GUI_BEGIN, true, battleFieldScale, battleFieldYOffset);
        Runner_draw3DSBottomBattleUI(runner, drawables, drawableCount, DRAW_GUI_BEGIN, false, 1.0f, 0.0f);
        runner->frameDrawBattleReplayMs += Runner_nowMs() - battleReplayStartMs;
        }
    }
#endif
    phaseStartMs = Runner_nowMs();
    fireDrawSubtype(runner, drawables, drawableCount, DRAW_GUI);
    runner->frameDrawGuiEventMs += Runner_nowMs() - phaseStartMs;
#ifdef __3DS__
    if (runner->renderer != NULL) {
        Runner_prepare3DSDrawBattleState(runner);
        if (runner->n3dsDrawBattleActive) {
        double battleReplayStartMs = Runner_nowMs();
        bool isDodgingBullets = runner->n3dsDrawDodgingBullets;
        bool isAsrielBattle = Runner_is3DSAsrielBattle(runner);
        bool isUndyneBattle = Runner_is3DSUndyneBattle(runner);
        float battleFieldScale = isDodgingBullets ? ((isUndyneBattle || isAsrielBattle) ? 1.0f : k3DSBottomBattleFieldScale) : 1.0f;
        float battleFieldYOffset = isDodgingBullets ? (isAsrielBattle ? 0.0f : k3DSBottomBattleFieldYOffset) : 0.0f;
        Runner_draw3DSBottomBattleUI(runner, drawables, drawableCount, DRAW_GUI, true, battleFieldScale, battleFieldYOffset);
        Runner_draw3DSBottomBattleUI(runner, drawables, drawableCount, DRAW_GUI, false, 1.0f, 0.0f);
        runner->frameDrawBattleReplayMs += Runner_nowMs() - battleReplayStartMs;
        }
    }
#endif
    phaseStartMs = Runner_nowMs();
    fireDrawSubtype(runner, drawables, drawableCount, DRAW_GUI_END);
    runner->frameDrawGuiEventMs += Runner_nowMs() - phaseStartMs;
#ifdef __3DS__
    if (runner->renderer != NULL) {
        Runner_prepare3DSDrawBattleState(runner);
        if (runner->n3dsDrawBattleActive) {
        double battleReplayStartMs = Runner_nowMs();
        bool isDodgingBullets = runner->n3dsDrawDodgingBullets;
        bool isAsrielBattle = Runner_is3DSAsrielBattle(runner);
        bool isUndyneBattle = Runner_is3DSUndyneBattle(runner);
        float battleFieldScale = isDodgingBullets ? ((isUndyneBattle || isAsrielBattle) ? 1.0f : k3DSBottomBattleFieldScale) : 1.0f;
        float battleFieldYOffset = isDodgingBullets ? (isAsrielBattle ? 0.0f : k3DSBottomBattleFieldYOffset) : 0.0f;
        Runner_draw3DSBottomBattleUI(runner, drawables, drawableCount, DRAW_GUI_END, true, battleFieldScale, battleFieldYOffset);
        Runner_draw3DSBottomBattleUI(runner, drawables, drawableCount, DRAW_GUI_END, false, 1.0f, 0.0f);
        runner->frameDrawBattleReplayMs += Runner_nowMs() - battleReplayStartMs;
        }
    }
#endif
}

void Runner_computeViewDisplayScale(Runner* runner, int32_t gameW, int32_t gameH, float* outScaleX, float* outScaleY) {
    *outScaleX = 1.0f;
    *outScaleY = 1.0f;
    
    Room* activeRoom = runner->currentRoom;
    bool viewsEnabled = (activeRoom->flags & 1) != 0;
    if (viewsEnabled) {
        int32_t minLeft = INT32_MAX, minTop = INT32_MAX;
        int32_t maxRight = INT32_MIN, maxBottom = INT32_MIN;
        repeat(MAX_VIEWS, vi) {
            RuntimeView* view = &runner->views[vi];
            if (!view->enabled) continue;
            if (minLeft > view->portX) minLeft = view->portX;
            if (minTop > view->portY) minTop = view->portY;
            int32_t right = view->portX + view->portWidth;
            int32_t bottom = view->portY + view->portHeight;
            if (right > maxRight) maxRight = right;
            if (bottom > maxBottom) maxBottom = bottom;
        }
        if (maxRight > minLeft && maxBottom > minTop) {
            *outScaleX = (float) gameW / (float) (maxRight - minLeft);
            *outScaleY = (float) gameH / (float) (maxBottom - minTop);
        }
    }
}

void Runner_drawViews(Runner* runner, int32_t gameW, int32_t gameH, float displayScaleX, float displayScaleY, bool debugShowCollisionMasks) {
    Renderer* renderer = runner->renderer;
    Room* activeRoom = runner->currentRoom;
    bool anyViewRendered = false;

    runner->drawViewBoundsValid = false;
    runner->n3dsDrawBattleStateValid = false;
    runner->n3dsBattleReplayListsValid = false;
    runner->n3dsTopScreenGUIListValid = false;
    repeat(3, i) runner->n3dsTopScreenGUIResponderListsValid[i] = false;

    bool viewsEnabled = (activeRoom->flags & 1) != 0;

    if (viewsEnabled) {
        repeat(MAX_VIEWS, vi) {
            RuntimeView* view = &runner->views[vi];
            if (!view->enabled) continue;

            int32_t viewX = view->viewX;
            int32_t viewY = view->viewY;
            int32_t viewW = view->viewWidth;
            int32_t viewH = view->viewHeight;
            int32_t portX = (int32_t) ((float) view->portX * displayScaleX + 0.5f);
            int32_t portY = (int32_t) ((float) view->portY * displayScaleY + 0.5f);
            int32_t portW = (int32_t) ((float) view->portWidth * displayScaleX + 0.5f);
            int32_t portH = (int32_t) ((float) view->portHeight * displayScaleY + 0.5f);
            float viewAngle = view->viewAngle;

#ifdef __3DS__
            Runner_prepare3DSDrawBattleState(runner);
            bool topBattleViewActive = Runner_is3DSBattleActive(runner);
            N3DSRenderer_setTopScreenBattleViewActive(renderer, topBattleViewActive);
#endif

            runner->viewCurrent = (int32_t) vi;
            runner->drawViewBoundsValid = true;
            runner->drawViewX = viewX;
            runner->drawViewY = viewY;
            runner->drawViewWidth = viewW;
            runner->drawViewHeight = viewH;
            renderer->vtable->beginView(renderer, viewX, viewY, viewW, viewH, portX, portY, portW, portH, viewAngle);

            Runner_draw(runner);

            if (debugShowCollisionMasks) DebugOverlay_drawCollisionMasks(runner);

            renderer->vtable->endView(renderer);
#ifdef __3DS__
            N3DSRenderer_setTopScreenBattleViewActive(renderer, false);
#endif

            int32_t guiW = runner->guiWidth > 0 ? runner->guiWidth : portW;
            int32_t guiH = runner->guiHeight > 0 ? runner->guiHeight : portH;
            renderer->vtable->beginGUI(renderer, guiW, guiH, portX, portY, portW, portH);
            double guiStartMs = Runner_nowMs();
            Runner_drawGUI(runner);
            runner->frameDrawGuiMs += Runner_nowMs() - guiStartMs;
            renderer->vtable->endGUI(renderer);

            anyViewRendered = true;
        }
    }

    if (!anyViewRendered) {
        // No views enabled: render with default full-screen view
        runner->viewCurrent = 0;
        runner->drawViewBoundsValid = true;
        runner->drawViewX = 0;
        runner->drawViewY = 0;
        runner->drawViewWidth = gameW;
        runner->drawViewHeight = gameH;
        int32_t portX = 0;
        int32_t portY = 0;
        int32_t portW = gameW;
        int32_t portH = gameH;
#ifdef __3DS__
        Runner_prepare3DSDrawBattleState(runner);
        bool topBattleViewActive = Runner_is3DSBattleActive(runner);
        N3DSRenderer_setTopScreenBattleViewActive(renderer, topBattleViewActive);
#endif
        renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, portX, portY, portW, portH, 0.0f);
        Runner_draw(runner);

        if (debugShowCollisionMasks) DebugOverlay_drawCollisionMasks(runner);

        renderer->vtable->endView(renderer);
#ifdef __3DS__
        N3DSRenderer_setTopScreenBattleViewActive(renderer, false);
#endif

        int32_t guiW = runner->guiWidth > 0 ? runner->guiWidth : portW;
        int32_t guiH = runner->guiHeight > 0 ? runner->guiHeight : portH;
        renderer->vtable->beginGUI(renderer, guiW, guiH, portX, portY, portW, portH);
        double guiStartMs = Runner_nowMs();
        Runner_drawGUI(runner);
        runner->frameDrawGuiMs += Runner_nowMs() - guiStartMs;
        renderer->vtable->endGUI(renderer);
    }

    // Reset view_current to 0 so non-Draw events (Step, Alarm, Create) see view_current = 0
    runner->drawViewBoundsValid = false;
    runner->n3dsDrawBattleStateValid = false;
    runner->n3dsBattleReplayListsValid = false;
    runner->n3dsTopScreenGUIListValid = false;
    repeat(3, i) runner->n3dsTopScreenGUIResponderListsValid[i] = false;
    runner->viewCurrent = 0;
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

    hmput(runner->instancesById, instanceId, inst);
    arrput(runner->instances, inst);
    Runner_addInstanceToObjectLists(runner, inst);
    runner->drawableListStructureDirty = true;

#ifdef ENABLE_VM_TRACING
    if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, objDef->name) != -1) {
        fprintf(stderr, "VM: Instance %s (instanceId=%d,objectIndex=%d) created at (%f, %f)\n", objDef->name, instanceId, inst->objectIndex, x, y);
    }
#endif

    return inst;
}

// ===[ Room Management ]===

// Collect persistent instances from the previous room (they travel with the player), and free the rest.
// You should re-append them at the tail AFTER creating the new room's own instances, so the iteration order matches the native runner: room-local instances first, persistent arrivals last.
static Instance** takePersistentInstances(Runner* runner) {
    Instance** carriedPersistent = nullptr;
    int32_t oldCount = (int32_t) arrlen(runner->instances);
    repeat(oldCount, i) {
        Instance* inst = runner->instances[i];
        if (inst->persistent) {
#ifdef ENABLE_VM_TRACING
            GameObject* gameObject = &runner->dataWin->objt.objects[inst->objectIndex];
            if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, gameObject->name) != -1) {
                fprintf(stderr, "VM: Instance %s (instanceId=%d,objectIndex=%d) has been persisted at (%f, %f) due to room change\n", gameObject->name, inst->instanceId, inst->objectIndex, inst->x, inst->y);
            }
#endif

            arrput(carriedPersistent, inst);
        } else {
#ifdef ENABLE_VM_TRACING
            GameObject* gameObject = &runner->dataWin->objt.objects[inst->objectIndex];
            if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, gameObject->name) != -1) {
                fprintf(stderr, "VM: Instance %s (instanceId=%d,objectIndex=%d) destroyed at (%f, %f) due to room change\n", gameObject->name, inst->instanceId, inst->objectIndex, inst->x, inst->y);
            }
#endif

            hmdel(runner->instancesById, inst->instanceId);
            Instance_free(inst);
        }
    }

    arrfree(runner->instances);
    runner->instances = nullptr;

    // The per-object lists referenced both the freed non-persistents and the carried persistents; clear them entirely.
    // Persistents are re-added when they return via returnPersistentInstances, and room-local instances are added as they get created.
    Runner_clearAllObjectLists(runner);

    return carriedPersistent;
}

// Append the carried-over persistent instances at the tail of runner->instances and free the temporary array. Pairs with takePersistentInstances.
static void returnPersistentInstances(Runner* runner, Instance** carriedPersistent) {
    repeat(arrlen(carriedPersistent), i) {
        arrput(runner->instances, carriedPersistent[i]);
        Runner_addInstanceToObjectLists(runner, carriedPersistent[i]);
    }
    arrfree(carriedPersistent);
}

static void copyRoomViewToRuntimeView(RoomView* roomView, RuntimeView* runtimeView) {
    runtimeView->enabled = roomView->enabled;
    runtimeView->viewX = roomView->viewX;
    runtimeView->viewY = roomView->viewY;
    runtimeView->viewWidth = roomView->viewWidth;
    runtimeView->viewHeight = roomView->viewHeight;
    runtimeView->portX = roomView->portX;
    runtimeView->portY = roomView->portY;
    runtimeView->portWidth = roomView->portWidth;
    runtimeView->portHeight = roomView->portHeight;
    runtimeView->borderX = roomView->borderX;
    runtimeView->borderY = roomView->borderY;
    runtimeView->speedX = roomView->speedX;
    runtimeView->speedY = roomView->speedY;
    runtimeView->objectId = roomView->objectId;
    runtimeView->viewAngle = 0;
}

static void initRoom(Runner* runner, int32_t roomIndex) {
    DataWin* dataWin = runner->dataWin;
    require(roomIndex >= 0 && dataWin->room.count > (uint32_t) roomIndex);
    double roomStartMs = Runner_nowMs();
    double phaseStartMs = roomStartMs;

    Room* room = &dataWin->room.rooms[roomIndex];
    fprintf(stderr, "Runner: Initializing room: %s (index %d)\n", room->name, roomIndex);

    // Lazy-room load: if the payload wasn't loaded, read it from the data.win file now before anything touches the room's game objects/tiles/layers.
    if (!room->payloadLoaded) {
        DataWin_loadRoomPayload(dataWin, roomIndex);
    }
    Runner_logTiming("room payload load", phaseStartMs, roomStartMs);

    SavedRoomState* savedState = &runner->savedRoomStates[roomIndex];

    runner->currentRoom = room;
    runner->currentRoomIndex = roomIndex;
    // Tile set, runtime layers, and instance list all change when entering a room.
    runner->drawableListStructureDirty = true;
    Runner_buildTileLayerCaches(runner);
    // It could be the first time we are initializing the grid
    if (runner->spatialGrid != nullptr)
        SpatialGrid_free(runner->spatialGrid);
    runner->spatialGrid = SpatialGrid_create(room->width, room->height);

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
        memcpy(runner->views, savedState->views, sizeof(runner->views));

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

        Instance** carriedPersistent = takePersistentInstances(runner);

        // The native runner restores the room's own linked list first, then appends persistent arrivals at the tail.
        // Event iteration is forward (oldest first), so a persistent instance runs after the room's own instances.
        int32_t savedCount = (int32_t) arrlen(savedState->instances);
        repeat(savedCount, i) {
            arrput(runner->instances, savedState->instances[i]);
            Runner_addInstanceToObjectLists(runner, savedState->instances[i]);
        }
        arrfree(savedState->instances);
        savedState->instances = nullptr;

        returnPersistentInstances(runner, carriedPersistent);

        // No Create events, no preCreateCode, no creationCode, no room creation code
        fprintf(stderr, "Runner: Room restored (persistent): %s (room %d) with %d instances\n", room->name, roomIndex, (int) arrlen(runner->instances));
        phaseStartMs = Runner_nowMs();
        if (runner->renderer != nullptr && runner->renderer->vtable->prewarmRoom != nullptr) {
            runner->renderer->vtable->prewarmRoom(runner->renderer, runner);
        }
        Runner_logTiming("renderer prewarm (restore)", phaseStartMs, roomStartMs);
        phaseStartMs = Runner_nowMs();
        if (runner->audioSystem != nullptr && runner->audioSystem->vtable->prewarmRoom != nullptr) {
            runner->audioSystem->vtable->prewarmRoom(runner->audioSystem, runner);
        }
        Runner_logTiming("audio prewarm (restore)", phaseStartMs, roomStartMs);
        Runner_logTiming("initRoom persistent total", roomStartMs, roomStartMs);
        return;
    }

    // === Normal room initialization (first visit, or non-persistent room) ===

    // Initialize the views from scratch
    repeat(MAX_VIEWS, vi) {
        copyRoomViewToRuntimeView(&room->views[vi], &runner->views[vi]);
    }

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

    Instance** carriedPersistent = takePersistentInstances(runner);

    // Two-pass instance creation (matches HTML5 runner behavior):
    // Pass 1: Create all instance objects so they exist for cross-references
    // Pass 2: Fire preCreateCode, CREATE events, and creationCode
    // This ensures that when an instance's Create event reads another instance
    // (e.g. obj_mainchara reading obj_markerA.x), the target already exists.

    // Pass 1: Create all instances without firing events
    phaseStartMs = Runner_nowMs();
    repeat(room->gameObjectCount, i) {
        RoomGameObject* roomObj = &room->gameObjects[i];

        // Skip if a persistent instance carried over from the previous room already owns this ID (re-entering the persistent instance's home room, don't create a duplicate!).
        if (hmget(runner->instancesById, roomObj->instanceID) != nullptr) continue;
        if (isObjectDisabled(runner, roomObj->objectDefinition)) continue;

        Instance* inst = createAndInitInstance(runner, roomObj->instanceID, roomObj->objectDefinition, (GMLReal) roomObj->x, (GMLReal) roomObj->y);
        inst->imageXscale = (float) roomObj->scaleX;
        inst->imageYscale = (float) roomObj->scaleY;
        inst->imageAngle = (float) roomObj->rotation;
        inst->imageSpeed = roomObj->imageSpeed;
        inst->imageIndex = (float) roomObj->imageIndex;
    }
    Runner_logTiming("instance create pass1", phaseStartMs, roomStartMs);

    // In GMS2, instances get their depth from their room layer, not the object definition.
    // This must happen before firing Create events so scripts like scr_depth() read the layer depth.
    phaseStartMs = Runner_nowMs();
    if (DataWin_isVersionAtLeast(runner->dataWin, 2, 0, 0, 0)) {
        repeat(room->layerCount, li) {
            RoomLayer* layer = &room->layers[li];
            if (layer->type != RoomLayerType_Instances || layer->instancesData == nullptr) continue;
            RoomLayerInstancesData* layerData = layer->instancesData;
            repeat(layerData->instanceCount, ii) {
                Instance* inst = hmget(runner->instancesById, layerData->instanceIds[ii]);
                if (inst != nullptr) {
                    inst->depth = layer->depth;
                    inst->layer = (int32_t) layer->id;
                }
            }
        }
    }
    Runner_logTiming("layer/depth setup", phaseStartMs, roomStartMs);

    // Append persistent instances carried over from the previous room at the tail, so forward event iteration processes the new room's own instances first and the travelers last.
    // We NEED to do this here BEFORE firing the room object's events, to avoid code that relies on persistent instances failing (example: if a object uses instance_number to get the number of instances in the room).
    returnPersistentInstances(runner, carriedPersistent);

    // Pass 2: Fire events for newly created instances (in room definition order)
    phaseStartMs = Runner_nowMs();
    repeat(room->gameObjectCount, i) {
        RoomGameObject* roomObj = &room->gameObjects[i];

        Instance* inst = hmget(runner->instancesById, roomObj->instanceID);
        if (inst == nullptr) continue;

        // Skip instances that already had their Create event fired (persistent carry-overs
        // that hmget also matches, since instancesById still holds them).
        if (inst->createEventFired) continue;
        inst->createEventFired = true;

        Runner_executeEvent(runner, inst, EVENT_PRECREATE, 0);
        executeCode(runner, inst, roomObj->preCreateCode);
        Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
        executeCode(runner, inst, roomObj->creationCode);
    }
    Runner_logTiming("instance create pass2", phaseStartMs, roomStartMs);

    // Run room creation code
    phaseStartMs = Runner_nowMs();
    if (room->creationCodeId >= 0 && dataWin->code.count > (uint32_t) room->creationCodeId) {
        // Room creation code runs in global context, the native runner creates a fake/dummy instance for the "self"
        Instance* dummy = Instance_create(0, -1, 0, 0);
        runner->vmContext->currentInstance = dummy;
        RValue result = VM_executeCode(runner->vmContext, room->creationCodeId);
        RValue_free(&result);
        runner->vmContext->currentInstance = nullptr;
        Instance_free(dummy);
    }
    Runner_logTiming("room creation code", phaseStartMs, roomStartMs);

    // Mark this room as initialized for persistent room support
    savedState->initialized = true;

    fprintf(stderr, "Runner: Room loaded: %s (room %d) with %d instances\n", room->name, roomIndex, (int) arrlen(runner->instances));
    phaseStartMs = Runner_nowMs();
    if (runner->renderer != nullptr && runner->renderer->vtable->prewarmRoom != nullptr) {
        runner->renderer->vtable->prewarmRoom(runner->renderer, runner);
    }
    Runner_logTiming("renderer prewarm", phaseStartMs, roomStartMs);
    phaseStartMs = Runner_nowMs();
    if (runner->audioSystem != nullptr && runner->audioSystem->vtable->prewarmRoom != nullptr) {
        runner->audioSystem->vtable->prewarmRoom(runner->audioSystem, runner);
    }
    Runner_logTiming("audio prewarm", phaseStartMs, roomStartMs);
    Runner_logTiming("initRoom total", roomStartMs, roomStartMs);
}

// Cleans up the runner state, used when freeing the Runner or when restarting the Runner
static void cleanupState(Runner* runner) {
    // Drop VM-side RValue holders (globals, stack, call frames) BEFORE freeing any Instance memory. This way any RVALUE_STRUCT refs decrement against still-live struct memory; otherwise we'd free a struct here and then have VM_free's later VM_reset try to decRef a dangling pointer.
    if (runner->vmContext != nullptr) {
        VM_reset(runner->vmContext);
    }

    // Free all instances
    repeat(arrlen(runner->instances), i) {
        hmdel(runner->instancesById, runner->instances[i]->instanceId);
        Instance_free(runner->instances[i]);
    }
    arrfree(runner->instances);
    runner->instances = nullptr;

    // Empty the per-object lists. We keep the outer instancesByObject array allocated so Runner_reset can be reused; Runner_free releases it.
    Runner_clearAllObjectLists(runner);

    // Free saved room states
    if (runner->savedRoomStates != nullptr) {
        repeat(runner->dataWin->room.count, i) {
            SavedRoomState* state = &runner->savedRoomStates[i];
            int32_t savedCount = (int32_t) arrlen(state->instances);
            repeat(savedCount, j) {
                hmdel(runner->instancesById, state->instances[j]->instanceId);
                Instance_free(state->instances[j]);
            }
            arrfree(state->instances);
            hmfree(state->tileLayerMap);
            freeRuntimeLayersArray(&state->runtimeLayers);
        }
        free(runner->savedRoomStates);
    }
    runner->savedRoomStates = nullptr;

    // Free struct instances (created via @@NewGMLObject@@). Anything still here at shutdown is leaked refs or a reference cycle - bulk free regardless of refCount.
    repeat(arrlen(runner->structInstances), i) {
        Instance* s = runner->structInstances[i];
        hmdel(runner->instancesById, s->instanceId);
        s->structRegistryIndex = -1;
        Instance_free(s);
    }
    arrfree(runner->structInstances);
    runner->structInstances = nullptr;

    hmfree(runner->instancesById);
    runner->instancesById = nullptr;
    hmfree(runner->tileLayerMap);
    runner->tileLayerMap = nullptr;
    Runner_freeTileLayerCaches(runner);
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

    // Free mp_grid pool
    repeat((int32_t) arrlen(runner->mpGridPool), i) {
        free(runner->mpGridPool[i].cells);
    }
    arrfree(runner->mpGridPool);
    runner->mpGridPool = nullptr;

    arrfree(runner->musicInstanceStack);
    runner->musicInstanceStack = nullptr;

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

    if (runner->spatialGrid != nullptr) {
        SpatialGrid_free(runner->spatialGrid);
        runner->spatialGrid = nullptr;
    }
}

// ===[ Public API ]===

void Runner_reset(Runner* runner) {
    // This actually sets the default runner values, used for initialization and restarting
    cleanupState(runner);

    // Reset VM state
    VM_reset(runner->vmContext);

    runner->pendingRoom = -1;
    runner->asyncLoadMapId = -1;
    runner->gameStartFired = false;
    runner->currentRoomIndex = -1;
    runner->currentRoomOrderPosition = -1;
    runner->nextInstanceId = runner->dataWin->gen8.lastObj + 1;
    runner->savedRoomStates = safeCalloc(runner->dataWin->room.count, sizeof(SavedRoomState));
    runner->nextLayerId = 1;
    runner->audioSystem->vtable->stopAll(runner->audioSystem);

    // Allocate the per-object instance list array once.
    // We don't need to reinitialize the list because the objt.count is fixed for this data.win.
    if (runner->instancesByObject == nullptr) {
        runner->instancesByObject = safeCalloc(runner->dataWin->objt.count, sizeof(Instance**));
    }
    if (runner->instancesByExactObject == nullptr) {
        runner->instancesByExactObject = safeCalloc(runner->dataWin->objt.count, sizeof(Instance**));
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
    arrsetlen(runner->musicInstanceStack, 0);

    arrsetlen(runner->cachedDrawables, 0);
    runner->drawableListStructureDirty = true;
    runner->drawableListSortDirty = false;
}

// Populates objectsWithAnyEventOfType[eventType] from the resolved event table: for each event type, the deduplicated list of concrete object indices that respond to ANY subtype of that event. Walks the inverted bySlot index per slot and dedups via a scratch byte set.
// Used by collision dispatch to skip non-collision objects in the outer loop, mirroring how the native obj_has_event table partitions instance iteration by event class.
static void populateObjectsWithAnyEventOfType(Runner* runner) {
    int32_t objectCount = (int32_t) runner->dataWin->objt.count;
    runner->objectsWithAnyEventOfType = safeCalloc(OBJT_EVENT_TYPE_COUNT, sizeof(int32_t*));
    if (objectCount == 0) return;

    uint8_t* seen = safeCalloc((size_t) objectCount, 1);

    repeat(OBJT_EVENT_TYPE_COUNT, t) {
        int16_t* dense = runner->eventSlotMap.denseLookup[t];
        if (dense == nullptr) continue;
        int32_t maxSub = runner->eventSlotMap.maxSubtypeByType[t];
        memset(seen, 0, (size_t) objectCount);

        for (int32_t sub = 0; maxSub >= sub; sub++) {
            int32_t slot = dense[sub];
            if (0 > slot) continue;
            uint32_t entryCount;
            SlotResponderEntry* entries = ResolvedEventTable_slotEntries(&runner->eventTable, slot, &entryCount);
            repeat(entryCount, i) {
                int32_t obj = entries[i].concreteObjectId;
                if (obj < 0 || obj >= objectCount) continue;
                if (seen[obj]) continue;
                seen[obj] = 1;
                arrput(runner->objectsWithAnyEventOfType[t], obj);
            }
        }
    }

    free(seen);
}

Runner* Runner_create(DataWin* dataWin, VMContext* vm, Renderer* renderer, FileSystem* fileSystem, AudioSystem* audioSystem) {
    requireNotNull(dataWin);
    requireNotNull(vm);
    requireNotNull(renderer);
    requireNotNull(fileSystem);
    requireNotNull(audioSystem);

    Runner* runner = safeCalloc(1, sizeof(Runner));
    runner->dataWin = dataWin;
    runner->vmContext = vm;
    runner->renderer = renderer;
    runner->fileSystem = fileSystem;
    runner->audioSystem = audioSystem;
    runner->frameCount = 0;
    runner->osType = OS_WINDOWS;
    runner->keyboard = RunnerKeyboard_create();
    runner->gamepads = RunnerGamepad_create();
    runner->frameStepTopObjectIndex = -1;
    if (dataWin->objt.count > 0) {
        runner->frameStepObjectMsByObject = safeCalloc(dataWin->objt.count, sizeof(double));
        runner->frameStepObjectCallsByObject = safeCalloc(dataWin->objt.count, sizeof(uint32_t));
    }

    // Collision compatibility mode is "enabled" for all pre-GM 2022.1 games AND for any post-GM 2022.1 games that have the bit 27 set
    runner->collisionCompatibilityMode = (dataWin->detectedFormat.major == 1) || (((dataWin->optn.info >> 27) & 1) != 0);

    // Build the event dispatch acceleration tables.
    EventSlotMap_build(&runner->eventSlotMap, dataWin);
    ResolvedEventTable_build(&runner->eventTable, dataWin, &runner->eventSlotMap);

    // Create assets map
    shdefault(runner->assetsByName, -1);
    repeat(dataWin->objt.count, i) {
        shput(runner->assetsByName, dataWin->objt.objects[i].name, i);
    }
    repeat(dataWin->sprt.count, i) {
        shput(runner->assetsByName, dataWin->sprt.sprites[i].name, i);
    }
    repeat(dataWin->sond.count, i) {
        shput(runner->assetsByName, dataWin->sond.sounds[i].name, i);
    }
    repeat(dataWin->bgnd.count, i) {
        shput(runner->assetsByName, dataWin->bgnd.backgrounds[i].name, i);
    }
    repeat(dataWin->path.count, i) {
        shput(runner->assetsByName, dataWin->path.paths[i].name, i);
    }
    repeat(dataWin->scpt.count, i) {
        shput(runner->assetsByName, dataWin->scpt.scripts[i].name, i);
    }
    repeat(dataWin->font.count, i) {
        shput(runner->assetsByName, dataWin->font.fonts[i].name, i);
    }
    repeat(dataWin->tmln.count, i) {
        shput(runner->assetsByName, dataWin->tmln.timelines[i].name, i);
    }
    repeat(dataWin->room.count, i) {
        shput(runner->assetsByName, dataWin->room.rooms[i].name, i);
    }

    Runner_reset(runner);

    populateObjectsWithAnyEventOfType(runner);

    // Link runner to VM context
    vm->runner = (struct Runner*) runner;

    renderer->vtable->init(renderer, dataWin);
    audioSystem->vtable->init(audioSystem, dataWin, fileSystem);

    return runner;
}

static inline void dispatchInstanceCreationEvents(Runner* runner, Instance* inst) {
    inst->createEventFired = true;
    Runner_executeEvent(runner, inst, EVENT_PRECREATE, 0);
    Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
}

Instance* Runner_createInstance(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex) {
    if (isObjectDisabled(runner, objectIndex)) return nullptr;
    Instance* inst = createAndInitInstance(runner, runner->nextInstanceId++, objectIndex, x, y);
    dispatchInstanceCreationEvents(runner, inst);
    return inst;
}

// Same as Runner_createInstance, but sets depth BEFORE firing Create events so scripts like scr_depth can override.
Instance* Runner_createInstanceWithDepth(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex, int32_t depth) {
    if (isObjectDisabled(runner, objectIndex)) return nullptr;
    Instance* inst = createAndInitInstance(runner, runner->nextInstanceId++, objectIndex, x, y);
    inst->depth = depth;
    dispatchInstanceCreationEvents(runner, inst);
    return inst;
}

Instance* Runner_createInstanceWithLayer(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex, int32_t layerId) {
    if (isObjectDisabled(runner, objectIndex)) return nullptr;
    RuntimeLayer* rl = Runner_findRuntimeLayerById(runner, layerId);
    if (rl == nullptr) {
        fprintf(stderr, "Runner: instance_create_layer: Layer ID %d not found!\n", layerId);
        return nullptr;
    }
    Instance* inst = createAndInitInstance(runner, runner->nextInstanceId++, objectIndex, x, y);
    inst->layer = layerId;
    inst->depth = rl->depth;
    dispatchInstanceCreationEvents(runner, inst);
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
    Runner_executeEvent(runner, inst, EVENT_DESTROY, 0);
    // A destroyed instance must ALWAYS be not active
    // If a destroyed instance is active, then well, something went VERY wrong
    inst->active = false;
    inst->destroyed = true;

#ifdef ENABLE_VM_TRACING
    GameObject* gameObject = &runner->dataWin->objt.objects[inst->objectIndex];
    if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, gameObject->name) != -1) {
        fprintf(stderr, "VM: Instance %s (instanceId=%d,objectIndex=%d) destroyed\n", gameObject->name, inst->instanceId, inst->objectIndex);
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

// Reaps GML structs whose only remaining ref is the structInstances registry's implicit +1.
// Walks backward so that swap-remove of dead entries doesn't disturb the indexes of entries we haven't visited yet.
static void Runner_sweepDeadStructs(Runner* runner) {
    int32_t count = (int32_t) arrlen(runner->structInstances);
    for (int32_t i = count - 1; i >= 0; i--) {
        Instance* s = runner->structInstances[i];
        if (s->refCount > 1) continue; // still referenced by user code
        require(s->refCount == 1);

        // Remove from runner->instancesById so future findInstanceByTarget(id) returns nullptr.
        hmdel(runner->instancesById, s->instanceId);

        // O(1) swap-remove from structInstances, keeping structRegistryIndex in sync.
        int32_t lastIdx = (int32_t) arrlen(runner->structInstances) - 1;
        if (i != lastIdx) {
            Instance* moved = runner->structInstances[lastIdx];
            runner->structInstances[i] = moved;
            moved->structRegistryIndex = i;
        }
        arrpop(runner->structInstances);

        s->structRegistryIndex = -1;
        s->refCount = 0; // drop the registry's ref; we are about to free
        Instance_free(s);
    }
}

void Runner_cleanupDestroyedInstances(Runner* runner) {
    int32_t count = (int32_t) arrlen(runner->instances);
    int32_t writeIdx = 0;
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->destroyed) {
            runner->instances[writeIdx++] = inst;
        } else {
            Runner_removeInstanceFromObjectLists(runner, inst);
            hmdel(runner->instancesById, inst->instanceId);
            Instance_free(inst);
            // Cached drawables hold raw Instance* that we just freed; force a rebuild before the next draw.
            runner->drawableListStructureDirty = true;
        }
    }
    arrsetlen(runner->instances, writeIdx);
}

void Runner_initFirstRoom(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    require(dataWin->gen8.roomOrderCount > 0);
    double initStartMs = Runner_nowMs();
    double phaseStartMs = initStartMs;

    int32_t firstRoomIndex = dataWin->gen8.roomOrder[0];
    fprintf(stderr, "Runner: First room index: %d, room count: %u\n", firstRoomIndex, dataWin->room.count);

    // Run global init scripts with the global scope instance as "self"
    // In GMS 2.3+ (BC17), GLOB scripts store function declarations on "self" via Pop.v.v
    runner->vmContext->currentInstance = runner->globalScopeInstance;
    repeat(dataWin->glob.count, i) {
        int32_t codeId = dataWin->glob.codeIds[i];
        if (codeId >= 0 && dataWin->code.count > (uint32_t) codeId) {
            fprintf(stderr, "Runner: Executing global init script: %s\n", dataWin->code.entries[codeId].name);
            RValue result = VM_executeCode(runner->vmContext, codeId);
            RValue_free(&result);
        }
    }
    runner->vmContext->currentInstance = nullptr;
    Runner_logTiming("global init scripts", phaseStartMs, initStartMs);

    // Initialize the first room
    phaseStartMs = Runner_nowMs();
    initRoom(runner, firstRoomIndex);
    Runner_logTiming("initRoom(first)", phaseStartMs, initStartMs);

    // Fire Game Start for all instances
    phaseStartMs = Runner_nowMs();
    Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_GAME_START);
    runner->gameStartFired = true;
    Runner_logTiming("game start events", phaseStartMs, initStartMs);

    // Fire Room Start for all instances
    phaseStartMs = Runner_nowMs();
    Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_START);
    Runner_logTiming("room start events", phaseStartMs, initStartMs);

    // Handle room_goto (or game_restart) called during Create/GameStart/RoomStart events.
    // This is common in GMS2 games that use an initializer object to jump to the real first room.
    // We process room transitions here so the main loop always starts in the correct room,
    // without running a wasted step on the initializer room first.
    if (runner->pendingRoom == ROOM_RESTARTGAME) {
        Runner_logTiming("init pending restart", initStartMs, initStartMs);
        Runner_reset(runner);
        Runner_initFirstRoom(runner);
        return;
    }
    if (runner->pendingRoom >= 0) {
        int32_t newRoomIndex = runner->pendingRoom;
        runner->pendingRoom = -1;
        require(dataWin->room.count > (uint32_t) newRoomIndex);
        fprintf(stderr, "Runner: room_goto called during init, transitioning: %s (room %d) -> %s (room %d)\n",
            runner->currentRoom->name, runner->currentRoomIndex,
            dataWin->room.rooms[newRoomIndex].name, newRoomIndex);

        // Fire Room End on the init room's instances before leaving
        Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_END);

        if (runner->dataWin->lazyLoadRooms && runner->currentRoom != nullptr &&
            !runner->currentRoom->eagerlyLoaded && newRoomIndex != runner->currentRoomIndex) {
            DataWin_freeRoomPayload(runner->currentRoom);
        }

        phaseStartMs = Runner_nowMs();
        initRoom(runner, newRoomIndex);
        Runner_logTiming("initRoom(pending)", phaseStartMs, initStartMs);
        phaseStartMs = Runner_nowMs();
        Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_START);
        Runner_logTiming("room start pending", phaseStartMs, initStartMs);
    }
    Runner_logTiming("Runner_initFirstRoom total", initStartMs, initStartMs);
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
    int32_t codeId = findEventCodeIdAndOwner(runner, self->objectIndex, EVENT_COLLISION, targetObjectIndex, &ownerObjectIndex);

    vm->currentEventObjectIndex = ownerObjectIndex;

#ifdef ENABLE_VM_TRACING
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
    PathPositionResult pos0 = GamePath_getPosition(path, 0.0f);
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
                PathPositionResult pos1 = GamePath_getPosition(path, 1.0f);
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

    SpatialGrid_markInstanceAsDirty(runner->spatialGrid, inst);

    return atPathEnd;
}

static void dispatchCollisionEvents(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    // Iterate only the objects that have any collision event in their parent chain.
    int32_t* selfObjects = (runner->objectsWithAnyEventOfType != nullptr) ? runner->objectsWithAnyEventOfType[EVENT_COLLISION] : nullptr;
    if (selfObjects == nullptr) return;
    int32_t selfObjCount = (int32_t) arrlen(selfObjects);

    repeat(selfObjCount, soIdx) {
        int32_t selfObjIdx = selfObjects[soIdx];
        Instance** selfBucket = runner->instancesByExactObject[selfObjIdx];
        int32_t selfBucketCount = (int32_t) arrlen(selfBucket);
        if (selfBucketCount == 0) continue;

        // Snapshot the self bucket: collision handlers can spawn/destroy/instance_change. Iterating a snapshot also keeps newly-created instances from firing collisions in this same phase.
        int32_t selfSnapBase = (int32_t) arrlen(runner->instanceSnapshots);
        arrsetlen(runner->instanceSnapshots, selfSnapBase + selfBucketCount);
        memcpy(&runner->instanceSnapshots[selfSnapBase], selfBucket, (size_t) selfBucketCount * sizeof(Instance*));

        repeat(selfBucketCount, si) {
            Instance* self = runner->instanceSnapshots[selfSnapBase + si];
            if (!self->active) continue;

        InstanceBBox bboxSelf;
        Sprite* sprSelf;
        bool selfDirty = true;

        // Walk the parent chain to find all collision event handlers for this object
        int32_t currentObj = self->objectIndex;
        int depth = 0;
        while (currentObj >= 0 && dataWin->objt.count > (uint32_t) currentObj && 32 > depth) {
            GameObject* obj = &dataWin->objt.objects[currentObj];

            ObjectEventList* eventList = &obj->eventLists[EVENT_COLLISION];
            repeat(eventList->eventCount, evtIdx) {
                ObjectEvent* evt = &eventList->events[evtIdx];
                int32_t targetObjIndex = (int32_t) evt->eventSubtype;

                if (evt->actionCount == 0 || 0 > evt->actions[0].codeId) continue;

                // Iterate only the descendant-inclusive list for the target object via a snapshot, so nested user code (collision handlers calling instance_exists, with (...), etc.) can push/pop their own snapshots above ours without corrupting this iteration.
                int32_t snapBase = Runner_pushInstancesOfObject(runner, targetObjIndex);
                int32_t snapEnd  = (int32_t) arrlen(runner->instanceSnapshots);
                for (int32_t snapIdx = snapBase; snapEnd > snapIdx; snapIdx++) {
                    Instance* other = runner->instanceSnapshots[snapIdx];
                    if (!other->active) continue;
                    if (other == self) continue;

                    // Compute bboxes
                    if (selfDirty) {
                        bboxSelf = Collision_computeBBox(dataWin, self);
                        sprSelf = Collision_getSprite(dataWin, self);
                        selfDirty = false;
                    }
                    InstanceBBox bboxOther = Collision_computeBBox(dataWin, other);
                    if (!bboxSelf.valid || !bboxOther.valid) continue;

                    // AABB overlap test
                    if (bboxSelf.left >= bboxOther.right || bboxOther.left >= bboxSelf.right || bboxSelf.top >= bboxOther.bottom || bboxOther.top >= bboxSelf.bottom)
                        continue;

                    // Precise collision check if either sprite needs it (per-pixel for sepMasks==1, OBB SAT for rotated sepMasks==2).
                    Sprite* sprOther = Collision_getSprite(dataWin, other);
                    bool needsPrecise = (sprSelf != nullptr && sprSelf->sepMasks == 1) || (sprOther != nullptr && sprOther->sepMasks == 1) || Collision_obbNeedsSAT(sprSelf, self) || Collision_obbNeedsSAT(sprOther, other);

                    if (needsPrecise) {
                        if (!Collision_instancesOverlapPrecise(dataWin, runner->collisionCompatibilityMode, self, other, bboxSelf, bboxOther)) continue;
                    }

                    // Collision detected! If either instance is solid, restore both to xprevious/yprevious.
                    bool hadSolid = self->solid || other->solid;
                    if (hadSolid) {
                        self->x = self->xprevious;
                        self->y = self->yprevious;
                        if (self->pathIndex >= 0) self->pathPosition = self->pathPositionPrevious;
                        other->x = other->xprevious;
                        other->y = other->yprevious;
                        if (other->pathIndex >= 0) other->pathPosition = other->pathPositionPrevious;
                        SpatialGrid_markInstanceAsDirty(runner->spatialGrid, self);
                        SpatialGrid_markInstanceAsDirty(runner->spatialGrid, other);
                    }

                    // We don't need to call "SpatialGrid_markInstanceAsDirty" here because *technically* just because a collision happened, doesn't mean that the instances have moved
                    // And if it DOES move via GML, the variable write handlers will set it to dirty

                    executeCollisionEvent(runner, self, other, targetObjIndex);

                    // Native parity for solids: collision event can alter path state, so run one
                    // post-event path adaptation and apply its hspeed/vspeed step.
                    if (hadSolid && self->active && other->active) {
                        adaptPath(runner, self);
                        adaptPath(runner, other);
                        if (self->hspeed != 0.0f || self->vspeed != 0.0f) {
                            self->x += self->hspeed;
                            self->y += self->vspeed;
                            SpatialGrid_markInstanceAsDirty(runner->spatialGrid, self);
                        }
                        if (other->hspeed != 0.0f || other->vspeed != 0.0f) {
                            other->x += other->hspeed;
                            other->y += other->vspeed;
                            SpatialGrid_markInstanceAsDirty(runner->spatialGrid, other);
                        }
                    }

                    // The collision event may have moved our instance, so we'll need to regenerate our self attributes!
                    selfDirty = true;
                }
                Runner_popInstanceSnapshot(runner, snapBase);
            }

            currentObj = obj->parentId;
            depth++;
        }
        }

        arrsetlen(runner->instanceSnapshots, selfSnapBase);
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

    repeat(MAX_VIEWS, vi) {
        RuntimeView* view = &runner->views[vi];
        if (!view->enabled || 0 > view->objectId) continue;

        // Find first active instance of the target object.
        Instance* target = nullptr;
        if (view->objectId >= 0 && runner->dataWin->objt.count > (uint32_t) view->objectId) {
            Instance** bucket = runner->instancesByObject[view->objectId];
            int32_t bucketCount = (int32_t) arrlen(bucket);
            repeat(bucketCount, i) {
                if (bucket[i]->active) { target = bucket[i]; break; }
            }
        }

        if (target != nullptr) {
            int32_t ix = (int32_t) GMLReal_floor(target->x);
            int32_t iy = (int32_t) GMLReal_floor(target->y);
            view->viewX = followAxis(view->viewX, view->viewWidth, ix, view->borderX, view->speedX, (int32_t) room->width);
            view->viewY = followAxis(view->viewY, view->viewHeight, iy, view->borderY, view->speedY, (int32_t) room->height);
        }
    }
}

static void dispatchOutsideRoomEvents(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    int32_t outsideSlot = EventSlotMap_lookup(&runner->eventSlotMap, EVENT_OTHER, OTHER_OUTSIDE_ROOM);
    if (0 > outsideSlot) return;
    ResolvedEventTable* table = &runner->eventTable;
    uint32_t entryCount;
    SlotResponderEntry* entries = ResolvedEventTable_slotEntries(table, outsideSlot, &entryCount);
    if (entryCount == 0) return;

    int32_t roomWidth = (int32_t) runner->currentRoom->width;
    int32_t roomHeight = (int32_t) runner->currentRoom->height;

    repeat(entryCount, s) {
        int32_t objIdx = entries[s].concreteObjectId;
        Instance** bucket = runner->instancesByExactObject[objIdx];
        int32_t bucketCount = (int32_t) arrlen(bucket);
        if (bucketCount == 0) continue;

        // All instances in the bucket share the same exact objectIndex, so the handler resolves to one (codeId, owner).
        int32_t ownerObjectIndex = -1;
        int32_t codeId = ResolvedEventTable_lookup(table, objIdx, outsideSlot, &ownerObjectIndex);
        if (0 > codeId) continue;

        // Snapshot the bucket: an Outside Room handler can spawn/destroy/instance_change.
        int32_t snapshotBase = (int32_t) arrlen(runner->instanceSnapshots);
        arrsetlen(runner->instanceSnapshots, snapshotBase + bucketCount);
        memcpy(&runner->instanceSnapshots[snapshotBase], bucket, (size_t) bucketCount * sizeof(Instance*));

        repeat(bucketCount, i) {
            Instance* inst = runner->instanceSnapshots[snapshotBase + i];
            if (!inst->active) continue;

            bool outside;
            InstanceBBox bbox = Collision_computeBBox(dataWin, inst);
            if (bbox.valid) {
                outside = (0 > bbox.right || bbox.left > roomWidth || 0 > bbox.bottom || bbox.top > roomHeight);
            } else {
                outside = (0 > inst->x || inst->x > roomWidth || 0 > inst->y || inst->y > roomHeight);
            }

            if (outside && !inst->outsideRoom) {
                Runner_executeResolvedEvent(runner, inst, EVENT_OTHER, OTHER_OUTSIDE_ROOM, codeId, ownerObjectIndex);
                if (runner->pendingRoom >= 0) {
                    arrsetlen(runner->instanceSnapshots, snapshotBase);
                    return;
                }
            }

            inst->outsideRoom = outside;
        }

        arrsetlen(runner->instanceSnapshots, snapshotBase);
    }
}

static void persistRoomState(Runner* runner, int32_t roomIndex) {
    SavedRoomState* state = &runner->savedRoomStates[roomIndex];

    // Free any previously saved instances (from an earlier visit)
    int32_t prevSavedCount = (int32_t) arrlen(state->instances);
    repeat(prevSavedCount, i) {
        hmdel(runner->instancesById, state->instances[i]->instanceId);
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
            hmdel(runner->instancesById, inst->instanceId);
            Instance_free(inst);
        }
    }
    arrfree(runner->instances);
    runner->instances = keptInstances;

    // The per-object lists referenced the full pre-transition instance set (persistents, saved-to-state, and soon-to-be-freed). Only the kept persistents remain live, so rebuild from scratch from the final runner->instances.
    Runner_clearAllObjectLists(runner);
    repeat((int32_t) arrlen(runner->instances), i) {
        Runner_addInstanceToObjectLists(runner, runner->instances[i]);
    }

    // Save room visual state
    memcpy(state->backgrounds, runner->backgrounds, sizeof(runner->backgrounds));
    memcpy(state->views, runner->views, sizeof(runner->views));
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

void Runner_step(Runner* runner) {
    // The snapshot arena is stack-like and every push must be matched with a pop within the same frame. Assert that invariant at the top of each step: a non-zero length here means some site below pushed without popping, and we want a loud failure with the offending length so we can find it instead of silently leaking until the next frame.
    requireMessageFormatted(arrlen(runner->instanceSnapshots) == 0, "instanceSnapshots arena was not fully popped at end of previous frame (length=%td)", arrlen(runner->instanceSnapshots));

    runner->frameStepPrepMs = 0.0;
    runner->frameStepEventMs = 0.0;
    runner->frameStepMotionMs = 0.0;
    runner->frameStepCollisionMs = 0.0;
    runner->frameStepFinalizeMs = 0.0;
    runner->frameStepTopObjectMs = 0.0;
    runner->frameStepTopObjectCalls = 0;
    runner->frameStepTopObjectIndex = -1;
    if (runner->frameStepObjectMsByObject != NULL && runner->frameStepObjectCallsByObject != NULL) {
        memset(runner->frameStepObjectMsByObject, 0, (size_t) runner->dataWin->objt.count * sizeof(double));
        memset(runner->frameStepObjectCallsByObject, 0, (size_t) runner->dataWin->objt.count * sizeof(uint32_t));
    }
    double phaseStartMs = Runner_nowMs();

    // Check for gamepad connect/disconnect and fire Async System event
    for (int i = 0; MAX_GAMEPADS > i; i++) {
        GamepadSlot* slot = &runner->gamepads->slots[i];
        if (slot->connected != slot->connectedPrev) {
            DsMapEntry* map = nullptr;
            arrput(runner->dsMapPool, map);
            int32_t mapId = arrlen(runner->dsMapPool) - 1;
            
            DsMapEntry** mapPtr = &runner->dsMapPool[mapId];
            shput(*mapPtr, safeStrdup("event_type"), RValue_makeOwnedString(safeStrdup(slot->connected ? "gamepad discovered" : "gamepad lost")));
            shput(*mapPtr, safeStrdup("pad_index"), RValue_makeReal((GMLReal) i));
            
            runner->asyncLoadMapId = mapId;
            Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ASYNC_SYSTEM);
            
            // Clean up ds_map
            mapPtr = &runner->dsMapPool[mapId];
            if (*mapPtr != nullptr) {
                repeat(shlen(*mapPtr), j) {
                    free((*mapPtr)[j].key);
                    RValue_free(&(*mapPtr)[j].value);
                }
                shfree(*mapPtr);
                *mapPtr = nullptr;
            }
            runner->asyncLoadMapId = -1;
        }
    }

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
    int32_t animEndSlot = EventSlotMap_lookup(&runner->eventSlotMap, EVENT_OTHER, OTHER_ANIMATION_END);
    repeat(animCount, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;
        if (0 > inst->spriteIndex) continue;

        inst->imageIndex += inst->imageSpeed;

        // Wrap image_index (matches HTML5 runner: manual subtract/add instead of using fmod)
        Sprite* sprite = &runner->dataWin->sprt.sprites[inst->spriteIndex];
        float frameCount = (float) sprite->textureCount;
        bool wrapped = false;
        if (inst->imageIndex >= frameCount) {
            inst->imageIndex -= frameCount;
            wrapped = true;
        } else if (0.0f > inst->imageIndex) {
            inst->imageIndex += frameCount;
            wrapped = true;
        }
        if (wrapped && animEndSlot >= 0) {
            int32_t ownerObjectIndex = -1;
            int32_t codeId = ResolvedEventTable_lookup(&runner->eventTable, inst->objectIndex, animEndSlot, &ownerObjectIndex);
            if (codeId >= 0) Runner_executeResolvedEvent(runner, inst, EVENT_OTHER, OTHER_ANIMATION_END, codeId, ownerObjectIndex);
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
    runner->frameStepPrepMs += Runner_nowMs() - phaseStartMs;

    // Execute Begin Step for all instances
    phaseStartMs = Runner_nowMs();
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

    // Process alarms. Outer loop is over alarm slots (matching the native runner's HandleAlarm), and for each slot we walk only the objects in the event table's bySlot range and only those objects' exact instance buckets. Idle instances are further skipped via activeAlarmMask.
    repeat(GML_ALARM_COUNT, alarmIdx) {
        int32_t alarmSlot = EventSlotMap_lookup(&runner->eventSlotMap, EVENT_ALARM, alarmIdx);
        if (0 > alarmSlot) continue;
        ResolvedEventTable* table = &runner->eventTable;
        uint32_t entryCount;
        SlotResponderEntry* entries = ResolvedEventTable_slotEntries(table, alarmSlot, &entryCount);

        repeat(entryCount, s) {
            int32_t objIdx = entries[s].concreteObjectId;
            Instance** bucket = runner->instancesByExactObject[objIdx];
            int32_t bucketCount = (int32_t) arrlen(bucket);
            if (bucketCount == 0) continue;

            // All instances in the bucket share the same exact objectIndex, so the handler resolves to one (codeId, owner).
            int32_t ownerObjectIndex = -1;
            int32_t codeId = ResolvedEventTable_lookup(table, objIdx, alarmSlot, &ownerObjectIndex);
            if (0 > codeId) continue;

            // Snapshot the bucket before dispatch: alarm code can call instance_change/instance_destroy/instance_create which mutate the live bucket. Iterating the snapshot also ensures newly-created instances do not fire alarms in this same phase.
            int32_t snapshotBase = (int32_t) arrlen(runner->instanceSnapshots);
            arrsetlen(runner->instanceSnapshots, snapshotBase + bucketCount);
            memcpy(&runner->instanceSnapshots[snapshotBase], bucket, (size_t) bucketCount * sizeof(Instance*));

            repeat(bucketCount, i) {
                Instance* inst = runner->instanceSnapshots[snapshotBase + i];
                if (!inst->active) continue;
                uint16_t bit = (uint16_t) (1u << alarmIdx);
                if ((inst->activeAlarmMask & bit) == 0) continue;

#ifdef ENABLE_VM_TRACING
                GameObject* object = &runner->dataWin->objt.objects[inst->objectIndex];
                if (shgeti(runner->vmContext->alarmsToBeTraced, "*") != -1 || shgeti(runner->vmContext->alarmsToBeTraced, object->name) != -1) {
                    fprintf(stderr, "VM: [%s] Ticking down Alarm[%d] (instanceId=%d), current tick is %d\n", object->name, alarmIdx, inst->instanceId, inst->alarm[alarmIdx]);
                }
#endif

                inst->alarm[alarmIdx]--;
                if (inst->alarm[alarmIdx] == 0) {
                    inst->alarm[alarmIdx] = -1;
                    inst->activeAlarmMask &= (uint16_t) ~bit;

#ifdef ENABLE_VM_TRACING
                    if (shgeti(runner->vmContext->alarmsToBeTraced, "*") != -1 || shgeti(runner->vmContext->alarmsToBeTraced, object->name) != -1) {
                        fprintf(stderr, "VM: [%s] Firing Alarm[%d] (instanceId=%d)\n", object->name, alarmIdx, inst->instanceId);
                    }
#endif

                    Runner_executeResolvedEvent(runner, inst, EVENT_ALARM, alarmIdx, codeId, ownerObjectIndex);
                }
            }

            arrsetlen(runner->instanceSnapshots, snapshotBase);
        }
    }

    // Execute Normal Step for all instances
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_NORMAL);
    runner->frameStepEventMs += Runner_nowMs() - phaseStartMs;

    // Apply motion: friction, gravity, then x += hspeed, y += vspeed
    phaseStartMs = Runner_nowMs();
    int32_t motionCount = (int32_t) arrlen(runner->instances);
    int32_t endOfPathSlot = EventSlotMap_lookup(&runner->eventSlotMap, EVENT_OTHER, OTHER_END_OF_PATH);
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
        if (adaptPath(runner, inst) && endOfPathSlot >= 0) {
            int32_t ownerObjectIndex = -1;
            int32_t codeId = ResolvedEventTable_lookup(&runner->eventTable, inst->objectIndex, endOfPathSlot, &ownerObjectIndex);
            if (codeId >= 0) Runner_executeResolvedEvent(runner, inst, EVENT_OTHER, OTHER_END_OF_PATH, codeId, ownerObjectIndex);
        }

        // Apply movement
        if (inst->hspeed != 0.0f || inst->vspeed != 0.0f) {
            inst->x += inst->hspeed;
            inst->y += inst->vspeed;
            SpatialGrid_markInstanceAsDirty(runner->spatialGrid, inst);
        }
    }
    runner->frameStepMotionMs += Runner_nowMs() - phaseStartMs;

    // Dispatch outside room events
    phaseStartMs = Runner_nowMs();
    dispatchOutsideRoomEvents(runner);

    // Dispatch collision events
    dispatchCollisionEvents(runner);
    runner->frameStepCollisionMs += Runner_nowMs() - phaseStartMs;

    // Execute End Step for all instances
    phaseStartMs = Runner_nowMs();
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_END);

    // Update view following
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
        runner->frameStepFinalizeMs += Runner_nowMs() - phaseStartMs;
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
            persistRoomState(runner, oldRoomIndex);
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
    Runner_sweepDeadStructs(runner);
    runner->frameStepFinalizeMs += Runner_nowMs() - phaseStartMs;

    if (runner->frameStepObjectMsByObject != NULL && runner->frameStepObjectCallsByObject != NULL) {
        repeat(runner->dataWin->objt.count, i) {
            double objectMs = runner->frameStepObjectMsByObject[i];
            if (objectMs > runner->frameStepTopObjectMs) {
                runner->frameStepTopObjectMs = objectMs;
                runner->frameStepTopObjectCalls = runner->frameStepObjectCallsByObject[i];
                runner->frameStepTopObjectIndex = (int32_t) i;
            }
        }
    }

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
        printf("  Position: (%g, %g)\n", (double) inst->x, (double) inst->y);
        printf("  Depth: %d\n", inst->depth);
        printf("  Sprite: %s (index %d), imageIndex=%g, imageSpeed=%g\n", spriteName, inst->spriteIndex, (double) inst->imageIndex, (double) inst->imageSpeed);
        printf("  Scale: (%g, %g), Angle: %g, Alpha: %g, Blend: 0x%06X\n", (double) inst->imageXscale, (double) inst->imageYscale, (double) inst->imageAngle, (double) inst->imageAlpha, inst->imageBlend);
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
        repeat(inst->selfVars.capacity, svIdx) {
            IntRValueEntry* entry = &inst->selfVars.entries[svIdx];
            if (entry->key == INT_RVALUE_HASHMAP_EMPTY_KEY) continue;
            int32_t varID = entry->key;
            RValue val = entry->value;
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
                repeat(GMLArray_length1D(val.array), ai) {
                    RValue* cell = GMLArray_slot(val.array, ai);
                    if (cell == nullptr || cell->type == RVALUE_UNDEFINED) continue;
                    char* innerStr = RValue_toStringFancy(*cell);
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
        repeat(GMLArray_length1D(val.array), ai) {
            RValue* cell = GMLArray_slot(val.array, ai);
            if (cell == nullptr || cell->type == RVALUE_UNDEFINED) continue;
            char* innerStr = RValue_toStringFancy(*cell);
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
                repeat(GMLArray_length1D(val.array), ai) {
                    RValue* cell = GMLArray_slot(val.array, ai);
                    writeRValueJson(w, cell != nullptr ? *cell : (RValue){ .type = RVALUE_UNDEFINED });
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
        case RVALUE_STRUCT: {
            char buf[64];
            snprintf(buf, sizeof(buf), "<struct:%u>", val.structInst != nullptr ? val.structInst->instanceId : 0);
            JsonWriter_string(w, buf);
            break;
        }
    }
}

RunnerTelemetry Runner_collectTelemetry(Runner* runner) {
    RunnerTelemetry telemetry = {0};
    if (runner == NULL) return telemetry;

    telemetry.liveInstances = (uint32_t) arrlen(runner->instances);
    telemetry.liveStructInstances = (uint32_t) arrlen(runner->structInstances);

    telemetry.dsMapSlots = (uint32_t) arrlen(runner->dsMapPool);
    repeat((int32_t) arrlen(runner->dsMapPool), i) {
        if (runner->dsMapPool[i] != NULL) telemetry.dsMapLive++;
    }

    telemetry.dsListSlots = (uint32_t) arrlen(runner->dsListPool);
    repeat((int32_t) arrlen(runner->dsListPool), i) {
        DsList* list = &runner->dsListPool[i];
        if (!list->freed) telemetry.dsListLive++;
        telemetry.dsListItems += (uint32_t) arrlen(list->items);
    }

    return telemetry;
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
        repeat(inst->selfVars.capacity, svIdx) {
            IntRValueEntry* entry = &inst->selfVars.entries[svIdx];
            if (entry->key == INT_RVALUE_HASHMAP_EMPTY_KEY) continue;
            int32_t varID = entry->key;
            RValue val = entry->value;
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

    if (runner->instancesByObject != nullptr) {
        uint32_t objectCount = runner->dataWin->objt.count;
        repeat(objectCount, i) {
            arrfree(runner->instancesByObject[i]);
        }
        free(runner->instancesByObject);
        runner->instancesByObject = nullptr;
    }
    if (runner->instancesByExactObject != nullptr) {
        uint32_t objectCount = runner->dataWin->objt.count;
        repeat(objectCount, i) {
            arrfree(runner->instancesByExactObject[i]);
        }
        free(runner->instancesByExactObject);
        runner->instancesByExactObject = nullptr;
    }
    if (runner->objectsWithAnyEventOfType != nullptr) {
        repeat(OBJT_EVENT_TYPE_COUNT, t) {
            arrfree(runner->objectsWithAnyEventOfType[t]);
        }
        free(runner->objectsWithAnyEventOfType);
        runner->objectsWithAnyEventOfType = nullptr;
    }
    arrfree(runner->cachedDrawables);
    runner->cachedDrawables = nullptr;
    arrfree(runner->instanceSnapshots);
    runner->instanceSnapshots = nullptr;
    arrfree(runner->eventDispatchInstances);
    runner->eventDispatchInstances = nullptr;
    arrfree(runner->n3dsBattleFieldInstances);
    runner->n3dsBattleFieldInstances = nullptr;
    arrfree(runner->n3dsBattleUIInstances);
    runner->n3dsBattleUIInstances = nullptr;
    arrfree(runner->n3dsTopScreenGUIInstances);
    runner->n3dsTopScreenGUIInstances = nullptr;
    repeat(3, i) {
        arrfree(runner->n3dsTopScreenGUIResponderEvents[i]);
        runner->n3dsTopScreenGUIResponderEvents[i] = nullptr;
        runner->n3dsTopScreenGUIResponderListsValid[i] = false;
    }
    free(runner->frameStepObjectMsByObject);
    runner->frameStepObjectMsByObject = nullptr;
    free(runner->frameStepObjectCallsByObject);
    runner->frameStepObjectCallsByObject = nullptr;
    ResolvedEventTable_free(&runner->eventTable);
    EventSlotMap_destroy(&runner->eventSlotMap);
    shfree(runner->assetsByName);

    RunnerKeyboard_free(runner->keyboard);
    RunnerGamepad_free(runner->gamepads);
    Instance_free(runner->globalScopeInstance);
    free(runner);
}
