#include "vm_builtins.h"
#include "instance.h"
#include "json_reader.h"
#include "runner.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "rvalue.h"
#include "stb_ds.h"
#include "text_utils.h"
#include "collision.h"
#include "ini.h"
#include "audio_system.h"
#include "file_system.h"

#define MAX_VIEWS 8
#define MAX_BACKGROUNDS 8

// ===[ STUB LOGGING ]===

static void logStubbedFunction(VMContext* ctx, const char* funcName) {
    const char* callerName = VM_getCallerName(ctx);
    char* dedupKey = VM_createDedupKey(callerName, funcName);

    if (ctx->alwaysLogStubbedFunctions || 0 > shgeti(ctx->loggedStubbedFuncs, dedupKey)) {
        // shput stores the key pointer, so don't free it when inserting
        shput(ctx->loggedStubbedFuncs, dedupKey, true);
        fprintf(stderr, "VM: [%s] Stubbed function \"%s\"!\n", callerName, funcName);
    } else {
        free(dedupKey);
    }
}

// ===[ DS_MAP SYSTEM ]===

static int32_t dsMapCreate(Runner* runner) {
    DsMapEntry* newMap = nullptr;
    int32_t id = (int32_t) arrlen(runner->dsMapPool);
    arrput(runner->dsMapPool, newMap);
    return id;
}

static DsMapEntry** dsMapGet(Runner* runner, int32_t id) {
    if (id < 0 || (int32_t) arrlen(runner->dsMapPool) <= id) return nullptr;
    return &runner->dsMapPool[id];
}

// ===[ DS_LIST SYSTEM ]===

static int32_t dsListCreate(Runner* runner) {
    DsList newList = { .items = nullptr };
    int32_t id = (int32_t) arrlen(runner->dsListPool);
    arrput(runner->dsListPool, newList);
    return id;
}

static DsList* dsListGet(Runner* runner, int32_t id) {
    if (0 > id || id > (int32_t) arrlen(runner->dsListPool)) return nullptr;
    return &runner->dsListPool[id];
}

// ===[ BUILT-IN VARIABLE GET/SET ]===

/**
 * Gets the argument number from the name
 *
 * If it returns -1, then the name is not an argument variable
 *
 * @param name The name
 * @return The argument number, -1 if it is not an argument variable
 */
static int extractArgumentNumber(const char* name) {
    if (strncmp(name, "argument", 8) == 0) {
        char* end;
        long argNumber = strtol(name + 8, &end, 10);
        if (end == name + 8 || *end != '\0' || 0 > argNumber || argNumber > 15) return -1;
        return (int) argNumber;
    }
    return -1;
}

static bool isValidAlarmIndex(int alarmIndex) {
    return alarmIndex >= 0 && GML_ALARM_COUNT > alarmIndex;
}

// Resolves a built-in variable name to its enum ID
int16_t VMBuiltins_resolveBuiltinVarId(const char* name) {
    // Instance properties
    if (strcmp(name, "x") == 0) return BUILTIN_VAR_X;
    if (strcmp(name, "y") == 0) return BUILTIN_VAR_Y;
    if (strcmp(name, "xprevious") == 0) return BUILTIN_VAR_XPREVIOUS;
    if (strcmp(name, "yprevious") == 0) return BUILTIN_VAR_YPREVIOUS;
    if (strcmp(name, "xstart") == 0) return BUILTIN_VAR_XSTART;
    if (strcmp(name, "ystart") == 0) return BUILTIN_VAR_YSTART;
    if (strcmp(name, "image_speed") == 0) return BUILTIN_VAR_IMAGE_SPEED;
    if (strcmp(name, "image_index") == 0) return BUILTIN_VAR_IMAGE_INDEX;
    if (strcmp(name, "image_xscale") == 0) return BUILTIN_VAR_IMAGE_XSCALE;
    if (strcmp(name, "image_yscale") == 0) return BUILTIN_VAR_IMAGE_YSCALE;
    if (strcmp(name, "image_angle") == 0) return BUILTIN_VAR_IMAGE_ANGLE;
    if (strcmp(name, "image_alpha") == 0) return BUILTIN_VAR_IMAGE_ALPHA;
    if (strcmp(name, "image_blend") == 0) return BUILTIN_VAR_IMAGE_BLEND;
    if (strcmp(name, "image_number") == 0) return BUILTIN_VAR_IMAGE_NUMBER;
    if (strcmp(name, "sprite_index") == 0) return BUILTIN_VAR_SPRITE_INDEX;
    if (strcmp(name, "sprite_width") == 0) return BUILTIN_VAR_SPRITE_WIDTH;
    if (strcmp(name, "sprite_height") == 0) return BUILTIN_VAR_SPRITE_HEIGHT;
    if (strcmp(name, "sprite_xoffset") == 0) return BUILTIN_VAR_SPRITE_XOFFSET;
    if (strcmp(name, "sprite_yoffset") == 0) return BUILTIN_VAR_SPRITE_YOFFSET;
    if (strcmp(name, "bbox_left") == 0) return BUILTIN_VAR_BBOX_LEFT;
    if (strcmp(name, "bbox_right") == 0) return BUILTIN_VAR_BBOX_RIGHT;
    if (strcmp(name, "bbox_top") == 0) return BUILTIN_VAR_BBOX_TOP;
    if (strcmp(name, "bbox_bottom") == 0) return BUILTIN_VAR_BBOX_BOTTOM;
    if (strcmp(name, "visible") == 0) return BUILTIN_VAR_VISIBLE;
    if (strcmp(name, "depth") == 0) return BUILTIN_VAR_DEPTH;
    if (strcmp(name, "persistent") == 0) return BUILTIN_VAR_PERSISTENT;
    if (strcmp(name, "solid") == 0) return BUILTIN_VAR_SOLID;
    if (strcmp(name, "mask_index") == 0) return BUILTIN_VAR_MASK_INDEX;
    if (strcmp(name, "id") == 0) return BUILTIN_VAR_ID;
    if (strcmp(name, "object_index") == 0) return BUILTIN_VAR_OBJECT_INDEX;
    if (strcmp(name, "speed") == 0) return BUILTIN_VAR_SPEED;
    if (strcmp(name, "direction") == 0) return BUILTIN_VAR_DIRECTION;
    if (strcmp(name, "hspeed") == 0) return BUILTIN_VAR_HSPEED;
    if (strcmp(name, "vspeed") == 0) return BUILTIN_VAR_VSPEED;
    if (strcmp(name, "friction") == 0) return BUILTIN_VAR_FRICTION;
    if (strcmp(name, "gravity") == 0) return BUILTIN_VAR_GRAVITY;
    if (strcmp(name, "gravity_direction") == 0) return BUILTIN_VAR_GRAVITY_DIRECTION;
    if (strcmp(name, "alarm") == 0) return BUILTIN_VAR_ALARM;

    // Path instance variables
    if (strcmp(name, "path_index") == 0) return BUILTIN_VAR_PATH_INDEX;
    if (strcmp(name, "path_position") == 0) return BUILTIN_VAR_PATH_POSITION;
    if (strcmp(name, "path_positionprevious") == 0) return BUILTIN_VAR_PATH_POSITIONPREVIOUS;
    if (strcmp(name, "path_speed") == 0) return BUILTIN_VAR_PATH_SPEED;
    if (strcmp(name, "path_scale") == 0) return BUILTIN_VAR_PATH_SCALE;
    if (strcmp(name, "path_orientation") == 0) return BUILTIN_VAR_PATH_ORIENTATION;
    if (strcmp(name, "path_endaction") == 0) return BUILTIN_VAR_PATH_ENDACTION;

    // Room properties
    if (strcmp(name, "room") == 0) return BUILTIN_VAR_ROOM;
    if (strcmp(name, "room_first") == 0) return BUILTIN_VAR_ROOM_FIRST;
    if (strcmp(name, "room_speed") == 0) return BUILTIN_VAR_ROOM_SPEED;
    if (strcmp(name, "room_width") == 0) return BUILTIN_VAR_ROOM_WIDTH;
    if (strcmp(name, "room_height") == 0) return BUILTIN_VAR_ROOM_HEIGHT;
    if (strcmp(name, "room_persistent") == 0) return BUILTIN_VAR_ROOM_PERSISTENT;

    // View properties
    if (strcmp(name, "view_current") == 0) return BUILTIN_VAR_VIEW_CURRENT;
    if (strcmp(name, "view_xview") == 0) return BUILTIN_VAR_VIEW_XVIEW;
    if (strcmp(name, "view_yview") == 0) return BUILTIN_VAR_VIEW_YVIEW;
    if (strcmp(name, "view_wview") == 0) return BUILTIN_VAR_VIEW_WVIEW;
    if (strcmp(name, "view_hview") == 0) return BUILTIN_VAR_VIEW_HVIEW;
    if (strcmp(name, "view_xport") == 0) return BUILTIN_VAR_VIEW_XPORT;
    if (strcmp(name, "view_yport") == 0) return BUILTIN_VAR_VIEW_YPORT;
    if (strcmp(name, "view_wport") == 0) return BUILTIN_VAR_VIEW_WPORT;
    if (strcmp(name, "view_hport") == 0) return BUILTIN_VAR_VIEW_HPORT;
    if (strcmp(name, "view_visible") == 0) return BUILTIN_VAR_VIEW_VISIBLE;
    if (strcmp(name, "view_angle") == 0) return BUILTIN_VAR_VIEW_ANGLE;
    if (strcmp(name, "view_hborder") == 0) return BUILTIN_VAR_VIEW_HBORDER;
    if (strcmp(name, "view_vborder") == 0) return BUILTIN_VAR_VIEW_VBORDER;
    if (strcmp(name, "view_object") == 0) return BUILTIN_VAR_VIEW_OBJECT;
    if (strcmp(name, "view_hspeed") == 0) return BUILTIN_VAR_VIEW_HSPEED;
    if (strcmp(name, "view_vspeed") == 0) return BUILTIN_VAR_VIEW_VSPEED;

    // Background properties
    if (strcmp(name, "background_visible") == 0) return BUILTIN_VAR_BACKGROUND_VISIBLE;
    if (strcmp(name, "background_index") == 0) return BUILTIN_VAR_BACKGROUND_INDEX;
    if (strcmp(name, "background_x") == 0) return BUILTIN_VAR_BACKGROUND_X;
    if (strcmp(name, "background_y") == 0) return BUILTIN_VAR_BACKGROUND_Y;
    if (strcmp(name, "background_hspeed") == 0) return BUILTIN_VAR_BACKGROUND_HSPEED;
    if (strcmp(name, "background_vspeed") == 0) return BUILTIN_VAR_BACKGROUND_VSPEED;
    if (strcmp(name, "background_width") == 0) return BUILTIN_VAR_BACKGROUND_WIDTH;
    if (strcmp(name, "background_height") == 0) return BUILTIN_VAR_BACKGROUND_HEIGHT;
    if (strcmp(name, "background_alpha") == 0) return BUILTIN_VAR_BACKGROUND_ALPHA;
    if (strcmp(name, "background_color") == 0) return BUILTIN_VAR_BACKGROUND_COLOR;
    if (strcmp(name, "background_colour") == 0) return BUILTIN_VAR_BACKGROUND_COLOUR;

    // OS constants
    if (strcmp(name, "os_type") == 0) return BUILTIN_VAR_OS_TYPE;
    if (strcmp(name, "os_unknown") == 0) return BUILTIN_VAR_OS_UNKNOWN;
    if (strcmp(name, "os_win32") == 0) return BUILTIN_VAR_OS_WIN32;
    if (strcmp(name, "os_windows") == 0) return BUILTIN_VAR_OS_WINDOWS;
    if (strcmp(name, "os_macosx") == 0) return BUILTIN_VAR_OS_MACOSX;
    if (strcmp(name, "os_psp") == 0) return BUILTIN_VAR_OS_PSP;
    if (strcmp(name, "os_ios") == 0) return BUILTIN_VAR_OS_IOS;
    if (strcmp(name, "os_android") == 0) return BUILTIN_VAR_OS_ANDROID;
    if (strcmp(name, "os_symbian") == 0) return BUILTIN_VAR_OS_SYMBIAN;
    if (strcmp(name, "os_linux") == 0) return BUILTIN_VAR_OS_LINUX;
    if (strcmp(name, "os_winphone") == 0) return BUILTIN_VAR_OS_WINPHONE;
    if (strcmp(name, "os_tizen") == 0) return BUILTIN_VAR_OS_TIZEN;
    if (strcmp(name, "os_win8native") == 0) return BUILTIN_VAR_OS_WIN8NATIVE;
    if (strcmp(name, "os_wiiu") == 0) return BUILTIN_VAR_OS_WIIU;
    if (strcmp(name, "os_3ds") == 0) return BUILTIN_VAR_OS_3DS;
    if (strcmp(name, "os_psvita") == 0) return BUILTIN_VAR_OS_PSVITA;
    if (strcmp(name, "os_bb10") == 0) return BUILTIN_VAR_OS_BB10;
    if (strcmp(name, "os_ps4") == 0) return BUILTIN_VAR_OS_PS4;
    if (strcmp(name, "os_xboxone") == 0) return BUILTIN_VAR_OS_XBOXONE;
    if (strcmp(name, "os_ps3") == 0) return BUILTIN_VAR_OS_PS3;
    if (strcmp(name, "os_xbox360") == 0) return BUILTIN_VAR_OS_XBOX360;
    if (strcmp(name, "os_uwp") == 0) return BUILTIN_VAR_OS_UWP;
    if (strcmp(name, "os_amazon") == 0) return BUILTIN_VAR_OS_AMAZON;
    if (strcmp(name, "os_switch") == 0) return BUILTIN_VAR_OS_SWITCH;
    if (strcmp(name, "os_llvm_win32") == 0) return BUILTIN_VAR_OS_LLVM_WIN32;
    if (strcmp(name, "os_llvm_macosx") == 0) return BUILTIN_VAR_OS_LLVM_MACOSX;
    if (strcmp(name, "os_llvm_psp") == 0) return BUILTIN_VAR_OS_LLVM_PSP;
    if (strcmp(name, "os_llvm_ios") == 0) return BUILTIN_VAR_OS_LLVM_IOS;
    if (strcmp(name, "os_llvm_android") == 0) return BUILTIN_VAR_OS_LLVM_ANDROID;
    if (strcmp(name, "os_llvm_symbian") == 0) return BUILTIN_VAR_OS_LLVM_SYMBIAN;
    if (strcmp(name, "os_llvm_linux") == 0) return BUILTIN_VAR_OS_LLVM_LINUX;
    if (strcmp(name, "os_llvm_winphone") == 0) return BUILTIN_VAR_OS_LLVM_WINPHONE;

    // Timing
    if (strcmp(name, "current_time") == 0) return BUILTIN_VAR_CURRENT_TIME;

    // File system
    if (strcmp(name, "working_directory") == 0) return BUILTIN_VAR_WORKING_DIRECTORY;

    // Arguments
    if (strcmp(name, "argument_count") == 0) return BUILTIN_VAR_ARGUMENT_COUNT;
    if (strcmp(name, "argument") == 0) return BUILTIN_VAR_ARGUMENT;
    if (strcmp(name, "argument0") == 0) return BUILTIN_VAR_ARGUMENT0;
    if (strcmp(name, "argument1") == 0) return BUILTIN_VAR_ARGUMENT1;
    if (strcmp(name, "argument2") == 0) return BUILTIN_VAR_ARGUMENT2;
    if (strcmp(name, "argument3") == 0) return BUILTIN_VAR_ARGUMENT3;
    if (strcmp(name, "argument4") == 0) return BUILTIN_VAR_ARGUMENT4;
    if (strcmp(name, "argument5") == 0) return BUILTIN_VAR_ARGUMENT5;
    if (strcmp(name, "argument6") == 0) return BUILTIN_VAR_ARGUMENT6;
    if (strcmp(name, "argument7") == 0) return BUILTIN_VAR_ARGUMENT7;
    if (strcmp(name, "argument8") == 0) return BUILTIN_VAR_ARGUMENT8;
    if (strcmp(name, "argument9") == 0) return BUILTIN_VAR_ARGUMENT9;
    if (strcmp(name, "argument10") == 0) return BUILTIN_VAR_ARGUMENT10;
    if (strcmp(name, "argument11") == 0) return BUILTIN_VAR_ARGUMENT11;
    if (strcmp(name, "argument12") == 0) return BUILTIN_VAR_ARGUMENT12;
    if (strcmp(name, "argument13") == 0) return BUILTIN_VAR_ARGUMENT13;
    if (strcmp(name, "argument14") == 0) return BUILTIN_VAR_ARGUMENT14;
    if (strcmp(name, "argument15") == 0) return BUILTIN_VAR_ARGUMENT15;

    // Keyboard
    if (strcmp(name, "keyboard_key") == 0) return BUILTIN_VAR_KEYBOARD_KEY;
    if (strcmp(name, "keyboard_lastchar") == 0) return BUILTIN_VAR_KEYBOARD_LASTCHAR;
    if (strcmp(name, "keyboard_lastkey") == 0) return BUILTIN_VAR_KEYBOARD_LASTKEY;

    // Surfaces
    if (strcmp(name, "application_surface") == 0) return BUILTIN_VAR_APPLICATION_SURFACE;

    // Constants
    if (strcmp(name, "true") == 0) return BUILTIN_VAR_TRUE;
    if (strcmp(name, "false") == 0) return BUILTIN_VAR_FALSE;
    if (strcmp(name, "pi") == 0) return BUILTIN_VAR_PI;
    if (strcmp(name, "undefined") == 0) return BUILTIN_VAR_UNDEFINED;

    // Path action constants
    if (strcmp(name, "path_action_stop") == 0) return BUILTIN_VAR_PATH_ACTION_STOP;
    if (strcmp(name, "path_action_restart") == 0) return BUILTIN_VAR_PATH_ACTION_RESTART;
    if (strcmp(name, "path_action_continue") == 0) return BUILTIN_VAR_PATH_ACTION_CONTINUE;
    if (strcmp(name, "path_action_reverse") == 0) return BUILTIN_VAR_PATH_ACTION_REVERSE;

    // Buffer type constants
    if (strcmp(name, "buffer_fixed") == 0) return BUILTIN_VAR_BUFFER_FIXED;
    if (strcmp(name, "buffer_grow") == 0) return BUILTIN_VAR_BUFFER_GROW;
    if (strcmp(name, "buffer_wrap") == 0) return BUILTIN_VAR_BUFFER_WRAP;
    if (strcmp(name, "buffer_fast") == 0) return BUILTIN_VAR_BUFFER_FAST;

    // Buffer data type constants
    if (strcmp(name, "buffer_u8") == 0) return BUILTIN_VAR_BUFFER_U8;
    if (strcmp(name, "buffer_s8") == 0) return BUILTIN_VAR_BUFFER_S8;
    if (strcmp(name, "buffer_u16") == 0) return BUILTIN_VAR_BUFFER_U16;
    if (strcmp(name, "buffer_s16") == 0) return BUILTIN_VAR_BUFFER_S16;
    if (strcmp(name, "buffer_u32") == 0) return BUILTIN_VAR_BUFFER_U32;
    if (strcmp(name, "buffer_s32") == 0) return BUILTIN_VAR_BUFFER_S32;
    if (strcmp(name, "buffer_f16") == 0) return BUILTIN_VAR_BUFFER_F16;
    if (strcmp(name, "buffer_f32") == 0) return BUILTIN_VAR_BUFFER_F32;
    if (strcmp(name, "buffer_f64") == 0) return BUILTIN_VAR_BUFFER_F64;
    if (strcmp(name, "buffer_bool") == 0) return BUILTIN_VAR_BUFFER_BOOL;
    if (strcmp(name, "buffer_string") == 0) return BUILTIN_VAR_BUFFER_STRING;
    if (strcmp(name, "buffer_u64") == 0) return BUILTIN_VAR_BUFFER_U64;
    if (strcmp(name, "buffer_text") == 0) return BUILTIN_VAR_BUFFER_TEXT;

    // Buffer seek mode constants
    if (strcmp(name, "buffer_seek_start") == 0) return BUILTIN_VAR_BUFFER_SEEK_START;
    if (strcmp(name, "buffer_seek_relative") == 0) return BUILTIN_VAR_BUFFER_SEEK_RELATIVE;
    if (strcmp(name, "buffer_seek_end") == 0) return BUILTIN_VAR_BUFFER_SEEK_END;

    // Other
    if (strcmp(name, "fps") == 0) return BUILTIN_VAR_FPS;
    if (strcmp(name, "debug_mode") == 0) return BUILTIN_VAR_DEBUG_MODE;

    return BUILTIN_VAR_UNKNOWN;
}

RValue VMBuiltins_getVariable(VMContext* ctx, int16_t builtinVarId, const char* name, int32_t arrayIndex) {
    Instance* inst = (Instance*) ctx->currentInstance;
    Runner* runner = (Runner*) ctx->runner;
    requireNotNull(runner);

    // File system
    if (builtinVarId == BUILTIN_VAR_WORKING_DIRECTORY) {
        FileSystem* fs = runner->fileSystem;
        char* path = fs->vtable->resolvePath(fs, "");
        return RValue_makeOwnedString(path);
    }

    // OS constants
    if (builtinVarId == BUILTIN_VAR_OS_TYPE) return RValue_makeReal((double) runner->osType);
    if (builtinVarId == BUILTIN_VAR_OS_UNKNOWN) return RValue_makeReal((double) OS_UNKNOWN);
    if (builtinVarId == BUILTIN_VAR_OS_WIN32) return RValue_makeReal((double) OS_WINDOWS);
    if (builtinVarId == BUILTIN_VAR_OS_WINDOWS) return RValue_makeReal((double) OS_WINDOWS);
    if (builtinVarId == BUILTIN_VAR_OS_MACOSX) return RValue_makeReal((double) OS_MACOSX);
    if (builtinVarId == BUILTIN_VAR_OS_PSP) return RValue_makeReal((double) OS_PSP);
    if (builtinVarId == BUILTIN_VAR_OS_IOS) return RValue_makeReal((double) OS_IOS);
    if (builtinVarId == BUILTIN_VAR_OS_ANDROID) return RValue_makeReal((double) OS_ANDROID);
    if (builtinVarId == BUILTIN_VAR_OS_SYMBIAN) return RValue_makeReal((double) OS_SYMBIAN);
    if (builtinVarId == BUILTIN_VAR_OS_LINUX) return RValue_makeReal((double) OS_LINUX);
    if (builtinVarId == BUILTIN_VAR_OS_WINPHONE) return RValue_makeReal((double) OS_WINPHONE);
    if (builtinVarId == BUILTIN_VAR_OS_TIZEN) return RValue_makeReal((double) OS_TIZEN);
    if (builtinVarId == BUILTIN_VAR_OS_WIN8NATIVE) return RValue_makeReal((double) OS_WIN8NATIVE);
    if (builtinVarId == BUILTIN_VAR_OS_WIIU) return RValue_makeReal((double) OS_WIIU);
    if (builtinVarId == BUILTIN_VAR_OS_3DS) return RValue_makeReal((double) OS_3DS);
    if (builtinVarId == BUILTIN_VAR_OS_PSVITA) return RValue_makeReal((double) OS_PSVITA);
    if (builtinVarId == BUILTIN_VAR_OS_BB10) return RValue_makeReal((double) OS_BB10);
    if (builtinVarId == BUILTIN_VAR_OS_PS4) return RValue_makeReal((double) OS_PS4);
    if (builtinVarId == BUILTIN_VAR_OS_XBOXONE) return RValue_makeReal((double) OS_XBOXONE);
    if (builtinVarId == BUILTIN_VAR_OS_PS3) return RValue_makeReal((double) OS_PS3);
    if (builtinVarId == BUILTIN_VAR_OS_XBOX360) return RValue_makeReal((double) OS_XBOX360);
    if (builtinVarId == BUILTIN_VAR_OS_UWP) return RValue_makeReal((double) OS_UWP);
    if (builtinVarId == BUILTIN_VAR_OS_AMAZON) return RValue_makeReal((double) OS_AMAZON);
    if (builtinVarId == BUILTIN_VAR_OS_SWITCH) return RValue_makeReal((double) OS_SWITCH);
    if (builtinVarId == BUILTIN_VAR_OS_LLVM_WIN32) return RValue_makeReal((double) OS_LLVM_WIN32);
    if (builtinVarId == BUILTIN_VAR_OS_LLVM_MACOSX) return RValue_makeReal((double) OS_LLVM_MACOSX);
    if (builtinVarId == BUILTIN_VAR_OS_LLVM_PSP) return RValue_makeReal((double) OS_LLVM_PSP);
    if (builtinVarId == BUILTIN_VAR_OS_LLVM_IOS) return RValue_makeReal((double) OS_LLVM_IOS);
    if (builtinVarId == BUILTIN_VAR_OS_LLVM_ANDROID) return RValue_makeReal((double) OS_LLVM_ANDROID);
    if (builtinVarId == BUILTIN_VAR_OS_LLVM_SYMBIAN) return RValue_makeReal((double) OS_LLVM_SYMBIAN);
    if (builtinVarId == BUILTIN_VAR_OS_LLVM_LINUX) return RValue_makeReal((double) OS_LLVM_LINUX);
    if (builtinVarId == BUILTIN_VAR_OS_LLVM_WINPHONE) return RValue_makeReal((double) OS_LLVM_WINPHONE);

    // Per-instance properties
    if (inst != nullptr) {
        if (builtinVarId == BUILTIN_VAR_IMAGE_SPEED) return RValue_makeReal(inst->imageSpeed);
        if (builtinVarId == BUILTIN_VAR_IMAGE_INDEX) return RValue_makeReal(inst->imageIndex);
        if (builtinVarId == BUILTIN_VAR_IMAGE_XSCALE) return RValue_makeReal(inst->imageXscale);
        if (builtinVarId == BUILTIN_VAR_IMAGE_YSCALE) return RValue_makeReal(inst->imageYscale);
        if (builtinVarId == BUILTIN_VAR_IMAGE_ANGLE) return RValue_makeReal(inst->imageAngle);
        if (builtinVarId == BUILTIN_VAR_IMAGE_ALPHA) return RValue_makeReal(inst->imageAlpha);
        if (builtinVarId == BUILTIN_VAR_IMAGE_BLEND) return RValue_makeReal((GMLReal) inst->imageBlend);
        if (builtinVarId == BUILTIN_VAR_IMAGE_NUMBER) {
            if (inst->spriteIndex >= 0) {
                Sprite* sprite = &ctx->runner->dataWin->sprt.sprites[inst->spriteIndex];
                return RValue_makeReal((GMLReal) sprite->textureCount);
            }
            return RValue_makeReal(0.0);
        }
        if (builtinVarId == BUILTIN_VAR_SPRITE_INDEX) return RValue_makeReal((GMLReal) inst->spriteIndex);
        if (builtinVarId == BUILTIN_VAR_SPRITE_WIDTH) {
            if (inst->spriteIndex >= 0 && runner->dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
                return RValue_makeReal((GMLReal) runner->dataWin->sprt.sprites[inst->spriteIndex].width * inst->imageXscale);
            }
            return RValue_makeReal(0.0);
        }
        if (builtinVarId == BUILTIN_VAR_SPRITE_HEIGHT) {
            if (inst->spriteIndex >= 0 && runner->dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
                return RValue_makeReal((GMLReal) runner->dataWin->sprt.sprites[inst->spriteIndex].height * inst->imageYscale);
            }
            return RValue_makeReal(0.0);
        }
        if (builtinVarId == BUILTIN_VAR_SPRITE_XOFFSET) {
            if (inst->spriteIndex >= 0 && runner->dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
                return RValue_makeReal((GMLReal) runner->dataWin->sprt.sprites[inst->spriteIndex].originX);
            }
            return RValue_makeReal(0.0);
        }
        if (builtinVarId == BUILTIN_VAR_SPRITE_YOFFSET) {
            if (inst->spriteIndex >= 0 && runner->dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
                return RValue_makeReal((GMLReal) runner->dataWin->sprt.sprites[inst->spriteIndex].originY);
            }
            return RValue_makeReal(0.0);
        }
        if (builtinVarId == BUILTIN_VAR_BBOX_LEFT || builtinVarId == BUILTIN_VAR_BBOX_RIGHT || builtinVarId == BUILTIN_VAR_BBOX_TOP || builtinVarId == BUILTIN_VAR_BBOX_BOTTOM) {
            InstanceBBox bbox = Collision_computeBBox(runner->dataWin, inst);
            if (bbox.valid) {
                if (builtinVarId == BUILTIN_VAR_BBOX_LEFT) return RValue_makeReal(bbox.left);
                if (builtinVarId == BUILTIN_VAR_BBOX_RIGHT) return RValue_makeReal(bbox.right);
                if (builtinVarId == BUILTIN_VAR_BBOX_TOP) return RValue_makeReal(bbox.top);
                return RValue_makeReal(bbox.bottom);
            }
            if (builtinVarId == BUILTIN_VAR_BBOX_LEFT || builtinVarId == BUILTIN_VAR_BBOX_RIGHT) return RValue_makeReal(inst->x);
            return RValue_makeReal(inst->y);
        }
        if (builtinVarId == BUILTIN_VAR_VISIBLE) return RValue_makeBool(inst->visible);
        if (builtinVarId == BUILTIN_VAR_DEPTH) return RValue_makeReal((GMLReal) inst->depth);
        if (builtinVarId == BUILTIN_VAR_X) return RValue_makeReal(inst->x);
        if (builtinVarId == BUILTIN_VAR_Y) return RValue_makeReal(inst->y);
        if (builtinVarId == BUILTIN_VAR_XPREVIOUS) return RValue_makeReal(inst->xprevious);
        if (builtinVarId == BUILTIN_VAR_YPREVIOUS) return RValue_makeReal(inst->yprevious);
        if (builtinVarId == BUILTIN_VAR_XSTART) return RValue_makeReal(inst->xstart);
        if (builtinVarId == BUILTIN_VAR_YSTART) return RValue_makeReal(inst->ystart);
        if (builtinVarId == BUILTIN_VAR_MASK_INDEX) return RValue_makeReal((GMLReal) inst->maskIndex);
        if (builtinVarId == BUILTIN_VAR_ID) return RValue_makeReal((GMLReal) inst->instanceId);
        if (builtinVarId == BUILTIN_VAR_OBJECT_INDEX) return RValue_makeReal((GMLReal) inst->objectIndex);
        if (builtinVarId == BUILTIN_VAR_PERSISTENT) return RValue_makeBool(inst->persistent);
        if (builtinVarId == BUILTIN_VAR_SOLID) return RValue_makeBool(inst->solid);
        if (builtinVarId == BUILTIN_VAR_SPEED) return RValue_makeReal(inst->speed);
        if (builtinVarId == BUILTIN_VAR_DIRECTION) return RValue_makeReal(inst->direction);
        if (builtinVarId == BUILTIN_VAR_HSPEED) return RValue_makeReal(inst->hspeed);
        if (builtinVarId == BUILTIN_VAR_VSPEED) return RValue_makeReal(inst->vspeed);
        if (builtinVarId == BUILTIN_VAR_FRICTION) return RValue_makeReal(inst->friction);
        if (builtinVarId == BUILTIN_VAR_GRAVITY) return RValue_makeReal(inst->gravity);
        if (builtinVarId == BUILTIN_VAR_GRAVITY_DIRECTION) return RValue_makeReal(inst->gravityDirection);
        if (builtinVarId == BUILTIN_VAR_ALARM) {
            if (isValidAlarmIndex(arrayIndex)) {
                return RValue_makeReal((GMLReal) inst->alarm[arrayIndex]);
            }
            return RValue_makeReal(-1.0);
        }

        // Path instance variables
        if (builtinVarId == BUILTIN_VAR_PATH_INDEX) return RValue_makeReal((GMLReal) inst->pathIndex);
        if (builtinVarId == BUILTIN_VAR_PATH_POSITION) return RValue_makeReal(inst->pathPosition);
        if (builtinVarId == BUILTIN_VAR_PATH_POSITIONPREVIOUS) return RValue_makeReal(inst->pathPositionPrevious);
        if (builtinVarId == BUILTIN_VAR_PATH_SPEED) return RValue_makeReal(inst->pathSpeed);
        if (builtinVarId == BUILTIN_VAR_PATH_SCALE) return RValue_makeReal(inst->pathScale);
        if (builtinVarId == BUILTIN_VAR_PATH_ORIENTATION) return RValue_makeReal(inst->pathOrientation);
        if (builtinVarId == BUILTIN_VAR_PATH_ENDACTION) return RValue_makeReal((GMLReal) inst->pathEndAction);
    }

    // Room properties
    if (builtinVarId == BUILTIN_VAR_ROOM) return RValue_makeReal((GMLReal) runner->currentRoomIndex);
    if (builtinVarId == BUILTIN_VAR_ROOM_FIRST) return RValue_makeReal((GMLReal) runner->dataWin->gen8.roomOrder[0]);
    if (builtinVarId == BUILTIN_VAR_ROOM_SPEED) return RValue_makeReal((GMLReal) runner->currentRoom->speed);
    if (builtinVarId == BUILTIN_VAR_ROOM_WIDTH) return RValue_makeReal((GMLReal) runner->currentRoom->width);
    if (builtinVarId == BUILTIN_VAR_ROOM_HEIGHT) return RValue_makeReal((GMLReal) runner->currentRoom->height);
    if (builtinVarId == BUILTIN_VAR_ROOM_PERSISTENT) return RValue_makeBool(runner->currentRoom->persistent);
    if (builtinVarId == BUILTIN_VAR_VIEW_CURRENT) return RValue_makeReal((GMLReal) runner->viewCurrent);
    if (builtinVarId == BUILTIN_VAR_VIEW_XVIEW) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].viewX);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_YVIEW) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].viewY);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_WVIEW) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].viewWidth);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_HVIEW) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].viewHeight);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_XPORT) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].portX);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_YPORT) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].portY);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_WPORT) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].portWidth);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_HPORT) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].portHeight);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_VISIBLE) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeBool(runner->currentRoom->views[arrayIndex].enabled);
        return RValue_makeBool(false);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_ANGLE) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->viewAngles[arrayIndex]);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_HBORDER) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].borderX);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_VBORDER) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].borderY);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_OBJECT) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].objectId);
        return RValue_makeReal(-4.0);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_HSPEED) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].speedX);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_VIEW_VSPEED) {
        if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) return RValue_makeReal((GMLReal) runner->currentRoom->views[arrayIndex].speedY);
        return RValue_makeReal(0.0);
    }

    // Background properties
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_VISIBLE) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeBool(runner->backgrounds[arrayIndex].visible);
        return RValue_makeBool(false);
    }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_INDEX) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeReal((GMLReal) runner->backgrounds[arrayIndex].backgroundIndex);
        return RValue_makeReal(-1.0);
    }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_X) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeReal((GMLReal) runner->backgrounds[arrayIndex].x);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_Y) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeReal((GMLReal) runner->backgrounds[arrayIndex].y);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_HSPEED) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeReal((GMLReal) runner->backgrounds[arrayIndex].speedX);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_VSPEED) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeReal((GMLReal) runner->backgrounds[arrayIndex].speedY);
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_WIDTH) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) {
            int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(runner->dataWin, runner->backgrounds[arrayIndex].backgroundIndex);
            if (tpagIndex >= 0) return RValue_makeReal((GMLReal) runner->dataWin->tpag.items[tpagIndex].boundingWidth);
        }
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_HEIGHT) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) {
            int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(runner->dataWin, runner->backgrounds[arrayIndex].backgroundIndex);
            if (tpagIndex >= 0) return RValue_makeReal((GMLReal) runner->dataWin->tpag.items[tpagIndex].boundingHeight);
        }
        return RValue_makeReal(0.0);
    }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_ALPHA) {
        if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) return RValue_makeReal((GMLReal) runner->backgrounds[arrayIndex].alpha);
        return RValue_makeReal(1.0);
    }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_COLOR || builtinVarId == BUILTIN_VAR_BACKGROUND_COLOUR) {
        return RValue_makeReal((GMLReal) runner->backgroundColor);
    }

    // Timing
    if (builtinVarId == BUILTIN_VAR_CURRENT_TIME) {
        #ifdef _WIN32
        LARGE_INTEGER freq, counter;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&counter);
        GMLReal ms = (GMLReal) counter.QuadPart / (GMLReal) freq.QuadPart * 1000.0;
        #else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        GMLReal ms = (GMLReal) ts.tv_sec * 1000.0 + (GMLReal) ts.tv_nsec / 1000000.0;
        #endif
        return RValue_makeReal(ms);
    }

    // argument_count
    if (builtinVarId == BUILTIN_VAR_ARGUMENT_COUNT) return RValue_makeReal((GMLReal) ctx->scriptArgCount);

    // argument[N] - array-style access to script arguments
    if (builtinVarId == BUILTIN_VAR_ARGUMENT) {
        if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > arrayIndex && arrayIndex >= 0) {
            RValue val = ctx->scriptArgs[arrayIndex];
            val.ownsString = false;
            return val;
        }
        return RValue_makeUndefined();
    }

    // Argument variables (argument0..argument15)
    if (builtinVarId >= BUILTIN_VAR_ARGUMENT0 && BUILTIN_VAR_ARGUMENT15 >= builtinVarId) {
        int argNumber = builtinVarId - BUILTIN_VAR_ARGUMENT0;
        if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > argNumber) {
            RValue val = ctx->scriptArgs[argNumber];
            val.ownsString = false;
            return val;
        }
        return RValue_makeUndefined();
    }

    // Keyboard variables
    if (builtinVarId == BUILTIN_VAR_KEYBOARD_KEY) return RValue_makeReal((GMLReal) runner->keyboard->lastKey);
    if (builtinVarId == BUILTIN_VAR_KEYBOARD_LASTCHAR) return RValue_makeString(runner->keyboard->lastChar);
    if (builtinVarId == BUILTIN_VAR_KEYBOARD_LASTKEY) return RValue_makeReal((GMLReal) runner->keyboard->lastKey);

    // Surfaces
    if (builtinVarId == BUILTIN_VAR_APPLICATION_SURFACE) return RValue_makeReal(-1.0); // sentinel ID for the application surface

    // Constants that GMS defines
    if (builtinVarId == BUILTIN_VAR_TRUE) return RValue_makeBool(true);
    if (builtinVarId == BUILTIN_VAR_FALSE) return RValue_makeBool(false);
    if (builtinVarId == BUILTIN_VAR_PI) return RValue_makeReal(3.14159265358979323846);
    if (builtinVarId == BUILTIN_VAR_UNDEFINED) return RValue_makeUndefined();

    // Path action constants
    if (builtinVarId == BUILTIN_VAR_PATH_ACTION_STOP) return RValue_makeReal(0.0);
    if (builtinVarId == BUILTIN_VAR_PATH_ACTION_RESTART) return RValue_makeReal(1.0);
    if (builtinVarId == BUILTIN_VAR_PATH_ACTION_CONTINUE) return RValue_makeReal(2.0);
    if (builtinVarId == BUILTIN_VAR_PATH_ACTION_REVERSE) return RValue_makeReal(3.0);

    // Buffer type constants
    if (builtinVarId == BUILTIN_VAR_BUFFER_FIXED) return RValue_makeReal(GML_BUFFER_FIXED);
    if (builtinVarId == BUILTIN_VAR_BUFFER_GROW) return RValue_makeReal(GML_BUFFER_GROW);
    if (builtinVarId == BUILTIN_VAR_BUFFER_WRAP) return RValue_makeReal(GML_BUFFER_WRAP);
    if (builtinVarId == BUILTIN_VAR_BUFFER_FAST) return RValue_makeReal(GML_BUFFER_FAST);

    // Buffer data type constants
    if (builtinVarId == BUILTIN_VAR_BUFFER_U8) return RValue_makeReal(GML_BUFTYPE_U8);
    if (builtinVarId == BUILTIN_VAR_BUFFER_S8) return RValue_makeReal(GML_BUFTYPE_S8);
    if (builtinVarId == BUILTIN_VAR_BUFFER_U16) return RValue_makeReal(GML_BUFTYPE_U16);
    if (builtinVarId == BUILTIN_VAR_BUFFER_S16) return RValue_makeReal(GML_BUFTYPE_S16);
    if (builtinVarId == BUILTIN_VAR_BUFFER_U32) return RValue_makeReal(GML_BUFTYPE_U32);
    if (builtinVarId == BUILTIN_VAR_BUFFER_S32) return RValue_makeReal(GML_BUFTYPE_S32);
    if (builtinVarId == BUILTIN_VAR_BUFFER_F16) return RValue_makeReal(GML_BUFTYPE_F16);
    if (builtinVarId == BUILTIN_VAR_BUFFER_F32) return RValue_makeReal(GML_BUFTYPE_F32);
    if (builtinVarId == BUILTIN_VAR_BUFFER_F64) return RValue_makeReal(GML_BUFTYPE_F64);
    if (builtinVarId == BUILTIN_VAR_BUFFER_BOOL) return RValue_makeReal(GML_BUFTYPE_BOOL);
    if (builtinVarId == BUILTIN_VAR_BUFFER_STRING) return RValue_makeReal(GML_BUFTYPE_STRING);
    if (builtinVarId == BUILTIN_VAR_BUFFER_U64) return RValue_makeReal(GML_BUFTYPE_U64);
    if (builtinVarId == BUILTIN_VAR_BUFFER_TEXT) return RValue_makeReal(GML_BUFTYPE_TEXT);

    // Buffer seek mode constants
    if (builtinVarId == BUILTIN_VAR_BUFFER_SEEK_START) return RValue_makeReal(GML_BUFFER_SEEK_START);
    if (builtinVarId == BUILTIN_VAR_BUFFER_SEEK_RELATIVE) return RValue_makeReal(GML_BUFFER_SEEK_RELATIVE);
    if (builtinVarId == BUILTIN_VAR_BUFFER_SEEK_END) return RValue_makeReal(GML_BUFFER_SEEK_END);

    if (builtinVarId == BUILTIN_VAR_FPS) return RValue_makeReal(ctx->dataWin->gen8.gms2FPS);
    if (builtinVarId == BUILTIN_VAR_DEBUG_MODE) return RValue_makeBool(false);

    fprintf(stderr, "VM: Unhandled built-in variable read '%s' (arrayIndex=%d)\n", name, arrayIndex);
    return RValue_makeReal(0.0);
}

