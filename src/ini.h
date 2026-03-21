#pragma once

#include <stddef.h>
#include <stdbool.h>

#define INI_SERIALIZE_DEFAULT_INITIAL_CAPACITY 256

// ===[ IniFile Types ]===

typedef struct {
    char* name;
    char** keys;
    char** values;
    int count;
    int capacity;
} IniSection;

typedef struct {
    IniSection* sections;
    int count;
    int capacity;
} IniFile;

// ===[ Lifecycle ]===

IniFile* Ini_parse(const char* text);
void Ini_free(IniFile* ini);

// ===[ Queries ]===

const char* Ini_getString(const IniFile* ini, const char* section, const char* key);
bool Ini_hasSection(const IniFile* ini, const char* section);
bool Ini_hasKey(const IniFile* ini, const char* section, const char* key);

// ===[ Mutation ]===

void Ini_setString(IniFile* ini, const char* section, const char* key, const char* value);
void Ini_deleteKey(IniFile* ini, const char* section, const char* key);
void Ini_deleteSection(IniFile* ini, const char* section);

// ===[ Serialization ]===

char* Ini_serialize(const IniFile* ini, size_t initialCapacity);
