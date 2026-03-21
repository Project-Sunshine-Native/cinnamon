#include "ini.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text_utils.h"

// ===[ Internal Helpers ]===

static IniSection* findSection(const IniFile* ini, const char* name) {
    repeat(ini->count, i) {
        if (strcmp(ini->sections[i].name, name) == 0) {
            return &ini->sections[i];
        }
    }
    return nullptr;
}

static int findKeyIndex(const IniSection* section, const char* key) {
    repeat(section->count, i) {
        if (strcmp(section->keys[i], key) == 0) {
            return i;
        }
    }
    return -1;
}

static IniSection* addSection(IniFile* ini, const char* name) {
    if (ini->count >= ini->capacity) {
        ini->capacity = (ini->capacity == 0) ? 4 : ini->capacity * 2;
        ini->sections = safeRealloc(ini->sections, (size_t) ini->capacity * sizeof(IniSection));
    }
    IniSection* section = &ini->sections[ini->count++];
    section->name = safeStrdup(name);
    section->keys = nullptr;
    section->values = nullptr;
    section->count = 0;
    section->capacity = 0;
    return section;
}

static void addKeyValue(IniSection* section, const char* key, const char* value) {
    if (section->count >= section->capacity) {
        section->capacity = (section->capacity == 0) ? 4 : section->capacity * 2;
        section->keys = safeRealloc(section->keys, (size_t) section->capacity * sizeof(char*));
        section->values = safeRealloc(section->values, (size_t) section->capacity * sizeof(char*));
    }
    section->keys[section->count] = safeStrdup(key);
    section->values[section->count] = safeStrdup(value);
    section->count++;
}

static const char* skipWhitespace(const char* p) {
    while (TextUtils_isWhitespaceChar(*p)) {
        p++;
    }
    return p;
}

// ===[ Lifecycle ]===

IniFile* Ini_parse(const char* text) {
    IniFile* ini = safeCalloc(1, sizeof(IniFile));

    if (text == nullptr || *text == '\0') {
        return ini;
    }

    // Make a mutable copy to tokenize
    char* data = safeStrdup(text);
    IniSection* currentSection = nullptr;

    char* line = data;
    while (line != nullptr) {
        // Find end of line
        char* eol = strchr(line, '\n');
        if (eol != nullptr) {
            *eol = '\0';
        }

        // Trim leading whitespace
        const char* trimmed = skipWhitespace(line);

        // Skip empty lines and comments
        if (*trimmed == '\0' || *trimmed == ';' || *trimmed == '#') {
            line = eol != nullptr ? eol + 1 : nullptr;
            continue;
        }

        // Strip trailing whitespace/CR
        char* mutableTrimmed = (char*) trimmed;
        TextUtils_trimTrailingWhitespace(mutableTrimmed);

        if (*trimmed == '[') {
            // Section header
            const char* nameStart = trimmed + 1;
            char* closeBracket = strchr(mutableTrimmed, ']');
            if (closeBracket != nullptr) {
                *closeBracket = '\0';
                currentSection = findSection(ini, nameStart);
                if (currentSection == nullptr) {
                    currentSection = addSection(ini, nameStart);
                }
            } else {
                fprintf(stderr, "Ini: malformed section header: %s\n", trimmed);
            }
        } else {
            // Key=value pair
            char* equals = strchr(mutableTrimmed, '=');
            if (equals != nullptr && currentSection != nullptr) {
                *equals = '\0';
                char* key = mutableTrimmed;
                char* value = equals + 1;
                TextUtils_trimTrailingWhitespace(key);
                value = (char*) skipWhitespace(value);

                // Check if key already exists - overwrite if so
                int existingIndex = findKeyIndex(currentSection, key);
                if (existingIndex >= 0) {
                    free(currentSection->values[existingIndex]);
                    currentSection->values[existingIndex] = safeStrdup(value);
                } else {
                    addKeyValue(currentSection, key, value);
                }
            }
            // Silently skip key=value lines outside any section (matching GML behavior)
        }

        line = eol != nullptr ? eol + 1 : nullptr;
    }

    free(data);
    return ini;
}

void Ini_free(IniFile* ini) {
    if (ini == nullptr)
        return;

    repeat(ini->count, i) {
        IniSection* section = &ini->sections[i];
        repeat(section->count, j) {
            free(section->keys[j]);
            free(section->values[j]);
        }
        free(section->keys);
        free(section->values);
        free(section->name);
    }
    free(ini->sections);
    free(ini);
}

