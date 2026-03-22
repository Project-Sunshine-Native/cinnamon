#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ===[ JsonValue Types ]===

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonValueType;

typedef struct JsonValue {
    JsonValueType type;
    union {
        bool boolValue;
        double numberValue;
        char* stringValue;
        struct { struct JsonValue* items; int count; int capacity; } array;
        struct { char** keys; struct JsonValue* values; int count; int capacity; } object;
    };
} JsonValue;

// ===[ Lifecycle ]===

JsonValue* JsonReader_parse(const char* json);
void JsonReader_free(JsonValue* value);

// ===[ Type Checks ]===

bool JsonReader_isNull(const JsonValue* value);
bool JsonReader_isBool(const JsonValue* value);
bool JsonReader_isNumber(const JsonValue* value);
bool JsonReader_isString(const JsonValue* value);
bool JsonReader_isArray(const JsonValue* value);
bool JsonReader_isObject(const JsonValue* value);

// ===[ Value Getters ]===

bool JsonReader_getBool(const JsonValue* value);
double JsonReader_getDouble(const JsonValue* value);
int64_t JsonReader_getInt(const JsonValue* value);
const char* JsonReader_getString(const JsonValue* value);

// ===[ Array Access ]===

int JsonReader_arrayLength(const JsonValue* value);
JsonValue* JsonReader_getArrayElement(const JsonValue* value, int index);

// ===[ Array Bulk Read ]===

// Reads a JSON number array into a float C array. Asserts the array has exactly expectedLen elements.
void JsonReader_readFloatArray(const JsonValue* value, float* out, int expectedLen);
// Reads a JSON number array into an int32 C array. Asserts the array has exactly expectedLen elements.
void JsonReader_readInt32Array(const JsonValue* value, int32_t* out, int expectedLen);

// ===[ Object Access ]===

int JsonReader_objectLength(const JsonValue* value);
JsonValue* JsonReader_getObject(const JsonValue* value, const char* key);
const char* JsonReader_getObjectKey(const JsonValue* value, int index);
JsonValue* JsonReader_getObjectValue(const JsonValue* value, int index);