void VMBuiltins_setVariable(VMContext* ctx, int16_t builtinVarId, const char* name, RValue val, int32_t arrayIndex) {
    Instance* inst = (Instance*) ctx->currentInstance;
    Runner* runner = (Runner*) requireNotNullMessage(ctx->runner, "VM: setVariable called but no runner!");
    requireNotNull(runner);

    // Per-instance properties
    if (inst != nullptr) {
        if (builtinVarId == BUILTIN_VAR_IMAGE_SPEED) { inst->imageSpeed = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_IMAGE_INDEX) { inst->imageIndex = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_IMAGE_XSCALE) { inst->imageXscale = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_IMAGE_YSCALE) { inst->imageYscale = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_IMAGE_ANGLE) { inst->imageAngle = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_IMAGE_ALPHA) { inst->imageAlpha = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_IMAGE_BLEND) { inst->imageBlend = (uint32_t) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_SPRITE_INDEX) { inst->spriteIndex = RValue_toInt32(val); return; }
        if (builtinVarId == BUILTIN_VAR_VISIBLE) { inst->visible = RValue_toBool(val); return; }
        if (builtinVarId == BUILTIN_VAR_DEPTH) { inst->depth = RValue_toInt32(val); return; }
        if (builtinVarId == BUILTIN_VAR_X) { inst->x = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_Y) { inst->y = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_PERSISTENT) { inst->persistent = RValue_toBool(val); return; }
        if (builtinVarId == BUILTIN_VAR_SOLID) { inst->solid = RValue_toBool(val); return; }
        if (builtinVarId == BUILTIN_VAR_XPREVIOUS) { inst->xprevious = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_YPREVIOUS) { inst->yprevious = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_XSTART) { inst->xstart = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_YSTART) { inst->ystart = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_MASK_INDEX) { inst->maskIndex = RValue_toInt32(val); return; }
        if (builtinVarId == BUILTIN_VAR_SPEED) { inst->speed = (float) RValue_toReal(val); Instance_computeComponentsFromSpeed(inst); return; }
        if (builtinVarId == BUILTIN_VAR_DIRECTION) {
            GMLReal d = GMLReal_fmod(RValue_toReal(val), 360.0);
            if (d < 0.0) d += 360.0;
            inst->direction = (float) d;
            Instance_computeComponentsFromSpeed(inst);
            return;
        }
        if (builtinVarId == BUILTIN_VAR_HSPEED) { inst->hspeed = (float) RValue_toReal(val); Instance_computeSpeedFromComponents(inst); return; }
        if (builtinVarId == BUILTIN_VAR_VSPEED) { inst->vspeed = (float) RValue_toReal(val); Instance_computeSpeedFromComponents(inst); return; }
        if (builtinVarId == BUILTIN_VAR_FRICTION) { inst->friction = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_GRAVITY) { inst->gravity = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_GRAVITY_DIRECTION) { inst->gravityDirection = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_ALARM) {
            if (isValidAlarmIndex(arrayIndex)) {
                int32_t newValue = RValue_toInt32(val);

#ifndef DISABLE_VM_TRACING
                if (shgeti(ctx->alarmsToBeTraced, "*") != -1 || shgeti(ctx->alarmsToBeTraced, runner->dataWin->objt.objects[inst->objectIndex].name) != -1) {
                    fprintf(stderr, "VM: [%s] Setting Alarm[%d] = %d (instanceId=%d)\n", runner->dataWin->objt.objects[inst->objectIndex].name, arrayIndex, newValue, inst->instanceId);
                }
#endif

                inst->alarm[arrayIndex] = newValue;
            }
            return;
        }

        // Path instance variables (writable)
        if (builtinVarId == BUILTIN_VAR_PATH_POSITION) {
            // Native GMS runner clamps path_position to [0.0, 1.0] on set
            float pos = (float) RValue_toReal(val);
            if (pos < 0.0f) pos = 0.0f;
            else if (pos > 1.0f) pos = 1.0f;
            inst->pathPosition = pos;
            return;
        }
        if (builtinVarId == BUILTIN_VAR_PATH_SPEED) { inst->pathSpeed = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_PATH_SCALE) { inst->pathScale = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_PATH_ORIENTATION) { inst->pathOrientation = (float) RValue_toReal(val); return; }
        if (builtinVarId == BUILTIN_VAR_PATH_ENDACTION) { inst->pathEndAction = RValue_toInt32(val); return; }
    }

    // Keyboard variables
    if (builtinVarId == BUILTIN_VAR_KEYBOARD_KEY) {
        runner->keyboard->lastKey = RValue_toInt32(val);
        return;
    }
    if (builtinVarId == BUILTIN_VAR_KEYBOARD_LASTCHAR) {
        runner->keyboard->lastChar[0] = val.string[0];
        return;
    }
    if (builtinVarId == BUILTIN_VAR_KEYBOARD_LASTKEY) {
        runner->keyboard->lastKey = RValue_toInt32(val);
        return;
    }

    // View properties
    if (builtinVarId == BUILTIN_VAR_VIEW_XVIEW) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].viewX = RValue_toInt32(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_YVIEW) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].viewY = RValue_toInt32(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_WVIEW) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].viewWidth = RValue_toInt32(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_HVIEW) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].viewHeight = RValue_toInt32(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_XPORT) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].portX = RValue_toInt32(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_YPORT) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].portY = RValue_toInt32(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_WPORT) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].portWidth = RValue_toInt32(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_HPORT) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].portHeight = RValue_toInt32(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_VISIBLE) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].enabled = RValue_toBool(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_ANGLE) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->viewAngles[arrayIndex] = (float) RValue_toReal(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_HBORDER) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].borderX = RValue_toInt32(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_VBORDER) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].borderY = RValue_toInt32(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_OBJECT) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].objectId = RValue_toInt32(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_HSPEED) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].speedX = RValue_toInt32(val); } return; }
    if (builtinVarId == BUILTIN_VAR_VIEW_VSPEED) { if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { runner->currentRoom->views[arrayIndex].speedY = RValue_toInt32(val); } return; }

    // Background properties
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_VISIBLE) { if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].visible = RValue_toBool(val); return; }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_INDEX) { if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].backgroundIndex = RValue_toInt32(val); return; }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_X) { if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].x = (float) RValue_toReal(val); return; }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_Y) { if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].y = (float) RValue_toReal(val); return; }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_HSPEED) { if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].speedX = (float) RValue_toReal(val); return; }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_VSPEED) { if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].speedY = (float) RValue_toReal(val); return; }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_ALPHA) { if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) runner->backgrounds[arrayIndex].alpha = (float) RValue_toReal(val); return; }
    if (builtinVarId == BUILTIN_VAR_BACKGROUND_COLOR || builtinVarId == BUILTIN_VAR_BACKGROUND_COLOUR) {
        runner->backgroundColor = (uint32_t) RValue_toInt32(val);
        return;
    }

    // Room properties
    if (builtinVarId == BUILTIN_VAR_ROOM) { runner->pendingRoom = RValue_toInt32(val); return; }
    if (builtinVarId == BUILTIN_VAR_ROOM_PERSISTENT) { runner->currentRoom->persistent = RValue_toBool(val); return; }
    if (builtinVarId == BUILTIN_VAR_ROOM_WIDTH) { runner->currentRoom->width = (uint32_t) RValue_toInt32(val); return; }
    if (builtinVarId == BUILTIN_VAR_ROOM_HEIGHT) { runner->currentRoom->height = (uint32_t) RValue_toInt32(val); return; }
    if (builtinVarId == BUILTIN_VAR_ROOM_SPEED) { runner->currentRoom->speed = (uint32_t) RValue_toInt32(val); return; }

    // Read-only variables (silently ignore)
    if ((builtinVarId >= BUILTIN_VAR_OS_TYPE && BUILTIN_VAR_OS_LLVM_WINPHONE >= builtinVarId) ||
        builtinVarId == BUILTIN_VAR_ID || builtinVarId == BUILTIN_VAR_OBJECT_INDEX ||
        builtinVarId == BUILTIN_VAR_CURRENT_TIME ||
        builtinVarId == BUILTIN_VAR_VIEW_CURRENT || builtinVarId == BUILTIN_VAR_PATH_INDEX ||
        builtinVarId == BUILTIN_VAR_DEBUG_MODE || builtinVarId == BUILTIN_VAR_ROOM_FIRST ||
        (builtinVarId >= BUILTIN_VAR_BUFFER_FIXED && BUILTIN_VAR_BUFFER_SEEK_END >= builtinVarId)) {
        fprintf(stderr, "VM: Warning - attempted write to read-only built-in '%s'\n", name);
        return;
    }

    // argument[N] - array-style write to script arguments
    if (builtinVarId == BUILTIN_VAR_ARGUMENT) {
        if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > arrayIndex && arrayIndex >= 0) {
            RValue_free(&ctx->scriptArgs[arrayIndex]);
            ctx->scriptArgs[arrayIndex] = val;
        }
        return;
    }

    // Argument variables
    if (builtinVarId >= BUILTIN_VAR_ARGUMENT0 && BUILTIN_VAR_ARGUMENT15 >= builtinVarId) {
        int argNumber = builtinVarId - BUILTIN_VAR_ARGUMENT0;
        if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > argNumber) {
            RValue_free(&ctx->scriptArgs[argNumber]);
            ctx->scriptArgs[argNumber] = val;
        }
        return;
    }

    fprintf(stderr, "VM: Unhandled built-in variable write '%s' (arrayIndex=%d)\n", name, arrayIndex);
}

// ===[ BUILTIN FUNCTION IMPLEMENTATIONS ]===

static RValue builtinShowDebugMessage(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) {
        fprintf(stderr, "[show_debug_message] Expected at least 1 argument\n");
        return RValue_makeUndefined();
    }

    char* val = RValue_toString(args[0]);
    printf("Game: %s\n", val);
    free(val);

    return RValue_makeUndefined();
}

static RValue builtinStringLength(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeInt32(0);
    // GML converts non-string arguments to string before measuring length
    RValue value = args[0];
    // Fast path: If the RValue is already a string, just return its length instead of creating a copy
    if (value.type == RVALUE_STRING) {
        if (value.string == nullptr)
            return RValue_makeInt32(0);
        return RValue_makeInt32((int32_t) strlen(value.string));
    }
    char* str = RValue_toString(value);
    int32_t len = (int32_t) strlen(str);
    free(str);
    return RValue_makeInt32(len);
}

static RValue builtinReal(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(RValue_toReal(args[0]));
}

static RValue builtinString(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeOwnedString(safeStrdup(""));
    char* result = RValue_toString(args[0]);
    return RValue_makeOwnedString(result);
}

static RValue builtinFloor(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_floor(RValue_toReal(args[0])));
}

static RValue builtinCeil(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_ceil(RValue_toReal(args[0])));
}

static RValue builtinRound(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_round(RValue_toReal(args[0])));
}

static RValue builtinAbs(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_fabs(RValue_toReal(args[0])));
}

static RValue builtinSign(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    GMLReal val = RValue_toReal(args[0]);
    GMLReal result = (val > 0.0) ? 1.0 : ((0.0 > val) ? -1.0 : 0.0);
    return RValue_makeReal(result);
}

static RValue builtinMax(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    GMLReal result = -INFINITY;
    repeat(argCount, i) {
        GMLReal val = RValue_toReal(args[i]);
        if (val > result) result = val;
    }
    return RValue_makeReal(result);
}

static RValue builtinMin(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    GMLReal result = INFINITY;
    repeat(argCount, i) {
        GMLReal val = RValue_toReal(args[i]);
        if (result > val) result = val;
    }
    return RValue_makeReal(result);
}

static RValue builtinPower(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_pow(RValue_toReal(args[0]), RValue_toReal(args[1])));
}

static RValue builtinSqrt(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_sqrt(RValue_toReal(args[0])));
}

static RValue builtinSqr(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    GMLReal val = RValue_toReal(args[0]);
    return RValue_makeReal(val * val);
}

static RValue builtinIsString(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    return RValue_makeBool(args[0].type == RVALUE_STRING);
}

static RValue builtinIsReal(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    bool result = args[0].type == RVALUE_REAL || args[0].type == RVALUE_INT32 || args[0].type == RVALUE_INT64 || args[0].type == RVALUE_BOOL;
    return RValue_makeBool(result);
}

static RValue builtinIsUndefined(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(true);
    return RValue_makeBool(args[0].type == RVALUE_UNDEFINED);
}

// ===[ STRING FUNCTIONS ]===

static RValue builtinStringUpper(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    char* result = safeStrdup(args[0].string != nullptr ? args[0].string : "");
    for (char* p = result; *p; p++) *p = (char) toupper((unsigned char) *p);
    return RValue_makeOwnedString(result);
}

static RValue builtinStringLower(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    char* result = safeStrdup(args[0].string != nullptr ? args[0].string : "");
    for (char* p = result; *p; p++) *p = (char) tolower((unsigned char) *p);
    return RValue_makeOwnedString(result);
}

static RValue builtinStringCopy(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    const char* str = args[0].string != nullptr ? args[0].string : "";
    int32_t pos = RValue_toInt32(args[1]) - 1; // GMS is 1-based
    int32_t len = RValue_toInt32(args[2]);
    int32_t strLen = (int32_t) strlen(str);

    if (0 > pos) pos = 0;
    if (pos >= strLen || 0 >= len) return RValue_makeOwnedString(safeStrdup(""));
    if (pos + len > strLen) len = strLen - pos;

    char* result = safeMalloc(len + 1);
    memcpy(result, str + pos, len);
    result[len] = '\0';
    return RValue_makeOwnedString(result);
}

static RValue builtinStringRepeat(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    const char* str = args[0].string != nullptr ? args[0].string : "";
    int32_t count = RValue_toInt32(args[1]);
    if (0 >= count || str[0] == '\0') return RValue_makeOwnedString(safeStrdup(""));

    size_t strLen = strlen(str);
    size_t totalLen = strLen * (size_t) count;
    char* result = safeMalloc(totalLen + 1);
    repeat(count, i) {
        memcpy(result + i * strLen, str, strLen);
    }
    result[totalLen] = '\0';
    return RValue_makeOwnedString(result);
}

static RValue builtinOrd(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount || args[0].type != RVALUE_STRING || args[0].string == nullptr || args[0].string[0] == '\0') {
        return RValue_makeReal(0.0);
    }
    return RValue_makeReal((GMLReal) (unsigned char) args[0].string[0]);
}

static RValue builtinChr(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeOwnedString(safeStrdup(""));
    char buf[2] = { (char) RValue_toInt32(args[0]), '\0' };
    return RValue_makeOwnedString(safeStrdup(buf));
}

static RValue builtinStringPos(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount || args[0].type != RVALUE_STRING || args[1].type != RVALUE_STRING) return RValue_makeReal(0.0);
    const char* needle = args[0].string != nullptr ? args[0].string : "";
    const char* haystack = args[1].string != nullptr ? args[1].string : "";
    const char* found = strstr(haystack, needle);
    if (found == nullptr) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) (found - haystack + 1)); // 1-based
}

static RValue builtinStringCharAt(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    const char* str = args[0].string != nullptr ? args[0].string : "";
    int32_t pos = RValue_toInt32(args[1]) - 1; // 1-based
    int32_t strLen = (int32_t) strlen(str);
    if (0 > pos || pos >= strLen) return RValue_makeOwnedString(safeStrdup(""));
    char buf[2] = { str[pos], '\0' };
    return RValue_makeOwnedString(safeStrdup(buf));
}

static RValue builtinStringDelete(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    const char* str = args[0].string != nullptr ? args[0].string : "";
    int32_t pos = RValue_toInt32(args[1]) - 1; // 1-based
    int32_t count = RValue_toInt32(args[2]);
    int32_t strLen = (int32_t) strlen(str);

    if (0 > pos || pos >= strLen || 0 >= count) return RValue_makeOwnedString(safeStrdup(str));
    if (pos + count > strLen) count = strLen - pos;

    char* result = safeMalloc(strLen - count + 1);
    memcpy(result, str, pos);
    memcpy(result + pos, str + pos + count, strLen - pos - count);
    result[strLen - count] = '\0';
    return RValue_makeOwnedString(result);
}

static RValue builtinStringInsert(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount || args[0].type != RVALUE_STRING || args[1].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    const char* substr = args[0].string != nullptr ? args[0].string : "";
    const char* str = args[1].string != nullptr ? args[1].string : "";
    int32_t pos = RValue_toInt32(args[2]) - 1; // 1-based
    int32_t strLen = (int32_t) strlen(str);
    int32_t subLen = (int32_t) strlen(substr);

    if (0 > pos) pos = 0;
    if (pos > strLen) pos = strLen;

    char* result = safeMalloc(strLen + subLen + 1);
    memcpy(result, str, pos);
    memcpy(result + pos, substr, subLen);
    memcpy(result + pos + subLen, str + pos, strLen - pos);
    result[strLen + subLen] = '\0';
    return RValue_makeOwnedString(result);
}

static RValue builtinStringReplaceAll(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount || args[0].type != RVALUE_STRING || args[1].type != RVALUE_STRING || args[2].type != RVALUE_STRING) return RValue_makeOwnedString(safeStrdup(""));
    const char* str = args[0].string != nullptr ? args[0].string : "";
    const char* needle = args[1].string != nullptr ? args[1].string : "";
    const char* replacement = args[2].string != nullptr ? args[2].string : "";
    int32_t needleLen = (int32_t) strlen(needle);
    if (0 == needleLen) return RValue_makeOwnedString(safeStrdup(str));
    int32_t replacementLen = (int32_t) strlen(replacement);

    // Count occurrences to pre-allocate
    int32_t count = 0;
    const char* p = str;
    while ((p = strstr(p, needle)) != nullptr) { count++; p += needleLen; }

    int32_t strLen = (int32_t) strlen(str);
    int32_t resultLen = strLen + count * (replacementLen - needleLen);
    char* result = safeMalloc(resultLen + 1);
    char* out = result;
    p = str;
    const char* match;
    while ((match = strstr(p, needle)) != nullptr) {
        int32_t before = (int32_t) (match - p);
        memcpy(out, p, before);
        out += before;
        memcpy(out, replacement, replacementLen);
        out += replacementLen;
        p = match + needleLen;
    }
    strcpy(out, p);
    return RValue_makeOwnedString(result);
}

// ===[ MATH FUNCTIONS ]===

static RValue builtinDarctan2(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    GMLReal y = RValue_toReal(args[0]);
    GMLReal x = RValue_toReal(args[1]);
    return RValue_makeReal(GMLReal_atan2(y, x) * (180.0 / M_PI));
}

static RValue builtinSin(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_sin(RValue_toReal(args[0])));
}

static RValue builtinArcsin(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_asin(RValue_toReal(args[0])));
}

static RValue builtinCos(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_cos(RValue_toReal(args[0])));
}

static RValue builtinDsin(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_sin(RValue_toReal(args[0]) * (M_PI / 180.0)));
}

static RValue builtinDcos(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(GMLReal_cos(RValue_toReal(args[0]) * (M_PI / 180.0)));
}

static RValue builtinDegtorad(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(RValue_toReal(args[0]) * (M_PI / 180.0));
}

static RValue builtinRadtodeg(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(RValue_toReal(args[0]) * (180.0 / M_PI));
}

static RValue builtinClamp(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal(0.0);
    GMLReal val = RValue_toReal(args[0]);
    GMLReal lo = RValue_toReal(args[1]);
    GMLReal hi = RValue_toReal(args[2]);
    if (lo > val) val = lo;
    if (val > hi) val = hi;
    return RValue_makeReal(val);
}

static RValue builtinLerp(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal(0.0);
    GMLReal a = RValue_toReal(args[0]);
    GMLReal b = RValue_toReal(args[1]);
    GMLReal t = RValue_toReal(args[2]);
    return RValue_makeReal(a + (b - a) * t);
}

static RValue builtinPointDistance(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (4 > argCount) return RValue_makeReal(0.0);
    GMLReal dx = RValue_toReal(args[2]) - RValue_toReal(args[0]);
    GMLReal dy = RValue_toReal(args[3]) - RValue_toReal(args[1]);
    return RValue_makeReal(GMLReal_sqrt(dx * dx + dy * dy));
}

static RValue builtinDistanceToPoint(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    GMLReal px = RValue_toReal(args[0]);
    GMLReal py = RValue_toReal(args[1]);

    Instance* inst = ctx->currentInstance;
    int32_t sprIdx = (inst->maskIndex >= 0) ? inst->maskIndex : inst->spriteIndex;

    // Compute bounding box
    GMLReal bboxLeft, bboxRight, bboxTop, bboxBottom;
    if (0 > sprIdx || (uint32_t) sprIdx >= ctx->dataWin->sprt.count) {
        // No sprite/mask: treat bbox as a single point at (x, y)
        bboxLeft = inst->x;
        bboxRight = inst->x;
        bboxTop = inst->y;
        bboxBottom = inst->y;
    } else {
        Sprite* spr = &ctx->dataWin->sprt.sprites[sprIdx];
        bboxLeft = inst->x + inst->imageXscale * (spr->marginLeft - spr->originX);
        bboxRight = inst->x + inst->imageXscale * ((spr->marginRight + 1) - spr->originX);
        if (bboxLeft > bboxRight) {
            GMLReal t = bboxLeft;
            bboxLeft = bboxRight;
            bboxRight = t;
        }
        bboxTop = inst->y + inst->imageYscale * (spr->marginTop - spr->originY);
        bboxBottom = inst->y + inst->imageYscale * ((spr->marginBottom + 1) - spr->originY);
        if (bboxTop > bboxBottom) {
            GMLReal t = bboxTop;
            bboxTop = bboxBottom;
            bboxBottom = t;
        }
    }

    // Distance from point to nearest edge of bbox (0 if inside)
    GMLReal xd = 0.0;
    GMLReal yd = 0.0;
    if (px > bboxRight)  xd = px - bboxRight;
    if (px < bboxLeft)   xd = px - bboxLeft;
    if (py > bboxBottom) yd = py - bboxBottom;
    if (py < bboxTop)    yd = py - bboxTop;

    return RValue_makeReal(GMLReal_sqrt(xd * xd + yd * yd));
}

// distance_to_object(obj)
// Returns the minimum bbox-to-bbox distance between the calling instance and the nearest instance of the given object.
static RValue builtinDistanceToObject(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);

    Runner* runner = (Runner*) ctx->runner;
    int32_t targetObjIndex = RValue_toInt32(args[0]);
    Instance* self = ctx->currentInstance;

    // Compute self bbox
    Sprite* selfSpr = Collision_getSprite(ctx->dataWin, self);
    if (selfSpr == nullptr) return RValue_makeReal(0.0);
    InstanceBBox selfBBox = Collision_computeBBox(ctx->dataWin, self);
    if (!selfBBox.valid) return RValue_makeReal(0.0);

    GMLReal minDist = 10000000000.0;
    int32_t count = (int32_t) arrlen(runner->instances);

    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active || inst == self) continue;
        if (!Collision_matchesTarget(ctx->dataWin, inst, targetObjIndex)) continue;

        InstanceBBox otherBBox = Collision_computeBBox(ctx->dataWin, inst);
        if (!otherBBox.valid) continue;

        GMLReal xd = 0.0;
        GMLReal yd = 0.0;
        if (otherBBox.left > selfBBox.right)  xd = otherBBox.left - selfBBox.right;
        if (selfBBox.left > otherBBox.right)  xd = selfBBox.left - otherBBox.right;
        if (otherBBox.top > selfBBox.bottom)  yd = otherBBox.top - selfBBox.bottom;
        if (selfBBox.top > otherBBox.bottom)  yd = selfBBox.top - otherBBox.bottom;

        GMLReal dist = GMLReal_sqrt(xd * xd + yd * yd);
        if (minDist > dist) minDist = dist;
    }

    return RValue_makeReal(minDist);
}

static RValue builtinPointDirection(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (4 > argCount) return RValue_makeReal(0.0);
    GMLReal dx = RValue_toReal(args[2]) - RValue_toReal(args[0]);
    GMLReal dy = RValue_toReal(args[3]) - RValue_toReal(args[1]);
    return RValue_makeReal(GMLReal_atan2(-dy, dx) * (180.0 / M_PI));
}

