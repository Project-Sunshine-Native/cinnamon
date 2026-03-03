#include "vm_builtins.h"
#include "instance.h"
#include "runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

#include "stb_ds.h"
#include "utils.h"

// ===[ BUILTIN FUNCTION REGISTRY ]===
typedef struct {
    char* key;
    BuiltinFunc value;
} BuiltinEntry;

static bool initialized = false;
static BuiltinEntry* builtinMap = nullptr;

static void registerBuiltin(const char* name, BuiltinFunc func) {
    requireMessage(shgeti(builtinMap, name) == -1, "Trying to register an already registered builtin function!");
    shput(builtinMap, (char*) name, func);
}

BuiltinFunc VMBuiltins_find(const char* name) {
    ptrdiff_t idx = shgeti(builtinMap, (char*) name);
    if (0 > idx) return nullptr;
    return builtinMap[idx].value;
}

// ===[ STUB LOGGING ]===

static void logStubbedFunction(VMContext* ctx, const char* funcName) {
    const char* callerName = VM_getCallerName(ctx);
    char* dedupKey = VM_createDedupKey(callerName, funcName);

    if (0 > shgeti(ctx->loggedStubbedFuncs, dedupKey)) {
        // shput stores the key pointer, so don't free it when inserting
        shput(ctx->loggedStubbedFuncs, dedupKey, true);
        fprintf(stderr, "VM: [%s] Stubbed function \"%s\"!\n", callerName, funcName);
    } else {
        free(dedupKey);
    }
}

// ===[ DS_MAP SYSTEM ]===

typedef struct {
    char* key;
    RValue value;
} DsMapEntry;

static DsMapEntry** dsMapPool = nullptr; // stb_ds array of shash maps

static int32_t dsMapCreate(void) {
    DsMapEntry* newMap = nullptr;
    int32_t id = (int32_t) arrlen(dsMapPool);
    arrput(dsMapPool, newMap);
    return id;
}

static DsMapEntry** dsMapGet(int32_t id) {
    if (id < 0 || (int32_t) arrlen(dsMapPool) <= id) return nullptr;
    return &dsMapPool[id];
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

RValue VMBuiltins_getVariable(VMContext* ctx, const char* name, int32_t arrayIndex) {
    Instance* inst = (Instance*) ctx->currentInstance;
    Runner* runner = (Runner*) ctx->runner;

    // OS constants
    if (strcmp(name, "os_type") == 0) return RValue_makeReal(4.0); // os_linux
    if (strcmp(name, "os_windows") == 0) return RValue_makeReal(0.0);
    if (strcmp(name, "os_ps4") == 0) return RValue_makeReal(6.0);
    if (strcmp(name, "os_psvita") == 0) return RValue_makeReal(12.0);
    if (strcmp(name, "os_3ds") == 0) return RValue_makeReal(14.0);
    if (strcmp(name, "os_switch_") == 0) return RValue_makeReal(19.0);

    // Per-instance properties
    if (inst != nullptr) {
        if (strcmp(name, "image_speed") == 0) return RValue_makeReal(inst->imageSpeed);
        if (strcmp(name, "image_index") == 0) return RValue_makeReal(inst->imageIndex);
        if (strcmp(name, "image_xscale") == 0) return RValue_makeReal(inst->imageXscale);
        if (strcmp(name, "image_yscale") == 0) return RValue_makeReal(inst->imageYscale);
        if (strcmp(name, "image_angle") == 0) return RValue_makeReal(inst->imageAngle);
        if (strcmp(name, "image_alpha") == 0) return RValue_makeReal(inst->imageAlpha);
        if (strcmp(name, "image_blend") == 0) return RValue_makeReal((double) inst->imageBlend);
        if (strcmp(name, "sprite_index") == 0) return RValue_makeReal((double) inst->spriteIndex);
        if (strcmp(name, "visible") == 0) return RValue_makeBool(inst->visible);
        if (strcmp(name, "depth") == 0) return RValue_makeReal((double) inst->depth);
        if (strcmp(name, "x") == 0) return RValue_makeReal(inst->x);
        if (strcmp(name, "y") == 0) return RValue_makeReal(inst->y);
        if (strcmp(name, "id") == 0) return RValue_makeReal((double) inst->instanceId);
        if (strcmp(name, "object_index") == 0) return RValue_makeReal((double) inst->objectIndex);
        if (strcmp(name, "persistent") == 0) return RValue_makeBool(inst->persistent);
        if (strcmp(name, "solid") == 0) return RValue_makeBool(inst->solid);
        if (strcmp(name, "alarm") == 0) {
            if (isValidAlarmIndex(arrayIndex)) {
                return RValue_makeReal((double) inst->alarm[arrayIndex]);
            }
            return RValue_makeReal(-1.0);
        }
    }

    // Room properties
    if (runner != nullptr) {
        if (strcmp(name, "room") == 0) return RValue_makeReal((double) runner->currentRoomIndex);
        if (strcmp(name, "room_speed") == 0) return RValue_makeReal((double) runner->currentRoom->speed);
        if (strcmp(name, "room_width") == 0) return RValue_makeReal((double) runner->currentRoom->width);
        if (strcmp(name, "room_height") == 0) return RValue_makeReal((double) runner->currentRoom->height);
    }

    // Timing
    if (strcmp(name, "current_time") == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double ms = (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1000000.0;
        return RValue_makeReal(ms);
    }

    // argument_count
    if (strcmp(name, "argument_count") == 0) return RValue_makeReal((double) ctx->scriptArgCount);

    // argument[N] - array-style access to script arguments
    if (strcmp(name, "argument") == 0) {
        if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > arrayIndex && arrayIndex >= 0) {
            RValue val = ctx->scriptArgs[arrayIndex];
            val.ownsString = false;
            return val;
        }
        return RValue_makeUndefined();
    }

    // Argument variables (argument0..argument15 are built-in in GMS bytecode, stored in scriptArgs)
    const int argNumber = extractArgumentNumber(name);
    if (argNumber != -1) {
        if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > argNumber) {
            RValue val = ctx->scriptArgs[argNumber];
            val.ownsString = false;
            return val;
        }
        return RValue_makeUndefined();
    }

    // Constants that GMS defines
    if (strcmp(name, "true") == 0) return RValue_makeBool(true);
    if (strcmp(name, "false") == 0) return RValue_makeBool(false);
    if (strcmp(name, "pi") == 0) return RValue_makeReal(3.14159265358979323846);
    if (strcmp(name, "undefined") == 0) return RValue_makeUndefined();

    fprintf(stderr, "VM: Unhandled built-in variable read '%s' (arrayIndex=%d)\n", name, arrayIndex);
    return RValue_makeReal(0.0);
}

