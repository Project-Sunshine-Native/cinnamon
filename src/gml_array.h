#pragma once
#include <stdint.h>

// Forward declarations
struct RValue;
typedef struct RValue RValue;

// ===[ GMLArray - Refcounted RValue array ]===
// BC16 (GMS 1.4): "owner" stores a pointer to the RValue slot that "owns" the array (first slot to write). Write through a different slot with refCount > 1 triggers a fork (matches native `SET_RValue`).
// BC17+ (GMS 2.3): "owner" stores an opaque scope token set by BREAK_SETOWNER. Write with mismatching current owner triggers a fork (matches native `SET_RValue_Array` + `g_CurrentArrayOwner`).
//
// "b = a" bumps refCount and shares, never clones eagerly. All forking happens lazily on write.
typedef struct GMLArray {
    int32_t refCount;
    int32_t length;
    int32_t capacity;
    void* owner;
    RValue* data;
} GMLArray;

GMLArray* GMLArray_create(int32_t initialLength);
void GMLArray_incRef(GMLArray* arr);
// Decrement refCount. If it reaches 0, free all inner RValues + data buffer + struct. Safe on nullptr.
void GMLArray_decRef(GMLArray* arr);
// Deep copy. Every inner owned-string is strdup'd. Nested arrays have their refCount bumped (shared by default).
// New array starts at refCount=1, length=src->length, owner=newOwner.
GMLArray* GMLArray_clone(GMLArray* src, void* newOwner);
// Grow "arr->length" to at least "minLength", filling gap with RVALUE_UNDEFINED. May reallocate data buffer.
void GMLArray_growTo(GMLArray* arr, int32_t minLength);
