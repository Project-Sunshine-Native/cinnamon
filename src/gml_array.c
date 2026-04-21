#include "gml_array.h"
#include "rvalue.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>

GMLArray* GMLArray_create(int32_t initialLength) {
    GMLArray* arr = safeCalloc(1, sizeof(GMLArray));
    arr->refCount = 1;
    arr->length = initialLength;
    arr->capacity = initialLength > 0 ? initialLength : 0;
    arr->owner = nullptr;
    if (initialLength > 0) {
        arr->data = safeCalloc((uint32_t) initialLength, sizeof(RValue));
        repeat(initialLength, i) {
            arr->data[i] = (RValue){ .type = RVALUE_UNDEFINED };
        }
    } else {
        arr->data = nullptr;
    }
    return arr;
}

void GMLArray_incRef(GMLArray* arr) {
    if (arr == nullptr) return;
    arr->refCount++;
}

void GMLArray_decRef(GMLArray* arr) {
    if (arr == nullptr) return;
    require(arr->refCount > 0);
    arr->refCount--;
    if (arr->refCount > 0) return;

    repeat(arr->length, i) {
        RValue_free(&arr->data[i]);
    }
    free(arr->data);
    free(arr);
}

GMLArray* GMLArray_clone(GMLArray* src, void* newOwner) {
    if (src == nullptr) return nullptr;
    GMLArray* dst = safeCalloc(1, sizeof(GMLArray));
    dst->refCount = 1;
    dst->length = src->length;
    dst->capacity = src->length;
    dst->owner = newOwner;
    if (src->length > 0) {
        dst->data = safeCalloc((uint32_t) src->length, sizeof(RValue));
        repeat(src->length, i) {
            RValue srcVal = src->data[i];
            // Duplicate owned strings: for nested arrays, share the inner array (bump refCount).
            // Inner arrays get their own CoW check on first write through the new outer slot.
            if (srcVal.type == RVALUE_STRING && srcVal.ownsString && srcVal.string != nullptr) {
                dst->data[i] = RValue_makeOwnedString(safeStrdup(srcVal.string));
            } else if (srcVal.type == RVALUE_ARRAY && srcVal.array != nullptr) {
                GMLArray_incRef(srcVal.array);
                dst->data[i] = srcVal;
                dst->data[i].ownsString = true;
#if IS_BC17_OR_HIGHER_ENABLED
            } else if (srcVal.type == RVALUE_METHOD && srcVal.method != nullptr) {
                GMLMethod_incRef(srcVal.method);
                dst->data[i] = srcVal;
                dst->data[i].ownsString = true;
#endif
            } else {
                dst->data[i] = srcVal;
                dst->data[i].ownsString = false;
            }
        }
    } else {
        dst->data = nullptr;
    }
    return dst;
}

void GMLArray_growTo(GMLArray* arr, int32_t minLength) {
    if (arr == nullptr) return;
    if (arr->length >= minLength) return;

    if (minLength > arr->capacity) {
        int32_t newCapacity = arr->capacity > 0 ? arr->capacity : 4;
        while (minLength > newCapacity) newCapacity *= 2;
        arr->data = safeRealloc(arr->data, (uint32_t) newCapacity * sizeof(RValue));
        arr->capacity = newCapacity;
    }
    // Fill new slots with undefined
    for (int32_t i = arr->length; minLength > i; i++) {
        arr->data[i] = (RValue){ .type = RVALUE_UNDEFINED };
    }
    arr->length = minLength;
}
