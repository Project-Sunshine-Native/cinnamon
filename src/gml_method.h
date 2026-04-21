#pragma once
#include <stdint.h>

// ===[ GMLMethod - Refcounted method binding ]===
typedef struct GMLMethod {
    int32_t refCount;
    int32_t codeIndex;
    int32_t boundInstanceId;
} GMLMethod;

GMLMethod* GMLMethod_create(int32_t codeIndex, int32_t boundInstanceId);
void GMLMethod_incRef(GMLMethod* m);
// Decrement refCount. If it reaches 0, frees the struct. Safe on nullptr.
void GMLMethod_decRef(GMLMethod* m);
