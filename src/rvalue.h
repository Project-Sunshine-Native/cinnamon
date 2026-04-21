#pragma once
#include <stdint.h>
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "real_type.h"
#include "stb_ds.h"
#include "utils.h"
#include "bytecode_versions.h"

// Forward declarations
struct GMLArray;
typedef struct GMLArray GMLArray;
void GMLArray_decRef(struct GMLArray* arr);
void GMLArray_incRef(struct GMLArray* arr);

#include "gml_method.h"

// ===[ GML Data Types (4-bit type codes) ]===
#define GML_TYPE_DOUBLE   0x0
#define GML_TYPE_FLOAT    0x1
#define GML_TYPE_INT32    0x2
#define GML_TYPE_INT64    0x3
#define GML_TYPE_BOOL     0x4
#define GML_TYPE_VARIABLE 0x5
#define GML_TYPE_STRING   0x6
#define GML_TYPE_INT16    0xF

// ===[ RValue - Tagged Union ]===
typedef enum {
    RVALUE_REAL = 0,
    RVALUE_STRING = 1,
    RVALUE_INT32 = 2,
    RVALUE_INT64 = 3,
    RVALUE_BOOL = 4,
    RVALUE_UNDEFINED = 5,
    RVALUE_ARRAY = 6,
    RVALUE_METHOD = 7,
} RValueType;

typedef struct RValue {
    union {
        GMLReal real;
        int32_t int32;
#ifndef NO_RVALUE_INT64
        int64_t int64;
#endif
        const char* string;
        GMLArray* array;
#if IS_BC17_OR_HIGHER_ENABLED
        GMLMethod* method;
#endif
    };
    // We use uint8_t for the type instead of RValueType because a enum value occupies 4 bytes, while uint8_t occupies 1 byte
    uint8_t type;
    // For RVALUE_STRING: true = the `string` buffer is owned and must be freed on RValue_free.
    // For RVALUE_ARRAY: true = this RValue holds one strong ref and must decRef on RValue_free.
    // Non-owning ("weak") RValues are short-lived views returned by getters, caller must NOT free them.
    bool ownsString;
#if IS_BC17_OR_HIGHER_ENABLED
    uint8_t gmlStackType; // GML data type from the instruction that pushed this value
#endif
} RValue;

// Helper to initialize .gmlStackType only on BC17+ builds
#if IS_BC17_OR_HIGHER_ENABLED
#  define RVALUE_INIT_GMLTYPE(t) .gmlStackType = (t)
#else
#  define RVALUE_INIT_GMLTYPE(t)
#endif

static RValue RValue_makeReal(GMLReal val) {
    return (RValue){ .real = val, .type = RVALUE_REAL, RVALUE_INIT_GMLTYPE(GML_TYPE_DOUBLE) };
}

static RValue RValue_makeInt32(int32_t val) {
    return (RValue){ .int32 = val, .type = RVALUE_INT32, RVALUE_INIT_GMLTYPE(GML_TYPE_INT32) };
}

static RValue RValue_makeInt64(int64_t val) {
#ifdef NO_RVALUE_INT64
    // Values that don't fit in int32 get promoted to real instead of clamped, because clamping to INT32_MIN causes arithmetic overflow bugs
    // (example: Undertale's mercymod = -99999999999999 in the Asriel fight)
    if (val > INT32_MAX || INT32_MIN > val) {
        return (RValue){ .real = (GMLReal) val, .type = RVALUE_REAL, RVALUE_INIT_GMLTYPE(GML_TYPE_DOUBLE) };
    } else {
        return (RValue){ .int32 = (int32_t) val, .type = RVALUE_INT32, RVALUE_INIT_GMLTYPE(GML_TYPE_INT32) };
    }
#else
    return (RValue){ .int64 = val, .type = RVALUE_INT64, RVALUE_INIT_GMLTYPE(GML_TYPE_INT64) };
#endif
}

static RValue RValue_makeBool(bool val) {
    return (RValue){ .int32 = val ? 1 : 0, .type = RVALUE_BOOL, RVALUE_INIT_GMLTYPE(GML_TYPE_BOOL) };
}

static RValue RValue_makeString(const char* val) {
    return (RValue){ .string = val, .type = RVALUE_STRING, .ownsString = false, RVALUE_INIT_GMLTYPE(GML_TYPE_STRING) };
}

