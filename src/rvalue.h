#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "stb_ds.h"
#include "utils.h"

#include "json_writer.h"
#include "json_reader.h"

// ===[ RValue - Tagged Union ]===
typedef enum {
    RVALUE_REAL = 0,
    RVALUE_STRING = 1,
    RVALUE_INT32 = 2,
    RVALUE_INT64 = 3,
    RVALUE_BOOL = 4,
    RVALUE_UNDEFINED = 5,
    RVALUE_ARRAY_REF = 6,
    RVALUE_DS_MAP = 7,
    RVALUE_DS_LIST = 8,
} RValueType;

typedef struct {
    union {
        double real;
        int32_t int32;
        int64_t int64;
        const char* string;
    };
    RValueType type;
    bool ownsString;
} RValue;

typedef struct {
    const char* key;
    RValue value;
} RValueMapEntry;

// ===[ Constructors ]===
static RValue RValue_makeReal(double val) {
    return (RValue){ .real = val, .type = RVALUE_REAL };
}

static RValue RValue_makeInt32(int32_t val) {
    return (RValue){ .int32 = val, .type = RVALUE_INT32 };
}

static RValue RValue_makeInt64(int64_t val) {
    return (RValue){ .int64 = val, .type = RVALUE_INT64 };
}

static RValue RValue_makeBool(bool val) {
    return (RValue){ .int32 = val ? 1 : 0, .type = RVALUE_BOOL };
}

static RValue RValue_makeString(const char* val) {
    return (RValue){ .string = val, .type = RVALUE_STRING, .ownsString = false };
}

static RValue RValue_makeOwnedString(char* val) {
    return (RValue){ .string = val, .type = RVALUE_STRING, .ownsString = true };
}

static RValue RValue_makeUndefined(void) {
    return (RValue){ .type = RVALUE_UNDEFINED };
}

static RValue RValue_makeArrayRef(int32_t sourceVarID) {
    return (RValue){ .int32 = sourceVarID, .type = RVALUE_ARRAY_REF };
}

static RValue RValue_makeDsMap(intptr_t ptr) {
    return (RValue){ .int64 = (int64_t) ptr, .type = RVALUE_DS_MAP };
}

static RValue RValue_makeDsList(intptr_t ptr) {
    return (RValue){ .int64 = (int64_t) ptr, .type = RVALUE_DS_LIST };
}

// ===[ RValue -> string conversions ]===
static char* RValue_toString(RValue val) {
    char buf[64];
    switch (val.type) {
        case RVALUE_REAL:
            snprintf(buf, sizeof(buf), "%.16g", val.real);
            return strdup(buf);
        case RVALUE_INT32:
            snprintf(buf, sizeof(buf), "%d", val.int32);
            return strdup(buf);
        case RVALUE_INT64:
            snprintf(buf, sizeof(buf), "%lld", (long long) val.int64);
            return strdup(buf);
        case RVALUE_STRING:
            return strdup(val.string ? val.string : "");
        case RVALUE_BOOL:
            return strdup(val.int32 ? "1" : "0");
        case RVALUE_UNDEFINED:
            return strdup("undefined");
        case RVALUE_ARRAY_REF:
            snprintf(buf, sizeof(buf), "<array_ref:%d>", val.int32);
            return strdup(buf);
        case RVALUE_DS_MAP:
            snprintf(buf, sizeof(buf), "<ds_map:%lld>", (long long) val.int64);
            return strdup(buf);
        case RVALUE_DS_LIST:
            snprintf(buf, sizeof(buf), "<ds_list:%lld>", (long long) val.int64);
            return strdup(buf);
    }
    return strdup("");
}

static char* RValue_toStringFancy(RValue val) {
    if (val.type == RVALUE_STRING) {
        char* s = RValue_toString(val);
        size_t needed = strlen(s) + 3;
        char* out = safeCalloc(needed, sizeof(char));
        snprintf(out, needed, "\"%s\"", s);
        free(s);
        return out;
    }
    return RValue_toString(val);
}

static char* RValue_toStringTyped(RValue val) {
    char buf[128];
    switch (val.type) {
        case RVALUE_REAL:
            snprintf(buf, sizeof(buf), "real(%.16g)", val.real);
            return strdup(buf);
        case RVALUE_INT32:
            snprintf(buf, sizeof(buf), "int32(%d)", val.int32);
            return strdup(buf);
        case RVALUE_INT64:
            snprintf(buf, sizeof(buf), "int64(%lld)", (long long) val.int64);
            return strdup(buf);
        case RVALUE_STRING: {
            const char* str = val.string ? val.string : "";
            size_t needed = strlen(str) + 3;
            char* out = safeCalloc(needed, sizeof(char));
            snprintf(out, needed, "\"%s\"", str);
            return out;
        }
        case RVALUE_BOOL:
            return strdup(val.int32 ? "bool(true)" : "bool(false)");
        case RVALUE_UNDEFINED:
            return strdup("undefined");
        case RVALUE_ARRAY_REF:
            snprintf(buf, sizeof(buf), "<array_ref:%d>", val.int32);
            return strdup(buf);
        case RVALUE_DS_MAP:
            snprintf(buf, sizeof(buf), "<ds_map:%lld>", (long long) val.int64);
            return strdup(buf);
        case RVALUE_DS_LIST:
            snprintf(buf, sizeof(buf), "<ds_list:%lld>", (long long) val.int64);
            return strdup(buf);
    }
    return strdup("???");
}