void VMBuiltins_setVariable(VMContext* ctx, const char* name, RValue val, int32_t arrayIndex) {
    Instance* inst = (Instance*) ctx->currentInstance;

    // Per-instance properties
    if (inst != nullptr) {
        if (strcmp(name, "image_speed") == 0) { inst->imageSpeed = RValue_toReal(val); return; }
        if (strcmp(name, "image_index") == 0) { inst->imageIndex = RValue_toReal(val); return; }
        if (strcmp(name, "image_xscale") == 0) { inst->imageXscale = RValue_toReal(val); return; }
        if (strcmp(name, "image_yscale") == 0) { inst->imageYscale = RValue_toReal(val); return; }
        if (strcmp(name, "image_angle") == 0) { inst->imageAngle = RValue_toReal(val); return; }
        if (strcmp(name, "image_alpha") == 0) { inst->imageAlpha = RValue_toReal(val); return; }
        if (strcmp(name, "image_blend") == 0) { inst->imageBlend = (uint32_t) RValue_toReal(val); return; }
        if (strcmp(name, "sprite_index") == 0) { inst->spriteIndex = RValue_toInt32(val); return; }
        if (strcmp(name, "visible") == 0) { inst->visible = RValue_toBool(val); return; }
        if (strcmp(name, "depth") == 0) { inst->depth = RValue_toInt32(val); return; }
        if (strcmp(name, "x") == 0) { inst->x = RValue_toReal(val); return; }
        if (strcmp(name, "y") == 0) { inst->y = RValue_toReal(val); return; }
        if (strcmp(name, "persistent") == 0) { inst->persistent = RValue_toBool(val); return; }
        if (strcmp(name, "solid") == 0) { inst->solid = RValue_toBool(val); return; }
        if (strcmp(name, "alarm") == 0) {
            if (isValidAlarmIndex(arrayIndex)) {
                inst->alarm[arrayIndex] = RValue_toInt32(val);
            }
            return;
        }
    }

    // Read-only variables (silently ignore)
    if (strcmp(name, "os_type") == 0 || strcmp(name, "os_windows") == 0 ||
        strcmp(name, "os_ps4") == 0 || strcmp(name, "os_psvita") == 0 ||
        strcmp(name, "id") == 0 || strcmp(name, "object_index") == 0 ||
        strcmp(name, "current_time") == 0 || strcmp(name, "room") == 0) {
        fprintf(stderr, "VM: Warning - attempted write to read-only built-in '%s'\n", name);
        return;
    }

    // argument[N] - array-style write to script arguments
    if (strcmp(name, "argument") == 0) {
        if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > arrayIndex && arrayIndex >= 0) {
            RValue_free(&ctx->scriptArgs[arrayIndex]);
            ctx->scriptArgs[arrayIndex] = val;
        }
        return;
    }

    // Argument variables
    const int argNumber = extractArgumentNumber(name);
    if (argNumber != -1) {
        if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > argNumber) {
            RValue_free(&ctx->scriptArgs[argNumber]);
            ctx->scriptArgs[argNumber] = val;
        }
        return;
    }

    fprintf(stderr, "VM: Unhandled built-in variable write '%s' (arrayIndex=%d)\n", name, arrayIndex);
}

// ===[ BUILTIN FUNCTION IMPLEMENTATIONS ]===

static RValue builtinShowDebugMessage(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) {
        fprintf(stderr, "[show_debug_message] Expected at least 1 argument\n");
        return RValue_makeUndefined();
    }

    char* val = RValue_toString(args[0]);
    printf("Game: %s\n", val);
    free(val);

    return RValue_makeUndefined();
}

static RValue builtinStringLength(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount || args[0].type != RVALUE_STRING) {
        return RValue_makeInt32(0);
    }
    return RValue_makeInt32((int32_t) strlen(args[0].string));
}

static RValue builtinReal(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(RValue_toReal(args[0]));
}

static RValue builtinString(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeOwnedString(strdup(""));
    char* result = RValue_toString(args[0]);
    return RValue_makeOwnedString(result);
}

static RValue builtinFloor(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(floor(RValue_toReal(args[0])));
}

static RValue builtinCeil(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(ceil(RValue_toReal(args[0])));
}

static RValue builtinRound(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(round(RValue_toReal(args[0])));
}

static RValue builtinAbs(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(fabs(RValue_toReal(args[0])));
}

static RValue builtinSign(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    double val = RValue_toReal(args[0]);
    double result = (val > 0.0) ? 1.0 : ((0.0 > val) ? -1.0 : 0.0);
    return RValue_makeReal(result);
}

static RValue builtinMax(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    double result = -INFINITY;
    repeat(argCount, i) {
        double val = RValue_toReal(args[i]);
        if (val > result) result = val;
    }
    return RValue_makeReal(result);
}

static RValue builtinMin(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    double result = INFINITY;
    repeat(argCount, i) {
        double val = RValue_toReal(args[i]);
        if (result > val) result = val;
    }
    return RValue_makeReal(result);
}

