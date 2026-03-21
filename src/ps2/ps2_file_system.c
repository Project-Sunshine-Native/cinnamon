#include "ps2_file_system.h"
#include "ps2_utils.h"
#include "../json_reader.h"
#include "../utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "stb_ds.h"

// ===[ Internal Types ]===

typedef struct {
    char* key; // game-relative file name
    char** value; // stb_ds dynamic array of resolved device paths
} Ps2FileMapping;

// Parsed save icon configuration from CONFIG.JSN "saveIcon" section
typedef struct {
    uint32_t bgAlpha;
    int32_t bgColors[4][4]; // 4 corners x RGBA
    float lightDirs[3][4]; // 3 lights x XYZW
    float lightColors[3][4]; // 3 lights x RGBA
    float ambient[4]; // RGBA
} SaveIconConfig;

typedef struct {
    FileSystem base;
    Ps2FileMapping* mappings; // stb_ds string hashmap
    char* gameTitle; // game display name for icon.sys generation
    SaveIconConfig saveIconConfig;
} Ps2FileSystem;

// ===[ Helpers ]===

// Expands $BOOT: prefix to the boot device path, or returns a strdup of the input
static char* expandBootPrefix(const char* path) {
    const char* bootPrefix = "$BOOT:";
    size_t bootPrefixLen = strlen(bootPrefix);

    if (strncmp(path, bootPrefix, bootPrefixLen) == 0) {
        const char* relativePart = path + bootPrefixLen;
        return PS2Utils_createDevicePath(relativePart);
    }

    return safeStrdup(path);
}

// ===[ icon.sys Generation ]===
// icon.sys is a fixed 964-byte file required by the PS2 memory card browser
// Without it, the save directory shows as "Corrupted Data"

#define ICON_SYS_SIZE 964

// Converts an ASCII character to its full-width Shift-JIS encoding (2 bytes)
// The PS2 memory card browser only renders Shift-JIS glyphs, not plain ASCII
// If a character is not supported, it will fall back to a space character
static void asciiToShiftJIS(char c, uint8_t* out) {
    if (c == ' ') {
        out[0] = 0x81; out[1] = 0x40;
    } else if (c >= '0' && '9' >= c) {
        out[0] = 0x82; out[1] = 0x4F + (c - '0');
    } else if (c >= 'A' && 'Z' >= c) {
        out[0] = 0x82; out[1] = 0x60 + (c - 'A');
    } else if (c >= 'a' && 'z' >= c) {
        out[0] = 0x82; out[1] = 0x81 + (c - 'a');
    } else if (c == '!') {
        out[0] = 0x81; out[1] = 0x49;
    } else if (c == '?') {
        out[0] = 0x81; out[1] = 0x48;
    } else if (c == '.') {
        out[0] = 0x81; out[1] = 0x44;
    } else if (c == ',') {
        out[0] = 0x81; out[1] = 0x43;
    } else if (c == ':') {
        out[0] = 0x81; out[1] = 0x46;
    } else if (c == '-') {
        out[0] = 0x81; out[1] = 0x7C;
    } else if (c == '(') {
        out[0] = 0x81; out[1] = 0x69;
    } else if (c == ')') {
        out[0] = 0x81; out[1] = 0x6A;
    } else {
        // Unsupported character, use full-width space as fallback
        out[0] = 0x81; out[1] = 0x40;
    }
}

// Writes the game title as full-width Shift-JIS into the icon.sys title field (68 bytes max)
static void writeShiftJISTitle(uint8_t* titleField, const char* gameTitle) {
    size_t srcLen = strlen(gameTitle);
    size_t dstPos = 0;
    size_t maxBytes = 66; // 68 bytes minus 2 for null terminator safety

    repeat(srcLen, i) {
        if (dstPos + 2 > maxBytes)
            break;
        asciiToShiftJIS(gameTitle[i], titleField + dstPos);
        dstPos += 2; // Each Shift-JIS character is 2 bytes
    }
}