static RValue builtinAngleDifference(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    GMLReal src = RValue_toReal(args[0]);
    GMLReal dest = RValue_toReal(args[1]);
    return RValue_makeReal(GMLReal_fmod(GMLReal_fmod(src - dest, 360.0) + 540.0, 360.0) - 180.0);
}

static RValue builtinMoveTowardsPoint(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal targetX = RValue_toReal(args[0]);
    GMLReal targetY = RValue_toReal(args[1]);
    GMLReal spd = RValue_toReal(args[2]);
    Instance* inst = ctx->currentInstance;
    GMLReal dx = targetX - inst->x;
    GMLReal dy = targetY - inst->y;
    GMLReal dir = GMLReal_atan2(-dy, dx) * (180.0 / M_PI);
    if (dir < 0.0) dir += 360.0;
    inst->direction = (float) dir;
    inst->speed = (float) spd;
    Instance_computeComponentsFromSpeed(inst);
    return RValue_makeReal(0.0);
}

static RValue builtinMoveSnap(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal hsnap = RValue_toReal(args[0]);
    GMLReal vsnap = RValue_toReal(args[1]);
    Instance* inst = ctx->currentInstance;
    if (hsnap > 0.0) inst->x = (float) (GMLReal_floor((inst->x / hsnap) + 0.5) * hsnap);
    if (vsnap > 0.0) inst->y = (float) (GMLReal_floor((inst->y / vsnap) + 0.5) * vsnap);
    return RValue_makeReal(0.0);
}

static RValue builtinLengthdir_x(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    GMLReal len = RValue_toReal(args[0]);
    GMLReal dir = RValue_toReal(args[1]) * (M_PI / 180.0);
    return RValue_makeReal(len * GMLReal_cos(dir));
}

static RValue builtinLengthdir_y(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    GMLReal len = RValue_toReal(args[0]);
    GMLReal dir = RValue_toReal(args[1]) * (M_PI / 180.0);
    return RValue_makeReal(-len * GMLReal_sin(dir));
}

// ===[ RANDOM FUNCTIONS ]===

static RValue builtinRandom(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    GMLReal n = RValue_toReal(args[0]);
    return RValue_makeReal(((GMLReal) rand() / (GMLReal) RAND_MAX) * n);
}

static RValue builtinRandomRange(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    GMLReal lo = RValue_toReal(args[0]);
    GMLReal hi = RValue_toReal(args[1]);
    return RValue_makeReal(lo + ((GMLReal) rand() / (GMLReal) RAND_MAX) * (hi - lo));
}

static RValue builtinIrandom(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    int32_t n = RValue_toInt32(args[0]);
    if (0 >= n) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) (rand() % (n + 1)));
}

static RValue builtinIrandomRange(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    int32_t lo = RValue_toInt32(args[0]);
    int32_t hi = RValue_toInt32(args[1]);
    if (lo > hi) { int32_t tmp = lo; lo = hi; hi = tmp; }
    int32_t range = hi - lo + 1;
    if (0 >= range) return RValue_makeReal((GMLReal) lo);
    return RValue_makeReal((GMLReal) (lo + rand() % range));
}

static RValue builtinChoose(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t idx = rand() % argCount;
    // Must duplicate the value since args will be freed
    RValue val = args[idx];
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        return RValue_makeOwnedString(safeStrdup(val.string));
    }
    return val;
}

static RValue builtinRandomize(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    if (ctx->hasFixedSeed) return RValue_makeUndefined();
    srand((unsigned int) time(nullptr));
    return RValue_makeUndefined();
}

// ===[ ROOM FUNCTIONS ]===

static RValue builtinGameGetSpeed(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t type = RValue_toInt32(args[0]);
    GMLReal fps = (GMLReal) ctx->runner->currentRoom->speed;
    // gamespeed_fps = 0, gamespeed_microseconds = 1
    if (type == 0) return RValue_makeReal(fps);
    return RValue_makeReal((GMLReal) 1000000.0 / fps);
}

static RValue builtinRoomExists(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t roomId = RValue_toInt32(args[0]);
    return RValue_makeBool(roomId >= 0 && (uint32_t) roomId < ctx->runner->dataWin->room.count);
}

static RValue builtinRoomGetName(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Room* room = &ctx->dataWin->room.rooms[RValue_toInt32(args[0])];
    return RValue_makeOwnedString(safeStrdup(room->name));
}

static RValue builtinRoomGotoNext(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_goto_next called but no runner!");

    // Some games (Undertale) can transition from rooms that are not present in GEN8.roomOrder.
    // In that case currentRoomOrderPosition is -1 and the old logic would jump to roomOrder[0].
    int32_t pos = runner->currentRoomOrderPosition;
    if (pos < 0) {
        // Try to recover by finding the current room in roomOrder.
        repeat(runner->dataWin->gen8.roomOrderCount, i) {
            if (runner->dataWin->gen8.roomOrder[i] == runner->currentRoomIndex) {
                pos = (int32_t) i;
                runner->currentRoomOrderPosition = pos;
                break;
            }
        }
    }

    if (pos >= 0) {
        int32_t nextPos = pos + 1;
        if ((int32_t) runner->dataWin->gen8.roomOrderCount > nextPos) {
            runner->pendingRoom = runner->dataWin->gen8.roomOrder[nextPos];
        } else {
            fprintf(stderr, "VM: room_goto_next - already at last room!\n");
        }
    } else {
        // Last-resort fallback: advance by numeric room index.
        int32_t nextRoom = runner->currentRoomIndex + 1;
        if (nextRoom >= 0 && (uint32_t) nextRoom < runner->dataWin->room.count) {
            runner->pendingRoom = nextRoom;
        } else {
            fprintf(stderr, "VM: room_goto_next - could not resolve next room from index=%d\n", runner->currentRoomIndex);
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinRoomGotoPrevious(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_goto_previous called but no runner!");

    int32_t pos = runner->currentRoomOrderPosition;
    if (pos < 0) {
        repeat(runner->dataWin->gen8.roomOrderCount, i) {
            if (runner->dataWin->gen8.roomOrder[i] == runner->currentRoomIndex) {
                pos = (int32_t) i;
                runner->currentRoomOrderPosition = pos;
                break;
            }
        }
    }

    if (pos >= 0) {
        int32_t previousPos = pos - 1;
        if (previousPos >= 0) {
            runner->pendingRoom = runner->dataWin->gen8.roomOrder[previousPos];
        } else {
            fprintf(stderr, "VM: room_goto_previous - already at first room!\n");
        }
    } else {
        int32_t prevRoom = runner->currentRoomIndex - 1;
        if (prevRoom >= 0 && (uint32_t) prevRoom < runner->dataWin->room.count) {
            runner->pendingRoom = prevRoom;
        } else {
            fprintf(stderr, "VM: room_goto_previous - could not resolve previous room from index=%d\n", runner->currentRoomIndex);
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinRoomGoto(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_goto called but no runner!");
    runner->pendingRoom = RValue_toInt32(args[0]);
    return RValue_makeUndefined();
}

static RValue builtinRoomRestart(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_restart called but no runner!");
    runner->pendingRoom = runner->currentRoomIndex;
    return RValue_makeUndefined();
}

static RValue builtinRoomNext(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_next called but no runner!");
    int32_t roomId = RValue_toInt32(args[0]);
    DataWin* dw = runner->dataWin;
    repeat(dw->gen8.roomOrderCount, i) {
        if (dw->gen8.roomOrder[i] == roomId && dw->gen8.roomOrderCount > i + 1) {
            return RValue_makeReal(dw->gen8.roomOrder[i + 1]);
        }
    }
    return RValue_makeReal(-1);
}

static RValue builtinRoomPrevious(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_previous called but no runner!");
    int32_t roomId = RValue_toInt32(args[0]);
    DataWin* dw = runner->dataWin;
    repeat(dw->gen8.roomOrderCount, i) {
        if (dw->gen8.roomOrder[i] == roomId && i > 0) {
            return RValue_makeReal(dw->gen8.roomOrder[i - 1]);
        }
    }
    return RValue_makeReal(-1);
}

static RValue builtinRoomSetPersistent(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();

    int32_t roomId = RValue_toInt32(args[0]);
    bool persistent = RValue_toBool(args[1]);
    // The HTML5 room_set_persistent does do this (it checks if the room is null)
    if (0 > roomId || (uint32_t) roomId >= ctx->runner->dataWin->room.count) return RValue_makeUndefined();
    ctx->runner->dataWin->room.rooms[roomId].persistent = persistent;

    return RValue_makeUndefined();
}

// GMS2 camera compatibility - we treat view index as camera ID
static RValue builtinViewGetCamera(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1);
    int32_t viewIndex = RValue_toInt32(args[0]);
    if (viewIndex >= 0 && MAX_VIEWS > viewIndex) {
        return RValue_makeReal((double) viewIndex);
    }
    return RValue_makeReal(-1);
}

static RValue builtinCameraGetViewX(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1);
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_get_view_x called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        return RValue_makeReal((double) runner->currentRoom->views[cameraId].viewX);
    }
    return RValue_makeReal(-1);
}

static RValue builtinCameraGetViewY(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1);
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_get_view_y called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        return RValue_makeReal((double) runner->currentRoom->views[cameraId].viewY);
    }
    return RValue_makeReal(-1);
}

static RValue builtinCameraGetViewWidth(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1);
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_get_view_width called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        return RValue_makeReal((double) runner->currentRoom->views[cameraId].viewWidth);
    }
    return RValue_makeReal(-1);
}

static RValue builtinCameraGetViewHeight(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1);
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_get_view_height called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        return RValue_makeReal((double) runner->currentRoom->views[cameraId].viewHeight);
    }
    return RValue_makeReal(-1);
}

static RValue builtinCameraSetViewPos(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1);
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_set_view_pos called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    int32_t x = RValue_toInt32(args[1]);
    int32_t y = RValue_toInt32(args[2]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        runner->currentRoom->views[cameraId].viewX = x;
        runner->currentRoom->views[cameraId].viewY = y;
    }
    return RValue_makeUndefined();
}

static RValue builtinCameraGetViewTarget(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1);
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_get_view_target called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        return RValue_makeReal((double) runner->currentRoom->views[cameraId].objectId);
    }
    return RValue_makeReal(-1);
}

static RValue builtinCameraSetViewTarget(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_set_view_target called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    int32_t objectId = RValue_toInt32(args[1]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        runner->currentRoom->views[cameraId].objectId = objectId;
    }
    return RValue_makeUndefined();
}

static RValue cameraGetViewBorder(VMContext* ctx, RValue* args, int32_t argCount, bool wantY) {
    if (1 > argCount) return RValue_makeReal(-1);
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_get_view_border called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        RoomView* v = &runner->currentRoom->views[cameraId];
        return RValue_makeReal((double) (wantY ? v->borderY : v->borderX));
    }
    return RValue_makeReal(-1);
}

static RValue builtinCameraGetViewBorderX(VMContext* ctx, RValue* args, int32_t argCount) {
    return cameraGetViewBorder(ctx, args, argCount, false);
}

static RValue builtinCameraGetViewBorderY(VMContext* ctx, RValue* args, int32_t argCount) {
    return cameraGetViewBorder(ctx, args, argCount, true);
}

static RValue builtinCameraSetViewBorder(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeUndefined();
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: camera_set_view_border called but no runner!");
    int32_t cameraId = RValue_toInt32(args[0]);
    int32_t bx = RValue_toInt32(args[1]);
    int32_t by = RValue_toInt32(args[2]);
    if (cameraId >= 0 && MAX_VIEWS > cameraId) {
        runner->currentRoom->views[cameraId].borderX = (uint32_t) bx;
        runner->currentRoom->views[cameraId].borderY = (uint32_t) by;
    }
    return RValue_makeUndefined();
}

// ===[ VARIABLE FUNCTIONS ]===

static RValue builtinVariableGlobalExists(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount || args[0].type != RVALUE_STRING) return RValue_makeReal(0.0);
    const char* name = args[0].string;
    ptrdiff_t idx = shgeti(ctx->globalVarNameMap, (char*) name);
    if (0 > idx) return RValue_makeReal(0.0);
    int32_t varID = ctx->globalVarNameMap[idx].value;
    if (ctx->globalVarCount > (uint32_t) varID && ctx->globalVars[varID].type != RVALUE_UNDEFINED) {
        return RValue_makeReal(1.0);
    }
    return RValue_makeReal(0.0);
}

static RValue builtinVariableGlobalGet(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount || args[0].type != RVALUE_STRING) return RValue_makeUndefined();
    const char* name = args[0].string;
    ptrdiff_t idx = shgeti(ctx->globalVarNameMap, (char*) name);
    if (0 > idx) return RValue_makeUndefined();
    int32_t varID = ctx->globalVarNameMap[idx].value;
    if (ctx->globalVarCount > (uint32_t) varID) {
        RValue val = ctx->globalVars[varID];
        // Duplicate owned strings
        if (val.type == RVALUE_STRING && val.ownsString && val.string != nullptr) {
            return RValue_makeOwnedString(safeStrdup(val.string));
        }
        return val;
    }
    return RValue_makeUndefined();
}

static RValue builtinVariableGlobalSet(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount || args[0].type != RVALUE_STRING) return RValue_makeUndefined();
    const char* name = args[0].string;
    ptrdiff_t idx = shgeti(ctx->globalVarNameMap, (char*) name);
    if (0 > idx) return RValue_makeUndefined();
    int32_t varID = ctx->globalVarNameMap[idx].value;
    if (ctx->globalVarCount > (uint32_t) varID) {
        RValue_free(&ctx->globalVars[varID]);
        RValue val = args[1];
        // Duplicate owned strings since args will be freed
        if (val.type == RVALUE_STRING && val.string != nullptr) {
            ctx->globalVars[varID] = RValue_makeOwnedString(safeStrdup(val.string));
        } else {
            ctx->globalVars[varID] = val;
        }
    }
    return RValue_makeUndefined();
}

// ===[ VARIABLE_INSTANCE ]===

static void variableInstanceSetOn(VMContext* ctx, Instance* target, const char* name, RValue val) {
    int16_t builtinId = VMBuiltins_resolveBuiltinVarId(name);
    if (builtinId != BUILTIN_VAR_UNKNOWN) {
        Instance* saved = (Instance*) ctx->currentInstance;
        ctx->currentInstance = target;
        VMBuiltins_setVariable(ctx, builtinId, name, val, -1);
        ctx->currentInstance = saved;
        return;
    }
    // Lookup varID by name from VARI (self scope)
    int32_t varID = -1;
    forEach(Variable, v, ctx->dataWin->vari.variables, ctx->dataWin->vari.variableCount) {
        if (v->varID >= 0 && (v->instanceType == INSTANCE_SELF || 0 > v->instanceType) && strcmp(v->name, name) == 0) {
            varID = v->varID;
            break;
        }
    }
    if (0 > varID) {
        fprintf(stderr, "variable_instance_set: variable '%s' not found in VARI table\n", name);
        return;
    }
    Instance_setSelfVar(target, varID, val);
}

static RValue variableInstanceGetOn(VMContext* ctx, Instance* target, const char* name) {
    int16_t builtinId = VMBuiltins_resolveBuiltinVarId(name);
    if (builtinId != BUILTIN_VAR_UNKNOWN) {
        Instance* saved = (Instance*) ctx->currentInstance;
        ctx->currentInstance = target;
        RValue val = VMBuiltins_getVariable(ctx, builtinId, name, -1);
        ctx->currentInstance = saved;
        // Duplicate string so caller-owned args cleanup does not affect it
        if (val.type == RVALUE_STRING && val.string != nullptr && !val.ownsString) {
            return RValue_makeOwnedString(safeStrdup(val.string));
        }
        return val;
    }
    int32_t varID = -1;
    forEach(Variable, v, ctx->dataWin->vari.variables, ctx->dataWin->vari.variableCount) {
        if (v->varID >= 0 && (v->instanceType == INSTANCE_SELF || 0 > v->instanceType) && strcmp(v->name, name) == 0) {
            varID = v->varID;
            break;
        }
    }
    if (0 > varID) return RValue_makeUndefined();
    RValue val = Instance_getSelfVar(target, varID);
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        return RValue_makeOwnedString(safeStrdup(val.string));
    }
    return val;
}

static RValue builtinVariableInstanceGet(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount || args[1].type != RVALUE_STRING) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    const char* name = args[1].string;

    Runner* runner = (Runner*) ctx->runner;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);

    if (id >= 100000) {
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst->active && (int32_t) inst->instanceId == id) {
                return variableInstanceGetOn(ctx, inst, name);
            }
        }
        return RValue_makeUndefined();
    }

    // Object index: return value from first matching active instance
    repeat(instanceCount, i) {
        Instance* inst = runner->instances[i];
        if (inst->active && VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, id)) {
            return variableInstanceGetOn(ctx, inst, name);
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinVariableInstanceSet(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount || args[1].type != RVALUE_STRING) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    const char* name = args[1].string;
    RValue val = args[2];

    Runner* runner = (Runner*) ctx->runner;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);

    if (id >= 100000) {
        // Specific instance ID
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst->active && (int32_t) inst->instanceId == id) {
                variableInstanceSetOn(ctx, inst, name, val);
                return RValue_makeUndefined();
            }
        }
        return RValue_makeUndefined();
    }

    // Object index: set on all active instances matching (including descendants)
    repeat(instanceCount, i) {
        Instance* inst = runner->instances[i];
        if (inst->active && VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, id)) {
            variableInstanceSetOn(ctx, inst, name, val);
        }
    }
    return RValue_makeUndefined();
}

static bool variableInstanceExistsOn(VMContext* ctx, Instance* target, const char* name) {
    if (VMBuiltins_resolveBuiltinVarId(name) != BUILTIN_VAR_UNKNOWN) return true;
    int32_t varID = -1;
    forEach(Variable, v, ctx->dataWin->vari.variables, ctx->dataWin->vari.variableCount) {
        if (v->varID >= 0 && (v->instanceType == INSTANCE_SELF || 0 > v->instanceType) && strcmp(v->name, name) == 0) {
            varID = v->varID;
            break;
        }
    }
    if (0 > varID) return false;
    return hmgeti(target->selfVars, varID) >= 0;
}

static RValue builtinVariableInstanceExists(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount || args[1].type != RVALUE_STRING) return RValue_makeBool(false);
    int32_t id = RValue_toInt32(args[0]);
    const char* name = args[1].string;

    Runner* runner = (Runner*) ctx->runner;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);

    if (id >= 100000) {
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst->active && (int32_t) inst->instanceId == id) {
                return RValue_makeBool(variableInstanceExistsOn(ctx, inst, name));
            }
        }
        return RValue_makeBool(false);
    }

    repeat(instanceCount, i) {
        Instance* inst = runner->instances[i];
        if (inst->active && VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, id)) {
            return RValue_makeBool(variableInstanceExistsOn(ctx, inst, name));
        }
    }
    return RValue_makeBool(false);
}

// ===[ METHOD ]===

#if IS_BC17_OR_HIGHER_ENABLED
static RValue builtinMethod(VMContext* ctx, MAYBE_UNUSED RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();

    int32_t boundInstance = RValue_toInt32(args[0]);
    int32_t codeIndex = RValue_toInt32(args[1]);

    // If binding to current self (-1), capture the actual instance ID
    if (boundInstance == -1 && ctx->currentInstance != nullptr) {
        boundInstance = ((Instance*) ctx->currentInstance)->instanceId;
    }

    return RValue_makeMethod(codeIndex, boundInstance);
}
#endif

// ===[ SCRIPT EXECUTE ]===

static RValue builtinScriptExecute(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();

    int32_t codeId;

#if IS_BC17_OR_HIGHER_ENABLED
    if (args[0].type == RVALUE_METHOD) {
        // If it is a method value, we'll need to extract code index directly
        codeId = args[0].method->codeIndex;
    } else
#endif
    {
        // Numeric script index
        int32_t scriptIdx = RValue_toInt32(args[0]);

        // Look up the script to get its codeId
        if (scriptIdx < 0 || (uint32_t) scriptIdx >= ctx->dataWin->scpt.count) {
            fprintf(stderr, "VM: script_execute - invalid script index %d\n", scriptIdx);
            return RValue_makeUndefined();
        }

        codeId = ctx->dataWin->scpt.scripts[scriptIdx].codeId;
    }

    if (0 > codeId || ctx->dataWin->code.count <= (uint32_t) codeId) {
        fprintf(stderr, "VM: script_execute - invalid codeId %d\n", codeId);
        return RValue_makeUndefined();
    }

    // Pass remaining args (skip the script index)
    RValue* scriptArgs = (argCount > 1) ? &args[1] : nullptr;
    int32_t scriptArgCount = argCount - 1;

    // If the method has a bound instance, temporarily swap currentInstance
    Instance* savedInstance = (Instance*) ctx->currentInstance;
#if IS_BC17_OR_HIGHER_ENABLED
    if (args[0].type == RVALUE_METHOD && args[0].method->boundInstanceId >= 0) {
        Runner* runner = (Runner*) ctx->runner;
        repeat(arrlen(runner->instances), i) {
            if (runner->instances[i]->instanceId == (uint32_t) args[0].method->boundInstanceId) {
                ctx->currentInstance = runner->instances[i];
                break;
            }
        }
    }
#endif

    RValue result = VM_callCodeIndex(ctx, codeId, scriptArgs, scriptArgCount);

    ctx->currentInstance = savedInstance;
    return result;
}

// ===[ OS FUNCTIONS ]===

static RValue builtinOsGetLanguage(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    return RValue_makeOwnedString(safeStrdup("en"));
}

static RValue builtinOsGetRegion(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    return RValue_makeOwnedString(safeStrdup("US"));
}

// ===[ DS_MAP BUILTIN FUNCTIONS ]===

static inline ptrdiff_t getValueIndexInMap(DsMapEntry** mapPtr, RValue keyRvalue) {
    ptrdiff_t idx;
    if (keyRvalue.type == RVALUE_STRING && keyRvalue.string != nullptr) {
        // Fast path: No need to convert the RValue to a string if it is already a string
        idx = shgeti(*mapPtr, keyRvalue.string);
    } else {
        char* key = RValue_toString(keyRvalue);
        idx = shgeti(*mapPtr, key);
        free(key);
    }

    return idx;
}

static RValue builtinDsMapCreate(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    return RValue_makeReal((GMLReal) dsMapCreate(runner));
}

static RValue builtinDsMapAdd(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(runner, id);
    if (mapPtr == nullptr) return RValue_makeUndefined();

    char* key = RValue_toString(args[1]);

    // Only add if key doesn't exist
    bool exists = shgeti(*mapPtr, key) != -1;

    if (exists) {
        free(key); // Key already exists, we didn't insert it
    } else {
        RValue val = args[2];
        if (val.type == RVALUE_STRING && val.string != nullptr) {
            val = RValue_makeOwnedString(safeStrdup(val.string));
        }
        shput(*mapPtr, key, val);
        // The RValue is now "owned" by the map, we do not need to free it!
    }

    return RValue_makeUndefined();
}

static RValue builtinDsMapSet(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(runner, id);
    if (mapPtr == nullptr) return RValue_makeUndefined();

    char* key = RValue_toString(args[1]);

    ptrdiff_t existingKeyIndex = shgeti(*mapPtr, key);

    if (existingKeyIndex != -1) {
        // If it already exists, we'll get the current value and free it
        RValue_free(&(*mapPtr)[existingKeyIndex].value);
    }

    RValue val = args[2];
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        val = RValue_makeOwnedString(safeStrdup(val.string));
    }

    shput(*mapPtr, key, val);

    if (existingKeyIndex != -1) {
        // If it already existed, then shput still owns the old key
        // So we'll need to free the created key
        free(key);
    }

    return RValue_makeUndefined();
}

static RValue builtinDsMapReplace(VMContext* ctx, RValue* args, int32_t argCount) {
    // ds_map_replace is the same as ds_map_set in GMS 1.4
    return builtinDsMapSet(ctx, args, argCount);
}

static RValue builtinDsMapFindValue(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(runner, id);
    if (mapPtr == nullptr) return RValue_makeUndefined();

    ptrdiff_t idx = getValueIndexInMap(mapPtr, args[1]);

    if (0 > idx) return RValue_makeUndefined();
    RValue val = (*mapPtr)[idx].value;
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        return RValue_makeOwnedString(safeStrdup(val.string));
    }
    return val;
}

static RValue builtinDsMapExists(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(runner, id);
    if (mapPtr == nullptr) return RValue_makeReal(0.0);

    ptrdiff_t idx = getValueIndexInMap(mapPtr, args[1]);

    return RValue_makeReal(idx >= 0 ? 1.0 : 0.0);
}

static RValue builtinDsMapFindFirst(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(runner, id);
    if (mapPtr == nullptr || shlen(*mapPtr) == 0) return RValue_makeUndefined();
    return RValue_makeOwnedString(safeStrdup((*mapPtr)[0].key));
}

static RValue builtinDsMapFindNext(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(runner, id);
    if (mapPtr == nullptr) return RValue_makeUndefined();

    ptrdiff_t idx = getValueIndexInMap(mapPtr, args[1]);
    if (0 > idx || idx + 1 >= shlen(*mapPtr)) return RValue_makeUndefined();
    return RValue_makeOwnedString(safeStrdup((*mapPtr)[idx + 1].key));
}

static RValue builtinDsMapSize(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(runner, id);
    if (mapPtr == nullptr) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) shlen(*mapPtr));
}

static RValue builtinDsMapDestroy(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(runner, id);
    if (mapPtr == nullptr) return RValue_makeUndefined();
    // Free all keys and values
    for (ptrdiff_t i = 0; shlen(*mapPtr) > i; i++) {
        free((*mapPtr)[i].key);
        RValue_free(&(*mapPtr)[i].value);
    }
    shfree(*mapPtr);
    *mapPtr = nullptr;
    return RValue_makeUndefined();
}

// ===[ DS_LIST FUNCTIONS ]===

static RValue builtinDsListCreate(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    return RValue_makeReal((GMLReal) dsListCreate(runner));
}

static RValue builtinDsListAdd(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    DsList* list = dsListGet(runner, id);
    if (list == nullptr) return RValue_makeUndefined();
    // ds_list_add can take multiple values after the list id
    repeat(argCount - 1, i) {
        RValue val = args[i + 1];
        if (val.type == RVALUE_STRING) {
            val = RValue_makeOwnedString(safeStrdup(val.string));
        }
        arrput(list->items, val);
    }
    return RValue_makeUndefined();
}

static RValue builtinDsListSize(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    DsList* list = dsListGet(runner, id);
    if (list == nullptr) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) arrlen(list->items));
}

static RValue builtinDsListFindIndex(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    DsList* list = dsListGet(runner, id);
    if (list == nullptr) return RValue_makeReal(-1.0);
    RValue needle = args[1];
    for (int32_t i = 0; (int32_t) arrlen(list->items) > i; i++) {
        RValue item = list->items[i];
        if (item.type != needle.type) continue;
        switch (item.type) {
            case RVALUE_REAL:
                if (item.real == needle.real) return RValue_makeReal((GMLReal) i);
                break;
            case RVALUE_INT32:
            case RVALUE_BOOL:
                if (item.int32 == needle.int32) return RValue_makeReal((GMLReal) i);
                break;
#ifndef NO_RVALUE_INT64
            case RVALUE_INT64:
                if (item.int64 == needle.int64) return RValue_makeReal((GMLReal) i);
                break;
#endif
            case RVALUE_STRING:
                if (item.string != nullptr && needle.string != nullptr && strcmp(item.string, needle.string) == 0) return RValue_makeReal((GMLReal) i);
                break;
            default:
                break;
        }
    }
    return RValue_makeReal(-1.0);
}

// ===[ ARRAY FUNCTIONS ]===

static RValue builtinArrayLength1d(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx; (void) argCount;
    if (args[0].type != RVALUE_ARRAY || args[0].array == nullptr) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) args[0].array->length);
}

// ===[ COLLISION FUNCTIONS]===

static RValue builtinPlaceFree(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeBool(true);

    Runner* runner = (Runner*) ctx->runner;
    Instance* caller = (Instance*) ctx->currentInstance;
    if (caller == nullptr) return RValue_makeBool(true);

    GMLReal testX = RValue_toReal(args[0]);
    GMLReal testY = RValue_toReal(args[1]);

    // Save current position and temporarily move to test position
    GMLReal savedX = caller->x;
    GMLReal savedY = caller->y;
    caller->x = testX;
    caller->y = testY;

    InstanceBBox callerBBox = Collision_computeBBox(runner->dataWin, caller);
    bool free = true;

    if (callerBBox.valid) {
        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        repeat(instanceCount, i) {
            Instance* other = runner->instances[i];
            if (!other->active || !other->solid || other == caller) continue;

            InstanceBBox otherBBox = Collision_computeBBox(runner->dataWin, other);
            if (!otherBBox.valid) continue;

            if (Collision_instancesOverlapPrecise(runner->dataWin, caller, other, callerBBox, otherBBox)) {
                free = false;
                break;
            }
        }
    }

    // Restore original position
    caller->x = savedX;
    caller->y = savedY;

    return RValue_makeBool(free);
}

// place_empty(x, y) - returns true if no instance overlaps at position (x, y), checking ALL instances (not just solid)
static bool placeEmptyAt(Runner* runner, Instance* caller, GMLReal testX, GMLReal testY) {
    GMLReal savedX = caller->x;
    GMLReal savedY = caller->y;
    caller->x = testX;
    caller->y = testY;

    InstanceBBox callerBBox = Collision_computeBBox(runner->dataWin, caller);
    bool empty = true;

    if (callerBBox.valid) {
        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        repeat(instanceCount, i) {
            Instance* other = runner->instances[i];
            if (!other->active || other == caller) continue;

            InstanceBBox otherBBox = Collision_computeBBox(runner->dataWin, other);
            if (!otherBBox.valid) continue;

            if (Collision_instancesOverlapPrecise(runner->dataWin, caller, other, callerBBox, otherBBox)) {
                empty = false;
                break;
            }
        }
    }

    caller->x = savedX;
    caller->y = savedY;
    return empty;
}

// placeFreeAt - returns true if no SOLID instance overlaps at position (x, y)
static bool placeFreeAt(Runner* runner, Instance* caller, GMLReal testX, GMLReal testY) {
    GMLReal savedX = caller->x;
    GMLReal savedY = caller->y;
    caller->x = testX;
    caller->y = testY;

    InstanceBBox callerBBox = Collision_computeBBox(runner->dataWin, caller);
    bool free = true;

    if (callerBBox.valid) {
        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        repeat(instanceCount, i) {
            Instance* other = runner->instances[i];
            if (!other->active || !other->solid || other == caller) continue;

            InstanceBBox otherBBox = Collision_computeBBox(runner->dataWin, other);
            if (!otherBBox.valid) continue;

            if (Collision_instancesOverlapPrecise(runner->dataWin, caller, other, callerBBox, otherBBox)) {
                free = false;
                break;
            }
        }
    }

    caller->x = savedX;
    caller->y = savedY;
    return free;
}

// noCollisionWithObject - returns true if no instance of the given object overlaps at position (x, y)
static bool noCollisionWithObject(Runner* runner, Instance* caller, GMLReal testX, GMLReal testY, int32_t objIndex) {
    GMLReal savedX = caller->x;
    GMLReal savedY = caller->y;
    caller->x = testX;
    caller->y = testY;

    InstanceBBox callerBBox = Collision_computeBBox(runner->dataWin, caller);
    bool free = true;

    if (callerBBox.valid) {
        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        repeat(instanceCount, i) {
            Instance* other = runner->instances[i];
            if (!other->active || other == caller) continue;
            if (!Collision_matchesTarget(runner->dataWin, other, objIndex)) continue;

            InstanceBBox otherBBox = Collision_computeBBox(runner->dataWin, other);
            if (!otherBBox.valid) continue;

            if (Collision_instancesOverlapPrecise(runner->dataWin, caller, other, callerBBox, otherBBox)) {
                free = false;
                break;
            }
        }
    }

    caller->x = savedX;
    caller->y = savedY;
    return free;
}

// Tests whether a position is free for the given collision mode
// objIndex == INSTANCE_ALL with checkall=false: check solid only (place_free)
// objIndex == INSTANCE_ALL with checkall=true: check all instances (place_empty)
// objIndex == specific object/instance: check that specific target (instance_place == noone)
static bool mpTestFree(Runner* runner, Instance* inst, GMLReal x, GMLReal y, int32_t objIndex, bool checkall) {
    if (objIndex == INSTANCE_ALL) {
        if (checkall) {
            return placeEmptyAt(runner, inst, x, y);
        } else {
            return placeFreeAt(runner, inst, x, y);
        }
    } else {
        return noCollisionWithObject(runner, inst, x, y, objIndex);
    }
}

// place_empty(x, y) - returns true if no instance (solid or not) overlaps at position (x, y)
static RValue builtinPlaceEmpty(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeBool(true);

    Runner* runner = (Runner*) ctx->runner;
    Instance* caller = (Instance*) ctx->currentInstance;
    if (caller == nullptr) return RValue_makeBool(true);

    GMLReal testX = RValue_toReal(args[0]);
    GMLReal testY = RValue_toReal(args[1]);
    return RValue_makeBool(placeEmptyAt(runner, caller, testX, testY));
}

// ===[ Motion Planning ]===

