#include "json_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

// ===[ Parser State ]===

typedef struct {
    const char* input;
    size_t position;
    size_t length;
} JsonParser;

// ===[ Internal Helpers ]===

static void skipWhitespace(JsonParser* parser) {
    while (parser->position < parser->length) {
        char c = parser->input[parser->position];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            parser->position++;
        } else {
            break;
        }
    }
}

static char peek(JsonParser* parser) {
    if (parser->position >= parser->length) return '\0';
    return parser->input[parser->position];
}

static char advance(JsonParser* parser) {
    if (parser->position >= parser->length) return '\0';
    return parser->input[parser->position++];
}

static JsonValue* makeValue(JsonValueType type) {
    JsonValue* value = safeCalloc(1, sizeof(JsonValue));
    if (value == NULL) {
        fprintf(stderr, "JsonReader: calloc failed\n");
        abort();
    }
    value->type = type;
    return value;
}

// Forward declaration for recursive parsing
static JsonValue* parseValue(JsonParser* parser);

static JsonValue* parseString(JsonParser* parser) {
    // Skip opening quote
    advance(parser);

    size_t capacity = 64;
    size_t length = 0;
    char* buffer = safeMalloc(capacity);
    if (buffer == NULL) {
        fprintf(stderr, "JsonReader: malloc failed\n");
        abort();
    }

    while (parser->position < parser->length) {
        char c = advance(parser);

        if (c == '"') {
            // End of string
            buffer[length] = '\0';
            JsonValue* value = makeValue(JSON_STRING);
            value->stringValue = buffer;
            return value;
        }

        if (c == '\\') {
            // Escape sequence
            char escaped = advance(parser);
            switch (escaped) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u': {
                    // Parse 4-digit hex code point
                    char hex[5] = {0};
                    repeat(4, i) {
                        hex[i] = advance(parser);
                    }
                    unsigned long codePoint = strtoul(hex, NULL, 16);

                    // Encode as UTF-8
                    if (128 > codePoint) {
                        c = (char) codePoint;
                    } else if (2048 > codePoint) {
                        if (length + 2 >= capacity) {
                            capacity *= 2;
                            buffer = safeRealloc(buffer, capacity);
                        }
                        buffer[length++] = (char) (0xC0 | (codePoint >> 6));
                        c = (char) (0x80 | (codePoint & 0x3F));
                    } else {
                        if (length + 3 >= capacity) {
                            capacity *= 2;
                            buffer = safeRealloc(buffer, capacity);
                        }
                        buffer[length++] = (char) (0xE0 | (codePoint >> 12));
                        buffer[length++] = (char) (0x80 | ((codePoint >> 6) & 0x3F));
                        c = (char) (0x80 | (codePoint & 0x3F));
                    }
                    break;
                }
                default:
                    fprintf(stderr, "JsonReader: unknown escape sequence '\\%c'\n", escaped);
                    free(buffer);
                    return NULL;
            }
        }

        // Grow buffer if needed
        if (length + 1 >= capacity) {
            capacity *= 2;
            buffer = safeRealloc(buffer, capacity);
            if (buffer == NULL) {
                fprintf(stderr, "JsonReader: realloc failed\n");
                abort();
            }
        }
        buffer[length++] = c;
    }

    // Unterminated string
    fprintf(stderr, "JsonReader: unterminated string\n");
    free(buffer);
    return NULL;
}

static JsonValue* parseNumber(JsonParser* parser) {
    const char* start = parser->input + parser->position;
    char* end = NULL;
    double number = strtod(start, &end);
    if (end == start) {
        fprintf(stderr, "JsonReader: invalid number\n");
        return NULL;
    }
    parser->position += (size_t) (end - start);

    JsonValue* value = makeValue(JSON_NUMBER);
    value->numberValue = number;
    return value;
}