// ===[ Free ]===
static void RValue_free(RValue* val) {
    if (val->type == RVALUE_STRING && val->ownsString && val->string) {
        free((void*) val->string);
        val->string = NULL;
        val->ownsString = false;
    }
}

// ===[ Conversions ]===
static double RValue_toReal(RValue val) {
    switch (val.type) {
        case RVALUE_REAL: return val.real;
        case RVALUE_INT32: return (double) val.int32;
        case RVALUE_INT64: return (double) val.int64;
        case RVALUE_BOOL: return (double) val.int32;
        case RVALUE_STRING: return strtod(val.string, NULL);
        default: return 0.0;
    }
}

static int32_t RValue_toInt32(RValue val) {
    switch (val.type) {
        case RVALUE_REAL: return (int32_t) val.real;
        case RVALUE_INT32: return val.int32;
        case RVALUE_INT64: return (int32_t) val.int64;
        case RVALUE_BOOL: return val.int32;
        case RVALUE_STRING: return (int32_t) strtod(val.string, NULL);
        default: return 0;
    }
}

static int64_t RValue_toInt64(RValue val) {
    switch (val.type) {
        case RVALUE_REAL: return (int64_t) val.real;
        case RVALUE_INT32: return (int64_t) val.int32;
        case RVALUE_INT64: return val.int64;
        case RVALUE_BOOL: return (int64_t) val.int32;
        case RVALUE_STRING: return (int64_t) strtod(val.string, NULL);
        default: return 0;
    }
}

static bool RValue_toBool(RValue val) {
    switch (val.type) {
        case RVALUE_REAL: return val.real > 0.5;
        case RVALUE_INT32: return val.int32 > 0;
        case RVALUE_INT64: return val.int64 > 0;
        case RVALUE_BOOL: return val.int32 != 0;
        case RVALUE_STRING: return val.string && val.string[0] != '\0';
        default: return false;
    }
}

// ===[ JSON <-> RValue using JsonReader ]===
static RValue jsonToRValue(JsonValue* node) {
    if (JsonReader_isObject(node)) {
        RValueMapEntry* map = NULL;

        int count = JsonReader_objectLength(node);
        for (int i = 0; i < count; i++) {
            const char* key = JsonReader_getObjectKey(node, i);
            JsonValue* child = JsonReader_getObjectValue(node, i);
            RValue val = jsonToRValue(child);
            hmput(map, key, val);
        }

        RValue r = RValue_makeUndefined();
        r.type = RVALUE_DS_MAP;
        r.int64 = (intptr_t) map;
        return r;
    }

    if (JsonReader_isArray(node)) {
        RValue* list = NULL;

        int count = JsonReader_arrayLength(node);
        for (int i = 0; i < count; i++) {
            JsonValue* item = JsonReader_getArrayElement(node, i);
            RValue val = jsonToRValue(item);
            arrput(list, val);
        }

        RValue r = RValue_makeUndefined();
        r.type = RVALUE_DS_LIST;
        r.int64 = (intptr_t) list;
        return r;
    }

    if (JsonReader_isString(node)) return RValue_makeOwnedString(strdup(JsonReader_getString(node)));
    if (JsonReader_isBool(node))   return RValue_makeBool(JsonReader_getBool(node));
    if (JsonReader_isNumber(node)) return RValue_makeReal(JsonReader_getDouble(node));
    if (JsonReader_isNull(node))   return RValue_makeUndefined();

    return RValue_makeUndefined();
}

static void rvalueToJson(JsonWriter* writer, RValue* v) {
    switch (v->type) {
        case RVALUE_REAL:
            JsonWriter_double(writer, v->real);
            break;

        case RVALUE_INT32:
            JsonWriter_int(writer, v->int32);
            break;

        case RVALUE_INT64:
            JsonWriter_int(writer, v->int64);
            break;

        case RVALUE_STRING:
            JsonWriter_string(writer, v->string);
            break;

        case RVALUE_BOOL:
            JsonWriter_bool(writer, v->int32 != 0);
            break;

        case RVALUE_DS_MAP: {
            RValueMapEntry* map = (RValueMapEntry*) (intptr_t) v->int64;
            JsonWriter_beginObject(writer);
            int n = hmlen(map);
            for (int i = 0; i < n; i++) {
                JsonWriter_key(writer, map[i].key);
                rvalueToJson(writer, &map[i].value);
            }
            JsonWriter_endObject(writer);
            break;
        }

        case RVALUE_DS_LIST: {
            RValue* list = (RValue*) (intptr_t) v->int64;
            JsonWriter_beginArray(writer);
            int n = arrlen(list);
            for (int i = 0; i < n; i++) {
                rvalueToJson(writer, &list[i]);
            }
            JsonWriter_endArray(writer);
            break;
        }

        default:
            JsonWriter_null(writer);
            break;
    }
}

// ===[ ArrayMapEntry - used by all array variable storage ]===
typedef struct {
    int64_t key;
    RValue value;
} ArrayMapEntry;

static void RValue_freeAllRValuesInMap(ArrayMapEntry* map) {
    repeat(hmlen(map), i) {
        RValue_free(&map[i].value);
    }
}