static RValue builtinMpLinearStepCommon(VMContext* ctx, GMLReal goalX, GMLReal goalY, GMLReal stepsize, int32_t objIndex, bool checkall) {
    Runner* runner = (Runner*) ctx->runner;
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr) return RValue_makeBool(false);

    // Check whether already at the correct position
    if (inst->x == (float) goalX && inst->y == (float) goalY) return RValue_makeBool(true);

    // Check whether close enough for a single step
    GMLReal dx = inst->x - goalX;
    GMLReal dy = inst->y - goalY;
    GMLReal dist = GMLReal_sqrt(dx * dx + dy * dy);

    GMLReal newX, newY;
    bool reached;
    if (dist <= stepsize) {
        newX = goalX;
        newY = goalY;
        reached = true;
    } else {
        newX = inst->x + stepsize * (goalX - inst->x) / dist;
        newY = inst->y + stepsize * (goalY - inst->y) / dist;
        reached = false;
    }

    // Check whether free
    if (!mpTestFree(runner, inst, newX, newY, objIndex, checkall)) return RValue_makeBool(reached);

    inst->direction = (float) (GMLReal_atan2(-(newY - inst->y), newX - inst->x) * (180.0 / M_PI));
    inst->x = (float) newX;
    inst->y = (float) newY;
    return RValue_makeBool(reached);
}

// mp_linear_step(x, y, stepsize, checkall)
static RValue builtinMpLinearStep(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal goalX = RValue_toReal(args[0]);
    GMLReal goalY = RValue_toReal(args[1]);
    GMLReal stepsize = RValue_toReal(args[2]);
    bool checkall = RValue_toBool(args[3]);
    return builtinMpLinearStepCommon(ctx, goalX, goalY, stepsize, INSTANCE_ALL, checkall);
}

// mp_linear_step_object(x, y, stepsize, obj)
static RValue builtinMpLinearStepObject(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal goalX = RValue_toReal(args[0]);
    GMLReal goalY = RValue_toReal(args[1]);
    GMLReal stepsize = RValue_toReal(args[2]);
    int32_t obj = RValue_toInt32(args[3]);
    return builtinMpLinearStepCommon(ctx, goalX, goalY, stepsize, obj, true);
}


// Computes the shortest angular difference between two directions (result 0-180)
static GMLReal mpDiffDir(GMLReal dir1, GMLReal dir2) {
    while (dir1 <= 0.0) dir1 += 360.0;
    while (dir1 >= 360.0) dir1 -= 360.0;
    while (dir2 < 0.0) dir2 += 360.0;
    while (dir2 >= 360.0) dir2 -= 360.0;
    GMLReal result = dir2 - dir1;
    if (result < 0.0) result = -result;
    if (result > 180.0) result = 360.0 - result;
    return result;
}

// Tries a step in the indicated direction; returns whether successful
// If successful, moves the instance and sets its direction
static bool mpTryDir(GMLReal dir, Runner* runner, Instance* inst, GMLReal speed, int32_t objIndex, bool checkall) {
    // See whether angle is acceptable
    if (mpDiffDir(dir, inst->direction) > runner->mpPotMaxrot) return false;

    GMLReal dirRad = dir * (M_PI / 180.0);
    GMLReal cosDir = GMLReal_cos(dirRad);
    GMLReal sinDir = GMLReal_sin(dirRad);

    // Check position a bit ahead
    GMLReal aheadX = inst->x + speed * runner->mpPotAhead * cosDir;
    GMLReal aheadY = inst->y - speed * runner->mpPotAhead * sinDir;
    if (!mpTestFree(runner, inst, aheadX, aheadY, objIndex, checkall)) return false;

    // Check next position
    GMLReal nextX = inst->x + speed * cosDir;
    GMLReal nextY = inst->y - speed * sinDir;
    if (!mpTestFree(runner, inst, nextX, nextY, objIndex, checkall)) return false;

    // OK, so set the position
    inst->direction = (float) dir;
    inst->x = (float) nextX;
    inst->y = (float) nextY;
    return true;
}

static RValue builtinMpPotentialStepCommon(VMContext* ctx, GMLReal goalX, GMLReal goalY, GMLReal stepsize, int32_t objIndex, bool checkall) {
    Runner* runner = (Runner*) ctx->runner;
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr) return RValue_makeBool(false);

    // Check whether already at the correct position
    if (inst->x == (float) goalX && inst->y == (float) goalY) return RValue_makeBool(true);

    // Check whether close enough for a single step
    GMLReal dx = inst->x - goalX;
    GMLReal dy = inst->y - goalY;
    GMLReal dist = GMLReal_sqrt(dx * dx + dy * dy);
    if (stepsize >= dist) {
        if (mpTestFree(runner, inst, goalX, goalY, objIndex, checkall)) {
            GMLReal dir = GMLReal_atan2(-(goalY - inst->y), goalX - inst->x) * (180.0 / M_PI);
            inst->direction = (float) dir;
            inst->x = (float) goalX;
            inst->y = (float) goalY;
        }
        return RValue_makeBool(true);
    }

    // Try directions as much as possible towards the goal
    GMLReal goaldir = GMLReal_atan2(-(goalY - inst->y), goalX - inst->x) * (180.0 / M_PI);
    GMLReal curdir = 0.0;
    while (180.0 > curdir) {
        if (mpTryDir(goaldir - curdir, runner, inst, stepsize, objIndex, checkall)) return RValue_makeBool(false);
        if (mpTryDir(goaldir + curdir, runner, inst, stepsize, objIndex, checkall)) return RValue_makeBool(false);
        curdir += runner->mpPotStep;
    }

    // If we did not succeed, a local minima was reached
    // To avoid the instance getting stuck we rotate on the spot
    if (runner->mpPotOnSpot) {
        inst->direction = (float) (inst->direction + runner->mpPotMaxrot);
    }

    return RValue_makeBool(false);
}

// mp_potential_step(x, y, stepsize, checkall)
static RValue builtinMpPotentialStep(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal goalX = RValue_toReal(args[0]);
    GMLReal goalY = RValue_toReal(args[1]);
    GMLReal stepsize = RValue_toReal(args[2]);
    bool checkall = RValue_toBool(args[3]);
    return builtinMpPotentialStepCommon(ctx, goalX, goalY, stepsize, INSTANCE_ALL, checkall);
}

// mp_potential_step_object(x, y, stepsize, obj)
static RValue builtinMpPotentialStepObject(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal goalX = RValue_toReal(args[0]);
    GMLReal goalY = RValue_toReal(args[1]);
    GMLReal stepsize = RValue_toReal(args[2]);
    int32_t obj = RValue_toInt32(args[3]);
    return builtinMpPotentialStepCommon(ctx, goalX, goalY, stepsize, obj, true);
}

// mp_potential_settings(maxrot, rotstep, ahead, onspot)
static RValue builtinMpPotentialSettings(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    GMLReal maxrot = RValue_toReal(args[0]);
    GMLReal rotstep = RValue_toReal(args[1]);
    GMLReal ahead = RValue_toReal(args[2]);
    bool onspot = RValue_toBool(args[3]);
    runner->mpPotMaxrot = (maxrot < 1.0) ? 1.0 : maxrot;
    runner->mpPotStep = (rotstep < 1.0) ? 1.0 : rotstep;
    runner->mpPotAhead = (ahead < 1.0) ? 1.0 : ahead;
    runner->mpPotOnSpot = onspot;
    return RValue_makeReal(0.0);
}

// ===[ STUBBED FUNCTIONS ]===

#define STUB_RETURN_ZERO(name) \
    static RValue builtin_##name(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) { \
        logStubbedFunction(ctx, #name); \
        return RValue_makeReal(0.0); \
    }

#define STUB_RETURN_TRUE(name) \
    static RValue builtin_##name(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) { \
        logStubbedFunction(ctx, #name); \
        return RValue_makeBool(true); \
    }

#define STUB_RETURN_VALUE(name, value) \
    static RValue builtin_##name(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) { \
        logStubbedFunction(ctx, #name); \
        return RValue_makeReal(value); \
    }

#define STUB_RETURN_UNDEFINED(name) \
    static RValue builtin_##name(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) { \
        logStubbedFunction(ctx, #name); \
        return RValue_makeUndefined(); \
    }

// Steam stubs
STUB_RETURN_ZERO(steam_initialised)
STUB_RETURN_ZERO(steam_stats_ready)
STUB_RETURN_ZERO(steam_file_exists)
STUB_RETURN_UNDEFINED(steam_file_write)
STUB_RETURN_UNDEFINED(steam_file_read)
STUB_RETURN_ZERO(steam_get_persona_name)

// ===[ Audio Built-in Functions ]===

// Helper to get the AudioSystem from VMContext (returns nullptr if no audio)
static AudioSystem* getAudioSystem(VMContext* ctx) {
    Runner* runner = (Runner*) ctx->runner;
    return runner->audioSystem;
}


static RValue builtin_audioChannelNum(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t count = RValue_toInt32(args[0]);
    audio->vtable->setChannelCount(audio, count);
    return RValue_makeUndefined();
}

static RValue builtin_audioPlaySound(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(-1.0);

    // Do not attempt to play "undefined" sounds (matches GameMaker-HTML5 behavior, and fixes random sound effects on room transitions in DELTARUNE Chapter 2)
    if (args[0].type == RVALUE_UNDEFINED)
        return RValue_makeReal(-1.0);

    int32_t soundIndex = RValue_toInt32(args[0]);
    int32_t priority = RValue_toInt32(args[1]);
    bool loop = RValue_toBool(args[2]);
    int32_t instanceId = audio->vtable->playSound(audio, soundIndex, priority, loop);
    return RValue_makeReal((GMLReal) instanceId);
}

static RValue builtin_audioStopSound(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    audio->vtable->stopSound(audio, soundOrInstance);
    return RValue_makeUndefined();
}

static RValue builtin_audioStopAll(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    audio->vtable->stopAll(audio);
    runner->lastMusicInstance = -1;
    return RValue_makeUndefined();
}

static RValue builtin_audioIsPlaying(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeBool(false);
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    bool playing = audio->vtable->isPlaying(audio, soundOrInstance);
    return RValue_makeBool(playing);
}

static RValue builtin_audioIsPaused(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeBool(false);
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    bool playing = audio->vtable->isPlaying(audio, soundOrInstance);
    return RValue_makeBool(!playing);
}


static RValue builtin_audioSoundGain(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    float gain = (float) RValue_toReal(args[1]);
    uint32_t timeMs = (uint32_t) RValue_toInt32(args[2]);
    audio->vtable->setSoundGain(audio, soundOrInstance, gain, timeMs);
    return RValue_makeUndefined();
}

static RValue builtin_audioSoundPitch(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    float pitch = (float) RValue_toReal(args[1]);
    audio->vtable->setSoundPitch(audio, soundOrInstance, pitch);
    return RValue_makeUndefined();
}

static RValue builtin_audioSoundGetGain(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(0.0);
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    float gain = audio->vtable->getSoundGain(audio, soundOrInstance);
    return RValue_makeReal((GMLReal) gain);
}

static RValue builtin_audioSoundGetPitch(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(1.0);
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    float pitch = audio->vtable->getSoundPitch(audio, soundOrInstance);
    return RValue_makeReal((GMLReal) pitch);
}

static RValue builtin_audioMasterGain(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    float gain = (float) RValue_toReal(args[0]);
    audio->vtable->setMasterGain(audio, gain);
    return RValue_makeUndefined();
}

static RValue builtin_audioGroupLoad(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t groupIndex = RValue_toInt32(args[0]);
    audio->vtable->groupLoad(audio, groupIndex);
    return RValue_makeUndefined();
}

static RValue builtin_audioGroupIsLoaded(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeBool(false);
    int32_t groupIndex = RValue_toInt32(args[0]);
    bool loaded = audio->vtable->groupIsLoaded(audio, groupIndex);
    return RValue_makeBool(loaded);
}

static RValue builtin_audioPlayMusic(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(-1.0);
    int32_t soundIndex = RValue_toInt32(args[0]);
    int32_t priority = RValue_toInt32(args[1]);
    bool loop = RValue_toBool(args[2]);
    Runner* runner = (Runner*) ctx->runner;
    int32_t instanceId = audio->vtable->playSound(audio, soundIndex, priority, loop);
    runner->lastMusicInstance = instanceId;
    return RValue_makeReal((GMLReal) instanceId);
}

static RValue builtin_audioStopMusic(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    if (runner->lastMusicInstance >= 0) {
        audio->vtable->stopSound(audio, runner->lastMusicInstance);
        runner->lastMusicInstance = -1;
    }
    return RValue_makeUndefined();
}

static RValue builtin_audioMusicGain(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    if (runner->lastMusicInstance >= 0) {
        float gain = (float) RValue_toReal(args[0]);
        uint32_t timeMs = (uint32_t) RValue_toInt32(args[1]);
        audio->vtable->setSoundGain(audio, runner->lastMusicInstance, gain, timeMs);
    }
    return RValue_makeUndefined();
}

static RValue builtin_audioMusicIsPlaying(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeBool(false);
    Runner* runner = (Runner*) ctx->runner;
    if (runner->lastMusicInstance >= 0) {
        return RValue_makeBool(audio->vtable->isPlaying(audio, runner->lastMusicInstance));
    }
    return RValue_makeBool(false);
}

static RValue builtin_audioPauseSound(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    audio->vtable->pauseSound(audio, soundOrInstance);
    return RValue_makeUndefined();
}

static RValue builtin_audioResumeSound(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    audio->vtable->resumeSound(audio, soundOrInstance);
    return RValue_makeUndefined();
}

static RValue builtin_audioPauseAll(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    audio->vtable->pauseAll(audio);
    return RValue_makeUndefined();
}

static RValue builtin_audioResumeAll(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    audio->vtable->resumeAll(audio);
    return RValue_makeUndefined();
}

static RValue builtin_audioSoundGetTrackPosition(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(0.0);
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    float pos = audio->vtable->getTrackPosition(audio, soundOrInstance);
    return RValue_makeReal((GMLReal) pos);
}

static RValue builtin_audioSoundSetTrackPosition(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeUndefined();
    int32_t soundOrInstance = RValue_toInt32(args[0]);
    float pos = (float) RValue_toReal(args[1]);
    audio->vtable->setTrackPosition(audio, soundOrInstance, pos);
    return RValue_makeUndefined();
}

static RValue builtin_audioCreateStream(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(-1.0);
    char* filename = RValue_toString(args[0]);
    int32_t streamIndex = audio->vtable->createStream(audio, filename);
    free(filename);
    return RValue_makeReal((GMLReal) streamIndex);
}

static RValue builtin_audioDestroyStream(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    AudioSystem* audio = getAudioSystem(ctx);
    if (audio == nullptr) return RValue_makeReal(-1.0);
    int32_t streamIndex = RValue_toInt32(args[0]);
    bool success = audio->vtable->destroyStream(audio, streamIndex);
    return RValue_makeReal(success ? 1.0 : -1.0);
}

// Application surface stubs
STUB_RETURN_UNDEFINED(application_surface_enable)
STUB_RETURN_UNDEFINED(application_surface_draw_enable)

// Gamepad stubs
STUB_RETURN_ZERO(gamepad_get_device_count)
STUB_RETURN_ZERO(gamepad_is_connected)
STUB_RETURN_ZERO(gamepad_button_check)
STUB_RETURN_ZERO(gamepad_button_check_pressed)
STUB_RETURN_ZERO(gamepad_button_check_released)
STUB_RETURN_ZERO(gamepad_axis_value)
STUB_RETURN_ZERO(gamepad_get_description)
STUB_RETURN_ZERO(gamepad_button_value)

// ===[ INI Functions ]===

static void discardIniCache(Runner* runner) {
    if (runner->cachedIni != nullptr) {
        Ini_free(runner->cachedIni);
        runner->cachedIni = nullptr;
    }
    free(runner->cachedIniPath);
    runner->cachedIniPath = nullptr;
}

static RValue builtinIniOpen(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();

    Runner* runner = (Runner*) ctx->runner;
    const char* path = (args[0].type == RVALUE_STRING ? args[0].string : "");

    // If the same file is already open, do nothing
    if (runner->currentIni != nullptr && runner->currentIniPath != nullptr && strcmp(runner->currentIniPath, path) == 0) {
        return RValue_makeUndefined();
    }

    // Close any previously open INI (implicit close, no disk write)
    if (runner->currentIni != nullptr) {
        Ini_free(runner->currentIni);
        runner->currentIni = nullptr;
    }
    free(runner->currentIniPath);
    runner->currentIniPath = nullptr;

    // Check if we have a cached INI for this path
    if (runner->cachedIni != nullptr && runner->cachedIniPath != nullptr && strcmp(runner->cachedIniPath, path) == 0) {
        runner->currentIni = runner->cachedIni;
        runner->currentIniPath = runner->cachedIniPath;
        runner->cachedIni = nullptr;
        runner->cachedIniPath = nullptr;
        runner->currentIniDirty = false;
        return RValue_makeUndefined();
    }

    // Cache miss, discard the old cache and read from disk
    discardIniCache(runner);

    FileSystem* fs = runner->fileSystem;

    runner->currentIniPath = safeStrdup(path);

    char* content = fs->vtable->readFileText(fs, path);
    if (content != nullptr) {
        runner->currentIni = Ini_parse(content);
        free(content);
    } else {
        runner->currentIni = Ini_parse("");
    }

    runner->currentIniDirty = false;

    return RValue_makeUndefined();
}

static RValue builtinIniClose(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->currentIni != nullptr) {
        FileSystem* fs = runner->fileSystem;

        if (runner->currentIniDirty) {
            char* serialized = Ini_serialize(runner->currentIni, INI_SERIALIZE_DEFAULT_INITIAL_CAPACITY);
            fs->vtable->writeFileText(fs, runner->currentIniPath, serialized);
            free(serialized);
        }

        // Move to cache instead of freeing
        discardIniCache(runner);
        runner->cachedIni = runner->currentIni;
        runner->cachedIniPath = runner->currentIniPath;
        runner->currentIni = nullptr;
        runner->currentIniPath = nullptr;
    } else {
        free(runner->currentIniPath);
        runner->currentIniPath = nullptr;
    }

    return RValue_makeUndefined();
}

static RValue builtinIniReadString(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (3 > argCount || runner->currentIni == nullptr) return RValue_makeOwnedString(safeStrdup(""));

    const char* section = (args[0].type == RVALUE_STRING ? args[0].string : "");
    const char* key = (args[1].type == RVALUE_STRING ? args[1].string : "");

    const char* value = Ini_getString(runner->currentIni, section, key);
    if (value != nullptr) {
        return RValue_makeOwnedString(safeStrdup(value));
    }

    // Return the default value (3rd arg)
    if (args[2].type == RVALUE_STRING && args[2].string != nullptr) {
        return RValue_makeOwnedString(safeStrdup(args[2].string));
    }
    char* str = RValue_toString(args[2]);
    return RValue_makeOwnedString(str);
}

static RValue builtinIniReadReal(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (3 > argCount || runner->currentIni == nullptr) return RValue_makeReal(0.0);

    const char* section = (args[0].type == RVALUE_STRING ? args[0].string : "");
    const char* key = (args[1].type == RVALUE_STRING ? args[1].string : "");

    const char* value = Ini_getString(runner->currentIni, section, key);
    if (value != nullptr) {
        return RValue_makeReal(atof(value));
    }

    return RValue_makeReal(RValue_toReal(args[2]));
}

static RValue builtinIniWriteString(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (3 > argCount || runner->currentIni == nullptr) return RValue_makeUndefined();

    const char* section = (args[0].type == RVALUE_STRING ? args[0].string : "");
    const char* key = (args[1].type == RVALUE_STRING ? args[1].string : "");
    const char* value = (args[2].type == RVALUE_STRING ? args[2].string : "");

    Ini_setString(runner->currentIni, section, key, value);
    runner->currentIniDirty = true;
    return RValue_makeUndefined();
}

static RValue builtinIniWriteReal(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (3 > argCount || runner->currentIni == nullptr) return RValue_makeUndefined();

    const char* section = (args[0].type == RVALUE_STRING ? args[0].string : "");
    const char* key = (args[1].type == RVALUE_STRING ? args[1].string : "");
    char* valueStr = RValue_toString(args[2]);

    Ini_setString(runner->currentIni, section, key, valueStr);
    runner->currentIniDirty = true;
    free(valueStr);
    return RValue_makeUndefined();
}

static RValue builtinIniSectionExists(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (1 > argCount || runner->currentIni == nullptr) return RValue_makeBool(false);

    const char* section = (args[0].type == RVALUE_STRING ? args[0].string : "");
    return RValue_makeBool(Ini_hasSection(runner->currentIni, section));
}

// ===[ Text File Functions ]===

static int32_t findFreeTextFileSlot(Runner* runner) {
    repeat(MAX_OPEN_TEXT_FILES, i) {
        if (!runner->openTextFiles[i].isOpen) return (int32_t) i;
    }
    return -1;
}

static RValue builtinFileExists(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    const char* path = (args[0].type == RVALUE_STRING ? args[0].string : "");
    Runner* runner = (Runner*) ctx->runner;
    FileSystem* fs = runner->fileSystem;
    return RValue_makeBool(fs->vtable->fileExists(fs, path));
}

static RValue builtinFileTextOpenRead(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1.0);
    const char* path = (args[0].type == RVALUE_STRING ? args[0].string : "");
    Runner* runner = (Runner*) ctx->runner;
    FileSystem* fs = runner->fileSystem;

    int32_t slot = findFreeTextFileSlot(runner);
    if (0 > slot) {
        fprintf(stderr, "Warning: Too many open text files!\n");
        abort();
    }

    char* content = fs->vtable->readFileText(fs, path);
    if (content == nullptr) {
        // GML returns a valid handle even if the file doesn't exist; eof is immediately true
        content = safeStrdup("");
    }

    runner->openTextFiles[slot] = (OpenTextFile) {
        .content = content,
        .writeBuffer = nullptr,
        .filePath = nullptr,
        .readPos = 0,
        .contentLen = (int32_t) strlen(content),
        .isWriteMode = false,
        .isOpen = true,
    };

    return RValue_makeReal((GMLReal) slot);
}

static RValue builtinFileTextOpenWrite(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(-1.0);
    const char* path = (args[0].type == RVALUE_STRING ? args[0].string : "");
    Runner* runner = (Runner*) ctx->runner;

    int32_t slot = findFreeTextFileSlot(runner);
    if (0 > slot) {
        fprintf(stderr, "Warning: Too many open text files!\n");
        abort();
    }

    runner->openTextFiles[slot] = (OpenTextFile) {
        .content = nullptr,
        .writeBuffer = safeStrdup(""),
        .filePath = safeStrdup(path),
        .readPos = 0,
        .contentLen = 0,
        .isWriteMode = true,
        .isOpen = true,
    };

    return RValue_makeReal((GMLReal) slot);
}

static RValue builtinFileTextClose(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !runner->openTextFiles[handle].isOpen) return RValue_makeUndefined();

    OpenTextFile* file = &runner->openTextFiles[handle];
    if (file->isWriteMode && file->writeBuffer != nullptr && file->filePath != nullptr) {
        FileSystem* fs = runner->fileSystem;
        fs->vtable->writeFileText(fs, file->filePath, file->writeBuffer);
    }

    free(file->content);
    free(file->writeBuffer);
    free(file->filePath);
    *file = (OpenTextFile) {0};
    return RValue_makeUndefined();
}

static RValue builtinFileTextReadString(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeOwnedString(safeStrdup(""));
    Runner* runner = (Runner*) ctx->runner;
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !runner->openTextFiles[handle].isOpen) return RValue_makeOwnedString(safeStrdup(""));

    OpenTextFile* file = &runner->openTextFiles[handle];
    if (file->readPos >= file->contentLen) return RValue_makeOwnedString(safeStrdup(""));

    // Read until newline, carriage return, or EOF (does NOT consume the newline)
    int32_t start = file->readPos;
    while (file->contentLen > file->readPos) {
        char c = file->content[file->readPos];
        if (TextUtils_isNewlineChar(c))
            break;
        file->readPos++;
    }

    int32_t len = file->readPos - start;
    char* result = safeMalloc((size_t) len + 1);
    memcpy(result, file->content + start, (size_t) len);
    result[len] = '\0';
    return RValue_makeOwnedString(result);
}

static RValue builtinFileTextReadln(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeOwnedString(safeStrdup(""));
    Runner* runner = (Runner*) ctx->runner;
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || MAX_OPEN_TEXT_FILES <= handle || !runner->openTextFiles[handle].isOpen) return RValue_makeOwnedString(safeStrdup(""));

    OpenTextFile* file = &runner->openTextFiles[handle];

    int size = 0;
    int readPos = file->readPos;

    // First we read everything to figure out what will be the size of the string
    // Skip past the current line (consume everything up to and including the newline)
    while (file->contentLen > readPos) {
        char c = file->content[readPos];
        readPos++;
        if (c == '\n')
            break;
        if (c == '\r') {
            // Handle \r\n
            if (file->contentLen > readPos && file->content[readPos] == '\n') {
                readPos++;
            }
            break;
        }
        size++;
    }

    // Now we copy it because we already know the size of the string!
    char* string = safeMalloc(size + 1); // +1 because the last one is null
    memcpy(string, file->content + file->readPos, size);
    string[size] = '\0';
    file->readPos = readPos;
    return RValue_makeOwnedString(string);
}

static RValue builtinFileTextReadReal(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !runner->openTextFiles[handle].isOpen) return RValue_makeReal(0.0);

    OpenTextFile* file = &runner->openTextFiles[handle];
    if (file->readPos >= file->contentLen) return RValue_makeReal(0.0);

    // strtod will parse the number and advance past it
    char* endPtr = nullptr;
    GMLReal value = GMLReal_strtod(file->content + file->readPos, &endPtr);
    if (endPtr != nullptr) {
        file->readPos = (int32_t) (endPtr - file->content);
    }

    return RValue_makeReal(value);
}

static RValue builtinFileTextWriteString(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !runner->openTextFiles[handle].isOpen) return RValue_makeUndefined();

    OpenTextFile* file = &runner->openTextFiles[handle];
    if (!file->isWriteMode) return RValue_makeUndefined();

    char* str = RValue_toString(args[1]);
    size_t oldLen = strlen(file->writeBuffer);
    size_t addLen = strlen(str);
    file->writeBuffer = safeRealloc(file->writeBuffer, oldLen + addLen + 1);
    memcpy(file->writeBuffer + oldLen, str, addLen);
    file->writeBuffer[oldLen + addLen] = '\0';
    free(str);

    return RValue_makeUndefined();
}

static RValue builtinFileTextWriteln(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !runner->openTextFiles[handle].isOpen) return RValue_makeUndefined();

    OpenTextFile* file = &runner->openTextFiles[handle];
    if (!file->isWriteMode) return RValue_makeUndefined();

    size_t oldLen = strlen(file->writeBuffer);
    file->writeBuffer = safeRealloc(file->writeBuffer, oldLen + 2);
    file->writeBuffer[oldLen] = '\n';
    file->writeBuffer[oldLen + 1] = '\0';

    return RValue_makeUndefined();
}

static RValue builtinFileTextWriteReal(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !runner->openTextFiles[handle].isOpen) return RValue_makeUndefined();

    OpenTextFile* file = &runner->openTextFiles[handle];
    if (!file->isWriteMode) return RValue_makeUndefined();

    char* str = RValue_toString(args[1]);
    size_t oldLen = strlen(file->writeBuffer);
    size_t addLen = strlen(str);
    file->writeBuffer = safeRealloc(file->writeBuffer, oldLen + addLen + 1);
    memcpy(file->writeBuffer + oldLen, str, addLen);
    file->writeBuffer[oldLen + addLen] = '\0';
    free(str);

    return RValue_makeUndefined();
}

static RValue builtinFileTextEof(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(true);
    Runner* runner = (Runner*) ctx->runner;
    int32_t handle = RValue_toInt32(args[0]);
    if (0 > handle || handle >= MAX_OPEN_TEXT_FILES || !runner->openTextFiles[handle].isOpen) return RValue_makeBool(true);

    OpenTextFile* file = &runner->openTextFiles[handle];
    return RValue_makeBool(file->readPos >= file->contentLen);
}

static RValue builtinFileDelete(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    const char* path = (args[0].type == RVALUE_STRING ? args[0].string : "");
    Runner* runner = (Runner*) ctx->runner;
    FileSystem* fs = runner->fileSystem;
    fs->vtable->deleteFile(fs, path);
    return RValue_makeUndefined();
}

// Keyboard functions
static RValue builtinKeyboardCheck(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    Runner* runner = (Runner*) ctx->runner;
    int32_t key = RValue_toInt32(args[0]);
    return RValue_makeBool(RunnerKeyboard_check(runner->keyboard, key));
}

static RValue builtinKeyboardCheckPressed(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    Runner* runner = (Runner*) ctx->runner;
    int32_t key = RValue_toInt32(args[0]);
    return RValue_makeBool(RunnerKeyboard_checkPressed(runner->keyboard, key));
}

static RValue builtinKeyboardCheckReleased(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    Runner* runner = (Runner*) ctx->runner;
    int32_t key = RValue_toInt32(args[0]);
    return RValue_makeBool(RunnerKeyboard_checkReleased(runner->keyboard, key));
}

static RValue builtinKeyboardCheckDirect(VMContext* ctx, RValue* args, int32_t argCount) {
    // keyboard_check_direct is the same as keyboard_check for our purposes
    return builtinKeyboardCheck(ctx, args, argCount);
}

static RValue builtinKeyboardKeyPress(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t key = RValue_toInt32(args[0]);
    RunnerKeyboard_simulatePress(runner->keyboard, key);
    return RValue_makeUndefined();
}

static RValue builtinKeyboardKeyRelease(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t key = RValue_toInt32(args[0]);
    RunnerKeyboard_simulateRelease(runner->keyboard, key);
    return RValue_makeUndefined();
}

static RValue builtinKeyboardClear(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t key = RValue_toInt32(args[0]);
    RunnerKeyboard_clear(runner->keyboard, key);
    return RValue_makeUndefined();
}

// Joystick stubs
STUB_RETURN_ZERO(joystick_exists)
STUB_RETURN_ZERO(joystick_xpos)
STUB_RETURN_ZERO(joystick_ypos)
STUB_RETURN_ZERO(joystick_direction)
STUB_RETURN_ZERO(joystick_pov)
STUB_RETURN_ZERO(joystick_check_button)

// Window stubs
STUB_RETURN_ZERO(window_get_fullscreen)
STUB_RETURN_UNDEFINED(window_set_fullscreen)
STUB_RETURN_UNDEFINED(window_set_size)
STUB_RETURN_UNDEFINED(window_center)
static RValue builtinWindowGetWidth(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    return RValue_makeReal((GMLReal) ctx->dataWin->gen8.defaultWindowWidth);
}

static RValue builtinWindowGetHeight(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    return RValue_makeReal((GMLReal) ctx->dataWin->gen8.defaultWindowHeight);
}

static RValue builtinWindowSetCaption(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    char* val = RValue_toString(args[0]);
    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", val);

    Runner* runner = (Runner*) ctx->runner;
    if (runner->setWindowTitle && runner->nativeWindow) {
        runner->setWindowTitle(runner->nativeWindow, windowTitle);
        printf("GL: Window title set to: %s\n", val);
    }
    
    free(val);
    return RValue_makeUndefined();
}

// ===[ Game State Functions ]===
static RValue builtinGameRestart(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    ctx->runner->pendingRoom = ROOM_RESTARTGAME;
    return RValue_makeUndefined();
}

static RValue builtinGameEnd(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    runner->shouldExit = true;
    return RValue_makeUndefined();
}
STUB_RETURN_UNDEFINED(game_save)
STUB_RETURN_UNDEFINED(game_load)

static RValue builtinInstanceNumber(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    int32_t objectIndex = RValue_toInt32(args[0]);
    int32_t count = 0;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);
    repeat(instanceCount, i) {
        Instance* inst = runner->instances[i];
        if (inst->active && VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, objectIndex)) {
            count++;
        }
    }
    return RValue_makeReal((GMLReal) count);
}

static RValue builtinInstanceFind(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(-4.0); // noone
    Runner* runner = (Runner*) ctx->runner;
    int32_t objectIndex = RValue_toInt32(args[0]);
    int32_t n = RValue_toInt32(args[1]);
    int32_t count = 0;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);
    repeat(instanceCount, i) {
        Instance* inst = runner->instances[i];
        if (inst->active && VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, objectIndex)) {
            if (count == n) return RValue_makeReal((GMLReal) inst->instanceId);
            count++;
        }
    }
    return RValue_makeReal(-4.0); // noone
}

static RValue builtinInstanceExists(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    bool found = false;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);
    if (id >= 0 && runner->dataWin->objt.count > (uint32_t) id) {
        // Object type index: search for any active instance of this object (or descendants)
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst->active && VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, id)) {
                found = true;
                break;
            }
        }
    } else {
        // Instance ID: search for a specific instance
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst->active && inst->instanceId == (uint32_t) id) {
                found = true;
                break;
            }
        }
    }
    return RValue_makeBool(found);
}