static RValue builtinPower(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (2 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(pow(RValue_toReal(args[0]), RValue_toReal(args[1])));
}

static RValue builtinSqrt(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(sqrt(RValue_toReal(args[0])));
}

static RValue builtinSqr(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    double val = RValue_toReal(args[0]);
    return RValue_makeReal(val * val);
}

static RValue builtinIsString(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeBool(false);
    return RValue_makeBool(args[0].type == RVALUE_STRING);
}

static RValue builtinIsReal(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeBool(false);
    bool result = args[0].type == RVALUE_REAL || args[0].type == RVALUE_INT32 || args[0].type == RVALUE_INT64 || args[0].type == RVALUE_BOOL;
    return RValue_makeBool(result);
}

static RValue builtinIsUndefined(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeBool(true);
    return RValue_makeBool(args[0].type == RVALUE_UNDEFINED);
}

// ===[ STRING FUNCTIONS ]===

static RValue builtinStringUpper(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(strdup(""));
    char* result = strdup(args[0].string != nullptr ? args[0].string : "");
    for (char* p = result; *p; p++) *p = (char) toupper((unsigned char) *p);
    return RValue_makeOwnedString(result);
}

static RValue builtinStringLower(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(strdup(""));
    char* result = strdup(args[0].string != nullptr ? args[0].string : "");
    for (char* p = result; *p; p++) *p = (char) tolower((unsigned char) *p);
    return RValue_makeOwnedString(result);
}

static RValue builtinStringCopy(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (3 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(strdup(""));
    const char* str = args[0].string != nullptr ? args[0].string : "";
    int32_t pos = RValue_toInt32(args[1]) - 1; // GMS is 1-based
    int32_t len = RValue_toInt32(args[2]);
    int32_t strLen = (int32_t) strlen(str);

    if (0 > pos) pos = 0;
    if (pos >= strLen || 0 >= len) return RValue_makeOwnedString(strdup(""));
    if (pos + len > strLen) len = strLen - pos;

    char* result = malloc(len + 1);
    memcpy(result, str + pos, len);
    result[len] = '\0';
    return RValue_makeOwnedString(result);
}

static RValue builtinOrd(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount || args[0].type != RVALUE_STRING || args[0].string == nullptr || args[0].string[0] == '\0') {
        return RValue_makeReal(0.0);
    }
    return RValue_makeReal((double) (unsigned char) args[0].string[0]);
}

static RValue builtinChr(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeOwnedString(strdup(""));
    char buf[2] = { (char) RValue_toInt32(args[0]), '\0' };
    return RValue_makeOwnedString(strdup(buf));
}

static RValue builtinStringPos(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (2 > argCount || args[0].type != RVALUE_STRING || args[1].type != RVALUE_STRING) return RValue_makeReal(0.0);
    const char* needle = args[0].string != nullptr ? args[0].string : "";
    const char* haystack = args[1].string != nullptr ? args[1].string : "";
    const char* found = strstr(haystack, needle);
    if (found == nullptr) return RValue_makeReal(0.0);
    return RValue_makeReal((double) (found - haystack + 1)); // 1-based
}

static RValue builtinStringCharAt(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (2 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(strdup(""));
    const char* str = args[0].string != nullptr ? args[0].string : "";
    int32_t pos = RValue_toInt32(args[1]) - 1; // 1-based
    int32_t strLen = (int32_t) strlen(str);
    if (0 > pos || pos >= strLen) return RValue_makeOwnedString(strdup(""));
    char buf[2] = { str[pos], '\0' };
    return RValue_makeOwnedString(strdup(buf));
}

static RValue builtinStringDelete(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (3 > argCount || args[0].type != RVALUE_STRING) return RValue_makeOwnedString(strdup(""));
    const char* str = args[0].string != nullptr ? args[0].string : "";
    int32_t pos = RValue_toInt32(args[1]) - 1; // 1-based
    int32_t count = RValue_toInt32(args[2]);
    int32_t strLen = (int32_t) strlen(str);

    if (0 > pos || pos >= strLen || 0 >= count) return RValue_makeOwnedString(strdup(str));
    if (pos + count > strLen) count = strLen - pos;

    char* result = malloc(strLen - count + 1);
    memcpy(result, str, pos);
    memcpy(result + pos, str + pos + count, strLen - pos - count);
    result[strLen - count] = '\0';
    return RValue_makeOwnedString(result);
}

static RValue builtinStringInsert(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (3 > argCount || args[0].type != RVALUE_STRING || args[1].type != RVALUE_STRING) return RValue_makeOwnedString(strdup(""));
    const char* substr = args[0].string != nullptr ? args[0].string : "";
    const char* str = args[1].string != nullptr ? args[1].string : "";
    int32_t pos = RValue_toInt32(args[2]) - 1; // 1-based
    int32_t strLen = (int32_t) strlen(str);
    int32_t subLen = (int32_t) strlen(substr);

    if (0 > pos) pos = 0;
    if (pos > strLen) pos = strLen;

    char* result = malloc(strLen + subLen + 1);
    memcpy(result, str, pos);
    memcpy(result + pos, substr, subLen);
    memcpy(result + pos + subLen, str + pos, strLen - pos);
    result[strLen + subLen] = '\0';
    return RValue_makeOwnedString(result);
}

// ===[ MATH FUNCTIONS ]===

static RValue builtinDarctan2(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (2 > argCount) return RValue_makeReal(0.0);
    double y = RValue_toReal(args[0]);
    double x = RValue_toReal(args[1]);
    return RValue_makeReal(atan2(y, x) * (180.0 / M_PI));
}

static RValue builtinSin(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(sin(RValue_toReal(args[0])));
}

static RValue builtinCos(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(cos(RValue_toReal(args[0])));
}

static RValue builtinDegtorad(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(RValue_toReal(args[0]) * (M_PI / 180.0));
}

static RValue builtinRadtodeg(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(RValue_toReal(args[0]) * (180.0 / M_PI));
}

static RValue builtinClamp(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (3 > argCount) return RValue_makeReal(0.0);
    double val = RValue_toReal(args[0]);
    double lo = RValue_toReal(args[1]);
    double hi = RValue_toReal(args[2]);
    if (lo > val) val = lo;
    if (val > hi) val = hi;
    return RValue_makeReal(val);
}

static RValue builtinLerp(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (3 > argCount) return RValue_makeReal(0.0);
    double a = RValue_toReal(args[0]);
    double b = RValue_toReal(args[1]);
    double t = RValue_toReal(args[2]);
    return RValue_makeReal(a + (b - a) * t);
}

static RValue builtinPointDistance(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (4 > argCount) return RValue_makeReal(0.0);
    double dx = RValue_toReal(args[2]) - RValue_toReal(args[0]);
    double dy = RValue_toReal(args[3]) - RValue_toReal(args[1]);
    return RValue_makeReal(sqrt(dx * dx + dy * dy));
}

static RValue builtinPointDirection(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (4 > argCount) return RValue_makeReal(0.0);
    double dx = RValue_toReal(args[2]) - RValue_toReal(args[0]);
    double dy = RValue_toReal(args[3]) - RValue_toReal(args[1]);
    return RValue_makeReal(atan2(-dy, dx) * (180.0 / M_PI));
}

static RValue builtinLengthdir_x(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (2 > argCount) return RValue_makeReal(0.0);
    double len = RValue_toReal(args[0]);
    double dir = RValue_toReal(args[1]) * (M_PI / 180.0);
    return RValue_makeReal(len * cos(dir));
}

static RValue builtinLengthdir_y(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (2 > argCount) return RValue_makeReal(0.0);
    double len = RValue_toReal(args[0]);
    double dir = RValue_toReal(args[1]) * (M_PI / 180.0);
    return RValue_makeReal(-len * sin(dir));
}

// ===[ RANDOM FUNCTIONS ]===

static RValue builtinRandom(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    double n = RValue_toReal(args[0]);
    return RValue_makeReal(((double) rand() / (double) RAND_MAX) * n);
}

static RValue builtinRandomRange(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (2 > argCount) return RValue_makeReal(0.0);
    double lo = RValue_toReal(args[0]);
    double hi = RValue_toReal(args[1]);
    return RValue_makeReal(lo + ((double) rand() / (double) RAND_MAX) * (hi - lo));
}

static RValue builtinIrandom(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    int32_t n = RValue_toInt32(args[0]);
    if (0 >= n) return RValue_makeReal(0.0);
    return RValue_makeReal((double) (rand() % (n + 1)));
}

static RValue builtinIrandomRange(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (2 > argCount) return RValue_makeReal(0.0);
    int32_t lo = RValue_toInt32(args[0]);
    int32_t hi = RValue_toInt32(args[1]);
    if (lo > hi) { int32_t tmp = lo; lo = hi; hi = tmp; }
    int32_t range = hi - lo + 1;
    if (0 >= range) return RValue_makeReal((double) lo);
    return RValue_makeReal((double) (lo + rand() % range));
}

static RValue builtinChoose(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeUndefined();
    int32_t idx = rand() % argCount;
    // Must duplicate the value since args will be freed
    RValue val = args[idx];
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        return RValue_makeOwnedString(strdup(val.string));
    }
    return val;
}

static RValue builtinRandomize(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) args; (void) argCount;
    logStubbedFunction(ctx, "randomize");
    srand((unsigned int) time(nullptr));
    return RValue_makeUndefined();
}