static void generateIconSys(uint8_t* buffer, const char* gameTitle, const SaveIconConfig* config) {
    memset(buffer, 0, ICON_SYS_SIZE);

    // Magic "PS2D"
    memcpy(buffer + 0x000, "PS2D", 4);

    // Offset 4: 2 bytes reserved (0)
    // Offset 6: 2 bytes newline offset in title (0 = no line break)
    // Offset 8: 4 bytes reserved (0)
    // All left as 0 from memset

    // Background transparency (0x00-0x80)
    memcpy(buffer + 0x00C, &config->bgAlpha, 4);

    // Background colors: 4 corners x RGBA as int32 (0-255 range)
    memcpy(buffer + 0x010, config->bgColors, sizeof(config->bgColors));

    // Light directions: 3 lights x XYZW as float
    memcpy(buffer + 0x050, config->lightDirs, sizeof(config->lightDirs));

    // Light colors: 3 lights x RGBA as float (0.0-1.0)
    memcpy(buffer + 0x080, config->lightColors, sizeof(config->lightColors));

    // Ambient color: RGBA as float
    memcpy(buffer + 0x0B0, config->ambient, sizeof(config->ambient));

    // Title (68 bytes, full-width Shift-JIS encoded)
    writeShiftJISTitle(buffer + 0x0C0, gameTitle);

    // Icon filenames (64 bytes each) at 0x104, 0x144, 0x184
    // All three (normal, copy, delete) reference the same icon file
    const char* iconFileName = "ICON.ICO";
    size_t iconNameLen = strlen(iconFileName);
    memcpy(buffer + 0x104, iconFileName, iconNameLen);
    memcpy(buffer + 0x144, iconFileName, iconNameLen);
    memcpy(buffer + 0x184, iconFileName, iconNameLen);
}

// Copies a file from src to dst (binary). Returns true on success.
static bool copyFile(const char* srcPath, const char* dstPath) {
    FILE* src = fopen(srcPath, "rb");
    if (src == nullptr)
        return false;

    fseek(src, 0, SEEK_END);
    long size = ftell(src);
    fseek(src, 0, SEEK_SET);

    uint8_t* data = safeMalloc((size_t) size);
    size_t bytesRead = fread(data, 1, (size_t) size, src);
    fclose(src);

    FILE* dst = fopen(dstPath, "wb");
    if (dst == nullptr) {
        free(data);
        return false;
    }

    size_t written = fwrite(data, 1, bytesRead, dst);
    fclose(dst);
    free(data);
    return written == bytesRead;
}

// Copies ICON.ICO from the boot device into the given directory if it doesn't already exist
static void copyIconIcoIfMissing(const char* dirPath) {
    size_t dirLen = strlen(dirPath);
    size_t pathLen = dirLen + 1 + 8 + 1; // "/ICON.ICO\0"
    char* dstPath = safeMalloc(pathLen);
    snprintf(dstPath, pathLen, "%s/ICON.ICO", dirPath);

    // Check if it already exists on the memory card
    FILE* check = fopen(dstPath, "rb");
    if (check != nullptr) {
        fclose(check);
        free(dstPath);
        return;
    }

    // Copy from boot device
    char* srcPath = PS2Utils_createDevicePath("ICON.ICO");
    if (copyFile(srcPath, dstPath)) {
        fprintf(stderr, "Ps2FileSystem: Copied ICON.ICO to %s\n", dirPath);
    } else {
        fprintf(stderr, "Ps2FileSystem: Failed to copy ICON.ICO from %s to %s\n", srcPath, dstPath);
    }

    free(srcPath);
    free(dstPath);
}

// Writes icon.sys into the given directory if it doesn't already exist
static void writeIconSysIfMissing(const char* dirPath, const char* gameTitle, const SaveIconConfig* config) {
    // Build path: "dirPath/icon.sys"
    size_t dirLen = strlen(dirPath);
    size_t pathLen = dirLen + 1 + 8 + 1; // "/icon.sys\0"
    char* iconSysPath = safeMalloc(pathLen);
    snprintf(iconSysPath, pathLen, "%s/icon.sys", dirPath);

    // Check if it already exists
    FILE* check = fopen(iconSysPath, "rb");
    if (check != nullptr) {
        fclose(check);
        free(iconSysPath);
        return;
    }

    // Generate and write
    uint8_t buffer[ICON_SYS_SIZE];
    generateIconSys(buffer, gameTitle, config);

    FILE* f = fopen(iconSysPath, "wb");
    if (f != nullptr) {
        fwrite(buffer, 1, ICON_SYS_SIZE, f);
        fclose(f);
        fprintf(stderr, "Ps2FileSystem: Created icon.sys in %s\n", dirPath);
    } else {
        fprintf(stderr, "Ps2FileSystem: Failed to create icon.sys in %s\n", dirPath);
    }

    free(iconSysPath);
}