static JsonValue* parseArray(JsonParser* parser) {
    // Skip opening bracket
    advance(parser);

    JsonValue* value = makeValue(JSON_ARRAY);
    value->array.items = NULL;
    value->array.count = 0;
    value->array.capacity = 0;

    skipWhitespace(parser);
    if (peek(parser) == ']') {
        advance(parser);
        return value;
    }

    while (true) {
        skipWhitespace(parser);
        JsonValue* item = parseValue(parser);
        if (item == NULL) {
            JsonReader_free(value);
            return NULL;
        }

        // Grow items array if needed
        if (value->array.count >= value->array.capacity) {
            int newCapacity = (value->array.capacity == 0) ? 8 : value->array.capacity * 2;
            value->array.items = safeRealloc(value->array.items, (size_t) newCapacity * sizeof(JsonValue));
            if (value->array.items == NULL) {
                fprintf(stderr, "JsonReader: realloc failed\n");
                abort();
            }
            value->array.capacity = newCapacity;
        }

        // Copy item into array and free the container
        value->array.items[value->array.count++] = *item;
        free(item);

        skipWhitespace(parser);
        if (peek(parser) == ',') {
            advance(parser);
        } else if (peek(parser) == ']') {
            advance(parser);
            return value;
        } else {
            fprintf(stderr, "JsonReader: expected ',' or ']' in array\n");
            JsonReader_free(value);
            return NULL;
        }
    }
}

static JsonValue* parseObject(JsonParser* parser) {
    // Skip opening brace
    advance(parser);

    JsonValue* value = makeValue(JSON_OBJECT);
    value->object.keys = NULL;
    value->object.values = NULL;
    value->object.count = 0;
    value->object.capacity = 0;

    skipWhitespace(parser);
    if (peek(parser) == '}') {
        advance(parser);
        return value;
    }

    while (true) {
        skipWhitespace(parser);
        if (peek(parser) != '"') {
            fprintf(stderr, "JsonReader: expected string key in object\n");
            JsonReader_free(value);
            return NULL;
        }

        JsonValue* keyValue = parseString(parser);
        if (keyValue == NULL) {
            JsonReader_free(value);
            return NULL;
        }
        char* key = keyValue->stringValue;
        // Free just the JsonValue container, keep the string
        free(keyValue);

        skipWhitespace(parser);
        if (peek(parser) != ':') {
            fprintf(stderr, "JsonReader: expected ':' after object key\n");
            free(key);
            JsonReader_free(value);
            return NULL;
        }
        advance(parser);

        skipWhitespace(parser);
        JsonValue* itemValue = parseValue(parser);
        if (itemValue == NULL) {
            free(key);
            JsonReader_free(value);
            return NULL;
        }

        // Grow arrays if needed
        if (value->object.count >= value->object.capacity) {
            int newCapacity = (value->object.capacity == 0) ? 8 : value->object.capacity * 2;
            value->object.keys = safeRealloc(value->object.keys, (size_t) newCapacity * sizeof(char*));
            value->object.values = safeRealloc(value->object.values, (size_t) newCapacity * sizeof(JsonValue));
            if (value->object.keys == NULL || value->object.values == NULL) {
                fprintf(stderr, "JsonReader: realloc failed\n");
                abort();
            }
            value->object.capacity = newCapacity;
        }

        value->object.keys[value->object.count] = key;
        value->object.values[value->object.count] = *itemValue;
        value->object.count++;
        free(itemValue);

        skipWhitespace(parser);
        if (peek(parser) == ',') {
            advance(parser);
        } else if (peek(parser) == '}') {
            advance(parser);
            return value;
        } else {
            fprintf(stderr, "JsonReader: expected ',' or '}' in object\n");
            JsonReader_free(value);
            return NULL;
        }
    }
}

static JsonValue* parseLiteral(JsonParser* parser, const char* literal, size_t literalLen) {
    if (parser->position + literalLen > parser->length) {
        return NULL;
    }
    if (memcmp(parser->input + parser->position, literal, literalLen) != 0) {
        return NULL;
    }
    parser->position += literalLen;
    return makeValue(JSON_NULL); // Caller overrides type as needed
}

static JsonValue* parseValue(JsonParser* parser) {
    skipWhitespace(parser);
    char c = peek(parser);

    switch (c) {
        case '"':
            return parseString(parser);
        case '{':
            return parseObject(parser);
        case '[':
            return parseArray(parser);
        case 't': {
            JsonValue* value = parseLiteral(parser, "true", 4);
            if (value == NULL) {
                fprintf(stderr, "JsonReader: invalid literal\n");
                return NULL;
            }
            value->type = JSON_BOOL;
            value->boolValue = true;
            return value;
        }
        case 'f': {
            JsonValue* value = parseLiteral(parser, "false", 5);
            if (value == NULL) {
                fprintf(stderr, "JsonReader: invalid literal\n");
                return NULL;
            }
            value->type = JSON_BOOL;
            value->boolValue = false;
            return value;
        }
        case 'n': {
            JsonValue* value = parseLiteral(parser, "null", 4);
            if (value == NULL) {
                fprintf(stderr, "JsonReader: invalid literal\n");
                return NULL;
            }
            return value;
        }
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                return parseNumber(parser);
            }
            fprintf(stderr, "JsonReader: unexpected character '%c' at position %zu\n", c, parser->position);
            return NULL;
    }
}