static RValue builtinInstanceDestroy(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (1 > argCount) {
        // No args: destroy the current instance
        if (ctx->currentInstance != nullptr) {
            Runner_destroyInstance(runner, (Instance*) ctx->currentInstance);
        }
        return RValue_makeUndefined();
    }
    // 1 arg: find and destroy matching instances
    int32_t id = RValue_toInt32(args[0]);
    int32_t instanceCount = (int32_t) arrlen(runner->instances);
    if (id >= 0 && runner->dataWin->objt.count > (uint32_t) id) {
        // Object type index: destroy all active instances of this object (or descendants)
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst->active && VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, id)) {
                Runner_destroyInstance(runner, inst);
            }
        }
    } else {
        // Instance ID: destroy that specific instance
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst->active && inst->instanceId == (uint32_t) id) {
                Runner_destroyInstance(runner, inst);
                break;
            }
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinInstanceCreate(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    GMLReal x = RValue_toReal(args[0]);
    GMLReal y = RValue_toReal(args[1]);
    int32_t objectIndex = RValue_toInt32(args[2]);
    if (0 > objectIndex || runner->dataWin->objt.count <= (uint32_t) objectIndex) {
        fprintf(stderr, "VM: instance_create: objectIndex %d out of range\n", objectIndex);
        return RValue_makeReal(0.0);
    }
    Instance* callerInst = (Instance*) ctx->currentInstance;
    Instance* inst = Runner_createInstance(runner, x, y, objectIndex);
    if (inst == nullptr) return RValue_makeReal(-4.0); // noone
    if (callerInst != nullptr && ctx->creatorVarID >= 0) {
        Instance_setSelfVar(inst, ctx->creatorVarID, RValue_makeReal((GMLReal) callerInst->instanceId));
    }
    return RValue_makeReal((GMLReal) inst->instanceId);
}

static RValue builtinInstanceCopy(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    Instance* source = (Instance*) ctx->currentInstance;
    if (source == nullptr) {
        fprintf(stderr, "VM: instance_copy: no current instance\n");
        return RValue_makeReal(-4.0); // noone
    }
    bool performEvent = argCount > 0 ? RValue_toBool(args[0]) : false;
    Instance* inst = Runner_copyInstance(runner, source, performEvent);
    if (inst == nullptr) return RValue_makeReal(-4.0);
    return RValue_makeReal((GMLReal) inst->instanceId);
}

static RValue builtinInstanceCreateDepth(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    GMLReal x = RValue_toReal(args[0]);
    GMLReal y = RValue_toReal(args[1]);
    int32_t depth = RValue_toInt32(args[2]);
    int32_t objectIndex = RValue_toInt32(args[3]);
    if (0 > objectIndex || runner->dataWin->objt.count <= (uint32_t) objectIndex) {
        fprintf(stderr, "VM: instance_create: objectIndex %d out of range\n", objectIndex);
        return RValue_makeReal(0.0);
    }
    Instance* callerInst = (Instance*) ctx->currentInstance;
    Instance* inst = Runner_createInstance(runner, x, y, objectIndex);
    if (inst == nullptr) return RValue_makeReal(-4.0); // noone
    if (callerInst != nullptr && ctx->creatorVarID >= 0) {
        Instance_setSelfVar(inst, ctx->creatorVarID, RValue_makeReal((GMLReal) callerInst->instanceId));
    }
    inst->depth = depth;
    return RValue_makeReal((GMLReal) inst->instanceId);
}

static RValue builtinInstanceChange(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr) return RValue_makeUndefined();

    int32_t objectIndex = RValue_toInt32(args[0]);
    bool performEvents = RValue_toBool(args[1]);

    if (0 > objectIndex || (uint32_t) objectIndex >= runner->dataWin->objt.count) {
        fprintf(stderr, "VM: instance_change: objectIndex %d out of range\n", objectIndex);
        return RValue_makeUndefined();
    }

    // Fire destroy event on old object if requested
    if (performEvents) {
        Runner_executeEvent(runner, inst, EVENT_DESTROY, 0);
    }

    // Change object index and copy properties from new object definition
    GameObject* newObjDef = &runner->dataWin->objt.objects[objectIndex];
    inst->objectIndex = objectIndex;
    inst->spriteIndex = newObjDef->spriteId;
    inst->visible = newObjDef->visible;
    inst->solid = newObjDef->solid;
    inst->persistent = newObjDef->persistent;
    inst->depth = newObjDef->depth;
    inst->maskIndex = newObjDef->textureMaskId;
    inst->imageIndex = 0.0;

    // Fire create event on new object if requested
    if (performEvents) {
        Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
    }

    return RValue_makeUndefined();
}

static RValue builtinInstanceDeactivateAll(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    bool notme = RValue_toBool(args[0]);

    int instances = arrlen(ctx->runner->instances);
    repeat(instances, i) {
        Instance* instance = ctx->runner->instances[i];

        if (!notme || instance != ctx->currentInstance) {
            instance->active = false;
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinInstanceActivateAll(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    int instances = arrlen(ctx->runner->instances);
    repeat(instances, i) {
        Instance* instance = ctx->runner->instances[i];
        if (!instance->destroyed)
            ctx->runner->instances[i]->active = true;
    }
    return RValue_makeUndefined();
}

static RValue builtinInstanceActivateObject(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t objIndex = RValue_toInt32(args[0]);

    int instances = arrlen(ctx->runner->instances);
    repeat(instances, i) {
        Instance* instance = ctx->runner->instances[i];
        if (!instance->active && !instance->destroyed && (objIndex == INSTANCE_ALL || VM_isObjectOrDescendant(ctx->dataWin, instance->objectIndex, objIndex))) {
            instance->active = true;
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinInstanceDeactivateObject(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t objIndex = RValue_toInt32(args[0]);

    int instances = arrlen(ctx->runner->instances);
    repeat(instances, i) {
        Instance* instance = ctx->runner->instances[i];
        if (instance->active && !instance->destroyed && (objIndex == INSTANCE_ALL || VM_isObjectOrDescendant(ctx->dataWin, instance->objectIndex, objIndex))) {
            instance->active = false;
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinEventInherited(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr || 0 > ctx->currentEventObjectIndex || 0 > ctx->currentEventType) {
        fprintf(stderr, "VM: event_inherited called with no event context (inst=%p, eventObjIdx=%d, eventType=%d)\n", (void*) inst, ctx->currentEventObjectIndex, ctx->currentEventType);
        return RValue_makeReal(0.0);
    }

    DataWin* dataWin = ctx->dataWin;
    int32_t ownerObjectIndex = ctx->currentEventObjectIndex;
    if ((uint32_t) ownerObjectIndex >= dataWin->objt.count) {
        fprintf(stderr, "VM: event_inherited ownerObjectIndex %d out of range\n", ownerObjectIndex);
        return RValue_makeReal(0.0);
    }

    int32_t parentObjectIndex = dataWin->objt.objects[ownerObjectIndex].parentId;
    if (ctx->traceEventInherited) {
        fprintf(stderr, "VM: [%s] event_inherited owner=%s(%d) parent=%s(%d) event=%s (instanceId=%d)\n", dataWin->objt.objects[inst->objectIndex].name, dataWin->objt.objects[ownerObjectIndex].name, ownerObjectIndex, (0 > parentObjectIndex) ? "none" : dataWin->objt.objects[parentObjectIndex].name, parentObjectIndex, Runner_getEventName(ctx->currentEventType, ctx->currentEventSubtype), inst->instanceId);
    }
    if (0 > parentObjectIndex) return RValue_makeReal(0.0);

    Runner_executeEventFromObject(runner, inst, parentObjectIndex, ctx->currentEventType, ctx->currentEventSubtype);
    return RValue_makeReal(0.0);
}

static RValue builtinEventUser(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr) return RValue_makeReal(0.0);

    int32_t subevent = RValue_toInt32(args[0]);
    if (0 > subevent || 15 < subevent) return RValue_makeReal(0.0);

    Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0 + subevent);
    return RValue_makeReal(0.0);
}

static RValue builtinEventPerform(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr) return RValue_makeReal(0.0);

    int32_t eventType = RValue_toInt32(args[0]);
    int32_t eventSubtype = RValue_toInt32(args[1]);

    Runner_executeEvent(runner, inst, eventType, eventSubtype);
    return RValue_makeReal(0.0);
}

static RValue builtinActionKillObject(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (ctx->currentInstance != nullptr) {
        Runner_destroyInstance(runner, (Instance*) ctx->currentInstance);
    }
    return RValue_makeUndefined();
}

static RValue builtinActionCreateObject(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t objectIndex = RValue_toInt32(args[0]);
    GMLReal x = RValue_toReal(args[1]);
    GMLReal y = RValue_toReal(args[2]);
    if (0 > objectIndex || runner->dataWin->objt.count <= (uint32_t) objectIndex) {
        fprintf(stderr, "VM: action_create_object: objectIndex %d out of range\n", objectIndex);
        return RValue_makeUndefined();
    }
    Instance* callerInst = (Instance*) ctx->currentInstance;
    if (ctx->actionRelativeFlag && callerInst != nullptr) {
        x += callerInst->x;
        y += callerInst->y;
    }
    Instance* inst = Runner_createInstance(runner, x, y, objectIndex);
    if (callerInst != nullptr && ctx->creatorVarID >= 0) {
        Instance_setSelfVar(inst, ctx->creatorVarID, RValue_makeReal((GMLReal) callerInst->instanceId));
    }
    return RValue_makeUndefined();
}

static RValue builtinActionSetRelative(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    ctx->actionRelativeFlag = RValue_toInt32(args[0]) != 0;
    return RValue_makeUndefined();
}

static RValue builtinActionMove(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    // action_move(direction_string, speed)
    // Direction string is 9 chars of '0'/'1' encoding a 3x3 direction grid:
    //   Pos: 0=UL(225) 1=U(270) 2=UR(315) 3=L(180) 4=STOP 5=R(0) 6=DL(135) 7=D(90) 8=DR(45)
    char* dirs = RValue_toString(args[0]);
    GMLReal spd = RValue_toReal(args[1]);

    static const GMLReal angles[] = {225, 270, 315, 180, -1, 0, 135, 90, 45};

    // Collect all enabled directions
    int candidates[9];
    int count = 0;
    for (int i = 0; 9 > i && dirs[i] != '\0'; i++) {
        if (dirs[i] == '1') {
            candidates[count++] = i;
        }
    }

    if (count == 0) {
        free(dirs);
        return RValue_makeUndefined();
    }

    // Pick one at random
    int pick = candidates[0 == count - 1 ? 0 : rand() % count];

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        if (4 == pick) {
            // STOP
            if (ctx->actionRelativeFlag) {
                inst->speed += (float) spd;
            } else {
                inst->speed = 0;
            }
        } else {
            GMLReal angle = angles[pick];
            if (ctx->actionRelativeFlag) {
                inst->direction += (float) angle;
                inst->speed += (float) spd;
            } else {
                inst->direction = (float) angle;
                inst->speed = (float) spd;
            }
        }
        Instance_computeComponentsFromSpeed(inst);
    }
    free(dirs);
    return RValue_makeUndefined();
}

static RValue builtinActionMoveTo(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal ax = RValue_toReal(args[0]);
    GMLReal ay = RValue_toReal(args[1]);

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        if (ctx->actionRelativeFlag) {
            inst->x += (float) ax;
            inst->y += (float) ay;
        } else {
            inst->x = (float) ax;
            inst->y = (float) ay;
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinActionSnap(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal hsnap = RValue_toReal(args[0]);
    GMLReal vsnap = RValue_toReal(args[1]);

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        if (hsnap > 0.0) {
            inst->x = (float) ((int32_t) GMLReal_round(inst->x / hsnap) * hsnap);
        }
        if (vsnap > 0.0) {
            inst->y = (float) ((int32_t) GMLReal_round(inst->y / vsnap) * vsnap);
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinActionSetFriction(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal val = RValue_toReal(args[0]);

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        if (ctx->actionRelativeFlag) {
            inst->friction += (float) val;
        } else {
            inst->friction = (float) val;
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinActionSetGravity(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal dir = RValue_toReal(args[0]);
    GMLReal grav = RValue_toReal(args[1]);

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        if (ctx->actionRelativeFlag) {
            inst->gravityDirection += (float) dir;
            inst->gravity += (float) grav;
        } else {
            inst->gravityDirection = (float) dir;
            inst->gravity = (float) grav;
        }
    }
    return RValue_makeUndefined();
}

static RValue builtinActionSetHspeed(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal val = RValue_toReal(args[0]);

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        if (ctx->actionRelativeFlag) {
            inst->hspeed += (float) val;
        } else {
            inst->hspeed = (float) val;
        }
        Instance_computeSpeedFromComponents(inst);
    }
    return RValue_makeUndefined();
}

static RValue builtinActionSetVspeed(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    GMLReal val = RValue_toReal(args[0]);

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        if (ctx->actionRelativeFlag) {
            inst->vspeed += (float) val;
        } else {
            inst->vspeed = (float) val;
        }
        Instance_computeSpeedFromComponents(inst);
    }
    return RValue_makeUndefined();
}

// ===[ GML BUFFER SYSTEM ]===

static int32_t gmlBufferCreate(Runner* runner, int32_t size, int32_t type, int32_t alignment) {
    GmlBuffer buf = {0};
    buf.size = size > 0 ? size : 1;
    buf.data = safeCalloc((size_t) buf.size, 1);
    buf.position = 0;
    buf.usedSize = (type == GML_BUFFER_GROW) ? 0 : buf.size;
    buf.alignment = alignment > 0 ? alignment : 1;
    buf.type = type;
    buf.isValid = true;
    int32_t id = (int32_t) arrlen(runner->gmlBufferPool);
    arrput(runner->gmlBufferPool, buf);
    return id;
}

static GmlBuffer* gmlBufferGet(Runner* runner, int32_t id) {
    if (0 > id || id >= (int32_t) arrlen(runner->gmlBufferPool)) return nullptr;
    GmlBuffer* buf = &runner->gmlBufferPool[id];
    if (!buf->isValid) return nullptr;
    return buf;
}

// Aligns position up to the buffer's alignment boundary
static int32_t gmlBufferAlign(int32_t position, int32_t alignment) {
    if (1 >= alignment) return position;
    return ((position + alignment - 1) / alignment) * alignment;
}

// Ensures the grow buffer has at least newSize bytes allocated
static void gmlBufferEnsureSize(GmlBuffer* buf, int32_t newSize) {
    if (buf->type != GML_BUFFER_GROW || newSize <= buf->size) return;
    // Double or use newSize, whichever is larger
    int32_t newAlloc = buf->size * 2;
    if (newAlloc < newSize) newAlloc = newSize;
    buf->data = safeRealloc(buf->data, (size_t) newAlloc);
    memset(buf->data + buf->size, 0, (size_t) (newAlloc - buf->size));
    buf->size = newAlloc;
}

static RValue builtin_bufferCreate(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t size = RValue_toInt32(args[0]);
    int32_t type = RValue_toInt32(args[1]);
    int32_t alignment = RValue_toInt32(args[2]);
    int32_t id = gmlBufferCreate(runner, size, type, alignment);
    return RValue_makeReal((GMLReal) id);
}

static RValue builtin_bufferDelete(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    GmlBuffer* buf = gmlBufferGet(runner, id);
    if (buf != nullptr) {
        free(buf->data);
        buf->data = nullptr;
        buf->isValid = false;
    }
    return RValue_makeUndefined();
}

static RValue builtin_bufferWrite(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    int32_t dataType = RValue_toInt32(args[1]);
    GmlBuffer* buf = gmlBufferGet(runner, id);
    if (buf == nullptr) return RValue_makeUndefined();

    switch (dataType) {
        case GML_BUFTYPE_U8:
        case GML_BUFTYPE_BOOL: {
            uint8_t val = (uint8_t) RValue_toInt32(args[2]);
            gmlBufferEnsureSize(buf, buf->position + 1);
            if (buf->size > buf->position) buf->data[buf->position] = val;
            buf->position += 1;
            break;
        }
        case GML_BUFTYPE_S8: {
            int8_t val = (int8_t) RValue_toInt32(args[2]);
            gmlBufferEnsureSize(buf, buf->position + 1);
            if (buf->size > buf->position) buf->data[buf->position] = (uint8_t) val;
            buf->position += 1;
            break;
        }
        case GML_BUFTYPE_U16: {
            uint16_t val = (uint16_t) RValue_toInt32(args[2]);
            gmlBufferEnsureSize(buf, buf->position + 2);
            if (buf->position + 2 <= buf->size) {
                buf->data[buf->position] = (uint8_t) (val & 0xFF);
                buf->data[buf->position + 1] = (uint8_t) ((val >> 8) & 0xFF);
            }
            buf->position += 2;
            break;
        }
        case GML_BUFTYPE_S16: {
            int16_t val = (int16_t) RValue_toInt32(args[2]);
            uint16_t uval = (uint16_t) val;
            gmlBufferEnsureSize(buf, buf->position + 2);
            if (buf->position + 2 <= buf->size) {
                buf->data[buf->position] = (uint8_t) (uval & 0xFF);
                buf->data[buf->position + 1] = (uint8_t) ((uval >> 8) & 0xFF);
            }
            buf->position += 2;
            break;
        }
        case GML_BUFTYPE_U32:
        case GML_BUFTYPE_S32: {
            int32_t val = RValue_toInt32(args[2]);
            uint32_t uval = (uint32_t) val;
            gmlBufferEnsureSize(buf, buf->position + 4);
            if (buf->position + 4 <= buf->size) {
                buf->data[buf->position] = (uint8_t) (uval & 0xFF);
                buf->data[buf->position + 1] = (uint8_t) ((uval >> 8) & 0xFF);
                buf->data[buf->position + 2] = (uint8_t) ((uval >> 16) & 0xFF);
                buf->data[buf->position + 3] = (uint8_t) ((uval >> 24) & 0xFF);
            }
            buf->position += 4;
            break;
        }
        case GML_BUFTYPE_F32: {
            float val = (float) RValue_toReal(args[2]);
            gmlBufferEnsureSize(buf, buf->position + 4);
            if (buf->position + 4 <= buf->size) {
                memcpy(buf->data + buf->position, &val, 4);
            }
            buf->position += 4;
            break;
        }
        case GML_BUFTYPE_F64: {
            double val = (double) RValue_toReal(args[2]);
            gmlBufferEnsureSize(buf, buf->position + 8);
            if (buf->position + 8 <= buf->size) {
                memcpy(buf->data + buf->position, &val, 8);
            }
            buf->position += 8;
            break;
        }
        case GML_BUFTYPE_STRING: {
            // Writes string bytes + null terminator
            char* str = RValue_toString(args[2]);
            int32_t len = (int32_t) strlen(str);
            int32_t writeLen = len + 1; // include null terminator
            gmlBufferEnsureSize(buf, buf->position + writeLen);
            if (buf->position + writeLen <= buf->size) {
                memcpy(buf->data + buf->position, str, (size_t) writeLen);
            }
            buf->position += writeLen;
            free(str);
            break;
        }
        case GML_BUFTYPE_TEXT: {
            // Writes string bytes WITHOUT null terminator
            char* str = RValue_toString(args[2]);
            int32_t len = (int32_t) strlen(str);
            gmlBufferEnsureSize(buf, buf->position + len);
            if (buf->position + len <= buf->size) {
                memcpy(buf->data + buf->position, str, (size_t) len);
            }
            buf->position += len;
            free(str);
            break;
        }
        default:
            fprintf(stderr, "buffer_write: unsupported data type %d\n", dataType);
            break;
    }

    buf->position = gmlBufferAlign(buf->position, buf->alignment);
    if (buf->type == GML_BUFFER_GROW && buf->position > buf->usedSize) {
        buf->usedSize = buf->position;
    }

    return RValue_makeUndefined();
}

static RValue builtin_bufferRead(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    int32_t dataType = RValue_toInt32(args[1]);
    GmlBuffer* buf = gmlBufferGet(runner, id);
    if (buf == nullptr) return RValue_makeReal(0.0);

    RValue result = RValue_makeReal(0.0);

    switch (dataType) {
        case GML_BUFTYPE_U8:
        case GML_BUFTYPE_BOOL: {
            if (buf->size > buf->position) {
                result = RValue_makeReal((GMLReal) buf->data[buf->position]);
            }
            buf->position += 1;
            break;
        }
        case GML_BUFTYPE_S8: {
            if (buf->size > buf->position) {
                result = RValue_makeReal((GMLReal) (int8_t) buf->data[buf->position]);
            }
            buf->position += 1;
            break;
        }
        case GML_BUFTYPE_U16: {
            if (buf->position + 2 <= buf->size) {
                uint16_t val = (uint16_t) buf->data[buf->position] | ((uint16_t) buf->data[buf->position + 1] << 8);
                result = RValue_makeReal((GMLReal) val);
            }
            buf->position += 2;
            break;
        }
        case GML_BUFTYPE_S16: {
            if (buf->position + 2 <= buf->size) {
                uint16_t uval = (uint16_t) buf->data[buf->position] | ((uint16_t) buf->data[buf->position + 1] << 8);
                result = RValue_makeReal((GMLReal) (int16_t) uval);
            }
            buf->position += 2;
            break;
        }
        case GML_BUFTYPE_U32: {
            if (buf->position + 4 <= buf->size) {
                uint32_t val = (uint32_t) buf->data[buf->position]
                    | ((uint32_t) buf->data[buf->position + 1] << 8)
                    | ((uint32_t) buf->data[buf->position + 2] << 16)
                    | ((uint32_t) buf->data[buf->position + 3] << 24);
                result = RValue_makeReal((GMLReal) val);
            }
            buf->position += 4;
            break;
        }
        case GML_BUFTYPE_S32: {
            if (buf->position + 4 <= buf->size) {
                uint32_t uval = (uint32_t) buf->data[buf->position]
                    | ((uint32_t) buf->data[buf->position + 1] << 8)
                    | ((uint32_t) buf->data[buf->position + 2] << 16)
                    | ((uint32_t) buf->data[buf->position + 3] << 24);
                result = RValue_makeReal((GMLReal) (int32_t) uval);
            }
            buf->position += 4;
            break;
        }
        case GML_BUFTYPE_F32: {
            if (buf->position + 4 <= buf->size) {
                float val;
                memcpy(&val, buf->data + buf->position, 4);
                result = RValue_makeReal((GMLReal) val);
            }
            buf->position += 4;
            break;
        }
        case GML_BUFTYPE_F64: {
            if (buf->position + 8 <= buf->size) {
                double val;
                memcpy(&val, buf->data + buf->position, 8);
                result = RValue_makeReal((GMLReal) val);
            }
            buf->position += 8;
            break;
        }
        case GML_BUFTYPE_STRING: {
            // Read until null terminator or end of buffer
            int32_t start = buf->position;
            while (buf->size > buf->position && buf->data[buf->position] != '\0') {
                buf->position++;
            }
            int32_t len = buf->position - start;
            char* str = safeMalloc((size_t) len + 1);
            memcpy(str, buf->data + start, (size_t) len);
            str[len] = '\0';
            // Skip past the null terminator
            if (buf->size > buf->position) buf->position++;
            result = RValue_makeOwnedString(str);
            break;
        }
        case GML_BUFTYPE_TEXT: {
            // Read all remaining bytes as text (no null terminator delimiter)
            int32_t start = buf->position;
            int32_t len = buf->size - start;
            if (0 > len) len = 0;
            char* str = safeMalloc((size_t) len + 1);
            if (len > 0) memcpy(str, buf->data + start, (size_t) len);
            str[len] = '\0';
            buf->position = buf->size;
            result = RValue_makeOwnedString(str);
            break;
        }
        default:
            fprintf(stderr, "buffer_read: unsupported data type %d\n", dataType);
            break;
    }

    buf->position = gmlBufferAlign(buf->position, buf->alignment);
    return result;
}

static RValue builtin_bufferSeek(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    int32_t seekMode = RValue_toInt32(args[1]);
    int32_t offset = RValue_toInt32(args[2]);
    GmlBuffer* buf = gmlBufferGet(runner, id);
    if (buf == nullptr) return RValue_makeUndefined();

    switch (seekMode) {
        case GML_BUFFER_SEEK_START:
            buf->position = offset;
            break;
        case GML_BUFFER_SEEK_RELATIVE:
            buf->position += offset;
            break;
        case GML_BUFFER_SEEK_END: {
            int32_t endPos = (buf->type == GML_BUFFER_GROW) ? buf->usedSize : buf->size;
            buf->position = endPos + offset;
            break;
        }
    }

    // Clamp position
    if (0 > buf->position) buf->position = 0;
    if (buf->position > buf->size) buf->position = buf->size;

    return RValue_makeUndefined();
}

static RValue builtin_bufferTell(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    GmlBuffer* buf = gmlBufferGet(runner, id);
    if (buf == nullptr) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) buf->position);
}

static RValue builtin_bufferGetSize(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    GmlBuffer* buf = gmlBufferGet(runner, id);
    if (buf == nullptr) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ((buf->type == GML_BUFFER_GROW) ? buf->usedSize : buf->size));
}

static RValue builtin_bufferLoad(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    FileSystem* fs = runner->fileSystem;
    char* filename = RValue_toString(args[0]);

    uint8_t* fileData = nullptr;
    int32_t fileSize = 0;
    bool ok = fs->vtable->readFileBinary(fs, filename, &fileData, &fileSize);
    free(filename);

    if (!ok) return RValue_makeReal(-1.0);

    // Create a fixed buffer with the loaded data
    int32_t id = gmlBufferCreate(runner, fileSize, GML_BUFFER_FIXED, 1);
    GmlBuffer* buf = gmlBufferGet(runner, id);
    free(buf->data);
    buf->data = fileData;
    buf->size = fileSize;
    buf->usedSize = fileSize;
    return RValue_makeReal((GMLReal) id);
}

static RValue builtin_bufferSave(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    FileSystem* fs = runner->fileSystem;
    int32_t id = RValue_toInt32(args[0]);
    char* filename = RValue_toString(args[1]);
    GmlBuffer* buf = gmlBufferGet(runner, id);

    if (buf != nullptr) {
        int32_t saveSize = (buf->type == GML_BUFFER_GROW) ? buf->usedSize : buf->size;
        fs->vtable->writeFileBinary(fs, filename, buf->data, saveSize);
    }

    free(filename);
    return RValue_makeUndefined();
}

STUB_RETURN_ZERO(buffer_base64_encode)

// PSN stubs
STUB_RETURN_UNDEFINED(psn_init)
STUB_RETURN_ZERO(psn_default_user)
STUB_RETURN_ZERO(psn_get_leaderboard_score)

// Draw functions
static RValue builtin_drawSprite(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    float x = (float) RValue_toReal(args[2]);
    float y = (float) RValue_toReal(args[3]);

    // If subimg < 0, use the current instance's imageIndex
    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    Renderer_drawSprite(runner->renderer, spriteIndex, subimg, x, y);
    return RValue_makeUndefined();
}

static RValue builtin_drawSpriteExt(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    float x = (float) RValue_toReal(args[2]);
    float y = (float) RValue_toReal(args[3]);
    float xscale = (float) RValue_toReal(args[4]);
    float yscale = (float) RValue_toReal(args[5]);
    float rot = (float) RValue_toReal(args[6]);
    uint32_t color = (uint32_t) RValue_toInt32(args[7]);
    float alpha = (float) RValue_toReal(args[8]);

    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    Renderer_drawSpriteExt(runner->renderer, spriteIndex, subimg, x, y, xscale, yscale, rot, color, alpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawSpriteTiled(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    float x = (float) RValue_toReal(args[2]);
    float y = (float) RValue_toReal(args[3]);

    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    float roomW = (float) runner->currentRoom->width;
    float roomH = (float) runner->currentRoom->height;
    Renderer_drawSpriteTiled(runner->renderer, spriteIndex, subimg, x, y, 1.0f, 1.0f, roomW, roomH, 0xFFFFFF, runner->renderer->drawAlpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawSpriteTiledExt(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    float x = (float) RValue_toReal(args[2]);
    float y = (float) RValue_toReal(args[3]);
    float xscale = (float) RValue_toReal(args[4]);
    float yscale = (float) RValue_toReal(args[5]);
    uint32_t color = (uint32_t) RValue_toInt32(args[6]);
    float alpha = (float) RValue_toReal(args[7]);

    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    float roomW = (float) runner->currentRoom->width;
    float roomH = (float) runner->currentRoom->height;
    Renderer_drawSpriteTiled(runner->renderer, spriteIndex, subimg, x, y, xscale, yscale, roomW, roomH, color, alpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawSpriteStretched(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    float x = (float) RValue_toReal(args[2]);
    float y = (float) RValue_toReal(args[3]);
    float w = (float) RValue_toReal(args[4]);
    float h = (float) RValue_toReal(args[5]);

    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    Renderer_drawSpriteStretched(runner->renderer, spriteIndex, subimg, x, y, w, h, 0xFFFFFF, runner->renderer->drawAlpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawSpriteStretchedExt(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    float x = (float) RValue_toReal(args[2]);
    float y = (float) RValue_toReal(args[3]);
    float w = (float) RValue_toReal(args[4]);
    float h = (float) RValue_toReal(args[5]);
    uint32_t color = (uint32_t) RValue_toInt32(args[6]);
    float alpha = (float) RValue_toReal(args[7]);

    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    Renderer_drawSpriteStretched(runner->renderer, spriteIndex, subimg, x, y, w, h, color, alpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawSpritePart(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    int32_t left = RValue_toInt32(args[2]);
    int32_t top = RValue_toInt32(args[3]);
    int32_t width = RValue_toInt32(args[4]);
    int32_t height = RValue_toInt32(args[5]);
    float x = (float) RValue_toReal(args[6]);
    float y = (float) RValue_toReal(args[7]);

    // If subimg < 0, use the current instance's imageIndex
    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    Renderer_drawSpritePart(runner->renderer, spriteIndex, subimg, left, top, width, height, x, y);
    return RValue_makeUndefined();
}

static RValue builtin_drawSpritePartExt(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t subimg = RValue_toInt32(args[1]);
    int32_t left = RValue_toInt32(args[2]);
    int32_t top = RValue_toInt32(args[3]);
    int32_t width = RValue_toInt32(args[4]);
    int32_t height = RValue_toInt32(args[5]);
    float x = (float) RValue_toReal(args[6]);
    float y = (float) RValue_toReal(args[7]);
    float xscale = (float) RValue_toReal(args[8]);
    float yscale = (float) RValue_toReal(args[9]);
    uint32_t color = (uint32_t) RValue_toInt32(args[10]);
    float alpha = (float) RValue_toReal(args[11]);

    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ((Instance*) ctx->currentInstance)->imageIndex;
    }

    Renderer_drawSpritePartExt(runner->renderer, spriteIndex, subimg, left, top, width, height, x, y, xscale, yscale, color, alpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawRectangle(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x1 = (float) RValue_toReal(args[0]);
    float y1 = (float) RValue_toReal(args[1]);
    float x2 = (float) RValue_toReal(args[2]);
    float y2 = (float) RValue_toReal(args[3]);
    bool outline = RValue_toBool(args[4]);

    runner->renderer->vtable->drawRectangle(runner->renderer, x1, y1, x2, y2, runner->renderer->drawColor, runner->renderer->drawAlpha, outline);
    return RValue_makeUndefined();
}

static RValue builtin_drawRectangleColor(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) { 
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x1 = (float) RValue_toReal(args[0]);
    float y1 = (float) RValue_toReal(args[1]);
    float x2 = (float) RValue_toReal(args[2]);
    float y2 = (float) RValue_toReal(args[3]);
    uint32_t color = (uint32_t) RValue_toInt32(args[4]);
    bool outline = RValue_toBool(args[8]);

    runner->renderer->vtable->drawRectangle(runner->renderer, x1, y1, x2, y2, color, runner->renderer->drawAlpha, outline);
    return RValue_makeUndefined();
}

static RValue builtin_drawHealthbar(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x1 = (float) RValue_toReal(args[0]);
    float y1 = (float) RValue_toReal(args[1]);
    float x2 = (float) RValue_toReal(args[2]);
    float y2 = (float) RValue_toReal(args[3]);
    float amount = (float) RValue_toReal(args[4]);

    amount = amount / (float)100; // 0 - 1;
    float healthbarX = (x1 * (1-amount) + x2 * amount);
    //float healthbarY = (y1 * (1-amount) + y2 * amount);

    uint32_t backCol = (uint32_t) RValue_toInt32(args[5]);
    uint32_t minCol = (uint32_t) RValue_toInt32(args[6]);
    uint32_t maxCol = (uint32_t) RValue_toInt32(args[7]);
    uint32_t intermediateColor = Renderer_mixColors(minCol,maxCol,amount);

    int32_t direction = RValue_toInt32(args[8]);
    
    bool showBack = RValue_toBool(args[9]);

    if (showBack) {
        runner->renderer->vtable->drawRectangle(runner->renderer, x1,y1,x2,y2,backCol, runner->renderer->drawAlpha, false);
    }

    runner->renderer->vtable->drawRectangle(runner->renderer,x1,y1,healthbarX,y2,intermediateColor, runner->renderer->drawAlpha, false);
    (void)direction;
    return RValue_makeUndefined();
}

static RValue builtin_drawSetColor(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        runner->renderer->drawColor = (uint32_t) RValue_toInt32(args[0]);
    }
    return RValue_makeUndefined();
}

static RValue builtin_drawSetAlpha(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        runner->renderer->drawAlpha = (float) RValue_toReal(args[0]);
    }
    return RValue_makeUndefined();
}

static RValue builtin_drawSetFont(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        runner->renderer->drawFont = RValue_toInt32(args[0]);
    }
    return RValue_makeUndefined();
}

static RValue builtin_drawSetHalign(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        runner->renderer->drawHalign = RValue_toInt32(args[0]);
    }
    return RValue_makeUndefined();
}

static RValue builtin_drawSetValign(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        runner->renderer->drawValign = RValue_toInt32(args[0]);
    }
    return RValue_makeUndefined();
}

static RValue builtin_drawText(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x = (float) RValue_toReal(args[0]);
    float y = (float) RValue_toReal(args[1]);
    char* str = RValue_toString(args[2]);

    PreprocessedText processedText = TextUtils_preprocessGmlTextIfNeeded(runner, str);
    runner->renderer->vtable->drawText(runner->renderer, processedText.text, x, y, 1.0f, 1.0f, 0.0f);
    PreprocessedText_free(processedText);
    free(str);
    return RValue_makeUndefined();
}

static RValue builtin_drawTextTransformed(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x = (float) RValue_toReal(args[0]);
    float y = (float) RValue_toReal(args[1]);
    char* str = RValue_toString(args[2]);
    float xscale = (float) RValue_toReal(args[3]);
    float yscale = (float) RValue_toReal(args[4]);
    float angle = (float) RValue_toReal(args[5]);

    PreprocessedText processedText = TextUtils_preprocessGmlTextIfNeeded(runner, str);
    runner->renderer->vtable->drawText(runner->renderer, processedText.text, x, y, xscale, yscale, angle);
    PreprocessedText_free(processedText);
    free(str);
    return RValue_makeUndefined();
}
STUB_RETURN_UNDEFINED(draw_text_ext)
STUB_RETURN_UNDEFINED(draw_text_ext_transformed)

static RValue builtin_drawTextColor(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x = (float) RValue_toReal(args[0]);
    float y = (float) RValue_toReal(args[1]);
    char* str = RValue_toString(args[2]);
    int32_t c1 = (float) RValue_toInt32(args[3]);
    int32_t c2 = (float) RValue_toInt32(args[4]);
    int32_t c3 = (float) RValue_toInt32(args[5]);
    int32_t c4 = (float) RValue_toInt32(args[6]);
    float alpha = (float) RValue_toReal(args[7]);

    PreprocessedText processedText = TextUtils_preprocessGmlTextIfNeeded(runner, str);
    runner->renderer->vtable->drawTextColor(runner->renderer, processedText.text, x, y, 1.0f, 1.0f, 0.0f, c1, c2, c3, c4, alpha);
    PreprocessedText_free(processedText);
    free(str);
    return RValue_makeUndefined();
}

static RValue builtin_drawTextColorTransformed(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();

    float x = (float) RValue_toReal(args[0]);
    float y = (float) RValue_toReal(args[1]);
    char* str = RValue_toString(args[2]);
    float xscale = (float) RValue_toReal(args[3]);
    float yscale = (float) RValue_toReal(args[4]);
    float angle = (float) RValue_toReal(args[5]);
    int32_t c1 = (float) RValue_toInt32(args[6]);
    int32_t c2 = (float) RValue_toInt32(args[7]);
    int32_t c3 = (float) RValue_toInt32(args[8]);
    int32_t c4 = (float) RValue_toInt32(args[9]);
    float alpha = (float) RValue_toReal(args[10]);

    PreprocessedText processedText = TextUtils_preprocessGmlTextIfNeeded(runner, str);
    runner->renderer->vtable->drawTextColor(runner->renderer, processedText.text, x, y, xscale, yscale, angle, c1, c2, c3, c4, alpha);
    PreprocessedText_free(processedText);
    free(str);
    return RValue_makeUndefined();
}
STUB_RETURN_UNDEFINED(draw_text_color_ext)
STUB_RETURN_UNDEFINED(draw_text_color_ext_transformed)

STUB_RETURN_UNDEFINED(draw_surface)
STUB_RETURN_UNDEFINED(draw_surface_ext)
static RValue builtin_drawBackground(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr || 3 > argCount) return RValue_makeUndefined();

    int32_t bgIndex = RValue_toInt32(args[0]);
    float x = (float) RValue_toReal(args[1]);
    float y = (float) RValue_toReal(args[2]);

    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(runner->dataWin, bgIndex);
    if (0 > tpagIndex) return RValue_makeUndefined();

    runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, x, y, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFF, runner->renderer->drawAlpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawBackgroundExt(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr || 8 > argCount) return RValue_makeUndefined();

    int32_t bgIndex = RValue_toInt32(args[0]);
    float x = (float) RValue_toReal(args[1]);
    float y = (float) RValue_toReal(args[2]);
    float xscale = (float) RValue_toReal(args[3]);
    float yscale = (float) RValue_toReal(args[4]);
    float rot = (float) RValue_toReal(args[5]);
    uint32_t color = (uint32_t) RValue_toInt32(args[6]);
    float alpha = (float) RValue_toReal(args[7]);

    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(runner->dataWin, bgIndex);
    if (0 > tpagIndex) return RValue_makeUndefined();

    runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, x, y, 0.0f, 0.0f, xscale, yscale, rot, color, alpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawBackgroundStretched(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr || 5 > argCount) return RValue_makeUndefined();

    int32_t bgIndex = RValue_toInt32(args[0]);
    float x = (float) RValue_toReal(args[1]);
    float y = (float) RValue_toReal(args[2]);
    float w = (float) RValue_toReal(args[3]);
    float h = (float) RValue_toReal(args[4]);

    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(runner->dataWin, bgIndex);
    if (0 > tpagIndex) return RValue_makeUndefined();

    TexturePageItem* tpag = &runner->dataWin->tpag.items[tpagIndex];
    float xscale = w / (float) tpag->boundingWidth;
    float yscale = h / (float) tpag->boundingHeight;

    runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, x, y, 0.0f, 0.0f, xscale, yscale, 0.0f, 0xFFFFFF, runner->renderer->drawAlpha);
    return RValue_makeUndefined();
}

static RValue builtin_drawBackgroundPartExt(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr || 11 > argCount) return RValue_makeUndefined();

    int32_t bgIndex = RValue_toInt32(args[0]);
    int32_t left = RValue_toInt32(args[1]);
    int32_t top = RValue_toInt32(args[2]);
    int32_t width = RValue_toInt32(args[3]);
    int32_t height = RValue_toInt32(args[4]);
    float x = (float) RValue_toReal(args[5]);
    float y = (float) RValue_toReal(args[6]);
    float xscale = (float) RValue_toReal(args[7]);
    float yscale = (float) RValue_toReal(args[8]);
    uint32_t color = (uint32_t) RValue_toInt32(args[9]);
    float alpha = (float) RValue_toReal(args[10]);

    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(runner->dataWin, bgIndex);
    if (0 > tpagIndex) return RValue_makeUndefined();

    runner->renderer->vtable->drawSpritePart(runner->renderer, tpagIndex, left, top, width, height, x, y, xscale, yscale, color, alpha);
    return RValue_makeUndefined();
}

static RValue builtinBackgroundGetWidth(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    int32_t bgIndex = RValue_toInt32(args[0]);
    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(ctx->dataWin, bgIndex);
    if (0 > tpagIndex) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->tpag.items[tpagIndex].boundingWidth);
}

static RValue builtinBackgroundGetHeight(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    int32_t bgIndex = RValue_toInt32(args[0]);
    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(ctx->dataWin, bgIndex);
    if (0 > tpagIndex) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->tpag.items[tpagIndex].boundingHeight);
}

static RValue builtin_draw_self(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr && ctx->currentInstance != nullptr) {
        Renderer_drawSelf(runner->renderer, (Instance*) ctx->currentInstance);
    }
    return RValue_makeUndefined();
}

// draw_line(x1, y1, x2, y2)
static RValue builtin_draw_line(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        float x1 = (float) RValue_toReal(args[0]);
        float y1 = (float) RValue_toReal(args[1]);
        float x2 = (float) RValue_toReal(args[2]);
        float y2 = (float) RValue_toReal(args[3]);
        runner->renderer->vtable->drawLine(runner->renderer, x1, y1, x2, y2, 1.0f, runner->renderer->drawColor, runner->renderer->drawAlpha);
    }
    return RValue_makeUndefined();
}

// draw_line_width(x1, y1, x2, y2, w)
static RValue builtin_draw_line_width(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        float x1 = (float) RValue_toReal(args[0]);
        float y1 = (float) RValue_toReal(args[1]);
        float x2 = (float) RValue_toReal(args[2]);
        float y2 = (float) RValue_toReal(args[3]);
        float w = (float) RValue_toReal(args[4]);
        runner->renderer->vtable->drawLine(runner->renderer, x1, y1, x2, y2, w, runner->renderer->drawColor, runner->renderer->drawAlpha);
    }
    return RValue_makeUndefined();
}

// draw_line_width_colour(x1, y1, x2, y2, w, col1, col2)
static RValue builtin_draw_line_width_colour(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        float x1 = (float) RValue_toReal(args[0]);
        float y1 = (float) RValue_toReal(args[1]);
        float x2 = (float) RValue_toReal(args[2]);
        float y2 = (float) RValue_toReal(args[3]);
        float w = (float) RValue_toReal(args[4]);
        uint32_t col1 = (uint32_t) RValue_toInt32(args[5]);
        uint32_t col2 = (uint32_t) RValue_toInt32(args[6]);
        runner->renderer->vtable->drawLineColor(runner->renderer, x1, y1, x2, y2, w, col1, col2, runner->renderer->drawAlpha);
    }
    return RValue_makeUndefined();
}

// draw_triangle(x1, y1, x2, y2, x3, y3, outline)
static RValue builtin_draw_triangle(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        float x1 = (float) RValue_toReal(args[0]);
        float y1 = (float) RValue_toReal(args[1]);
        float x2 = (float) RValue_toReal(args[2]);
        float y2 = (float) RValue_toReal(args[3]);
        float x3 = (float) RValue_toReal(args[4]);
        float y3 = (float) RValue_toReal(args[5]);
        bool outline = (float) RValue_toBool(args[6]);
        runner->renderer->vtable->drawTriangle(runner->renderer, x1, y1, x2, y2, x3, y3, outline);
    }
    return RValue_makeUndefined();
}