static RValue RValue_makeOwnedString(char* val) {
    return (RValue){ .string = val, .type = RVALUE_STRING, .ownsString = true, RVALUE_INIT_GMLTYPE(GML_TYPE_STRING) };
}

static RValue RValue_makeUndefined(void) {
    return (RValue){ .type = RVALUE_UNDEFINED, RVALUE_INIT_GMLTYPE(GML_TYPE_VARIABLE) };
}

// Takes ownership: refCount is NOT bumped (caller hands off its ref). The returned RValue decRefs on free.
// Use this when you have a freshly-allocated array (GMLArray_alloc) or after a GMLArray_incRef.
static RValue RValue_makeArray(GMLArray* arr) {
    return (RValue){ .array = arr, .type = RVALUE_ARRAY, .ownsString = true, RVALUE_INIT_GMLTYPE(GML_TYPE_VARIABLE) };
}

// Weak view: does not own (no decRef on free). Callers that stash the value long-term must incRef + set ownsString.
static RValue RValue_makeArrayWeak(GMLArray* arr) {
    return (RValue){ .array = arr, .type = RVALUE_ARRAY, .ownsString = false, RVALUE_INIT_GMLTYPE(GML_TYPE_VARIABLE) };
}

#if IS_BC17_OR_HIGHER_ENABLED
// Takes ownership: refCount is NOT bumped (caller hands off its ref). The returned RValue decRefs on free.
static RValue RValue_makeMethod(int32_t codeIndex, int32_t boundInstanceId) {
    return (RValue){ .method = GMLMethod_create(codeIndex, boundInstanceId), .type = RVALUE_METHOD, .ownsString = true, .gmlStackType = GML_TYPE_VARIABLE };
}

// Weak view: does not own (no decRef on free). Callers that stash the value long-term must incRef + set ownsString.
static RValue RValue_makeMethodWeak(GMLMethod* m) {
    return (RValue){ .method = m, .type = RVALUE_METHOD, .ownsString = false, .gmlStackType = GML_TYPE_VARIABLE };
}
#endif

// Converts an RValue to a heap-allocated string representation.
// The caller must free the returned string
static char* RValue_toString(RValue val) {
    char buf[64];
    switch (val.type) {
        case RVALUE_REAL:
            snprintf(buf, sizeof(buf), "%.16g", val.real);
            return safeStrdup(buf);
        case RVALUE_INT32:
            snprintf(buf, sizeof(buf), "%d", val.int32);
            return safeStrdup(buf);
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:
            snprintf(buf, sizeof(buf), "%lld", (long long) val.int64);
            return safeStrdup(buf);
#endif
        case RVALUE_STRING:
            return safeStrdup(val.string != nullptr ? val.string : "");
        case RVALUE_BOOL:
            return safeStrdup(val.int32 ? "1" : "0");
        case RVALUE_UNDEFINED:
            return safeStrdup("undefined");
        case RVALUE_ARRAY:
            snprintf(buf, sizeof(buf), "<array:%p>", (void*) val.array);
            return safeStrdup(buf);
#if IS_BC17_OR_HIGHER_ENABLED
        case RVALUE_METHOD:
            snprintf(buf, sizeof(buf), "<method:%d>", val.method->codeIndex);
            return safeStrdup(buf);
#endif
    }
    return safeStrdup("");
}

// Converts an RValue to a heap-allocated string representation, used for debug logs.
// The caller must free the returned string
static char* RValue_toStringFancy(RValue val) {
    switch (val.type) {
        case RVALUE_STRING: {
            char* valueAsString = RValue_toString(val);

            // length + quotes (2) + null terminator
            int newLength = strlen(valueAsString) + 3;
            char* valueWithQuotes = safeCalloc(newLength, sizeof(char));
            snprintf(valueWithQuotes, newLength, "\"%s\"", valueAsString);

            free(valueAsString);

            return valueWithQuotes;
        }
        default: {
            return RValue_toString(val);
        }
    }
}