// ===[ ROOM FUNCTIONS ]===

static RValue builtinRoomGotoNext(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) args; (void) argCount;
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_goto_next called but no runner!");

    int32_t nextPos = runner->currentRoomOrderPosition + 1;
    if ((int32_t) runner->dataWin->gen8.roomOrderCount > nextPos) {
        runner->pendingRoom = runner->dataWin->gen8.roomOrder[nextPos];
    } else {
        fprintf(stderr, "VM: room_goto_next - already at last room!\n");
    }
    return RValue_makeUndefined();
}

static RValue builtinRoomGoto(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    Runner* runner = requireNotNullMessage(ctx->runner, "VM: room_goto called but no runner!");
    runner->pendingRoom = RValue_toInt32(args[0]);
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
            return RValue_makeOwnedString(strdup(val.string));
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
            ctx->globalVars[varID] = RValue_makeOwnedString(strdup(val.string));
        } else {
            ctx->globalVars[varID] = val;
        }
    }
    return RValue_makeUndefined();
}

// ===[ SCRIPT EXECUTE ]===

static RValue builtinScriptExecute(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeUndefined();
    int32_t scriptIdx = RValue_toInt32(args[0]);

    // Look up the script to get its codeId
    if (scriptIdx < 0 || (uint32_t) scriptIdx >= ctx->dataWin->scpt.count) {
        fprintf(stderr, "VM: script_execute - invalid script index %d\n", scriptIdx);
        return RValue_makeUndefined();
    }

    int32_t codeId = ctx->dataWin->scpt.scripts[scriptIdx].codeId;
    if (0 > codeId || ctx->dataWin->code.count <= (uint32_t) codeId) {
        fprintf(stderr, "VM: script_execute - invalid codeId %d for script %d\n", codeId, scriptIdx);
        return RValue_makeUndefined();
    }

    // Pass remaining args (skip the script index)
    RValue* scriptArgs = (argCount > 1) ? &args[1] : nullptr;
    int32_t scriptArgCount = argCount - 1;

    return VM_callCodeIndex(ctx, codeId, scriptArgs, scriptArgCount);
}

// ===[ OS FUNCTIONS ]===

static RValue builtinOsGetLanguage(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx; (void) args; (void) argCount;
    return RValue_makeOwnedString(strdup("en"));
}

static RValue builtinOsGetRegion(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx; (void) args; (void) argCount;
    return RValue_makeOwnedString(strdup("US"));
}

// ===[ DS_MAP BUILTIN FUNCTIONS ]===

static RValue builtinDsMapCreate(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx; (void) args; (void) argCount;
    return RValue_makeReal((double) dsMapCreate());
}

static RValue builtinDsMapAdd(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (3 > argCount) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr) return RValue_makeUndefined();

    char* key = RValue_toString(args[1]);

    // Only add if key doesn't exist
    bool exists = shgeti(*mapPtr, key) != -1;

    if (exists) {
        free(key); // Key already exists, we didn't insert it
    } else {
        RValue val = args[2];
        if (val.type == RVALUE_STRING && val.string != nullptr) {
            val = RValue_makeOwnedString(strdup(val.string));
        }
        shput(*mapPtr, key, val);
        // The RValue is now "owned" by the map, we do not need to free it!
    }

    return RValue_makeUndefined();
}

static RValue builtinDsMapSet(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (3 > argCount) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr) return RValue_makeUndefined();

    char* key = RValue_toString(args[1]);

    ptrdiff_t existingKeyIndex = shgeti(*mapPtr, key);

    if (existingKeyIndex != -1) {
        // If it already exists, we'll get the current value and free it
        RValue_free(&(*mapPtr)[existingKeyIndex].value);
    }

    RValue val = args[2];
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        val = RValue_makeOwnedString(strdup(val.string));
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
    (void) ctx;
    if (2 > argCount) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr) return RValue_makeUndefined();

    char* key = RValue_toString(args[1]);
    ptrdiff_t idx = shgeti(*mapPtr, key);
    free(key);
    if (0 > idx) return RValue_makeUndefined();
    RValue val = (*mapPtr)[idx].value;
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        return RValue_makeOwnedString(strdup(val.string));
    }
    return val;
}

static RValue builtinDsMapExists(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (2 > argCount) return RValue_makeReal(0.0);
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr) return RValue_makeReal(0.0);

    char* key = RValue_toString(args[1]);
    ptrdiff_t idx = shgeti(*mapPtr, key);
    free(key);
    return RValue_makeReal(idx >= 0 ? 1.0 : 0.0);
}

static RValue builtinDsMapFindFirst(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr || shlen(*mapPtr) == 0) return RValue_makeUndefined();
    return RValue_makeOwnedString(strdup((*mapPtr)[0].key));
}

static RValue builtinDsMapFindNext(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (2 > argCount) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr) return RValue_makeUndefined();

    char* prevKey = RValue_toString(args[1]);
    ptrdiff_t idx = shgeti(*mapPtr, prevKey);
    free(prevKey);
    if (0 > idx || idx + 1 >= shlen(*mapPtr)) return RValue_makeUndefined();
    return RValue_makeOwnedString(strdup((*mapPtr)[idx + 1].key));
}

static RValue builtinDsMapSize(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeReal(0.0);
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
    if (mapPtr == nullptr) return RValue_makeReal(0.0);
    return RValue_makeReal((double) shlen(*mapPtr));
}

static RValue builtinDsMapDestroy(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return RValue_makeUndefined();
    int32_t id = RValue_toInt32(args[0]);
    DsMapEntry** mapPtr = dsMapGet(id);
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

// ===[ DS_LIST STUBS ]===

static RValue builtinDsListCreate(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) args; (void) argCount;
    logStubbedFunction(ctx, "ds_list_create");
    return RValue_makeReal(0.0);
}