static RValue builtin_draw_set_colour(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        runner->renderer->drawColor = (uint32_t) RValue_toInt32(args[0]);
    }
    return RValue_makeUndefined();
}

static RValue builtin_draw_get_colour(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        return RValue_makeReal((GMLReal) runner->renderer->drawColor);
    }
    return RValue_makeReal(0.0);
}

static RValue builtin_draw_get_color(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        return RValue_makeReal((GMLReal) runner->renderer->drawColor);
    }
    return RValue_makeReal(0.0);
}

static RValue builtin_draw_get_alpha(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer != nullptr) {
        return RValue_makeReal((GMLReal) runner->renderer->drawAlpha);
    }
    return RValue_makeReal(0.0);
}

// merge_color(col1, col2, amount) - lerps between two colors
static RValue builtinMergeColor(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t col1 = RValue_toInt32(args[0]);
    int32_t col2 = RValue_toInt32(args[1]);
    GMLReal amount = RValue_toReal(args[2]);

    int32_t b1 = (col1 >> 16) & 0xFF;
    int32_t g1 = (col1 >> 8) & 0xFF;
    int32_t r1 = col1 & 0xFF;

    int32_t b2 = (col2 >> 16) & 0xFF;
    int32_t g2 = (col2 >> 8) & 0xFF;
    int32_t r2 = col2 & 0xFF;

    GMLReal inv = 1.0 - amount;
    int32_t r = (int32_t) (r1 * inv + r2 * amount);
    int32_t g = (int32_t) (g1 * inv + g2 * amount);
    int32_t b = (int32_t) (b1 * inv + b2 * amount);

    return RValue_makeReal((GMLReal) (((b << 16) & 0xFF0000) | ((g << 8) & 0xFF00) | (r & 0xFF)));
}

// Surface stubs
STUB_RETURN_ZERO(surface_create)
STUB_RETURN_UNDEFINED(surface_free)
STUB_RETURN_UNDEFINED(surface_set_target)
STUB_RETURN_UNDEFINED(surface_reset_target)
STUB_RETURN_ZERO(surface_exists)
// application_surface is surface ID -1 (sentinel); for it, return the window dimensions
static RValue builtinSurfaceGetWidth(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t surfaceId = (int32_t) RValue_toReal(args[0]);
    if (surfaceId == -1) {
        return RValue_makeReal((GMLReal) ctx->dataWin->gen8.defaultWindowWidth);
    }
    logStubbedFunction(ctx, "surface_get_width");
    return RValue_makeReal(0.0);
}

static RValue builtinSurfaceGetHeight(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t surfaceId = (int32_t) RValue_toReal(args[0]);
    if (surfaceId == -1) {
        return RValue_makeReal((GMLReal) ctx->dataWin->gen8.defaultWindowHeight);
    }
    logStubbedFunction(ctx, "surface_get_height");
    return RValue_makeReal(0.0);
}

// Sprite functions
static RValue builtin_spriteGetWidth(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t spriteIndex = (int32_t) RValue_toReal(args[0]);
    if (0 > spriteIndex || (uint32_t) spriteIndex >= ctx->dataWin->sprt.count) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->sprt.sprites[spriteIndex].width);
}

static RValue builtin_spriteGetHeight(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t spriteIndex = (int32_t) RValue_toReal(args[0]);
    if (0 > spriteIndex || (uint32_t) spriteIndex >= ctx->dataWin->sprt.count) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->sprt.sprites[spriteIndex].height);
}

static RValue builtin_spriteGetNumber(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t spriteIndex = (int32_t) RValue_toReal(args[0]);
    if (0 > spriteIndex || (uint32_t) spriteIndex >= ctx->dataWin->sprt.count) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->sprt.sprites[spriteIndex].textureCount);
}

static RValue builtin_spriteGetXOffset(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t spriteIndex = (int32_t) RValue_toReal(args[0]);
    if (0 > spriteIndex || (uint32_t) spriteIndex >= ctx->dataWin->sprt.count) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->sprt.sprites[spriteIndex].originX);
}

static RValue builtin_spriteGetYOffset(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t spriteIndex = (int32_t) RValue_toReal(args[0]);
    if (0 > spriteIndex || (uint32_t) spriteIndex >= ctx->dataWin->sprt.count) return RValue_makeReal(0.0);
    return RValue_makeReal((GMLReal) ctx->dataWin->sprt.sprites[spriteIndex].originY);
}

static RValue builtin_spriteGetName(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t spriteIndex = (int32_t) RValue_toReal(args[0]);
    if (0 > spriteIndex || (uint32_t) spriteIndex >= ctx->dataWin->sprt.count) return RValue_makeString("<undefined>");
    const char* name = ctx->dataWin->sprt.sprites[spriteIndex].name;
    return RValue_makeString(name != nullptr ? name : "<undefined>");
}

// sprite_set_offset(sprite_index, xoff, yoff)
static RValue builtin_spriteSetOffset(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t spriteIndex = (int32_t) RValue_toReal(args[0]);
    if (0 > spriteIndex || (uint32_t) spriteIndex >= ctx->dataWin->sprt.count) return RValue_makeReal(0.0);
    ctx->dataWin->sprt.sprites[spriteIndex].originX = (int32_t) RValue_toReal(args[1]);
    ctx->dataWin->sprt.sprites[spriteIndex].originY = (int32_t) RValue_toReal(args[2]);
    return RValue_makeReal(0.0);
}

// sprite_create_from_surface(surface_id, x, y, w, h, removeback, smooth, xorig, yorig)
static RValue builtin_spriteCreateFromSurface(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr || runner->renderer->vtable->createSpriteFromSurface == nullptr) return RValue_makeReal(-1);

    // surface_id (arg0) is ignored - we always capture from the application surface (FBO)
    int32_t x = RValue_toInt32(args[1]);
    int32_t y = RValue_toInt32(args[2]);
    int32_t w = RValue_toInt32(args[3]);
    int32_t h = RValue_toInt32(args[4]);
    bool removeback = RValue_toBool(args[5]);
    bool smooth = RValue_toBool(args[6]);
    int32_t xorig = RValue_toInt32(args[7]);
    int32_t yorig = RValue_toInt32(args[8]);

    int32_t result = runner->renderer->vtable->createSpriteFromSurface(runner->renderer, x, y, w, h, removeback, smooth, xorig, yorig);
    return RValue_makeReal((GMLReal) result);
}

// sprite_delete(sprite_index)
static RValue builtin_spriteDelete(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    if (runner->renderer == nullptr || runner->renderer->vtable->deleteSprite == nullptr) return RValue_makeUndefined();

    int32_t spriteIndex = RValue_toInt32(args[0]);
    runner->renderer->vtable->deleteSprite(runner->renderer, spriteIndex);
    return RValue_makeUndefined();
}

// Font/text measurement
static RValue builtin_stringWidth(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    Renderer* renderer = runner->renderer;
    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || renderer->dataWin->font.count <= (uint32_t) fontIndex) return RValue_makeReal(0.0);

    Font* font = &renderer->dataWin->font.fonts[fontIndex];
    char* str = RValue_toString(args[0]);

    PreprocessedText processed = TextUtils_preprocessGmlTextIfNeeded(runner, str);
    int32_t textLen = (int32_t) strlen(processed.text);

    // Find the widest line
    float maxWidth = 0;
    int32_t lineStart = 0;
    while (textLen >= lineStart) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed.text[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, processed.text + lineStart, lineLen);
        if (lineWidth > maxWidth) maxWidth = lineWidth;

        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(processed.text, lineEnd, textLen);
        } else {
            break;
        }
    }

    PreprocessedText_free(processed);
    free(str);
    return RValue_makeReal((GMLReal) (maxWidth * font->scaleX));
}

static RValue builtin_stringHeight(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeReal(0.0);
    Runner* runner = (Runner*) ctx->runner;
    Renderer* renderer = runner->renderer;
    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || renderer->dataWin->font.count <= (uint32_t) fontIndex) return RValue_makeReal(0.0);

    Font* font = &renderer->dataWin->font.fonts[fontIndex];
    char* str = RValue_toString(args[0]);

    PreprocessedText processed = TextUtils_preprocessGmlTextIfNeeded(runner, str);
    int32_t textLen = (int32_t) strlen(processed.text);
    int32_t lineCount = TextUtils_countLines(processed.text, textLen);
    PreprocessedText_free(processed);
    free(str);

    // Match HTML5 runner: string_height = lines * TextHeight('M') = lines * max_glyph_height * scaleY.
    return RValue_makeReal((GMLReal) ((float) lineCount * TextUtils_lineStride(font) * font->scaleY));
}

STUB_RETURN_ZERO(string_width_ext)
STUB_RETURN_ZERO(string_height_ext)

// Color functions
static RValue builtinMakeColor(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal(0.0);
    int32_t r = RValue_toInt32(args[0]);
    int32_t g = RValue_toInt32(args[1]);
    int32_t b = RValue_toInt32(args[2]);
    return RValue_makeReal((GMLReal) (r | (g << 8) | (b << 16)));
}

static RValue builtinMakeColour(VMContext* ctx, RValue* args, int32_t argCount) {
    return builtinMakeColor(ctx, args, argCount);
}

static RValue builtinMakeColorHsv(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal(0.0);
    // GML uses 0-255 range for H, S, V
    GMLReal h = RValue_toReal(args[0]) / 255.0 * 360.0;
    GMLReal s = RValue_toReal(args[1]) / 255.0;
    GMLReal v = RValue_toReal(args[2]) / 255.0;

    GMLReal c = v * s;
    GMLReal x = c * (1.0 - GMLReal_fabs(GMLReal_fmod(h / 60.0, 2.0) - 1.0));
    GMLReal m = v - c;

    GMLReal r1, g1, b1;
    if (360.0 > h && h >= 300.0)      { r1 = c; g1 = 0; b1 = x; }
    else if (300.0 > h && h >= 240.0) { r1 = x; g1 = 0; b1 = c; }
    else if (240.0 > h && h >= 180.0) { r1 = 0; g1 = x; b1 = c; }
    else if (180.0 > h && h >= 120.0) { r1 = 0; g1 = c; b1 = x; }
    else if (120.0 > h && h >= 60.0)  { r1 = x; g1 = c; b1 = 0; }
    else                               { r1 = c; g1 = x; b1 = 0; }

    int32_t r = (int32_t) GMLReal_round((r1 + m) * 255.0);
    int32_t g = (int32_t) GMLReal_round((g1 + m) * 255.0);
    int32_t b = (int32_t) GMLReal_round((b1 + m) * 255.0);

    return RValue_makeReal((GMLReal) (r | (g << 8) | (b << 16)));
}

static RValue builtinMakeColourHsv(VMContext* ctx, RValue* args, int32_t argCount) {
    return builtinMakeColorHsv(ctx, args, argCount);
}

// Display stubs
STUB_RETURN_VALUE(display_get_width, 640.0)
STUB_RETURN_VALUE(display_get_height, 480.0)

static int32_t resolveGuiWidth(Runner* runner) {
    if (runner->guiWidth > 0) return runner->guiWidth;
    Room* room = runner->currentRoom;
    if (room != nullptr) {
        repeat(8, vi) {
            if (room->views[vi].enabled && room->views[vi].portWidth > 0) {
                return room->views[vi].portWidth;
            }
        }
        if (room->width > 0) return (int32_t) room->width;
    }
    return 320;
}

static int32_t resolveGuiHeight(Runner* runner) {
    if (runner->guiHeight > 0) return runner->guiHeight;
    Room* room = runner->currentRoom;
    if (room != nullptr) {
        repeat(8, vi) {
            if (room->views[vi].enabled && room->views[vi].portHeight > 0) {
                return room->views[vi].portHeight;
            }
        }
        if (room->height > 0) return (int32_t) room->height;
    }
    return 240;
}

static RValue builtinDisplayGetGuiWidth(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    return RValue_makeInt32(resolveGuiWidth(runner));
}

static RValue builtinDisplayGetGuiHeight(MAYBE_UNUSED VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    return RValue_makeInt32(resolveGuiHeight(runner));
}

static RValue builtinDisplaySetGuiSize(VMContext* ctx, RValue* args, int32_t argCount) {
    if (2 > argCount) return RValue_makeUndefined();
    Runner* runner = (Runner*) ctx->runner;
    int32_t w = RValue_toInt32(args[0]);
    int32_t h = RValue_toInt32(args[1]);
    runner->guiWidth = w > 0 ? w : 0;
    runner->guiHeight = h > 0 ? h : 0;
    return RValue_makeUndefined();
}

static RValue builtinDisplaySetGuiMaximise(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    // GMS: display_set_gui_maximise(xscale, yscale, xoffset, yoffset). We don't support scaling yet; reset to auto (match view).
    Runner* runner = (Runner*) ctx->runner;
    runner->guiWidth = 0;
    runner->guiHeight = 0;
    return RValue_makeUndefined();
}

// place_meeting(x, y, obj) - returns true if the calling instance would collide with obj at position (x, y)
static RValue builtinPlaceMeeting(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeBool(false);

    Runner* runner = (Runner*) ctx->runner;
    Instance* caller = (Instance*) ctx->currentInstance;
    if (caller == nullptr) return RValue_makeBool(false);

    GMLReal testX = RValue_toReal(args[0]);
    GMLReal testY = RValue_toReal(args[1]);
    int32_t target = RValue_toInt32(args[2]);

    // Save current position and temporarily move to test position
    GMLReal savedX = caller->x;
    GMLReal savedY = caller->y;
    caller->x = testX;
    caller->y = testY;

    InstanceBBox callerBBox = Collision_computeBBox(runner->dataWin, caller);
    bool found = false;

    if (callerBBox.valid) {
        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        repeat(instanceCount, i) {
            Instance* other = runner->instances[i];
            if (!other->active || other == caller) continue;
            if (!Collision_matchesTarget(runner->dataWin, other, target)) continue;

            InstanceBBox otherBBox = Collision_computeBBox(runner->dataWin, other);
            if (!otherBBox.valid) continue;

            if (Collision_instancesOverlapPrecise(runner->dataWin, caller, other, callerBBox, otherBBox)) {
                found = true;
                break;
            }
        }
    }

    // Restore original position
    caller->x = savedX;
    caller->y = savedY;

    return RValue_makeBool(found);
}
// collision_line(x1, y1, x2, y2, obj, prec, notme)
static RValue builtinCollisionLine(VMContext* ctx, RValue* args, int32_t argCount) {
    if (7 > argCount) return RValue_makeReal((GMLReal) INSTANCE_NOONE);

    Runner* runner = (Runner*) ctx->runner;
    GMLReal lx1 = RValue_toReal(args[0]);
    GMLReal ly1 = RValue_toReal(args[1]);
    GMLReal lx2 = RValue_toReal(args[2]);
    GMLReal ly2 = RValue_toReal(args[3]);
    int32_t targetObjIndex = RValue_toInt32(args[4]);
    int32_t prec = RValue_toInt32(args[5]);
    int32_t notme = RValue_toInt32(args[6]);

    Instance* self = (Instance*) ctx->currentInstance;
    int32_t count = (int32_t) arrlen(runner->instances);

    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;
        if (notme && inst == self) continue;

        if (!Collision_matchesTarget(ctx->dataWin, inst, targetObjIndex)) continue;

        InstanceBBox bbox = Collision_computeBBox(ctx->dataWin, inst);
        if (!bbox.valid) continue;

        // Fast reject: line's bounding rect vs instance bbox
        GMLReal lineLeft   = GMLReal_fmin(lx1, lx2);
        GMLReal lineRight  = GMLReal_fmax(lx1, lx2);
        GMLReal lineTop    = GMLReal_fmin(ly1, ly2);
        GMLReal lineBottom = GMLReal_fmax(ly1, ly2);
        // bbox.right/bbox.bottom are exclusive (marginRight + 1, marginBottom + 1), so use >= for those comparisons to correctly exclude boundary-touching cases
        // See GM-HTML5's yyInstance.js "Collision_Line"
        if (lineLeft >= bbox.right)
            continue;
        if (bbox.left > lineRight)
            continue;
        if (bbox.top > lineBottom)
            continue;
        if (lineTop >= bbox.bottom)
            continue;
        
        // Normalize line left-to-right for clipping
        GMLReal xl = lx1, yl = ly1, xr = lx2, yr = ly2;
        if (xl > xr) { GMLReal tmp = xl; xl = xr; xr = tmp; tmp = yl; yl = yr; yr = tmp; }

        GMLReal dx = xr - xl;
        GMLReal dy = yr - yl;

        // Clip line to bbox horizontally
        if (GMLReal_fabs(dx) > 0.0001) {
            if (bbox.left > xl) {
                GMLReal t = (bbox.left - xl) / dx;
                xl = bbox.left;
                yl = yl + t * dy;
            }
            if (xr > bbox.right) {
                GMLReal t = (bbox.right - xl) / (xr - xl);
                yr = yl + t * (yr - yl);
                xr = bbox.right;
            }
        }

        // Y-bounds check after horizontal clipping
        GMLReal clippedTop    = GMLReal_fmin(yl, yr);
        GMLReal clippedBottom = GMLReal_fmax(yl, yr);
        if (bbox.top > clippedBottom || clippedTop >= bbox.bottom) continue;

        // Bbox-only mode: collision confirmed
        if (prec == 0) {
            return RValue_makeReal((GMLReal) inst->instanceId);
        }

        // Precise mode: walk line pixel-by-pixel within bbox
        Sprite* spr = Collision_getSprite(ctx->dataWin, inst);
        if (spr == nullptr || spr->sepMasks != 1 || spr->masks == nullptr || spr->maskCount == 0) {
            // No precise mask available, treat as bbox hit
            return RValue_makeReal((GMLReal) inst->instanceId);
        }

        // Recompute dx/dy for the clipped segment
        GMLReal cdx = xr - xl;
        GMLReal cdy = yr - yl;
        bool found = false;

        if (GMLReal_fabs(cdy) >= GMLReal_fabs(cdx)) {
            // Vertical-major: normalize top-to-bottom
            GMLReal xt = xl, yt = yl, xb = xr, yb = yr;
            if (yt > yb) { GMLReal tmp = xt; xt = xb; xb = tmp; tmp = yt; yt = yb; yb = tmp; }
            GMLReal vdx = xb - xt;
            GMLReal vdy = yb - yt;

            int32_t startY = (int32_t) GMLReal_fmax(bbox.top, yt);
            int32_t endY   = (int32_t) GMLReal_fmin(bbox.bottom, yb);
            for (int32_t py = startY; endY >= py && !found; py++) {
                GMLReal px = (GMLReal_fabs(vdy) > 0.0001) ? xt + ((GMLReal) py - yt) * vdx / vdy : xt;
                if (Collision_pointInInstance(spr, inst, px + 0.5, (GMLReal) py + 0.5)) {
                    found = true;
                }
            }
        } else {
            // Horizontal-major
            int32_t startX = (int32_t) GMLReal_fmax(bbox.left, xl);
            int32_t endX   = (int32_t) GMLReal_fmin(bbox.right, xr);
            for (int32_t px = startX; endX >= px && !found; px++) {
                GMLReal py = (GMLReal_fabs(cdx) > 0.0001) ? yl + ((GMLReal) px - xl) * cdy / cdx : yl;
                if (Collision_pointInInstance(spr, inst, (GMLReal) px + 0.5, py + 0.5)) {
                    found = true;
                }
            }
        }

        if (!found) continue;
        return RValue_makeReal((GMLReal) inst->instanceId);
    }

    return RValue_makeReal((GMLReal) INSTANCE_NOONE);
}

// collision_rectangle(x1, y1, x2, y2, obj, prec, notme)
static RValue builtinCollisionRectangle(VMContext* ctx, RValue* args, int32_t argCount) {
    if (7 > argCount) return RValue_makeReal((GMLReal) INSTANCE_NOONE);

    Runner* runner = (Runner*) ctx->runner;
    GMLReal x1 = RValue_toReal(args[0]);
    GMLReal y1 = RValue_toReal(args[1]);
    GMLReal x2 = RValue_toReal(args[2]);
    GMLReal y2 = RValue_toReal(args[3]);
    int32_t targetObjIndex = RValue_toInt32(args[4]);
    int32_t prec = RValue_toInt32(args[5]);
    int32_t notme = RValue_toInt32(args[6]);

    // Normalize rect
    if (x1 > x2) { GMLReal tmp = x1; x1 = x2; x2 = tmp; }
    if (y1 > y2) { GMLReal tmp = y1; y1 = y2; y2 = tmp; }

    Instance* self = (Instance*) ctx->currentInstance;
    int32_t count = (int32_t) arrlen(runner->instances);

    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;
        if (notme && inst == self) continue;

        if (!Collision_matchesTarget(ctx->dataWin, inst, targetObjIndex)) continue;

        InstanceBBox bbox = Collision_computeBBox(ctx->dataWin, inst);
        if (!bbox.valid) continue;

        // AABB overlap test
        if (x1 >= bbox.right || bbox.left >= x2 || y1 >= bbox.bottom || bbox.top >= y2) continue;

        // Precise check if requested and sprite has precise masks
        if (prec != 0) {
            Sprite* spr = Collision_getSprite(ctx->dataWin, inst);
            if (Collision_hasFrameMasks(spr)) {
                // Check if any pixel in the overlap region hits the mask
                GMLReal iLeft   = GMLReal_fmax(x1, bbox.left);
                GMLReal iRight  = GMLReal_fmin(x2, bbox.right);
                GMLReal iTop    = GMLReal_fmax(y1, bbox.top);
                GMLReal iBottom = GMLReal_fmin(y2, bbox.bottom);

                bool found = false;
                int32_t startX = (int32_t) GMLReal_floor(iLeft);
                int32_t endX   = (int32_t) GMLReal_ceil(iRight);
                int32_t startY = (int32_t) GMLReal_floor(iTop);
                int32_t endY   = (int32_t) GMLReal_ceil(iBottom);

                for (int32_t py = startY; endY > py && !found; py++) {
                    for (int32_t px = startX; endX > px && !found; px++) {
                        if (Collision_pointInInstance(spr, inst, (GMLReal) px + 0.5, (GMLReal) py + 0.5)) {
                            found = true;
                        }
                    }
                }
                if (!found) continue;
            }
        }

        return RValue_makeReal((GMLReal) inst->instanceId);
    }

    return RValue_makeReal((GMLReal) INSTANCE_NOONE);
}

// collision_point(x, y, obj, prec, notme)
static RValue builtinCollisionPoint(VMContext* ctx, RValue* args, int32_t argCount) {
    if (5 > argCount) return RValue_makeReal((GMLReal) INSTANCE_NOONE);

    Runner* runner = (Runner*) ctx->runner;
    GMLReal px = RValue_toReal(args[0]);
    GMLReal py = RValue_toReal(args[1]);
    int32_t targetObjIndex = RValue_toInt32(args[2]);
    int32_t prec = RValue_toInt32(args[3]);
    int32_t notme = RValue_toInt32(args[4]);

    Instance* self = (Instance*) ctx->currentInstance;
    int32_t count = (int32_t) arrlen(runner->instances);

    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;
        if (notme && inst == self) continue;

        if (!Collision_matchesTarget(ctx->dataWin, inst, targetObjIndex)) continue;

        InstanceBBox bbox = Collision_computeBBox(ctx->dataWin, inst);
        if (!bbox.valid) continue;

        // Point-in-AABB test
        if (bbox.left > px || px >= bbox.right || bbox.top > py || py >= bbox.bottom) continue;

        // Precise check if requested
        if (prec != 0) {
            Sprite* spr = Collision_getSprite(ctx->dataWin, inst);
            if (Collision_hasFrameMasks(spr)) {
                if (!Collision_pointInInstance(spr, inst, px, py)) continue;
            }
        }

        return RValue_makeReal((GMLReal) inst->instanceId);
    }

    return RValue_makeReal((GMLReal) INSTANCE_NOONE);
}

