#pragma once

#include <stdint.h>
#include "rvalue.h"
#include "stb_ds.h"

#define GML_ALARM_COUNT 12

// Sparse self variable entry for stb_ds int-keyed hashmap
typedef struct { int32_t key; RValue value; } SelfVarEntry;

typedef struct Instance {
    uint32_t instanceId;
    int32_t objectIndex;
    double x, y;
    double xprevious, yprevious;
    double xstart, ystart;
    bool persistent, solid, active, visible, createEventFired, outsideRoom;
    int32_t maskIndex; // collision mask sprite override (-1 = use spriteIndex)

    // Per-instance self variable storage (sparse stb_ds hashmap, keyed by varID)
    SelfVarEntry* selfVars;
    ArrayMapEntry* selfArrayMap;
    struct { int32_t key; int32_t value; }* selfArrayVarTracker; // tracks which varIDs have array data

    // Built-in instance properties
    int32_t spriteIndex;
    double imageSpeed, imageIndex;
    double imageXscale, imageYscale, imageAngle, imageAlpha;
    uint32_t imageBlend;
    int32_t depth;

    // Motion properties
    double speed, direction;
    double hspeed, vspeed;
    double friction;
    double gravity, gravityDirection;

    // Path following state
    int32_t pathIndex;           // -1 = no path active
    double pathPosition;         // 0.0-1.0
    double pathPositionPrevious;
    double pathSpeed;
    double pathScale;            // default 1.0
    double pathOrientation;      // degrees, default 0.0
    int32_t pathEndAction;       // 0=stop, 1=restart, 2=continue, 3=reverse
    double pathXStart;           // origin for relative paths
    double pathYStart;

    int32_t alarm[GML_ALARM_COUNT];
} Instance;

Instance* Instance_create(uint32_t instanceId, int32_t objectIndex, double x, double y);
void Instance_free(Instance* instance);

// Get a self variable by varID. Returns RVALUE_UNDEFINED if absent. The returned RValue is non-owning.
static inline RValue Instance_getSelfVar(Instance* inst, int32_t varID) {
    ptrdiff_t idx = hmgeti(inst->selfVars, varID);
    if (0 > idx) return (RValue){ .type = RVALUE_UNDEFINED };
    RValue result = inst->selfVars[idx].value;
    result.ownsString = false;
    return result;
}

// Set a self variable by varID. Frees the old value if present, makes an owning string copy if needed.
static inline void Instance_setSelfVar(Instance* inst, int32_t varID, RValue val) {
    ptrdiff_t idx = hmgeti(inst->selfVars, varID);
    if (idx >= 0) {
        RValue_free(&inst->selfVars[idx].value);
    }
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        val = RValue_makeOwnedString(safeStrdup(val.string));
    }
    hmput(inst->selfVars, varID, val);
}

// Recompute speed/direction from hspeed/vspeed (called when hspeed or vspeed is set)
void Instance_computeSpeedFromComponents(Instance* inst);
// Recompute hspeed/vspeed from speed/direction (called when speed or direction is set)
void Instance_computeComponentsFromSpeed(Instance* inst);