// Ensures the parent directory exists for mc0:/mc1: paths by calling mkdir
// Also writes icon.sys and copies ICON.ICO if the directory is newly created
static void ensureParentDirectory(Ps2FileSystem* pfs, const char* path) {
    // Only do this for memory card paths
    if (strncmp(path, "mc0:", 4) != 0 && strncmp(path, "mc1:", 4) != 0)
        return;

    char* pathCopy = safeStrdup(path);
    char* lastSlash = strrchr(pathCopy, '/');
    if (lastSlash != nullptr && lastSlash != pathCopy) {
        *lastSlash = '\0';
        mkdir(pathCopy, 0777);
        writeIconSysIfMissing(pathCopy, pfs->gameTitle, &pfs->saveIconConfig);
        copyIconIcoIfMissing(pathCopy);
    }
    free(pathCopy);
}

// ===[ Vtable Implementations ]===

static char* resolvePath(FileSystem* fs, const char* relativePath) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    ptrdiff_t idx = shgeti(pfs->mappings, relativePath);
    if (0 > idx)
        return nullptr;

    // Return the first mapped path
    if (arrlen(pfs->mappings[idx].value) > 0)
        return safeStrdup(pfs->mappings[idx].value[0]);

    return nullptr;
}

static bool fileExists(FileSystem* fs, const char* relativePath) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    ptrdiff_t idx = shgeti(pfs->mappings, relativePath);
    if (0 > idx)
        return false;

    char** paths = pfs->mappings[idx].value;
    int pathCount = arrlen(paths);
    repeat(pathCount, i) {
        FILE* f = fopen(paths[i], "rb");
        if (f != nullptr) {
            fclose(f);
            return true;
        }
    }

    return false;
}

static char* readFileText(FileSystem* fs, const char* relativePath) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    ptrdiff_t idx = shgeti(pfs->mappings, relativePath);
    if (0 > idx)
        return nullptr;

    // For the PlayStation 2 target, we have multiple "search" paths for a specific file
    // The reason why we do this is because GameMaker allows files to be in two different folders: The save folder and the bundled folder
    // However, hitting the memory card for some specific files that ARE NOT in the memory card is a bit expensive
    char** paths = pfs->mappings[idx].value;
    int pathCount = arrlen(paths);
    repeat(pathCount, i) {
        FILE* f = fopen(paths[i], "rb");
        if (f == nullptr)
            continue;

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        char* content = safeMalloc((size_t) size + 1);
        size_t bytesRead = fread(content, 1, (size_t) size, f);
        content[bytesRead] = '\0';
        fclose(f);
        return content;
    }

    return nullptr;
}

static bool writeFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    ptrdiff_t idx = shgeti(pfs->mappings, relativePath);
    if (0 > idx)
        return false;

    char** paths = pfs->mappings[idx].value;
    if (arrlen(paths) == 0)
        return false;

    // Write to the first path (the first path is ALWAYS the writeable path)
    const char* writePath = paths[0];
    ensureParentDirectory(pfs, writePath);

    FILE* f = fopen(writePath, "wb");
    if (f == nullptr)
        return false;

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);
    return written == len;
}

static bool deleteFile(FileSystem* fs, const char* relativePath) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    ptrdiff_t idx = shgeti(pfs->mappings, relativePath);
    if (0 > idx)
        return false;

    char** paths = pfs->mappings[idx].value;
    if (arrlen(paths) == 0)
        return false;

    // Delete the first path
    return remove(paths[0]) == 0;
}

// ===[ Vtable ]===

static FileSystemVtable ps2FileSystemVtable = {
    .resolvePath = resolvePath,
    .fileExists = fileExists,
    .readFileText = readFileText,
    .writeFileText = writeFileText,
    .deleteFile = deleteFile,
};

// ===[ Lifecycle ]===