// ===[ Lifecycle ]===

JsonValue* JsonReader_parse(const char* json) {
    if (json == NULL) return NULL;

    JsonParser parser = {
        .input = json,
        .position = 0,
        .length = strlen(json),
    };

    JsonValue* result = parseValue(&parser);

    // Check for trailing non-whitespace
    if (result != NULL) {
        skipWhitespace(&parser);
        if (parser.position < parser.length) {
            fprintf(stderr, "JsonReader: trailing content after JSON value at position %zu\n", parser.position);
            JsonReader_free(result);
            return NULL;
        }
    }

    return result;
}

// Frees the contents of a JsonValue without freeing the JsonValue struct itself.
// Used for inline values (array items, object values stored by value).
static void freeContents(JsonValue* value) {
    switch (value->type) {
        case JSON_STRING:
            free(value->stringValue);
            break;
        case JSON_ARRAY:
            repeat(value->array.count, i) {
                freeContents(&value->array.items[i]);
            }
            free(value->array.items);
            break;
        case JSON_OBJECT:
            repeat(value->object.count, i) {
                free(value->object.keys[i]);
                freeContents(&value->object.values[i]);
            }
            free(value->object.keys);
            free(value->object.values);
            break;
        default:
            break;
    }
}

void JsonReader_free(JsonValue* value) {
    if (value == NULL) return;
    freeContents(value);
    free(value);
}

// ===[ Type Checks ]===

bool JsonReader_isNull(const JsonValue* value) {
    return value != NULL && value->type == JSON_NULL;
}

bool JsonReader_isBool(const JsonValue* value) {
    return value != NULL && value->type == JSON_BOOL;
}

bool JsonReader_isNumber(const JsonValue* value) {
    return value != NULL && value->type == JSON_NUMBER;
}

bool JsonReader_isString(const JsonValue* value) {
    return value != NULL && value->type == JSON_STRING;
}

bool JsonReader_isArray(const JsonValue* value) {
    return value != NULL && value->type == JSON_ARRAY;
}

bool JsonReader_isObject(const JsonValue* value) {
    return value != NULL && value->type == JSON_OBJECT;
}

// ===[ Value Getters ]===

bool JsonReader_getBool(const JsonValue* value) {
    return value->boolValue;
}

double JsonReader_getDouble(const JsonValue* value) {
    return value->numberValue;
}

int64_t JsonReader_getInt(const JsonValue* value) {
    return (int64_t) value->numberValue;
}

const char* JsonReader_getString(const JsonValue* value) {
    return value->stringValue;
}

// ===[ Array Access ]===

int JsonReader_arrayLength(const JsonValue* value) {
    return value->array.count;
}

JsonValue* JsonReader_getArrayElement(const JsonValue* value, int index) {
    if (0 > index || index >= value->array.count) return NULL;
    return &value->array.items[index];
}

// ===[ Array Bulk Read ]===

void JsonReader_readFloatArray(const JsonValue* value, float* out, int expectedLen) {
    require(value != NULL && value->type == JSON_ARRAY);
    require(value->array.count == expectedLen);
    repeat(expectedLen, i) {
        out[i] = (float) value->array.items[i].numberValue;
    }
}

void JsonReader_readInt32Array(const JsonValue* value, int32_t* out, int expectedLen) {
    require(value != NULL && value->type == JSON_ARRAY);
    require(value->array.count == expectedLen);
    repeat(expectedLen, i) {
        out[i] = (int32_t) value->array.items[i].numberValue;
    }
}

// ===[ Object Access ]===

int JsonReader_objectLength(const JsonValue* value) {
    return value->object.count;
}

JsonValue* JsonReader_getObject(const JsonValue* value, const char* key) {
    repeat(value->object.count, i) {
        if (strcmp(value->object.keys[i], key) == 0) {
            return &value->object.values[i];
        }
    }
    return NULL;
}

const char* JsonReader_getObjectKey(const JsonValue* value, int index) {
    if (0 > index || index >= value->object.count) return NULL;
    return value->object.keys[index];
}

JsonValue* JsonReader_getObjectValue(const JsonValue* value, int index) {
    if (0 > index || index >= value->object.count) return NULL;
    return &value->object.values[index];
}