// instance_position(x, y, obj)
static RValue builtinInstancePosition(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return RValue_makeReal((GMLReal) INSTANCE_NOONE);

    Runner* runner = (Runner*) ctx->runner;
    GMLReal px = RValue_toReal(args[0]);
    GMLReal py = RValue_toReal(args[1]);
    int32_t targetObjIndex = RValue_toInt32(args[2]);

    int32_t count = (int32_t) arrlen(runner->instances);

    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;

        if (!Collision_matchesTarget(ctx->dataWin, inst, targetObjIndex)) continue;

        InstanceBBox bbox = Collision_computeBBox(ctx->dataWin, inst);
        if (!bbox.valid) continue;

        // Point-in-AABB test (no precise, no notme)
        if (bbox.left > px || px >= bbox.right || bbox.top > py || py >= bbox.bottom) continue;

        return RValue_makeReal((GMLReal) inst->instanceId);
    }

    return RValue_makeReal((GMLReal) INSTANCE_NOONE);
}

// Misc stubs
STUB_RETURN_ZERO(get_timer)
static RValue builtinActionSetAlarm(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t steps = RValue_toInt32(args[0]);
    int32_t alarmIndex = RValue_toInt32(args[1]);

    if (0 > alarmIndex || alarmIndex >= GML_ALARM_COUNT) {
        return RValue_makeUndefined();
    }

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        Runner* runner = (Runner*) ctx->runner;

#ifndef DISABLE_VM_TRACING
        if (shgeti(ctx->alarmsToBeTraced, "*") != -1 || shgeti(ctx->alarmsToBeTraced, runner->dataWin->objt.objects[inst->objectIndex].name) != -1) {
            fprintf(stderr, "VM: [%s] Setting Alarm[%d] = %d (instanceId=%d)\n", runner->dataWin->objt.objects[inst->objectIndex].name, alarmIndex, steps, inst->instanceId);
        }
#endif

        inst->alarm[alarmIndex] = steps;
    }

    return RValue_makeUndefined();
}

static RValue builtinAlarmSet(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t alarmIndex = RValue_toInt32(args[0]);
    int32_t value = RValue_toInt32(args[1]);

    if (0 > alarmIndex || alarmIndex >= GML_ALARM_COUNT) {
        return RValue_makeUndefined();
    }

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;

#ifndef DISABLE_VM_TRACING
        Runner* runner = (Runner*) ctx->runner;
        if (shgeti(ctx->alarmsToBeTraced, "*") != -1 || shgeti(ctx->alarmsToBeTraced, runner->dataWin->objt.objects[inst->objectIndex].name) != -1) {
            fprintf(stderr, "VM: [%s] Setting Alarm[%d] = %d (instanceId=%d)\n", runner->dataWin->objt.objects[inst->objectIndex].name, alarmIndex, value, inst->instanceId);
        }
#endif

        inst->alarm[alarmIndex] = value;
    }

    return RValue_makeUndefined();
}

static RValue builtinAlarmGet(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    int32_t alarmIndex = RValue_toInt32(args[0]);

    if (0 > alarmIndex || alarmIndex >= GML_ALARM_COUNT) {
        return RValue_makeReal(-1);
    }

    if (ctx->currentInstance != nullptr) {
        Instance* inst = (Instance*) ctx->currentInstance;
        return RValue_makeReal((GMLReal) inst->alarm[alarmIndex]);
    }

    return RValue_makeReal(-1);
}

static RValue builtinActionIfVariable(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    bool check;
    switch (args[0].type) {
        case RVALUE_REAL: {
            check = args[0].real != 0.0;
            break;
        }
        case RVALUE_INT32: {
            check = args[0].int32 != 0;
            break;
        }
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64: {
            check = args[0].int64 != 0;
            break;
        }
#endif
        case RVALUE_BOOL: {
            check = args[0].int32 != 0;
            break;
        }
        case RVALUE_STRING: {
            check = args[0].string != nullptr && args[0].string[0] != '\0';
            break;
        }
        default: {
            check = false;
            break;
        }
    }

    int32_t idx = check ? 1 : 2;
    RValue result = args[idx];
    args[idx].ownsString = false; // Steal ownership to avoid double-free in handleCall
    return result;
}

STUB_RETURN_UNDEFINED(action_sound)

// ===[ Tile Layer Functions ]===

static TileLayerState* getOrCreateTileLayer(Runner* runner, int32_t depth) {
    ptrdiff_t idx = hmgeti(runner->tileLayerMap, depth);
    if (0 > idx) {
        TileLayerState defaultVal = { .visible = true, .offsetX = 0.0f, .offsetY = 0.0f };
        hmput(runner->tileLayerMap, depth, defaultVal);
        idx = hmgeti(runner->tileLayerMap, depth);
    }
    return &runner->tileLayerMap[idx].value;
}

static RValue builtinTileLayerHide(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t depth = RValue_toInt32(args[0]);
    TileLayerState* layer = getOrCreateTileLayer(runner, depth);
    layer->visible = false;
    return RValue_makeUndefined();
}

static RValue builtinTileLayerShow(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t depth = RValue_toInt32(args[0]);
    TileLayerState* layer = getOrCreateTileLayer(runner, depth);
    layer->visible = true;
    return RValue_makeUndefined();
}

static RValue builtinTileLayerShift(MAYBE_UNUSED VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t depth = RValue_toInt32(args[0]);
    float dx = (float) RValue_toReal(args[1]);
    float dy = (float) RValue_toReal(args[2]);
    TileLayerState* layer = getOrCreateTileLayer(runner, depth);
    layer->offsetX += dx;
    layer->offsetY += dy;
    return RValue_makeUndefined();
}

// ===[ Layer Functions ]===

static RValue builtinLayerForceDrawDepth(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    runner->forceDrawDepth = RValue_toBool(args[0]);
    runner->forcedDepth = RValue_toInt32(args[1]);
    return RValue_makeUndefined();
}

static RValue builtinLayerIsDrawDepthForced(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    return RValue_makeBool(runner->forceDrawDepth);
}

static RValue builtinLayerGetForcedDepth(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    return RValue_makeReal((GMLReal) runner->forcedDepth);
}

// ===[ GMS2 Layer Runtime API ]===

static RValue builtinLayerGetId(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    char* name = RValue_toString(args[0]);
    if (name == nullptr) return RValue_makeReal(-1.0);
    int32_t result = -1;
    // Check dynamic layers first (they may shadow a parsed layer by name).
    size_t runtimeLayerCount = arrlenu(runner->runtimeLayers);
    repeat(runtimeLayerCount, i) {
        RuntimeLayer* runtimeLayer = &runner->runtimeLayers[i];
        if (runtimeLayer->dynamic && runtimeLayer->dynamicName != nullptr && strcmp(runtimeLayer->dynamicName, name) == 0) {
            result = (int32_t) runtimeLayer->id;
            break;
        }
    }
    if (result == -1 && runner->currentRoom != nullptr) {
        repeat(runner->currentRoom->layerCount, i) {
            RoomLayer* layer = &runner->currentRoom->layers[i];
            if (layer->name != nullptr && strcmp(layer->name, name) == 0) {
                result = (int32_t) layer->id;
                break;
            }
        }
    }
    free(name);
    return RValue_makeReal((GMLReal) result);
}

static RValue builtinLayerExists(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    return RValue_makeBool(Runner_findRuntimeLayerById(runner, id) != nullptr);
}

static RValue builtinLayerGetName(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer != nullptr && runtimeLayer->dynamic && runtimeLayer->dynamicName != nullptr)
        return RValue_makeString(runtimeLayer->dynamicName);

    RoomLayer* roomLayer = Runner_findRoomLayerById(runner, id);
    if (roomLayer == nullptr || roomLayer->name == nullptr)
        return RValue_makeString("");

    return RValue_makeString(roomLayer->name);
}

static RValue builtinLayerGetDepth(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer == nullptr)
        return RValue_makeUndefined();

    return RValue_makeReal((GMLReal) runtimeLayer->depth);
}

static RValue builtinLayerDepth(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    int32_t depth = RValue_toInt32(args[1]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer != nullptr)
        runtimeLayer->depth = depth;

    return RValue_makeUndefined();
}

static RValue builtinLayerGetVisible(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer == nullptr)
        return RValue_makeBool(false);

    return RValue_makeBool(runtimeLayer->visible);
}

static RValue builtinLayerSetVisible(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    bool visible = RValue_toBool(args[1]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer != nullptr)
        runtimeLayer->visible = visible;

    return RValue_makeUndefined();
}

static RValue builtinLayerGetX(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer == nullptr)
        return RValue_makeReal(0.0);

    return RValue_makeReal((GMLReal) runtimeLayer->xOffset);
}

static RValue builtinLayerX(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    float x = (float) RValue_toReal(args[1]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer != nullptr)
        runtimeLayer->xOffset = x;

    return RValue_makeUndefined();
}

static RValue builtinLayerGetY(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer == nullptr)
        return RValue_makeReal(0.0);

    return RValue_makeReal((GMLReal) runtimeLayer->yOffset);
}

static RValue builtinLayerY(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    float y = (float) RValue_toReal(args[1]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer != nullptr)
        runtimeLayer->yOffset = y;

    return RValue_makeUndefined();
}

static RValue builtinLayerGetHspeed(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer == nullptr)
        return RValue_makeReal(0.0);

    return RValue_makeReal((GMLReal) runtimeLayer->hSpeed);
}

static RValue builtinLayerHspeed(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    float hs = (float) RValue_toReal(args[1]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer != nullptr)
        runtimeLayer->hSpeed = hs;

    return RValue_makeUndefined();
}

static RValue builtinLayerGetVspeed(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer == nullptr)
        return RValue_makeReal(0.0);

    return RValue_makeReal((GMLReal) runtimeLayer->vSpeed);
}

// Creates a new dynamic layer. Signatures: layer_create(depth) or layer_create(depth, name).
static RValue builtinLayerCreate(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t depth = RValue_toInt32(args[0]);
    char* name = nullptr;
    if (argCount > 1) {
        name = RValue_toString(args[1]);
    }
    uint32_t id = Runner_getNextLayerId(runner);
    RuntimeLayer runtimeLayer = {
        .id = id,
        .depth = depth,
        .visible = true,
        .xOffset = 0.0f, .yOffset = 0.0f,
        .hSpeed = 0.0f, .vSpeed = 0.0f,
        .dynamic = true,
        .dynamicName = name, // ownership transferred (nullptr if not provided)
        .elements = nullptr,
    };
    arrput(runner->runtimeLayers, runtimeLayer);
    return RValue_makeReal((GMLReal) id);
}

static RValue builtinLayerDestroy(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    size_t count = arrlenu(runner->runtimeLayers);
    repeat(count, i) {
        if ((int32_t) runner->runtimeLayers[i].id != id)
            continue;

        // Ignore if we are trying to delete a non-dynamic layer
        if (!runner->runtimeLayers[i].dynamic)
            return RValue_makeUndefined();

        Runner_freeRuntimeLayer(&runner->runtimeLayers[i]);
        arrdel(runner->runtimeLayers, i);
        break;
    }
    return RValue_makeUndefined();
}

static RValue builtinLayerBackgroundCreate(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t layerId = RValue_toInt32(args[0]);
    int32_t spriteIndex = RValue_toInt32(args[1]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, layerId);
    if (runtimeLayer == nullptr)
        return RValue_makeReal(-1.0);

    RuntimeBackgroundElement* bg = safeMalloc(sizeof(RuntimeBackgroundElement));
    bg->spriteIndex = spriteIndex;
    bg->visible = true;
    bg->htiled = false;
    bg->vtiled = false;
    bg->stretch = false;
    bg->xScale = 1.0f;
    bg->yScale = 1.0f;
    bg->blend = 0xFFFFFF;
    bg->alpha = 1.0f;
    bg->xOffset = 0.0f;
    bg->yOffset = 0.0f;
    RuntimeLayerElement el = {
        .id = Runner_getNextLayerId(runner),
        .type = RuntimeLayerElementType_Background,
        .backgroundElement = bg,
        .spriteElement = nullptr,
    };
    arrput(runtimeLayer->elements, el);
    return RValue_makeReal((GMLReal) el.id);
}

static RValue builtinLayerBackgroundExists(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t layerId = RValue_toInt32(args[0]);
    int32_t elementId = RValue_toInt32(args[1]);

    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, layerId);
    if (runtimeLayer == nullptr)
        return RValue_makeBool(false);

    size_t count = arrlenu(runtimeLayer->elements);
    repeat(count, i) {
        if ((int32_t) runtimeLayer->elements[i].id == elementId && runtimeLayer->elements[i].type == RuntimeLayerElementType_Background) {
            return RValue_makeBool(true);
        }
    }
    return RValue_makeBool(false);
}

static RuntimeBackgroundElement* findBackgroundElement(Runner* runner, int32_t elementId) {
    RuntimeLayerElement* el = Runner_findLayerElementById(runner, elementId, nullptr);
    if (el == nullptr || el->type != RuntimeLayerElementType_Background)
        return nullptr;
    return el->backgroundElement;
}

static RValue builtinLayerBackgroundVisible(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    RuntimeBackgroundElement* bg = findBackgroundElement(runner, RValue_toInt32(args[0]));
    if (bg != nullptr)
        bg->visible = RValue_toBool(args[1]);
    return RValue_makeUndefined();
}

static RValue builtinLayerBackgroundHtiled(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    RuntimeBackgroundElement* bg = findBackgroundElement(runner, RValue_toInt32(args[0]));
    if (bg != nullptr)
        bg->htiled = RValue_toBool(args[1]);
    return RValue_makeUndefined();
}

static RValue builtinLayerBackgroundVtiled(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    RuntimeBackgroundElement* bg = findBackgroundElement(runner, RValue_toInt32(args[0]));
    if (bg != nullptr)
        bg->vtiled = RValue_toBool(args[1]);
    return RValue_makeUndefined();
}

static RValue builtinLayerBackgroundXscale(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    RuntimeBackgroundElement* bg = findBackgroundElement(runner, RValue_toInt32(args[0]));
    if (bg != nullptr)
        bg->xScale = (float) RValue_toReal(args[1]);
    return RValue_makeUndefined();
}

static RValue builtinLayerBackgroundYscale(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    RuntimeBackgroundElement* bg = findBackgroundElement(runner, RValue_toInt32(args[0]));
    if (bg != nullptr)
        bg->yScale = (float) RValue_toReal(args[1]);
    return RValue_makeUndefined();
}

static RValue builtinLayerBackgroundStretch(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    RuntimeBackgroundElement* bg = findBackgroundElement(runner, RValue_toInt32(args[0]));
    if (bg != nullptr)
        bg->stretch = RValue_toBool(args[1]);
    return RValue_makeUndefined();
}

static RValue builtinLayerBackgroundBlend(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    RuntimeBackgroundElement* bg = findBackgroundElement(runner, RValue_toInt32(args[0]));
    if (bg != nullptr)
        bg->blend = (uint32_t) RValue_toInt32(args[1]) & 0x00FFFFFF;
    return RValue_makeUndefined();
}

static RValue builtinLayerBackgroundAlpha(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    RuntimeBackgroundElement* bg = findBackgroundElement(runner, RValue_toInt32(args[0]));
    if (bg != nullptr)
        bg->alpha = (float) RValue_toReal(args[1]);
    return RValue_makeUndefined();
}

#if IS_BC17_OR_HIGHER_ENABLED
static RValue builtinLayerGetAllElements(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);

    RValue arr = VM_createArray(ctx);
    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer == nullptr)
        return arr;

    int32_t i = 0;
    size_t count = arrlenu(runtimeLayer->elements);
    repeat(count, elementIndex) {
        VM_arraySet(ctx, &arr, i++, RValue_makeReal((GMLReal) runtimeLayer->elements[elementIndex].id));
    }
    return arr;
}
#endif

static RValue builtinLayerGetElementType(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);

    RuntimeLayerElement* el = Runner_findLayerElementById(runner, id, nullptr);
    // layerelementtype_undefined == 0 matches GML's return for unknown/missing elements.
    if (el == nullptr)
        return RValue_makeReal(0.0);

    return RValue_makeReal((GMLReal) el->type);
}

static RValue builtinLayerSpriteGetSprite(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);

    RuntimeLayerElement* el = Runner_findLayerElementById(runner, id, nullptr);
    if (el == nullptr || el->type != RuntimeLayerElementType_Sprite || el->spriteElement == nullptr)
        return RValue_makeReal(-1.0);

    return RValue_makeReal((GMLReal) el->spriteElement->spriteIndex);
}

static RValue builtinLayerSpriteDestroy(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);

    RuntimeLayer* owningLayer = nullptr;
    RuntimeLayerElement* el = Runner_findLayerElementById(runner, id, &owningLayer);
    if (el == nullptr || owningLayer == nullptr || el->type != RuntimeLayerElementType_Sprite)
        return RValue_makeUndefined();

    if (el->spriteElement != nullptr) {
        free(el->spriteElement);
        el->spriteElement = nullptr;
    }

    // Remove the element from the owning layer's element array to keep lookup + iteration tidy.
    size_t count = arrlenu(owningLayer->elements);
    repeat(count, i) {
        if (&owningLayer->elements[i] == el) {
            arrdel(owningLayer->elements, i);
            break;
        }
    }

    return RValue_makeUndefined();
}

#if IS_BC17_OR_HIGHER_ENABLED
static RValue builtinLayerGetAll(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    RValue arr = VM_createArray(ctx);
    int32_t i = 0;
    size_t count = arrlenu(runner->runtimeLayers);
    repeat(count, layerIndex) {
        VM_arraySet(ctx, &arr, i++, RValue_makeReal((GMLReal) runner->runtimeLayers[layerIndex].id));
    }
    return arr;
}

static RValue builtinLayerGetIdAtDepth(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t targetDepth = RValue_toInt32(args[0]);
    RValue arr = VM_createArray(ctx);
    int32_t i = 0;
    size_t count = arrlenu(runner->runtimeLayers);
    repeat(count, layerIndex) {
        if (runner->runtimeLayers[layerIndex].depth == targetDepth) {
            VM_arraySet(ctx, &arr, i++, RValue_makeReal((GMLReal) runner->runtimeLayers[layerIndex].id));
        }
    }
    // When no layer matches, return [-1] instead of an empty array.
    if (i == 0)
        VM_arraySet(ctx, &arr, 0, RValue_makeReal(-1.0));
    return arr;
}
#endif

static RValue builtinLayerVspeed(VMContext* ctx, RValue* args, MAYBE_UNUSED int32_t argCount) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    float vs = (float) RValue_toReal(args[1]);
    RuntimeLayer* runtimeLayer = Runner_findRuntimeLayerById(runner, id);
    if (runtimeLayer != nullptr) runtimeLayer->vSpeed = vs;
    return RValue_makeUndefined();
}

// ===[ Array Functions ]===

// @@NewGMLArray@@ - GMS2 internal function to create a new array literal (e.g. `[1, 2, 3]`).
// Allocates a fresh GMLArray populated with the argument values.
static RValue builtinNewGMLArray(VMContext* ctx, RValue* args, int32_t argCount) {
    RValue arr = VM_createArray(ctx);
    repeat(argCount, i) {
        VM_arraySet(ctx, &arr, i, args[i]);
    }
    return arr;
}

// @@This@@ - GMS2 internal function returning the current instance's ID.
// Emitted by the GMS2 compiler for expressions like `self` when used as a value.
static RValue builtinThis(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr) return RValue_makeInt32(INSTANCE_SELF);
    return RValue_makeInt32((int32_t) inst->instanceId);
}

// @@Other@@ - GMS2 internal function returning the "other" instance's ID.
// Falls back to the current instance when there is no other (matches GML semantics outside with/collision).
static RValue builtinOther(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Instance* other = (Instance*) ctx->otherInstance;
    if (other != nullptr) return RValue_makeInt32((int32_t) other->instanceId);
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr) return RValue_makeInt32(INSTANCE_SELF);
    return RValue_makeInt32((int32_t) inst->instanceId);
}

// ===[ PATH FUNCTIONS ]===

// path_start(path, speed, endaction, absolute) - HTML5: Assign_Path (yyInstance.js:2695-2743)
static RValue builtinPathStart(VMContext* ctx, RValue* args, int32_t argCount) {
    if (4 > argCount) return RValue_makeUndefined();

    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr) return RValue_makeUndefined();

    Runner* runner = (Runner*) ctx->runner;
    int32_t pathIdx = RValue_toInt32(args[0]);
    GMLReal speed = RValue_toReal(args[1]);
    int32_t endAction = RValue_toInt32(args[2]);
    bool absolute = RValue_toBool(args[3]);

    // Validate path index
    inst->pathIndex = -1;
    if (0 > pathIdx) return RValue_makeUndefined();
    if ((uint32_t) pathIdx >= runner->dataWin->path.count) return RValue_makeUndefined();

    GamePath* path = &runner->dataWin->path.paths[pathIdx];
    if (0.0 >= path->length) return RValue_makeUndefined();

    inst->pathIndex = pathIdx;
    inst->pathSpeed = (float) speed;

    if (inst->pathSpeed >= 0.0f) {
        inst->pathPosition = 0.0f;
    } else {
        inst->pathPosition = 1.0f;
    }

    inst->pathPositionPrevious = inst->pathPosition;
    inst->pathScale = 1.0f;
    inst->pathOrientation = 0.0f;
    inst->pathEndAction = endAction;

    if (absolute) {
        PathPositionResult startPos = GamePath_getPosition(path, inst->pathSpeed >= 0.0f ? 0.0 : 1.0);
        inst->x = (float) startPos.x;
        inst->y = (float) startPos.y;

        PathPositionResult origin = GamePath_getPosition(path, 0.0);
        inst->pathXStart = (float) origin.x;
        inst->pathYStart = (float) origin.y;
    } else {
        inst->pathXStart = inst->x;
        inst->pathYStart = inst->y;
    }

    return RValue_makeUndefined();
}

// path_end() - HTML5: Assign_Path(-1,...)
static RValue builtinPathEnd(VMContext* ctx, MAYBE_UNUSED RValue* args, MAYBE_UNUSED int32_t argCount) {
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst != nullptr) {
        inst->pathIndex = -1;
    }
    return RValue_makeUndefined();
}

// string_hash_to_newline - converts # to \n in a string
static RValue builtinStringHashToNewline(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) { 
    if (1 > argCount) return RValue_makeString("");
    RValue original = args[0]; // This is a copy

    if (original.type != RVALUE_STRING) {
        // Fast path: If the argument is not a string, return a copy of it
        return RValue_makeOwnedString(RValue_toString(original));
    }

    if (original.string == nullptr) {
        // Fast path: If the argument is a string but has no value, return an empty string
        return RValue_makeString("");
    }

    PreprocessedText result = TextUtils_preprocessGmlText(original.string);
    if (!result.owning) {
        // No # found, steal the reference to avoid copying the string
        args[0].ownsString = false;
        return original;
    }
    return RValue_makeOwnedString((char*) result.text);
}

// json_decode
static RValue builtinJsonDecode(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) {
        fprintf(stderr, "[json_decode] Expected at least 1 argument\n");
        return RValue_makeUndefined();
    }

    Runner* runner = (Runner*) ctx->runner;
    int32_t mapIndex = dsMapCreate(runner);
    DsMapEntry **mapPtr = dsMapGet(runner, mapIndex);
    const char* content = args[0].string;
    const JsonValue* json = JsonReader_parse(content);

    repeat(JsonReader_objectLength(json), i) {
        const char *key = safeStrdup(JsonReader_getObjectKey(json, i));
        RValue val = RValue_makeOwnedString(safeStrdup(JsonReader_getString(JsonReader_getObjectValue(json, i))));
        shput(*mapPtr, key, val);
    }

    JsonReader_free(json);

    return RValue_makeReal((double) mapIndex);
}

static RValue builtinObjectGetSprite(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) {
        fprintf(stderr, "[object_get_sprite] Expected at least 1 argument\n");
        return RValue_makeUndefined();
    }

    int32_t id = RValue_toInt32(args[0]);

    return RValue_makeReal((double) ctx->dataWin->objt.objects[id].spriteId);
}

// Shared implementation for font_add_sprite and font_add_sprite_ext
static RValue fontAddSpriteImpl(VMContext* ctx, int32_t spriteIndex, uint16_t* charCodes, uint32_t charCount, bool proportional, int32_t sep) {
    DataWin* dw = ctx->dataWin;

    if (0 > spriteIndex || (uint32_t) spriteIndex >= dw->sprt.count) {
        fprintf(stderr, "[font_add_sprite] Invalid sprite index %d\n", spriteIndex);
        return RValue_makeReal(-1.0);
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];

    if (charCount == 0 || sprite->textureCount == 0) {
        return RValue_makeReal(-1.0);
    }

    // Limit glyph count to sprite frame count
    uint32_t glyphCount = charCount;
    if (glyphCount > sprite->textureCount) glyphCount = sprite->textureCount;

    // Compute emSize (max bounding height across all frames) and biggestShift
    uint32_t maxHeight = 0;
    int32_t biggestShift = 0;
    repeat(glyphCount, i) {
        int32_t tpagIdx = DataWin_resolveTPAG(dw, sprite->textureOffsets[i]);
        if (0 > tpagIdx) continue;
        TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
        if (tpag->boundingHeight > maxHeight) maxHeight = tpag->boundingHeight;
        int32_t width = proportional ? (int32_t) tpag->sourceWidth : (int32_t) tpag->boundingWidth;
        if (width > biggestShift) biggestShift = width;
    }

    // Check if space (0x20) is in the string map
    bool hasSpace = false;
    repeat(glyphCount, i) {
        if (charCodes[i] == 0x20) { hasSpace = true; break; }
    }

    // Allocate glyphs (+ 1 for synthetic space if needed)
    uint32_t totalGlyphs = hasSpace ? glyphCount : glyphCount + 1;
    FontGlyph* glyphs = safeMalloc(totalGlyphs * sizeof(FontGlyph));

    repeat(glyphCount, i) {
        int32_t tpagIdx = DataWin_resolveTPAG(dw, sprite->textureOffsets[i]);
        FontGlyph* glyph = &glyphs[i];
        glyph->character = charCodes[i];
        glyph->kerningCount = 0;
        glyph->kerning = nullptr;

        if (0 > tpagIdx) {
            glyph->sourceX = 0;
            glyph->sourceY = 0;
            glyph->sourceWidth = 0;
            glyph->sourceHeight = 0;
            glyph->shift = (int16_t) sep;
            glyph->offset = 0;
            continue;
        }

        TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
        glyph->sourceX = 0; // not used for sprite fonts (TPAG resolved per glyph)
        glyph->sourceY = 0;
        glyph->sourceWidth = tpag->sourceWidth;
        glyph->sourceHeight = tpag->sourceHeight;

        int32_t advanceWidth = proportional ? (int32_t) tpag->sourceWidth : (int32_t) tpag->boundingWidth;
        glyph->shift = (int16_t) (advanceWidth + sep);

        // Horizontal offset: for proportional fonts, no offset; for non-proportional, use target offset minus origin
        glyph->offset = proportional ? 0 : (int16_t) ((int32_t) tpag->targetX - sprite->originX);
    }

    // Add synthetic space glyph if space is not in the string map
    if (!hasSpace) {
        FontGlyph* spaceGlyph = &glyphs[glyphCount];
        spaceGlyph->character = 0x20;
        spaceGlyph->sourceX = 0;
        spaceGlyph->sourceY = 0;
        spaceGlyph->sourceWidth = 0;
        spaceGlyph->sourceHeight = 0;
        spaceGlyph->shift = (int16_t) (biggestShift + sep);
        spaceGlyph->offset = 0;
        spaceGlyph->kerningCount = 0;
        spaceGlyph->kerning = nullptr;
    }

    // Grow the font array and create the new font
    uint32_t newFontIndex = dw->font.count;
    dw->font.count++;
    dw->font.fonts = safeRealloc(dw->font.fonts, dw->font.count * sizeof(Font));

    Font* font = &dw->font.fonts[newFontIndex];
    font->name = "sprite_font";
    font->displayName = "sprite_font";
    font->emSize = (maxHeight > 0) ? maxHeight : sprite->height;
    font->bold = false;
    font->italic = false;
    font->rangeStart = 0;
    font->charset = 0;
    font->antiAliasing = 0;
    font->rangeEnd = 0;
    font->textureOffset = 0; // not used for sprite fonts
    font->scaleX = 1.0f;
    font->scaleY = 1.0f;
    font->ascenderOffset = 0;
    font->glyphCount = totalGlyphs;
    font->glyphs = glyphs;
    font->maxGlyphHeight = maxHeight; // match what HTML5 runner uses for line stride
    font->isSpriteFont = true;
    font->spriteIndex = spriteIndex;

    return RValue_makeReal((GMLReal) newFontIndex);
}

// font_add_sprite_ext(sprite, string_map, prop, sep)
static RValue builtinFontAddSpriteExt(VMContext* ctx, RValue* args, int32_t argCount) {
    if (4 > argCount) {
        fprintf(stderr, "[font_add_sprite_ext] Expected 4 arguments, got %d\n", argCount);
        return RValue_makeReal(-1.0);
    }

    int32_t spriteIndex = RValue_toInt32(args[0]);
    char* stringMap = RValue_toString(args[1]);
    bool proportional = RValue_toBool(args[2]);
    int32_t sep = RValue_toInt32(args[3]);

    // Decode the string map to get character codes (UTF-8 -> codepoints)
    int32_t mapLen = (int32_t) strlen(stringMap);
    int32_t mapPos = 0;
    uint32_t charCount = 0;
    uint16_t charCodes[1024];
    while (mapLen > mapPos && 1024 > charCount) {
        charCodes[charCount++] = TextUtils_decodeUtf8(stringMap, mapLen, &mapPos);
    }
    free(stringMap);

    return fontAddSpriteImpl(ctx, spriteIndex, charCodes, charCount, proportional, sep);
}

// font_add_sprite(sprite, first, prop, sep)
static RValue builtinFontAddSprite(VMContext* ctx, RValue* args, int32_t argCount) {
    if (4 > argCount) {
        fprintf(stderr, "[font_add_sprite] Expected 4 arguments, got %ld\n", (long)argCount);
        return RValue_makeReal(-1.0);
    }

    DataWin* dw = ctx->dataWin;
    int32_t spriteIndex = RValue_toInt32(args[0]);
    int32_t first = RValue_toInt32(args[1]);
    bool proportional = RValue_toBool(args[2]);
    int32_t sep = RValue_toInt32(args[3]);

    // Build sequential character codes: first, first+1, first+2, ...
    uint32_t frameCount = 0;
    if (spriteIndex >= 0 && dw->sprt.count > (uint32_t) spriteIndex) {
        frameCount = dw->sprt.sprites[spriteIndex].textureCount;
    }
    if (frameCount > 1024) frameCount = 1024;

    uint16_t charCodes[1024];
    repeat(frameCount, i) {
        charCodes[i] = (uint16_t) (first + (int32_t) i);
    }

    return fontAddSpriteImpl(ctx, spriteIndex, charCodes, frameCount, proportional, sep);
}

static RValue builtinAssetGetIndex(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) {
        fprintf(stderr, "[asset_get_index] Expected at least 1 argument\n");
        return RValue_makeUndefined();
    }

    char* name = RValue_toString(args[0]);

    repeat(ctx->dataWin->objt.count, i) {
        if (strcmp(ctx->dataWin->objt.objects[i].name, name) == 0) {
            free(name);
            return RValue_makeReal((double) i);
        }
    }

    free(name);
    return RValue_makeReal((double) -1);
}

// ===[ REGISTRATION ]===