static RValue builtinDsListAdd(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) args; (void) argCount;
    logStubbedFunction(ctx, "ds_list_add");
    return RValue_makeUndefined();
}

static RValue builtinDsListSize(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) args; (void) argCount;
    logStubbedFunction(ctx, "ds_list_size");
    return RValue_makeReal(0.0);
}

// ===[ ARRAY FUNCTIONS ]===

static RValue builtinArrayLengthId(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    (void) args; (void) argCount;
    // array_length_1d / array_length_2d stubs
    logStubbedFunction(ctx, "array_length_1d");
    return RValue_makeReal(0.0);
}

// ===[ STUBBED FUNCTIONS ]===

#define STUB_RETURN_ZERO(name) \
    static RValue builtin_##name(VMContext* ctx, RValue* args, int32_t argCount) { \
        (void) args; (void) argCount; \
        logStubbedFunction(ctx, #name); \
        return RValue_makeReal(0.0); \
    }

#define STUB_RETURN_UNDEFINED(name) \
    static RValue builtin_##name(VMContext* ctx, RValue* args, int32_t argCount) { \
        (void) args; (void) argCount; \
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

// Audio stubs
STUB_RETURN_UNDEFINED(audio_channel_num)
STUB_RETURN_UNDEFINED(audio_play_sound)
STUB_RETURN_UNDEFINED(audio_stop_sound)
STUB_RETURN_UNDEFINED(audio_stop_all)
STUB_RETURN_ZERO(audio_is_playing)
STUB_RETURN_UNDEFINED(audio_sound_gain)
STUB_RETURN_UNDEFINED(audio_sound_pitch)
STUB_RETURN_ZERO(audio_sound_get_gain)
STUB_RETURN_ZERO(audio_sound_get_pitch)
STUB_RETURN_UNDEFINED(audio_master_gain)
STUB_RETURN_UNDEFINED(audio_group_load)
STUB_RETURN_ZERO(audio_group_is_loaded)
STUB_RETURN_UNDEFINED(audio_play_music)
STUB_RETURN_UNDEFINED(audio_stop_music)
STUB_RETURN_UNDEFINED(audio_music_gain)
STUB_RETURN_ZERO(audio_music_is_playing)

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

// INI stubs
STUB_RETURN_UNDEFINED(ini_open)
STUB_RETURN_UNDEFINED(ini_close)
STUB_RETURN_UNDEFINED(ini_write_real)
STUB_RETURN_UNDEFINED(ini_write_string)

static RValue builtinIniReadString(VMContext* ctx, RValue* args, int32_t argCount) {
    logStubbedFunction(ctx, "ini_read_string");
    if (3 > argCount) return RValue_makeOwnedString(strdup(""));
    // Return the default value (3rd arg)
    if (args[2].type == RVALUE_STRING && args[2].string != nullptr) {
        return RValue_makeOwnedString(strdup(args[2].string));
    }
    char* str = RValue_toString(args[2]);
    return RValue_makeOwnedString(str);
}

static RValue builtinIniReadReal(VMContext* ctx, RValue* args, int32_t argCount) {
    logStubbedFunction(ctx, "ini_read_real");
    if (3 > argCount) return RValue_makeReal(0.0);
    return RValue_makeReal(RValue_toReal(args[2]));
}

// File stubs
STUB_RETURN_ZERO(file_exists)
STUB_RETURN_ZERO(file_text_open_write)
STUB_RETURN_ZERO(file_text_open_read)
STUB_RETURN_UNDEFINED(file_text_close)
STUB_RETURN_UNDEFINED(file_text_write_string)
STUB_RETURN_UNDEFINED(file_text_writeln)
STUB_RETURN_UNDEFINED(file_text_write_real)
STUB_RETURN_ZERO(file_text_eof)
STUB_RETURN_UNDEFINED(file_delete)

static RValue builtinFileTextReadString(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) args; (void) argCount;
    logStubbedFunction(ctx, "file_text_read_string");
    return RValue_makeOwnedString(strdup(""));
}

static RValue builtinFileTextReadReal(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) args; (void) argCount;
    logStubbedFunction(ctx, "file_text_read_real");
    return RValue_makeReal(0.0);
}

// Keyboard stubs
STUB_RETURN_ZERO(keyboard_check)
STUB_RETURN_ZERO(keyboard_check_pressed)
STUB_RETURN_ZERO(keyboard_check_released)
STUB_RETURN_ZERO(keyboard_check_direct)
STUB_RETURN_UNDEFINED(keyboard_key_press)
STUB_RETURN_UNDEFINED(keyboard_key_release)
STUB_RETURN_UNDEFINED(keyboard_clear)

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
STUB_RETURN_UNDEFINED(window_set_caption)
STUB_RETURN_UNDEFINED(window_set_size)
STUB_RETURN_UNDEFINED(window_center)
STUB_RETURN_ZERO(window_get_width)
STUB_RETURN_ZERO(window_get_height)

// Game stubs
STUB_RETURN_UNDEFINED(game_restart)
STUB_RETURN_UNDEFINED(game_end)
STUB_RETURN_UNDEFINED(game_save)
STUB_RETURN_UNDEFINED(game_load)

// Instance stubs
STUB_RETURN_ZERO(instance_number)

static RValue builtinInstanceExists(VMContext* ctx, RValue* args, int32_t argCount) {
    if (1 > argCount) return RValue_makeBool(false);
    Runner* runner = (Runner*) ctx->runner;
    int32_t id = RValue_toInt32(args[0]);
    bool found = false;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);
    if (id >= 0 && runner->dataWin->objt.count > (uint32_t) id) {
        // Object type index: search for any active instance with that objectIndex
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst != nullptr && inst->active && inst->objectIndex == id) {
                found = true;
                break;
            }
        }
    } else {
        // Instance ID: search for a specific instance
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst != nullptr && inst->active && inst->instanceId == (uint32_t) id) {
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
        // Object type index: destroy all active instances with that objectIndex
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst != nullptr && inst->active && inst->objectIndex == id) {
                Runner_destroyInstance(runner, inst);
            }
        }
    } else {
        // Instance ID: destroy that specific instance
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (inst != nullptr && inst->active && inst->instanceId == (uint32_t) id) {
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
    double x = RValue_toReal(args[0]);
    double y = RValue_toReal(args[1]);
    int32_t objectIndex = RValue_toInt32(args[2]);
    if (0 > objectIndex || runner->dataWin->objt.count <= (uint32_t) objectIndex) {
        fprintf(stderr, "VM: instance_create: objectIndex %d out of range\n", objectIndex);
        return RValue_makeReal(0.0);
    }
    Instance* inst = Runner_createInstance(runner, x, y, objectIndex);
    return RValue_makeReal((double) inst->instanceId);
}

static RValue builtinEventInherited(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) args; (void) argCount;
    Runner* runner = (Runner*) ctx->runner;
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr || 0 > ctx->currentEventObjectIndex || 0 > ctx->currentEventType) return RValue_makeReal(0.0);

    DataWin* dataWin = ctx->dataWin;
    int32_t ownerObjectIndex = ctx->currentEventObjectIndex;
    if ((uint32_t) ownerObjectIndex >= dataWin->objt.count) return RValue_makeReal(0.0);

    int32_t parentObjectIndex = dataWin->objt.objects[ownerObjectIndex].parentId;
    if (0 > parentObjectIndex) return RValue_makeReal(0.0);

    Runner_executeEventFromObject(runner, inst, parentObjectIndex, ctx->currentEventType, ctx->currentEventSubtype);
    return RValue_makeReal(0.0);
}