static SaveIconConfig parseSaveIconConfig(JsonValue* configRoot) {
    JsonValue* saveIconObj = JsonReader_getObject(configRoot, "saveIcon");
    requireNotNullMessage(saveIconObj, "CONFIG.JSN is missing the 'saveIcon' section");
    require(JsonReader_isObject(saveIconObj));

    SaveIconConfig config = {0};

    // bgAlpha (0x00-0x80)
    JsonValue* bgAlphaVal = JsonReader_getObject(saveIconObj, "bgAlpha");
    requireNotNullMessage(bgAlphaVal, "saveIcon.bgAlpha is missing");
    config.bgAlpha = (uint32_t) JsonReader_getDouble(bgAlphaVal);

    // bgColors: array of 4 arrays of 3 ints [R, G, B] (A is always 0)
    JsonValue* bgColorsArr = JsonReader_getObject(saveIconObj, "bgColors");
    requireNotNullMessage(bgColorsArr, "saveIcon.bgColors is missing");
    require(JsonReader_isArray(bgColorsArr) && JsonReader_arrayLength(bgColorsArr) == 4);
    repeat(4, i) {
        JsonValue* corner = JsonReader_getArrayElement(bgColorsArr, i);
        JsonReader_readInt32Array(corner, config.bgColors[i], 3);
        config.bgColors[i][3] = 0; // A = 0
    }

    // lightDirs: array of 3 arrays of 3 floats [X, Y, Z] (W is always 0.0)
    JsonValue* lightDirsArr = JsonReader_getObject(saveIconObj, "lightDirs");
    requireNotNullMessage(lightDirsArr, "saveIcon.lightDirs is missing");
    require(JsonReader_isArray(lightDirsArr) && JsonReader_arrayLength(lightDirsArr) == 3);
    repeat(3, i) {
        JsonValue* dir = JsonReader_getArrayElement(lightDirsArr, i);
        JsonReader_readFloatArray(dir, config.lightDirs[i], 3);
        config.lightDirs[i][3] = 0.0f; // W = 0
    }

    // lightColors: array of 3 arrays of 3 floats [R, G, B] (A is always 0.0)
    JsonValue* lightColorsArr = JsonReader_getObject(saveIconObj, "lightColors");
    requireNotNullMessage(lightColorsArr, "saveIcon.lightColors is missing");
    require(JsonReader_isArray(lightColorsArr) && JsonReader_arrayLength(lightColorsArr) == 3);
    repeat(3, i) {
        JsonValue* color = JsonReader_getArrayElement(lightColorsArr, i);
        JsonReader_readFloatArray(color, config.lightColors[i], 3);
        config.lightColors[i][3] = 0.0f; // A = 0
    }

    // ambient: array of 3 floats [R, G, B] (A is always 0.0)
    JsonValue* ambientArr = JsonReader_getObject(saveIconObj, "ambient");
    requireNotNullMessage(ambientArr, "saveIcon.ambient is missing");
    JsonReader_readFloatArray(ambientArr, config.ambient, 3);
    config.ambient[3] = 0.0f; // A = 0

    return config;
}

FileSystem* Ps2FileSystem_create(JsonValue* configRoot, const char* gameTitle) {
    JsonValue* fileSystemObj = JsonReader_getObject(configRoot, "fileSystem");
    require(fileSystemObj != nullptr && JsonReader_isObject(fileSystemObj));

    Ps2FileSystem* pfs = safeCalloc(1, sizeof(Ps2FileSystem));
    pfs->base.vtable = &ps2FileSystemVtable;
    pfs->gameTitle = safeStrdup(gameTitle);
    pfs->saveIconConfig = parseSaveIconConfig(configRoot);
    pfs->mappings = nullptr;
    sh_new_strdup(pfs->mappings);

    int entryCount = JsonReader_objectLength(fileSystemObj);
    repeat(entryCount, i) {
        const char* gameFileName = JsonReader_getObjectKey(fileSystemObj, i);
        JsonValue* pathArray = JsonReader_getObjectValue(fileSystemObj, i);

        require(JsonReader_isArray(pathArray));

        char** resolvedPaths = nullptr;
        int pathCount = JsonReader_arrayLength(pathArray);
        repeat(pathCount, j) {
            JsonValue* pathElement = JsonReader_getArrayElement(pathArray, j);
            require(JsonReader_isString(pathElement));

            const char* rawPath = JsonReader_getString(pathElement);
            char* resolved = expandBootPrefix(rawPath);
            arrput(resolvedPaths, resolved);
            fprintf(stderr, "Ps2FileSystem: '%s' -> '%s'\n", gameFileName, resolved);
        }

        shput(pfs->mappings, gameFileName, resolvedPaths);
    }

    fprintf(stderr, "Ps2FileSystem: Loaded %d file mappings\n", (int) shlen(pfs->mappings));
    return (FileSystem*) pfs;
}

void Ps2FileSystem_destroy(FileSystem* fs) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    free(pfs->gameTitle);
    int mappingCount = shlen(pfs->mappings);
    repeat(mappingCount, i) {
        char** paths = pfs->mappings[i].value;
        int pathCount = arrlen(paths);
        repeat(pathCount, j) {
            free(paths[j]);
        }
        arrfree(paths);
    }
    shfree(pfs->mappings);
    free(pfs);
}
