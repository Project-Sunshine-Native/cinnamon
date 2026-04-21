#pragma once

#include "common.h"
#include <stdint.h>
#include "rvalue.h"
#include "gml_array.h"
#include "stb_ds.h"

#define GML_ALARM_COUNT 12

// Sparse self variable entry for stb_ds int-keyed hashmap
typedef struct { int32_t key; RValue value; } SelfVarEntry;

typedef struct Instance {
    uint32_t instanceId;
    int32_t objectIndex;
    // Native GMS runner stores all instance built-in variables as float (32-bit),
    // even though RValues use double. This matches the native precision model.
    float x, y;
    float xprevious, yprevious;
    float xstart, ystart;
    bool persistent, solid, active, destroyed, visible, createEventFired, outsideRoom;
    int32_t maskIndex; // collision mask sprite override (-1 = use spriteIndex)

    // Per-instance self variable storage (sparse stb_ds hashmap, keyed by varID).
    SelfVarEntry* selfVars;

    // Built-in instance properties
    int32_t spriteIndex;
    float imageSpeed, imageIndex;
    float imageXscale, imageYscale, imageAngle, imageAlpha;
    uint32_t imageBlend;
    int32_t depth;

    // Motion properties
    float speed, direction;
    float hspeed, vspeed;
    float friction;
    float gravity, gravityDirection;

    // Path following state
    int32_t pathIndex;           // -1 = no path active
    float pathPosition;           // 0.0-1.0
    float pathPositionPrevious;
    float pathSpeed;
    float pathScale;              // default 1.0
    float pathOrientation;        // degrees, default 0.0
    int32_t pathEndAction;       // 0=stop, 1=restart, 2=continue, 3=reverse
    float pathXStart;             // origin for relative paths
    float pathYStart;

    int32_t alarm[GML_ALARM_COUNT];
} Instance;

Instance* Instance_create(uint32_t instanceId, int32_t objectIndex, GMLReal x, GMLReal y);
void Instance_free(Instance* instance);

// Deep-copy all mutable fields from source to dst: built-in properties, alarms, selfVars.
// Does NOT copy instanceId, objectIndex, destroyed, or createEventFired. Strings are duplicated so ownership stays independent. Arrays bump refCount (shared - CoW handles forking on first write).
void Instance_copyFields(Instance* dst, Instance* source);

// Get a self variable by varID. Returns RVALUE_UNDEFINED if absent. The returned RValue is non-owning (weak view - do not RValue_free unless you incRef/strdup first to strengthen).
static inline RValue Instance_getSelfVar(Instance* inst, int32_t varID) {
    requireNotNull(inst);
    ptrdiff_t idx = hmgeti(inst->selfVars, varID);
    if (0 > idx) return (RValue){ .type = RVALUE_UNDEFINED };
    RValue result = inst->selfVars[idx].value;
    result.ownsString = false;
    return result;
}

// Set a self variable by varID. Frees the old value if present (decRefs owned arrays).
// Always takes an independent reference: strings are strdup'd, arrays are incRef'd, regardless of whether the caller's RValue was owning.
// The caller retains ownership of their original `val` and remains responsible for freeing it (via RValue_free) when done.
static inline void Instance_setSelfVar(Instance* inst, int32_t varID, RValue val) {
    requireNotNull(inst);
    ptrdiff_t idx = hmgeti(inst->selfVars, varID);
    if (idx >= 0) {
        RValue_free(&inst->selfVars[idx].value);
    }
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        val = RValue_makeOwnedString(safeStrdup(val.string));
    } else if (val.type == RVALUE_ARRAY && val.array != nullptr) {
        GMLArray_incRef(val.array);
        val.ownsString = true;
#if IS_BC17_OR_HIGHER_ENABLED
    } else if (val.type == RVALUE_METHOD && val.method != nullptr) {
        GMLMethod_incRef(val.method);
        val.ownsString = true;
#endif
    }
    hmput(inst->selfVars, varID, val);
}

// Recompute speed/direction from hspeed/vspeed (called when hspeed or vspeed is set)
void Instance_computeSpeedFromComponents(Instance* inst);
// Recompute hspeed/vspeed from speed/direction (called when speed or direction is set)
void Instance_computeComponentsFromSpeed(Instance* inst);