static RValue builtinActionKillObject(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) args; (void) argCount;
    Runner* runner = (Runner*) ctx->runner;
    if (ctx->currentInstance != nullptr) {
        Runner_destroyInstance(runner, (Instance*) ctx->currentInstance);
    }
    return RValue_makeUndefined();
}

// Buffer stubs
STUB_RETURN_ZERO(buffer_create)
STUB_RETURN_UNDEFINED(buffer_delete)
STUB_RETURN_UNDEFINED(buffer_write)
STUB_RETURN_ZERO(buffer_read)
STUB_RETURN_UNDEFINED(buffer_seek)
STUB_RETURN_ZERO(buffer_tell)
STUB_RETURN_ZERO(buffer_get_size)
STUB_RETURN_ZERO(buffer_base64_encode)

// PSN stubs
STUB_RETURN_UNDEFINED(psn_init)
STUB_RETURN_ZERO(psn_default_user)
STUB_RETURN_ZERO(psn_get_leaderboard_score)

// Draw stubs
STUB_RETURN_UNDEFINED(draw_sprite)
STUB_RETURN_UNDEFINED(draw_sprite_ext)
STUB_RETURN_UNDEFINED(draw_rectangle)
STUB_RETURN_UNDEFINED(draw_set_color)
STUB_RETURN_UNDEFINED(draw_set_alpha)
STUB_RETURN_UNDEFINED(draw_set_font)
STUB_RETURN_UNDEFINED(draw_set_halign)
STUB_RETURN_UNDEFINED(draw_set_valign)
STUB_RETURN_UNDEFINED(draw_text)
STUB_RETURN_UNDEFINED(draw_text_transformed)
STUB_RETURN_UNDEFINED(draw_text_ext)
STUB_RETURN_UNDEFINED(draw_text_ext_transformed)
STUB_RETURN_UNDEFINED(draw_surface)
STUB_RETURN_UNDEFINED(draw_surface_ext)
STUB_RETURN_UNDEFINED(draw_background)
STUB_RETURN_UNDEFINED(draw_background_ext)
STUB_RETURN_UNDEFINED(draw_self)
STUB_RETURN_UNDEFINED(draw_line)
STUB_RETURN_UNDEFINED(draw_set_colour)
STUB_RETURN_ZERO(draw_get_colour)
STUB_RETURN_ZERO(draw_get_color)
STUB_RETURN_ZERO(draw_get_alpha)

// Surface stubs
STUB_RETURN_ZERO(surface_create)
STUB_RETURN_UNDEFINED(surface_free)
STUB_RETURN_UNDEFINED(surface_set_target)
STUB_RETURN_UNDEFINED(surface_reset_target)
STUB_RETURN_ZERO(surface_exists)
STUB_RETURN_ZERO(surface_get_width)
STUB_RETURN_ZERO(surface_get_height)

// Sprite stubs
STUB_RETURN_ZERO(sprite_get_width)
STUB_RETURN_ZERO(sprite_get_height)
STUB_RETURN_ZERO(sprite_get_number)
STUB_RETURN_ZERO(sprite_get_xoffset)
STUB_RETURN_ZERO(sprite_get_yoffset)

// Font/text stubs
STUB_RETURN_ZERO(string_width)
STUB_RETURN_ZERO(string_height)
STUB_RETURN_ZERO(string_width_ext)
STUB_RETURN_ZERO(string_height_ext)

// Color functions
static RValue builtinMakeColor(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (3 > argCount) return RValue_makeReal(0.0);
    int32_t r = RValue_toInt32(args[0]);
    int32_t g = RValue_toInt32(args[1]);
    int32_t b = RValue_toInt32(args[2]);
    return RValue_makeReal((double) (r | (g << 8) | (b << 16)));
}

static RValue builtinMakeColour(VMContext* ctx, RValue* args, int32_t argCount) {
    return builtinMakeColor(ctx, args, argCount);
}

// Display stubs
STUB_RETURN_ZERO(display_get_width)
STUB_RETURN_ZERO(display_get_height)

// Collision stubs
STUB_RETURN_ZERO(place_meeting)
STUB_RETURN_ZERO(collision_rectangle)
STUB_RETURN_ZERO(collision_line)
STUB_RETURN_ZERO(collision_point)

// Misc stubs
STUB_RETURN_ZERO(get_timer)
STUB_RETURN_UNDEFINED(action_set_alarm)

// ===[ REGISTRATION ]===