// ===[ Queries ]===

const char* Ini_getString(const IniFile* ini, const char* section, const char* key) {
    IniSection* sec = findSection(ini, section);
    if (sec == nullptr)
        return nullptr;

    int idx = findKeyIndex(sec, key);
    if (0 > idx)
        return nullptr;

    return sec->values[idx];
}

bool Ini_hasSection(const IniFile* ini, const char* section) {
    return findSection(ini, section) != nullptr;
}

bool Ini_hasKey(const IniFile* ini, const char* section, const char* key) {
    IniSection* sec = findSection(ini, section);

    if (sec == nullptr)
        return false;

    return findKeyIndex(sec, key) >= 0;
}

// ===[ Mutation ]===

void Ini_setString(IniFile* ini, const char* section, const char* key, const char* value) {
    // If we are passing a null value, let's remove it!
    if (value == nullptr) {
        Ini_deleteKey(ini, section, key);
        return;
    }

    IniSection* sec = findSection(ini, section);
    if (sec == nullptr) {
        sec = addSection(ini, section);
    }

    int idx = findKeyIndex(sec, key);
    if (idx >= 0) {
        free(sec->values[idx]);
        sec->values[idx] = safeStrdup(value);
    } else {
        addKeyValue(sec, key, value);
    }
}

void Ini_deleteKey(IniFile* ini, const char* section, const char* key) {
    IniSection* sec = findSection(ini, section);
    if (sec == nullptr)
        return;

    int idx = findKeyIndex(sec, key);
    if (0 > idx)
        return;

    free(sec->keys[idx]);
    free(sec->values[idx]);

    // Shift remaining entries down
    for (int i = idx; sec->count - 1 > i; i++) {
        sec->keys[i] = sec->keys[i + 1];
        sec->values[i] = sec->values[i + 1];
    }
    sec->count--;
}

void Ini_deleteSection(IniFile* ini, const char* section) {
    int sectionIndex = -1;
    repeat(ini->count, i) {
        if (strcmp(ini->sections[i].name, section) == 0) {
            sectionIndex = (int) i;
            break;
        }
    }
    if (0 > sectionIndex) return;

    // Free the section's contents
    IniSection* sec = &ini->sections[sectionIndex];
    repeat(sec->count, j) {
        free(sec->keys[j]);
        free(sec->values[j]);
    }
    free(sec->keys);
    free(sec->values);
    free(sec->name);

    // Shift remaining sections down
    for (int i = sectionIndex; ini->count - 1 > i; i++) {
        ini->sections[i] = ini->sections[i + 1];
    }
    ini->count--;
}

// ===[ Serialization ]===

char* Ini_serialize(const IniFile* ini, size_t initialCapacity) {
    size_t capacity = initialCapacity;
    size_t length = 0;
    char* buffer = safeMalloc(capacity);
    buffer[0] = '\0';

    repeat(ini->count, i) {
        IniSection* section = &ini->sections[i];

        // Blank line before section (unless at start of output)
        // Format: \n[name]\nkey=value\n...
        const char* name = section->name;
        size_t nameLen = strlen(name);

        // Calculate space needed for section header
        // optional \n + [ + name + ] + \n
        size_t needed = (length > 0 ? 1 : 0) + 1 + nameLen + 1 + 1;

        // Calculate space needed for all key=value pairs
        repeat(section->count, j) {
            needed += strlen(section->keys[j]) + 1 + strlen(section->values[j]) + 1;
        }

        // Grow buffer if needed
        while (length + needed + 1 > capacity) {
            capacity *= 2;
        }
        buffer = safeRealloc(buffer, capacity);

        // Write section header
        if (length > 0) {
            buffer[length++] = '\n';
        }
        buffer[length++] = '[';
        memcpy(buffer + length, name, nameLen);
        length += nameLen;
        buffer[length++] = ']';
        buffer[length++] = '\n';

        // Write key=value pairs
        repeat(section->count, j) {
            const char* key = section->keys[j];
            const char* value = section->values[j];
            size_t keyLen = strlen(key);
            size_t valueLen = strlen(value);

            memcpy(buffer + length, key, keyLen);
            length += keyLen;
            buffer[length++] = '=';
            memcpy(buffer + length, value, valueLen);
            length += valueLen;
            buffer[length++] = '\n';
        }
    }

    buffer[length] = '\0';
    return buffer;
}