void VMBuiltins_registerAll(VMContext* ctx) {
    requireMessage(!ctx->registeredBuiltinFunctions, "Attempting to register all VMBuiltins, but it was already registered!");
    ctx->registeredBuiltinFunctions = true;

    const bool isGMS2 = DataWin_isVersionAtLeast(ctx->dataWin, 2, 0, 0, 0);

    // Core output
    VM_registerBuiltin(ctx, "show_debug_message", builtinShowDebugMessage);

    // String functions
    VM_registerBuiltin(ctx, "string_length", builtinStringLength);
    VM_registerBuiltin(ctx, "string", builtinString);
    VM_registerBuiltin(ctx, "string_upper", builtinStringUpper);
    VM_registerBuiltin(ctx, "string_lower", builtinStringLower);
    VM_registerBuiltin(ctx, "string_copy", builtinStringCopy);
    VM_registerBuiltin(ctx, "string_pos", builtinStringPos);
    VM_registerBuiltin(ctx, "string_char_at", builtinStringCharAt);
    VM_registerBuiltin(ctx, "string_delete", builtinStringDelete);
    VM_registerBuiltin(ctx, "string_insert", builtinStringInsert);
    VM_registerBuiltin(ctx, "string_replace_all", builtinStringReplaceAll);
    VM_registerBuiltin(ctx, "string_repeat", builtinStringRepeat);
    VM_registerBuiltin(ctx, "ord", builtinOrd);
    VM_registerBuiltin(ctx, "chr", builtinChr);

    // Type functions
    VM_registerBuiltin(ctx, "real", builtinReal);
    VM_registerBuiltin(ctx, "is_string", builtinIsString);
    VM_registerBuiltin(ctx, "is_real", builtinIsReal);
    VM_registerBuiltin(ctx, "is_undefined", builtinIsUndefined);

    // Math functions
    VM_registerBuiltin(ctx, "floor", builtinFloor);
    VM_registerBuiltin(ctx, "ceil", builtinCeil);
    VM_registerBuiltin(ctx, "round", builtinRound);
    VM_registerBuiltin(ctx, "abs", builtinAbs);
    VM_registerBuiltin(ctx, "sign", builtinSign);
    VM_registerBuiltin(ctx, "max", builtinMax);
    VM_registerBuiltin(ctx, "min", builtinMin);
    VM_registerBuiltin(ctx, "power", builtinPower);
    VM_registerBuiltin(ctx, "sqrt", builtinSqrt);
    VM_registerBuiltin(ctx, "sqr", builtinSqr);
    VM_registerBuiltin(ctx, "sin", builtinSin);
    VM_registerBuiltin(ctx, "arcsin", builtinArcsin);
    VM_registerBuiltin(ctx, "cos", builtinCos);
    VM_registerBuiltin(ctx, "dsin", builtinDsin);
    VM_registerBuiltin(ctx, "dcos", builtinDcos);
    VM_registerBuiltin(ctx, "darctan2", builtinDarctan2);
    VM_registerBuiltin(ctx, "degtorad", builtinDegtorad);
    VM_registerBuiltin(ctx, "radtodeg", builtinRadtodeg);
    VM_registerBuiltin(ctx, "clamp", builtinClamp);
    VM_registerBuiltin(ctx, "lerp", builtinLerp);
    VM_registerBuiltin(ctx, "point_distance", builtinPointDistance);
    VM_registerBuiltin(ctx, "point_direction", builtinPointDirection);
    VM_registerBuiltin(ctx, "angle_difference", builtinAngleDifference);
    VM_registerBuiltin(ctx, "distance_to_point", builtinDistanceToPoint);
    VM_registerBuiltin(ctx, "distance_to_object", builtinDistanceToObject);
    VM_registerBuiltin(ctx, "move_towards_point", builtinMoveTowardsPoint);
    VM_registerBuiltin(ctx, "action_move_point", builtinMoveTowardsPoint);
    VM_registerBuiltin(ctx, "move_snap", builtinMoveSnap);
    VM_registerBuiltin(ctx, "lengthdir_x", builtinLengthdir_x);
    VM_registerBuiltin(ctx, "lengthdir_y", builtinLengthdir_y);

    // Random
    VM_registerBuiltin(ctx, "random", builtinRandom);
    VM_registerBuiltin(ctx, "random_range", builtinRandomRange);
    VM_registerBuiltin(ctx, "irandom", builtinIrandom);
    VM_registerBuiltin(ctx, "irandom_range", builtinIrandomRange);
    VM_registerBuiltin(ctx, "choose", builtinChoose);
    VM_registerBuiltin(ctx, "randomize", builtinRandomize);

    // Room
    VM_registerBuiltin(ctx, "game_get_speed", builtinGameGetSpeed);
    VM_registerBuiltin(ctx, "room_exists", builtinRoomExists);
    VM_registerBuiltin(ctx, "room_get_name", builtinRoomGetName);
    VM_registerBuiltin(ctx, "room_goto_next", builtinRoomGotoNext);
    VM_registerBuiltin(ctx, "room_goto_previous", builtinRoomGotoPrevious);
    VM_registerBuiltin(ctx, "room_goto", builtinRoomGoto);
    VM_registerBuiltin(ctx, "room_restart", builtinRoomRestart);
    VM_registerBuiltin(ctx, "room_next", builtinRoomNext);
    VM_registerBuiltin(ctx, "room_previous", builtinRoomPrevious);
    VM_registerBuiltin(ctx, "room_set_persistent", builtinRoomSetPersistent);

    // GMS2 camera compatibility
    VM_registerBuiltin(ctx, "view_get_camera", builtinViewGetCamera);
    VM_registerBuiltin(ctx, "camera_get_view_x", builtinCameraGetViewX);
    VM_registerBuiltin(ctx, "camera_get_view_y", builtinCameraGetViewY);
    VM_registerBuiltin(ctx, "camera_get_view_width", builtinCameraGetViewWidth);
    VM_registerBuiltin(ctx, "camera_get_view_height", builtinCameraGetViewHeight);
    VM_registerBuiltin(ctx, "camera_set_view_pos", builtinCameraSetViewPos);
    VM_registerBuiltin(ctx, "camera_get_view_target", builtinCameraGetViewTarget);
    VM_registerBuiltin(ctx, "camera_set_view_target", builtinCameraSetViewTarget);
    VM_registerBuiltin(ctx, "camera_get_view_border_x", builtinCameraGetViewBorderX);
    VM_registerBuiltin(ctx, "camera_get_view_border_y", builtinCameraGetViewBorderY);
    VM_registerBuiltin(ctx, "camera_set_view_border", builtinCameraSetViewBorder);

    // Variables
    VM_registerBuiltin(ctx, "variable_global_exists", builtinVariableGlobalExists);
    VM_registerBuiltin(ctx, "variable_global_get", builtinVariableGlobalGet);
    VM_registerBuiltin(ctx, "variable_global_set", builtinVariableGlobalSet);
    VM_registerBuiltin(ctx, "variable_instance_set", builtinVariableInstanceSet);
    VM_registerBuiltin(ctx, "variable_instance_get", builtinVariableInstanceGet);
    VM_registerBuiltin(ctx, "variable_instance_exists", builtinVariableInstanceExists);

    // Script
    VM_registerBuiltin(ctx, "script_execute", builtinScriptExecute);
#if IS_BC17_OR_HIGHER_ENABLED
    VM_registerBuiltin(ctx, "method", builtinMethod);
#endif

    // OS
    VM_registerBuiltin(ctx, "os_get_language", builtinOsGetLanguage);
    VM_registerBuiltin(ctx, "os_get_region", builtinOsGetRegion);

    // ds_map
    VM_registerBuiltin(ctx, "ds_map_create", builtinDsMapCreate);
    VM_registerBuiltin(ctx, "ds_map_add", builtinDsMapAdd);
    VM_registerBuiltin(ctx, "ds_map_set", builtinDsMapSet);
    VM_registerBuiltin(ctx, "ds_map_replace", builtinDsMapReplace);
    VM_registerBuiltin(ctx, "ds_map_find_value", builtinDsMapFindValue);
    VM_registerBuiltin(ctx, "ds_map_exists", builtinDsMapExists);
    VM_registerBuiltin(ctx, "ds_map_find_first", builtinDsMapFindFirst);
    VM_registerBuiltin(ctx, "ds_map_find_next", builtinDsMapFindNext);
    VM_registerBuiltin(ctx, "ds_map_size", builtinDsMapSize);
    VM_registerBuiltin(ctx, "ds_map_destroy", builtinDsMapDestroy);

    // ds_list stubs
    VM_registerBuiltin(ctx, "ds_list_create", builtinDsListCreate);
    VM_registerBuiltin(ctx, "ds_list_add", builtinDsListAdd);
    VM_registerBuiltin(ctx, "ds_list_size", builtinDsListSize);
    VM_registerBuiltin(ctx, "ds_list_find_index", builtinDsListFindIndex);

    // Array
    VM_registerBuiltin(ctx, "array_length_1d", builtinArrayLength1d);

    // Steam stubs
    VM_registerBuiltin(ctx, "steam_initialised", builtin_steam_initialised);
    VM_registerBuiltin(ctx, "steam_stats_ready", builtin_steam_stats_ready);
    VM_registerBuiltin(ctx, "steam_file_exists", builtin_steam_file_exists);
    VM_registerBuiltin(ctx, "steam_file_write", builtin_steam_file_write);
    VM_registerBuiltin(ctx, "steam_file_read", builtin_steam_file_read);
    VM_registerBuiltin(ctx, "steam_get_persona_name", builtin_steam_get_persona_name);

    // Audio
    VM_registerBuiltin(ctx, "audio_channel_num", builtin_audioChannelNum);
    VM_registerBuiltin(ctx, "audio_play_sound", builtin_audioPlaySound);
    VM_registerBuiltin(ctx, "audio_stop_sound", builtin_audioStopSound);
    VM_registerBuiltin(ctx, "audio_stop_all", builtin_audioStopAll);
    VM_registerBuiltin(ctx, "audio_is_playing", builtin_audioIsPlaying);
    VM_registerBuiltin(ctx, "audio_is_paused", builtin_audioIsPaused);
    VM_registerBuiltin(ctx, "audio_sound_gain", builtin_audioSoundGain);
    VM_registerBuiltin(ctx, "audio_sound_pitch", builtin_audioSoundPitch);
    VM_registerBuiltin(ctx, "audio_sound_get_gain", builtin_audioSoundGetGain);
    VM_registerBuiltin(ctx, "audio_sound_get_pitch", builtin_audioSoundGetPitch);
    VM_registerBuiltin(ctx, "audio_master_gain", builtin_audioMasterGain);
    VM_registerBuiltin(ctx, "audio_group_load", builtin_audioGroupLoad);
    VM_registerBuiltin(ctx, "audio_group_is_loaded", builtin_audioGroupIsLoaded);
    VM_registerBuiltin(ctx, "audio_play_music", builtin_audioPlayMusic);
    VM_registerBuiltin(ctx, "audio_stop_music", builtin_audioStopMusic);
    VM_registerBuiltin(ctx, "audio_music_gain", builtin_audioMusicGain);
    VM_registerBuiltin(ctx, "audio_music_is_playing", builtin_audioMusicIsPlaying);
    VM_registerBuiltin(ctx, "audio_pause_sound", builtin_audioPauseSound);
    VM_registerBuiltin(ctx, "audio_resume_sound", builtin_audioResumeSound);
    VM_registerBuiltin(ctx, "audio_pause_all", builtin_audioPauseAll);
    VM_registerBuiltin(ctx, "audio_resume_all", builtin_audioResumeAll);
    VM_registerBuiltin(ctx, "audio_sound_get_track_position", builtin_audioSoundGetTrackPosition);
    VM_registerBuiltin(ctx, "audio_sound_set_track_position", builtin_audioSoundSetTrackPosition);
    VM_registerBuiltin(ctx, "audio_create_stream", builtin_audioCreateStream);
    VM_registerBuiltin(ctx, "audio_destroy_stream", builtin_audioDestroyStream);

    // Application surface
    VM_registerBuiltin(ctx, "application_surface_enable", builtin_application_surface_enable);
    VM_registerBuiltin(ctx, "application_surface_draw_enable", builtin_application_surface_draw_enable);

    // Gamepad
    VM_registerBuiltin(ctx, "gamepad_get_device_count", builtin_gamepad_get_device_count);
    VM_registerBuiltin(ctx, "gamepad_is_connected", builtin_gamepad_is_connected);
    VM_registerBuiltin(ctx, "gamepad_button_check", builtin_gamepad_button_check);
    VM_registerBuiltin(ctx, "gamepad_button_check_pressed", builtin_gamepad_button_check_pressed);
    VM_registerBuiltin(ctx, "gamepad_button_check_released", builtin_gamepad_button_check_released);
    VM_registerBuiltin(ctx, "gamepad_axis_value", builtin_gamepad_axis_value);
    VM_registerBuiltin(ctx, "gamepad_get_description", builtin_gamepad_get_description);
    VM_registerBuiltin(ctx, "gamepad_button_value", builtin_gamepad_button_value);

    // INI
    VM_registerBuiltin(ctx, "ini_open", builtinIniOpen);
    VM_registerBuiltin(ctx, "ini_close", builtinIniClose);
    VM_registerBuiltin(ctx, "ini_write_real", builtinIniWriteReal);
    VM_registerBuiltin(ctx, "ini_write_string", builtinIniWriteString);
    VM_registerBuiltin(ctx, "ini_read_string", builtinIniReadString);
    VM_registerBuiltin(ctx, "ini_read_real", builtinIniReadReal);
    VM_registerBuiltin(ctx, "ini_section_exists", builtinIniSectionExists);

    // File
    VM_registerBuiltin(ctx, "file_exists", builtinFileExists);
    VM_registerBuiltin(ctx, "file_text_open_write", builtinFileTextOpenWrite);
    VM_registerBuiltin(ctx, "file_text_open_read", builtinFileTextOpenRead);
    VM_registerBuiltin(ctx, "file_text_close", builtinFileTextClose);
    VM_registerBuiltin(ctx, "file_text_write_string", builtinFileTextWriteString);
    VM_registerBuiltin(ctx, "file_text_writeln", builtinFileTextWriteln);
    VM_registerBuiltin(ctx, "file_text_write_real", builtinFileTextWriteReal);
    VM_registerBuiltin(ctx, "file_text_eof", builtinFileTextEof);
    VM_registerBuiltin(ctx, "file_delete", builtinFileDelete);
    VM_registerBuiltin(ctx, "file_text_read_string", builtinFileTextReadString);
    VM_registerBuiltin(ctx, "file_text_read_real", builtinFileTextReadReal);
    VM_registerBuiltin(ctx, "file_text_readln", builtinFileTextReadln);

    // Keyboard
    VM_registerBuiltin(ctx, "keyboard_check", builtinKeyboardCheck);
    VM_registerBuiltin(ctx, "keyboard_check_pressed", builtinKeyboardCheckPressed);
    VM_registerBuiltin(ctx, "keyboard_check_released", builtinKeyboardCheckReleased);
    VM_registerBuiltin(ctx, "keyboard_check_direct", builtinKeyboardCheckDirect);
    VM_registerBuiltin(ctx, "keyboard_key_press", builtinKeyboardKeyPress);
    VM_registerBuiltin(ctx, "keyboard_key_release", builtinKeyboardKeyRelease);
    VM_registerBuiltin(ctx, "keyboard_clear", builtinKeyboardClear);

    // Joystick
    VM_registerBuiltin(ctx, "joystick_exists", builtin_joystick_exists);
    VM_registerBuiltin(ctx, "joystick_xpos", builtin_joystick_xpos);
    VM_registerBuiltin(ctx, "joystick_ypos", builtin_joystick_ypos);
    VM_registerBuiltin(ctx, "joystick_direction", builtin_joystick_direction);
    VM_registerBuiltin(ctx, "joystick_pov", builtin_joystick_pov);
    VM_registerBuiltin(ctx, "joystick_check_button", builtin_joystick_check_button);

    // Window
    VM_registerBuiltin(ctx, "window_get_fullscreen", builtin_window_get_fullscreen);
    VM_registerBuiltin(ctx, "window_set_fullscreen", builtin_window_set_fullscreen);
    VM_registerBuiltin(ctx, "window_set_caption", builtinWindowSetCaption);
    VM_registerBuiltin(ctx, "window_set_size", builtin_window_set_size);
    VM_registerBuiltin(ctx, "window_center", builtin_window_center);
    VM_registerBuiltin(ctx, "window_get_width", builtinWindowGetWidth);
    VM_registerBuiltin(ctx, "window_get_height", builtinWindowGetHeight);

    // Game
    VM_registerBuiltin(ctx, "game_restart", builtinGameRestart);
    VM_registerBuiltin(ctx, "game_end", builtinGameEnd);
    VM_registerBuiltin(ctx, "game_save", builtin_game_save);
    VM_registerBuiltin(ctx, "game_load", builtin_game_load);

    // Instance
    VM_registerBuiltin(ctx, "instance_exists", builtinInstanceExists);
    VM_registerBuiltin(ctx, "instance_number", builtinInstanceNumber);
    VM_registerBuiltin(ctx, "instance_find", builtinInstanceFind);
    VM_registerBuiltin(ctx, "instance_destroy", builtinInstanceDestroy);
    if(!isGMS2) {
        VM_registerBuiltin(ctx, "instance_create", builtinInstanceCreate);
    }
    else {
        VM_registerBuiltin(ctx, "instance_create_depth", builtinInstanceCreateDepth);
    }
    VM_registerBuiltin(ctx, "instance_copy", builtinInstanceCopy);
    VM_registerBuiltin(ctx, "instance_change", builtinInstanceChange);
    VM_registerBuiltin(ctx, "instance_deactivate_all", builtinInstanceDeactivateAll);
    VM_registerBuiltin(ctx, "instance_activate_all", builtinInstanceActivateAll);
    VM_registerBuiltin(ctx, "instance_activate_object", builtinInstanceActivateObject);
    VM_registerBuiltin(ctx, "instance_deactivate_object", builtinInstanceDeactivateObject);
    VM_registerBuiltin(ctx, "action_kill_object", builtinActionKillObject);
    VM_registerBuiltin(ctx, "action_create_object", builtinActionCreateObject);
    VM_registerBuiltin(ctx, "action_set_relative", builtinActionSetRelative);
    VM_registerBuiltin(ctx, "action_move", builtinActionMove);
    VM_registerBuiltin(ctx, "action_move_to", builtinActionMoveTo);
    VM_registerBuiltin(ctx, "action_snap", builtinActionSnap);
    VM_registerBuiltin(ctx, "action_set_friction", builtinActionSetFriction);
    VM_registerBuiltin(ctx, "action_set_gravity", builtinActionSetGravity);
    VM_registerBuiltin(ctx, "action_set_hspeed", builtinActionSetHspeed);
    VM_registerBuiltin(ctx, "action_set_vspeed", builtinActionSetVspeed);
    VM_registerBuiltin(ctx, "event_inherited", builtinEventInherited);
    VM_registerBuiltin(ctx, "action_inherited", builtinEventInherited);
    VM_registerBuiltin(ctx, "event_user", builtinEventUser);
    VM_registerBuiltin(ctx, "event_perform", builtinEventPerform);

    // Buffer
    VM_registerBuiltin(ctx, "buffer_create", builtin_bufferCreate);
    VM_registerBuiltin(ctx, "buffer_delete", builtin_bufferDelete);
    VM_registerBuiltin(ctx, "buffer_write", builtin_bufferWrite);
    VM_registerBuiltin(ctx, "buffer_read", builtin_bufferRead);
    VM_registerBuiltin(ctx, "buffer_seek", builtin_bufferSeek);
    VM_registerBuiltin(ctx, "buffer_tell", builtin_bufferTell);
    VM_registerBuiltin(ctx, "buffer_get_size", builtin_bufferGetSize);
    VM_registerBuiltin(ctx, "buffer_load", builtin_bufferLoad);
    VM_registerBuiltin(ctx, "buffer_save", builtin_bufferSave);
    VM_registerBuiltin(ctx, "buffer_base64_encode", builtin_buffer_base64_encode);

    // PSN
    VM_registerBuiltin(ctx, "psn_init", builtin_psn_init);
    VM_registerBuiltin(ctx, "psn_default_user", builtin_psn_default_user);
    VM_registerBuiltin(ctx, "psn_get_leaderboard_score", builtin_psn_get_leaderboard_score);

    // Draw
    VM_registerBuiltin(ctx, "draw_sprite", builtin_drawSprite);
    VM_registerBuiltin(ctx, "draw_sprite_ext", builtin_drawSpriteExt);
    VM_registerBuiltin(ctx, "draw_sprite_tiled", builtin_drawSpriteTiled);
    VM_registerBuiltin(ctx, "draw_sprite_tiled_ext", builtin_drawSpriteTiledExt);
    VM_registerBuiltin(ctx, "draw_sprite_stretched", builtin_drawSpriteStretched);
    VM_registerBuiltin(ctx, "draw_sprite_stretched_ext", builtin_drawSpriteStretchedExt);
    VM_registerBuiltin(ctx, "draw_sprite_part", builtin_drawSpritePart);
    VM_registerBuiltin(ctx, "draw_sprite_part_ext", builtin_drawSpritePartExt);
    VM_registerBuiltin(ctx, "draw_rectangle", builtin_drawRectangle);
    VM_registerBuiltin(ctx, "draw_rectangle_color", builtin_drawRectangleColor);
    VM_registerBuiltin(ctx, "draw_healthbar", builtin_drawHealthbar);
    VM_registerBuiltin(ctx, "draw_set_color", builtin_drawSetColor);
    VM_registerBuiltin(ctx, "draw_set_alpha", builtin_drawSetAlpha);
    VM_registerBuiltin(ctx, "draw_set_font", builtin_drawSetFont);
    VM_registerBuiltin(ctx, "draw_set_halign", builtin_drawSetHalign);
    VM_registerBuiltin(ctx, "draw_set_valign", builtin_drawSetValign);
    VM_registerBuiltin(ctx, "draw_text", builtin_drawText);
    VM_registerBuiltin(ctx, "draw_text_transformed", builtin_drawTextTransformed);
    VM_registerBuiltin(ctx, "draw_text_ext", builtin_draw_text_ext);
    VM_registerBuiltin(ctx, "draw_text_ext_transformed", builtin_draw_text_ext_transformed);
    VM_registerBuiltin(ctx, "draw_text_color", builtin_drawTextColor);
    VM_registerBuiltin(ctx, "draw_text_color_transformed", builtin_drawTextColorTransformed);
    VM_registerBuiltin(ctx, "draw_text_color_ext", builtin_draw_text_color_ext);
    VM_registerBuiltin(ctx, "draw_text_color_ext_transformed", builtin_draw_text_color_ext_transformed);
    VM_registerBuiltin(ctx, "draw_text_colour", builtin_drawTextColor);
    VM_registerBuiltin(ctx, "draw_text_colour_transformed", builtin_drawTextColorTransformed);
    VM_registerBuiltin(ctx, "draw_text_colour_ext", builtin_draw_text_color_ext);
    VM_registerBuiltin(ctx, "draw_text_colour_ext_transformed", builtin_draw_text_color_ext_transformed);
    VM_registerBuiltin(ctx, "draw_surface", builtin_draw_surface);
    VM_registerBuiltin(ctx, "draw_surface_ext", builtin_draw_surface_ext);
    if(!isGMS2) {
        VM_registerBuiltin(ctx, "draw_background", builtin_drawBackground);
        VM_registerBuiltin(ctx, "draw_background_ext", builtin_drawBackgroundExt);
        VM_registerBuiltin(ctx, "draw_background_stretched", builtin_drawBackgroundStretched);
        VM_registerBuiltin(ctx, "draw_background_part_ext", builtin_drawBackgroundPartExt);
        VM_registerBuiltin(ctx, "background_get_width", builtinBackgroundGetWidth);
        VM_registerBuiltin(ctx, "background_get_height", builtinBackgroundGetHeight);
    }
    VM_registerBuiltin(ctx, "draw_self", builtin_draw_self);
    VM_registerBuiltin(ctx, "draw_line", builtin_draw_line);
    VM_registerBuiltin(ctx, "draw_line_width", builtin_draw_line_width);
    VM_registerBuiltin(ctx, "draw_line_width_colour", builtin_draw_line_width_colour);
    VM_registerBuiltin(ctx, "draw_line_width_color", builtin_draw_line_width_colour);
    VM_registerBuiltin(ctx, "draw_triangle", builtin_draw_triangle);
    VM_registerBuiltin(ctx, "draw_set_colour", builtin_draw_set_colour);
    VM_registerBuiltin(ctx, "draw_get_colour", builtin_draw_get_colour);
    VM_registerBuiltin(ctx, "draw_get_color", builtin_draw_get_color);
    VM_registerBuiltin(ctx, "draw_get_alpha", builtin_draw_get_alpha);

    // Color
    VM_registerBuiltin(ctx, "merge_color", builtinMergeColor);
    VM_registerBuiltin(ctx, "merge_colour", builtinMergeColor);

    // Surface
    VM_registerBuiltin(ctx, "surface_create", builtin_surface_create);
    VM_registerBuiltin(ctx, "surface_free", builtin_surface_free);
    VM_registerBuiltin(ctx, "surface_set_target", builtin_surface_set_target);
    VM_registerBuiltin(ctx, "surface_reset_target", builtin_surface_reset_target);
    VM_registerBuiltin(ctx, "surface_exists", builtin_surface_exists);
    VM_registerBuiltin(ctx, "surface_get_width", builtinSurfaceGetWidth);
    VM_registerBuiltin(ctx, "surface_get_height", builtinSurfaceGetHeight);

    // Sprite info
    VM_registerBuiltin(ctx, "sprite_get_width", builtin_spriteGetWidth);
    VM_registerBuiltin(ctx, "sprite_get_height", builtin_spriteGetHeight);
    VM_registerBuiltin(ctx, "sprite_get_number", builtin_spriteGetNumber);
    VM_registerBuiltin(ctx, "sprite_get_xoffset", builtin_spriteGetXOffset);
    VM_registerBuiltin(ctx, "sprite_get_yoffset", builtin_spriteGetYOffset);
    VM_registerBuiltin(ctx, "sprite_get_name", builtin_spriteGetName);
    VM_registerBuiltin(ctx, "sprite_set_offset", builtin_spriteSetOffset);
    VM_registerBuiltin(ctx, "sprite_create_from_surface", builtin_spriteCreateFromSurface);
    VM_registerBuiltin(ctx, "sprite_delete", builtin_spriteDelete);

    // Text measurement
    VM_registerBuiltin(ctx, "string_width", builtin_stringWidth);
    VM_registerBuiltin(ctx, "string_height", builtin_stringHeight);
    VM_registerBuiltin(ctx, "string_width_ext", builtin_string_width_ext);
    VM_registerBuiltin(ctx, "string_height_ext", builtin_string_height_ext);

    // Color
    VM_registerBuiltin(ctx, "make_color_rgb", builtinMakeColor);
    VM_registerBuiltin(ctx, "make_colour_rgb", builtinMakeColour);
    VM_registerBuiltin(ctx, "make_color_hsv", builtinMakeColorHsv);
    VM_registerBuiltin(ctx, "make_colour_hsv", builtinMakeColourHsv);

    // Display
    VM_registerBuiltin(ctx, "display_get_width", builtin_display_get_width);
    VM_registerBuiltin(ctx, "display_get_height", builtin_display_get_height);
    VM_registerBuiltin(ctx, "display_get_gui_width", builtinDisplayGetGuiWidth);
    VM_registerBuiltin(ctx, "display_get_gui_height", builtinDisplayGetGuiHeight);
    VM_registerBuiltin(ctx, "display_set_gui_size", builtinDisplaySetGuiSize);
    VM_registerBuiltin(ctx, "display_set_gui_maximise", builtinDisplaySetGuiMaximise);
    VM_registerBuiltin(ctx, "display_set_gui_maximize", builtinDisplaySetGuiMaximise);

    // Collision
    VM_registerBuiltin(ctx, "place_meeting", builtinPlaceMeeting);
    VM_registerBuiltin(ctx, "collision_rectangle", builtinCollisionRectangle);
    VM_registerBuiltin(ctx, "collision_line", builtinCollisionLine);
    VM_registerBuiltin(ctx, "collision_point", builtinCollisionPoint);
    VM_registerBuiltin(ctx, "instance_position", builtinInstancePosition);
    VM_registerBuiltin(ctx, "place_free", builtinPlaceFree);
    VM_registerBuiltin(ctx, "place_empty", builtinPlaceEmpty);

    // Motion planning
    VM_registerBuiltin(ctx, "mp_linear_step", builtinMpLinearStep);
    VM_registerBuiltin(ctx, "mp_linear_step_object", builtinMpLinearStepObject);
    VM_registerBuiltin(ctx, "mp_potential_step", builtinMpPotentialStep);
    VM_registerBuiltin(ctx, "mp_potential_step_object", builtinMpPotentialStepObject);
    VM_registerBuiltin(ctx, "mp_potential_settings", builtinMpPotentialSettings);

    // Tile layers
    VM_registerBuiltin(ctx, "tile_layer_hide", builtinTileLayerHide);
    VM_registerBuiltin(ctx, "tile_layer_show", builtinTileLayerShow);
    VM_registerBuiltin(ctx, "tile_layer_shift", builtinTileLayerShift);

    // Layer
    VM_registerBuiltin(ctx, "layer_force_draw_depth", builtinLayerForceDrawDepth);
    VM_registerBuiltin(ctx, "layer_is_draw_depth_forced", builtinLayerIsDrawDepthForced);
    VM_registerBuiltin(ctx, "layer_get_forced_depth", builtinLayerGetForcedDepth);
    VM_registerBuiltin(ctx, "layer_get_id", builtinLayerGetId);
    VM_registerBuiltin(ctx, "layer_exists", builtinLayerExists);
    VM_registerBuiltin(ctx, "layer_get_name", builtinLayerGetName);
    VM_registerBuiltin(ctx, "layer_get_depth", builtinLayerGetDepth);
    VM_registerBuiltin(ctx, "layer_depth", builtinLayerDepth);
    VM_registerBuiltin(ctx, "layer_get_visible", builtinLayerGetVisible);
    VM_registerBuiltin(ctx, "layer_set_visible", builtinLayerSetVisible);
    VM_registerBuiltin(ctx, "layer_get_x", builtinLayerGetX);
    VM_registerBuiltin(ctx, "layer_x", builtinLayerX);
    VM_registerBuiltin(ctx, "layer_get_y", builtinLayerGetY);
    VM_registerBuiltin(ctx, "layer_y", builtinLayerY);
    VM_registerBuiltin(ctx, "layer_get_hspeed", builtinLayerGetHspeed);
    VM_registerBuiltin(ctx, "layer_hspeed", builtinLayerHspeed);
    VM_registerBuiltin(ctx, "layer_get_vspeed", builtinLayerGetVspeed);
    VM_registerBuiltin(ctx, "layer_vspeed", builtinLayerVspeed);
#if IS_BC17_OR_HIGHER_ENABLED
    VM_registerBuiltin(ctx, "layer_get_all", builtinLayerGetAll);
    VM_registerBuiltin(ctx, "layer_get_all_elements", builtinLayerGetAllElements);
#endif
    VM_registerBuiltin(ctx, "layer_get_element_type", builtinLayerGetElementType);
    VM_registerBuiltin(ctx, "layer_sprite_get_sprite", builtinLayerSpriteGetSprite);
    VM_registerBuiltin(ctx, "layer_sprite_destroy", builtinLayerSpriteDestroy);
#if IS_BC17_OR_HIGHER_ENABLED
    VM_registerBuiltin(ctx, "layer_get_id_at_depth", builtinLayerGetIdAtDepth);
#endif
    VM_registerBuiltin(ctx, "layer_create", builtinLayerCreate);
    VM_registerBuiltin(ctx, "layer_destroy", builtinLayerDestroy);
    VM_registerBuiltin(ctx, "layer_background_create", builtinLayerBackgroundCreate);
    VM_registerBuiltin(ctx, "layer_background_exists", builtinLayerBackgroundExists);
    VM_registerBuiltin(ctx, "layer_background_visible", builtinLayerBackgroundVisible);
    VM_registerBuiltin(ctx, "layer_background_htiled", builtinLayerBackgroundHtiled);
    VM_registerBuiltin(ctx, "layer_background_vtiled", builtinLayerBackgroundVtiled);
    VM_registerBuiltin(ctx, "layer_background_xscale", builtinLayerBackgroundXscale);
    VM_registerBuiltin(ctx, "layer_background_yscale", builtinLayerBackgroundYscale);
    VM_registerBuiltin(ctx, "layer_background_stretch", builtinLayerBackgroundStretch);
    VM_registerBuiltin(ctx, "layer_background_blend", builtinLayerBackgroundBlend);
    VM_registerBuiltin(ctx, "layer_background_alpha", builtinLayerBackgroundAlpha);

    // GMS2 internal
    VM_registerBuiltin(ctx, "@@NewGMLArray@@", builtinNewGMLArray);
    VM_registerBuiltin(ctx, "@@This@@", builtinThis);
    VM_registerBuiltin(ctx, "@@Other@@", builtinOther);

    // Path
    VM_registerBuiltin(ctx, "path_start", builtinPathStart);
    VM_registerBuiltin(ctx, "path_end", builtinPathEnd);

    // Misc
    VM_registerBuiltin(ctx, "get_timer", builtin_get_timer);
    VM_registerBuiltin(ctx, "action_if_variable", builtinActionIfVariable);
    VM_registerBuiltin(ctx, "action_set_alarm", builtinActionSetAlarm);
    VM_registerBuiltin(ctx, "alarm_set", builtinAlarmSet);
    VM_registerBuiltin(ctx, "alarm_get", builtinAlarmGet);
    VM_registerBuiltin(ctx, "action_sound",builtin_action_sound);
    VM_registerBuiltin(ctx, "string_hash_to_newline", builtinStringHashToNewline);
    VM_registerBuiltin(ctx, "json_decode", builtinJsonDecode);
    VM_registerBuiltin(ctx, "font_add_sprite", builtinFontAddSprite);
    VM_registerBuiltin(ctx, "font_add_sprite_ext", builtinFontAddSpriteExt);
    VM_registerBuiltin(ctx, "object_get_sprite", builtinObjectGetSprite);
    VM_registerBuiltin(ctx, "asset_get_index", builtinAssetGetIndex);
}