void VMBuiltins_registerAll(void) {
    requireMessage(!initialized, "Attempting to register all VMBuiltins, but it was already registered!");
    initialized = true;

    // Core output
    registerBuiltin("show_debug_message", builtinShowDebugMessage);

    // String functions
    registerBuiltin("string_length", builtinStringLength);
    registerBuiltin("string", builtinString);
    registerBuiltin("string_upper", builtinStringUpper);
    registerBuiltin("string_lower", builtinStringLower);
    registerBuiltin("string_copy", builtinStringCopy);
    registerBuiltin("substr", builtinStringCopy);
    registerBuiltin("string_pos", builtinStringPos);
    registerBuiltin("string_char_at", builtinStringCharAt);
    registerBuiltin("string_delete", builtinStringDelete);
    registerBuiltin("string_insert", builtinStringInsert);
    registerBuiltin("ord", builtinOrd);
    registerBuiltin("chr", builtinChr);

    // Type functions
    registerBuiltin("real", builtinReal);
    registerBuiltin("is_string", builtinIsString);
    registerBuiltin("is_real", builtinIsReal);
    registerBuiltin("is_undefined", builtinIsUndefined);

    // Math functions
    registerBuiltin("floor", builtinFloor);
    registerBuiltin("ceil", builtinCeil);
    registerBuiltin("round", builtinRound);
    registerBuiltin("abs", builtinAbs);
    registerBuiltin("sign", builtinSign);
    registerBuiltin("max", builtinMax);
    registerBuiltin("min", builtinMin);
    registerBuiltin("power", builtinPower);
    registerBuiltin("sqrt", builtinSqrt);
    registerBuiltin("sqr", builtinSqr);
    registerBuiltin("sin", builtinSin);
    registerBuiltin("cos", builtinCos);
    registerBuiltin("darctan2", builtinDarctan2);
    registerBuiltin("degtorad", builtinDegtorad);
    registerBuiltin("radtodeg", builtinRadtodeg);
    registerBuiltin("clamp", builtinClamp);
    registerBuiltin("lerp", builtinLerp);
    registerBuiltin("point_distance", builtinPointDistance);
    registerBuiltin("point_direction", builtinPointDirection);
    registerBuiltin("lengthdir_x", builtinLengthdir_x);
    registerBuiltin("lengthdir_y", builtinLengthdir_y);

    // Random
    registerBuiltin("random", builtinRandom);
    registerBuiltin("random_range", builtinRandomRange);
    registerBuiltin("irandom", builtinIrandom);
    registerBuiltin("irandom_range", builtinIrandomRange);
    registerBuiltin("choose", builtinChoose);
    registerBuiltin("randomize", builtinRandomize);

    // Room
    registerBuiltin("room_goto_next", builtinRoomGotoNext);
    registerBuiltin("room_goto", builtinRoomGoto);

    // Variables
    registerBuiltin("variable_global_exists", builtinVariableGlobalExists);
    registerBuiltin("variable_global_get", builtinVariableGlobalGet);
    registerBuiltin("variable_global_set", builtinVariableGlobalSet);

    // Script
    registerBuiltin("script_execute", builtinScriptExecute);

    // OS
    registerBuiltin("os_get_language", builtinOsGetLanguage);
    registerBuiltin("os_get_region", builtinOsGetRegion);

    // ds_map
    registerBuiltin("ds_map_create", builtinDsMapCreate);
    registerBuiltin("ds_map_add", builtinDsMapAdd);
    registerBuiltin("ds_map_set", builtinDsMapSet);
    registerBuiltin("ds_map_replace", builtinDsMapReplace);
    registerBuiltin("ds_map_find_value", builtinDsMapFindValue);
    registerBuiltin("ds_map_exists", builtinDsMapExists);
    registerBuiltin("ds_map_find_first", builtinDsMapFindFirst);
    registerBuiltin("ds_map_find_next", builtinDsMapFindNext);
    registerBuiltin("ds_map_size", builtinDsMapSize);
    registerBuiltin("ds_map_destroy", builtinDsMapDestroy);

    // ds_list stubs
    registerBuiltin("ds_list_create", builtinDsListCreate);
    registerBuiltin("ds_list_add", builtinDsListAdd);
    registerBuiltin("ds_list_size", builtinDsListSize);

    // Array
    registerBuiltin("array_length_1d", builtinArrayLengthId);

    // Steam stubs
    registerBuiltin("steam_initialised", builtin_steam_initialised);
    registerBuiltin("steam_stats_ready", builtin_steam_stats_ready);
    registerBuiltin("steam_file_exists", builtin_steam_file_exists);
    registerBuiltin("steam_file_write", builtin_steam_file_write);
    registerBuiltin("steam_file_read", builtin_steam_file_read);
    registerBuiltin("steam_get_persona_name", builtin_steam_get_persona_name);

    // Audio stubs
    registerBuiltin("audio_channel_num", builtin_audio_channel_num);
    registerBuiltin("audio_play_sound", builtin_audio_play_sound);
    registerBuiltin("audio_stop_sound", builtin_audio_stop_sound);
    registerBuiltin("audio_stop_all", builtin_audio_stop_all);
    registerBuiltin("audio_is_playing", builtin_audio_is_playing);
    registerBuiltin("audio_sound_gain", builtin_audio_sound_gain);
    registerBuiltin("audio_sound_pitch", builtin_audio_sound_pitch);
    registerBuiltin("audio_sound_get_gain", builtin_audio_sound_get_gain);
    registerBuiltin("audio_sound_get_pitch", builtin_audio_sound_get_pitch);
    registerBuiltin("audio_master_gain", builtin_audio_master_gain);
    registerBuiltin("audio_group_load", builtin_audio_group_load);
    registerBuiltin("audio_group_is_loaded", builtin_audio_group_is_loaded);
    registerBuiltin("audio_play_music", builtin_audio_play_music);
    registerBuiltin("audio_stop_music", builtin_audio_stop_music);
    registerBuiltin("audio_music_gain", builtin_audio_music_gain);
    registerBuiltin("audio_music_is_playing", builtin_audio_music_is_playing);

    // Application surface
    registerBuiltin("application_surface_enable", builtin_application_surface_enable);
    registerBuiltin("application_surface_draw_enable", builtin_application_surface_draw_enable);

    // Gamepad
    registerBuiltin("gamepad_get_device_count", builtin_gamepad_get_device_count);
    registerBuiltin("gamepad_is_connected", builtin_gamepad_is_connected);
    registerBuiltin("gamepad_button_check", builtin_gamepad_button_check);
    registerBuiltin("gamepad_button_check_pressed", builtin_gamepad_button_check_pressed);
    registerBuiltin("gamepad_button_check_released", builtin_gamepad_button_check_released);
    registerBuiltin("gamepad_axis_value", builtin_gamepad_axis_value);
    registerBuiltin("gamepad_get_description", builtin_gamepad_get_description);
    registerBuiltin("gamepad_button_value", builtin_gamepad_button_value);

    // INI
    registerBuiltin("ini_open", builtin_ini_open);
    registerBuiltin("ini_close", builtin_ini_close);
    registerBuiltin("ini_write_real", builtin_ini_write_real);
    registerBuiltin("ini_write_string", builtin_ini_write_string);
    registerBuiltin("ini_read_string", builtinIniReadString);
    registerBuiltin("ini_read_real", builtinIniReadReal);

    // File
    registerBuiltin("file_exists", builtin_file_exists);
    registerBuiltin("file_text_open_write", builtin_file_text_open_write);
    registerBuiltin("file_text_open_read", builtin_file_text_open_read);
    registerBuiltin("file_text_close", builtin_file_text_close);
    registerBuiltin("file_text_write_string", builtin_file_text_write_string);
    registerBuiltin("file_text_writeln", builtin_file_text_writeln);
    registerBuiltin("file_text_write_real", builtin_file_text_write_real);
    registerBuiltin("file_text_eof", builtin_file_text_eof);
    registerBuiltin("file_delete", builtin_file_delete);
    registerBuiltin("file_text_read_string", builtinFileTextReadString);
    registerBuiltin("file_text_read_real", builtinFileTextReadReal);

    // Keyboard
    registerBuiltin("keyboard_check", builtin_keyboard_check);
    registerBuiltin("keyboard_check_pressed", builtin_keyboard_check_pressed);
    registerBuiltin("keyboard_check_released", builtin_keyboard_check_released);
    registerBuiltin("keyboard_check_direct", builtin_keyboard_check_direct);
    registerBuiltin("keyboard_key_press", builtin_keyboard_key_press);
    registerBuiltin("keyboard_key_release", builtin_keyboard_key_release);
    registerBuiltin("keyboard_clear", builtin_keyboard_clear);

    // Joystick
    registerBuiltin("joystick_exists", builtin_joystick_exists);
    registerBuiltin("joystick_xpos", builtin_joystick_xpos);
    registerBuiltin("joystick_ypos", builtin_joystick_ypos);
    registerBuiltin("joystick_direction", builtin_joystick_direction);
    registerBuiltin("joystick_pov", builtin_joystick_pov);
    registerBuiltin("joystick_check_button", builtin_joystick_check_button);

    // Window
    registerBuiltin("window_get_fullscreen", builtin_window_get_fullscreen);
    registerBuiltin("window_set_fullscreen", builtin_window_set_fullscreen);
    registerBuiltin("window_set_caption", builtin_window_set_caption);
    registerBuiltin("window_set_size", builtin_window_set_size);
    registerBuiltin("window_center", builtin_window_center);
    registerBuiltin("window_get_width", builtin_window_get_width);
    registerBuiltin("window_get_height", builtin_window_get_height);

    // Game
    registerBuiltin("game_restart", builtin_game_restart);
    registerBuiltin("game_end", builtin_game_end);
    registerBuiltin("game_save", builtin_game_save);
    registerBuiltin("game_load", builtin_game_load);

    // Instance
    registerBuiltin("instance_exists", builtinInstanceExists);
    registerBuiltin("instance_number", builtin_instance_number);
    registerBuiltin("instance_destroy", builtinInstanceDestroy);
    registerBuiltin("instance_create", builtinInstanceCreate);
    registerBuiltin("action_kill_object", builtinActionKillObject);
    registerBuiltin("event_inherited", builtinEventInherited);

    // Buffer
    registerBuiltin("buffer_create", builtin_buffer_create);
    registerBuiltin("buffer_delete", builtin_buffer_delete);
    registerBuiltin("buffer_write", builtin_buffer_write);
    registerBuiltin("buffer_read", builtin_buffer_read);
    registerBuiltin("buffer_seek", builtin_buffer_seek);
    registerBuiltin("buffer_tell", builtin_buffer_tell);
    registerBuiltin("buffer_get_size", builtin_buffer_get_size);
    registerBuiltin("buffer_base64_encode", builtin_buffer_base64_encode);

    // PSN
    registerBuiltin("psn_init", builtin_psn_init);
    registerBuiltin("psn_default_user", builtin_psn_default_user);
    registerBuiltin("psn_get_leaderboard_score", builtin_psn_get_leaderboard_score);

    // Draw
    registerBuiltin("draw_sprite", builtin_draw_sprite);
    registerBuiltin("draw_sprite_ext", builtin_draw_sprite_ext);
    registerBuiltin("draw_rectangle", builtin_draw_rectangle);
    registerBuiltin("draw_set_color", builtin_draw_set_color);
    registerBuiltin("draw_set_alpha", builtin_draw_set_alpha);
    registerBuiltin("draw_set_font", builtin_draw_set_font);
    registerBuiltin("draw_set_halign", builtin_draw_set_halign);
    registerBuiltin("draw_set_valign", builtin_draw_set_valign);
    registerBuiltin("draw_text", builtin_draw_text);
    registerBuiltin("draw_text_transformed", builtin_draw_text_transformed);
    registerBuiltin("draw_text_ext", builtin_draw_text_ext);
    registerBuiltin("draw_text_ext_transformed", builtin_draw_text_ext_transformed);
    registerBuiltin("draw_surface", builtin_draw_surface);
    registerBuiltin("draw_surface_ext", builtin_draw_surface_ext);
    registerBuiltin("draw_background", builtin_draw_background);
    registerBuiltin("draw_background_ext", builtin_draw_background_ext);
    registerBuiltin("draw_self", builtin_draw_self);
    registerBuiltin("draw_line", builtin_draw_line);
    registerBuiltin("draw_set_colour", builtin_draw_set_colour);
    registerBuiltin("draw_get_colour", builtin_draw_get_colour);
    registerBuiltin("draw_get_color", builtin_draw_get_color);
    registerBuiltin("draw_get_alpha", builtin_draw_get_alpha);

    // Surface
    registerBuiltin("surface_create", builtin_surface_create);
    registerBuiltin("surface_free", builtin_surface_free);
    registerBuiltin("surface_set_target", builtin_surface_set_target);
    registerBuiltin("surface_reset_target", builtin_surface_reset_target);
    registerBuiltin("surface_exists", builtin_surface_exists);
    registerBuiltin("surface_get_width", builtin_surface_get_width);
    registerBuiltin("surface_get_height", builtin_surface_get_height);

    // Sprite info
    registerBuiltin("sprite_get_width", builtin_sprite_get_width);
    registerBuiltin("sprite_get_height", builtin_sprite_get_height);
    registerBuiltin("sprite_get_number", builtin_sprite_get_number);
    registerBuiltin("sprite_get_xoffset", builtin_sprite_get_xoffset);
    registerBuiltin("sprite_get_yoffset", builtin_sprite_get_yoffset);

    // Text measurement
    registerBuiltin("string_width", builtin_string_width);
    registerBuiltin("string_height", builtin_string_height);
    registerBuiltin("string_width_ext", builtin_string_width_ext);
    registerBuiltin("string_height_ext", builtin_string_height_ext);

    // Color
    registerBuiltin("make_color_rgb", builtinMakeColor);
    registerBuiltin("make_colour_rgb", builtinMakeColour);

    // Display
    registerBuiltin("display_get_width", builtin_display_get_width);
    registerBuiltin("display_get_height", builtin_display_get_height);

    // Collision
    registerBuiltin("place_meeting", builtin_place_meeting);
    registerBuiltin("collision_rectangle", builtin_collision_rectangle);
    registerBuiltin("collision_line", builtin_collision_line);
    registerBuiltin("collision_point", builtin_collision_point);

    // Misc
    registerBuiltin("get_timer", builtin_get_timer);
    registerBuiltin("action_set_alarm", builtin_action_set_alarm);
}
