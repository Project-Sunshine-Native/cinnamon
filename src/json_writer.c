#include "json_writer.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===[ Internal Helpers ]===

static void ensureCapacity(JsonWriter* writer, size_t additional) {
    size_t required = writer->length + additional;
    if (required <= writer->capacity) return;

    size_t newCapacity = writer->capacity;
    while (newCapacity < required) {
        newCapacity *= 2;
    }

    writer->buffer = safeRealloc(writer->buffer, newCapacity);
    if (writer->buffer == nullptr) {
        fprintf(stderr, "JsonWriter: realloc failed\n");
        abort();
    }
    writer->capacity = newCapacity;
}

static void appendRaw(JsonWriter* writer, const char* data, size_t len) {
    ensureCapacity(writer, len + 1);
    memcpy(writer->buffer + writer->length, data, len);
    writer->length += len;
    writer->buffer[writer->length] = '\0';
}

static void appendStr(JsonWriter* writer, const char* str) {
    appendRaw(writer, str, strlen(str));
}

static void appendChar(JsonWriter* writer, char c) {
    ensureCapacity(writer, 2);
    writer->buffer[writer->length++] = c;
    writer->buffer[writer->length] = '\0';
}

static void writeCommaIfNeeded(JsonWriter* writer) {
    if (writer->needsComma) {
        appendChar(writer, ',');
    }
}

static void writeEscapedString(JsonWriter* writer, const char* str) {
    appendChar(writer, '"');
    for (const char* p = str; *p != '\0'; p++) {
        unsigned char c = (unsigned char) *p;
        switch (c) {
            case '"':  appendStr(writer, "\\\""); break;
            case '\\': appendStr(writer, "\\\\"); break;
            case '\b': appendStr(writer, "\\b");  break;
            case '\f': appendStr(writer, "\\f");  break;
            case '\n': appendStr(writer, "\\n");  break;
            case '\r': appendStr(writer, "\\r");  break;
            case '\t': appendStr(writer, "\\t");  break;
            default:
                if (32 > c) {
                    char escape[7];
                    snprintf(escape, sizeof(escape), "\\u%04x", c);
                    appendStr(writer, escape);
                } else {
                    appendChar(writer, (char) c);
                }
                break;
        }
    }
    appendChar(writer, '"');
}

// ===[ Lifecycle ]===

JsonWriter JsonWriter_create(void) {
    size_t initialCapacity = 256;
    char* buffer = safeMalloc(initialCapacity);
    if (buffer == nullptr) {
        fprintf(stderr, "JsonWriter: malloc failed\n");
        abort();
    }
    buffer[0] = '\0';
    return (JsonWriter) {
        .buffer = buffer,
        .length = 0,
        .capacity = initialCapacity,
        .needsComma = false,
    };
}

void JsonWriter_free(JsonWriter* writer) {
    free(writer->buffer);
    writer->buffer = nullptr;
    writer->length = 0;
    writer->capacity = 0;
}

// ===[ Structure ]===

void JsonWriter_beginObject(JsonWriter* writer) {
    writeCommaIfNeeded(writer);
    appendChar(writer, '{');
    writer->needsComma = false;
}

void JsonWriter_endObject(JsonWriter* writer) {
    appendChar(writer, '}');
    writer->needsComma = true;
}

void JsonWriter_beginArray(JsonWriter* writer) {
    writeCommaIfNeeded(writer);
    appendChar(writer, '[');
    writer->needsComma = false;
}

void JsonWriter_endArray(JsonWriter* writer) {
    appendChar(writer, ']');
    writer->needsComma = true;
}

// ===[ Object Keys ]===

void JsonWriter_key(JsonWriter* writer, const char* key) {
    writeCommaIfNeeded(writer);
    writeEscapedString(writer, key);
    appendChar(writer, ':');
    writer->needsComma = false;
}

// ===[ Values ]===

void JsonWriter_string(JsonWriter* writer, const char* value) {
    writeCommaIfNeeded(writer);
    if (value == nullptr) {
        appendStr(writer, "null");
    } else {
        writeEscapedString(writer, value);
    }
    writer->needsComma = true;
}

void JsonWriter_int(JsonWriter* writer, int64_t value) {
    writeCommaIfNeeded(writer);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long) value);
    appendStr(writer, buf);
    writer->needsComma = true;
}

void JsonWriter_double(JsonWriter* writer, double value) {
    writeCommaIfNeeded(writer);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", value);
    appendStr(writer, buf);
    writer->needsComma = true;
}

void JsonWriter_bool(JsonWriter* writer, bool value) {
    writeCommaIfNeeded(writer);
    appendStr(writer, value ? "true" : "false");
    writer->needsComma = true;
}

void JsonWriter_null(JsonWriter* writer) {
    writeCommaIfNeeded(writer);
    appendStr(writer, "null");
    writer->needsComma = true;
}

// ===[ Property Convenience ]===

void JsonWriter_propertyString(JsonWriter* writer, const char* key, const char* value) {
    JsonWriter_key(writer, key);
    JsonWriter_string(writer, value);
}

void JsonWriter_propertyInt(JsonWriter* writer, const char* key, int64_t value) {
    JsonWriter_key(writer, key);
    JsonWriter_int(writer, value);
}

void JsonWriter_propertyDouble(JsonWriter* writer, const char* key, double value) {
    JsonWriter_key(writer, key);
    JsonWriter_double(writer, value);
}

void JsonWriter_propertyBool(JsonWriter* writer, const char* key, bool value) {
    JsonWriter_key(writer, key);
    JsonWriter_bool(writer, value);
}

void JsonWriter_propertyNull(JsonWriter* writer, const char* key) {
    JsonWriter_key(writer, key);
    JsonWriter_null(writer);
}

// ===[ Output ]===

const char* JsonWriter_getOutput(const JsonWriter* writer) {
    return writer->buffer;
}

char* JsonWriter_copyOutput(const JsonWriter* writer) {
    return safeStrdup(writer->buffer);
}

size_t JsonWriter_getLength(const JsonWriter* writer) {
    return writer->length;
}