// Converts an RValue to a heap-allocated string with a type tag prefix, used for trace-stack output.
// Examples: int32(42), real(3.14), "hello", bool(true), undefined, <array:0x...>
// The caller must free the returned string
static char* RValue_toStringTyped(RValue val) {
    char buf[128];
    switch (val.type) {
        case RVALUE_REAL:
            snprintf(buf, sizeof(buf), "real(%.16g)", val.real);
            return safeStrdup(buf);
        case RVALUE_INT32:
            snprintf(buf, sizeof(buf), "int32(%d)", val.int32);
            return safeStrdup(buf);
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:
            snprintf(buf, sizeof(buf), "int64(%lld)", (long long) val.int64);
            return safeStrdup(buf);
#endif
        case RVALUE_STRING: {
            const char* str = val.string != nullptr ? val.string : "";
            size_t needed = strlen(str) + 3;
            char* result = safeCalloc(needed, sizeof(char));
            snprintf(result, needed, "\"%s\"", str);
            return result;
        }
        case RVALUE_BOOL:
            return safeStrdup(val.int32 ? "bool(true)" : "bool(false)");
        case RVALUE_UNDEFINED:
            return safeStrdup("undefined");
        case RVALUE_ARRAY:
            snprintf(buf, sizeof(buf), "<array:%p>", (void*) val.array);
            return safeStrdup(buf);
#if IS_BC17_OR_HIGHER_ENABLED
        case RVALUE_METHOD:
            snprintf(buf, sizeof(buf), "method(code=%d, inst=%d)", val.method->codeIndex, val.method->boundInstanceId);
            return safeStrdup(buf);
#endif
    }
    return safeStrdup("???");
}

static void RValue_free(RValue* val) {
    if (val->type == RVALUE_STRING && val->ownsString && val->string != nullptr) {
        free((void*) val->string);
        val->string = nullptr;
        val->ownsString = false;
    } else if (val->type == RVALUE_ARRAY && val->ownsString && val->array != nullptr) {
        GMLArray_decRef(val->array);
        val->array = nullptr;
        val->ownsString = false;
#if IS_BC17_OR_HIGHER_ENABLED
    } else if (val->type == RVALUE_METHOD && val->ownsString && val->method != nullptr) {
        GMLMethod_decRef(val->method);
        val->method = nullptr;
        val->ownsString = false;
#endif
    }
}

static GMLReal RValue_toReal(RValue val) {
    switch (val.type) {
        case RVALUE_REAL:   return val.real;
        case RVALUE_INT32:  return (GMLReal) val.int32;
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:  return (GMLReal) val.int64;
#endif
        case RVALUE_BOOL:   return (GMLReal) val.int32;
        case RVALUE_STRING: return GMLReal_strtod(val.string, nullptr);
        case RVALUE_ARRAY:  return 0.0;
#if IS_BC17_OR_HIGHER_ENABLED
        case RVALUE_METHOD: return 0.0;
#endif
        default:            return 0.0;
    }
}

static int32_t RValue_toInt32(RValue val) {
    switch (val.type) {
        case RVALUE_REAL:   return (int32_t) val.real;
        case RVALUE_INT32:  return val.int32;
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:  return (int32_t) val.int64;
#endif
        case RVALUE_BOOL:   return val.int32;
        case RVALUE_STRING: return (int32_t) GMLReal_strtod(val.string, nullptr);
        case RVALUE_ARRAY:  return 0;
#if IS_BC17_OR_HIGHER_ENABLED
        case RVALUE_METHOD: return 0;
#endif
        default:            return 0;
    }
}

static int64_t RValue_toInt64(RValue val) {
    switch (val.type) {
        case RVALUE_REAL:   return (int64_t) val.real;
        case RVALUE_INT32:  return (int64_t) val.int32;
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:  return val.int64;
#endif
        case RVALUE_BOOL:   return (int64_t) val.int32;
        case RVALUE_STRING: return (int64_t) GMLReal_strtod(val.string, nullptr);
        case RVALUE_ARRAY:  return 0;
#if IS_BC17_OR_HIGHER_ENABLED
        case RVALUE_METHOD: return 0;
#endif
        default:            return 0;
    }
}

static bool RValue_toBool(RValue val) {
    switch (val.type) {
        case RVALUE_REAL:   return val.real > 0.5;
        case RVALUE_INT32:  return val.int32 > 0;
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:  return val.int64 > 0;
#endif
        case RVALUE_BOOL:   return val.int32 != 0;
        case RVALUE_STRING: return val.string != nullptr && val.string[0] != '\0';
        case RVALUE_ARRAY:  return false;
#if IS_BC17_OR_HIGHER_ENABLED
        case RVALUE_METHOD: return true;
#endif
        default:            return false;
    }
}
