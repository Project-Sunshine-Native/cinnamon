#include "../../src/data_win.h"
#include "../../src/gl/image_decoder.h"
#include "../../src/utils.h"

#include <stb/ds/stb_ds.h>
#include <stb/image/stb_image.h>
#include <stb/image/stb_image_write.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_VORBIS_HEADER_ONLY
#include "../../vendor/stb/vorbis/stb_vorbis.c"

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#define N3DS_PREPROCESS_HOST_WINDOWS 1
#else
#define N3DS_PREPROCESS_HOST_WINDOWS 0
#endif

#if N3DS_PREPROCESS_HOST_WINDOWS
#include <windows.h>
#include <process.h>
#endif

#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MSYS__)
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define MKDIR(path) mkdir(path, 0777)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define N3DS_PREPROCESS_MAYBE_UNUSED __attribute__((unused))
#else
#define N3DS_PREPROCESS_MAYBE_UNUSED
#endif

#if N3DS_PREPROCESS_HOST_WINDOWS
#define N3DS_PREPROCESS_PATH_SEP "\\"
#else
#define N3DS_PREPROCESS_PATH_SEP "/"
#endif

#define N3DS_ATLAS_MAGIC 0x5441334Eu /* N3AT */
#define N3DS_ATLAS_VERSION_T3X 2u
#define N3DS_ATLAS_VERSION_FRAGMENTED 3u
#define N3DS_ATLAS_VERSION_FRAGMENTED_TILES 4u
#define N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS 5u
#define N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT 6u
#define N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT 7u
#define N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED 8u
#define N3DS_DIRECT_ASSET_MAGIC 0x3152444Eu /* NDR1 */
#define N3DS_DIRECT_ASSET_VERSION 1u
#define N3DS_ROOM_MANIFEST_MAGIC 0x4D52334Eu /* N3RM */
#define N3DS_ROOM_MANIFEST_VERSION 1u
#define N3DS_SOUND_BANK_MAGIC 0x314B4253u /* SBK1 */
#define N3DS_SOUND_BANK_VERSION 2u
#define N3DS_SOUND_BANK_ENTRY_CHANNEL_MASK 0x000000FFu
#define N3DS_SOUND_BANK_ENTRY_FLAG_LOOP 0x00000100u
#define N3DS_SOUND_BANK_ENTRY_FLAG_PCM16 0x00000200u
#define N3DS_SMALL_TEXTURE_SIZE 256u
#define N3DS_LARGE_TEXTURE_SIZE 512u
#define GMS2_TILE_INDEX_MASK  0x0007FFFFu
#define CWAV_VERSION 0x02010000u
#define CWAV_REF_DSP_ADPCM_INFO 0x0300u
#define CWAV_REF_SAMPLE_DATA 0x1F00u
#define CWAV_REF_INFO_BLOCK 0x7000u
#define CWAV_REF_DATA_BLOCK 0x7001u
#define CWAV_REF_CHANNEL_INFO 0x7100u

typedef enum {
    N3DS_ATLAS_PAGE_MODE_AUTO = 0,
    N3DS_ATLAS_PAGE_MODE_FORCE_256,
    N3DS_ATLAS_PAGE_MODE_FORCE_512,
} N3DSAtlasPageMode;

typedef enum {
    N3DS_TEXFMT_RGBA5551 = 0,
    N3DS_TEXFMT_ETC1A4 = 1,
    N3DS_TEXFMT_INDEXED8 = 2,
    N3DS_TEXFMT_HYBRID = 3,
    N3DS_TEXFMT_L4 = 4,
    N3DS_TEXFMT_LA4 = 5,
} N3DSTextureFormat;

typedef struct {
    uint32_t key;
    uint32_t value;
} ColorFreqMap;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t fragmentStart;
    uint16_t fragmentCount;
} OutputItem;

typedef struct {
    uint16_t atlasId;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint16_t sourceX;
    uint16_t sourceY;
} OutputFragment;

typedef struct {
    int16_t bgDef;
    uint16_t srcX;
    uint16_t srcY;
    uint16_t srcW;
    uint16_t srcH;
    uint16_t atlasId;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t fragmentStart;
    uint16_t fragmentCount;
} OutputTileEntry;

typedef struct {
    int16_t bgDef;
    uint16_t srcX;
    uint16_t srcY;
    uint16_t srcW;
    uint16_t srcH;
} TileLookupKey;

typedef struct {
    TileLookupKey key;
    uint32_t value;
} TileRequestMap;

typedef struct {
    const char* inputPath;
    const char* outputDir;
    const char* tex3dsExe;
    const char* spriteReplacementDir;
    const char* borderAssetDir;
    const char* pageFormatOverridesPath;
    bool keepPng;
    bool dumpPagePreviews;
    bool enableTargetedBattleDialogueMono;
    bool interactiveMode;
    bool inspectRoomMode;
    int32_t inspectRoomIndex;
    N3DSAtlasPageMode atlasPageMode;
    N3DSTextureFormat textureFormat;
    char inputPathStorage[1024];
    char outputDirStorage[1024];
    char finalOutputDirStorage[1024];
    char stagingOutputDirStorage[1024];
    char tex3dsExeStorage[1024];
    char spriteReplacementDirStorage[1200];
    char borderAssetDirStorage[1200];
    char toolDirStorage[1024];
    bool stageOutputLocally;
    bool spriteReplacementDirAvailable;
    bool borderAssetDirAvailable;
} Options;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t textureFormat;
    char path[1024];
    char previewLabel[128];
    char debugNames[2048];
    bool containsSprite;
    bool containsFont;
    bool containsBackground;
    bool containsNonMonoContent;
} OutputPage;

typedef struct {
    uint32_t pageIndex;
    uint32_t width;
    uint32_t height;
    uint32_t cursorX;
    uint32_t cursorY;
    uint32_t rowHeight;
    uint8_t* pixels;
} PackedPage;

typedef struct {
    bool sawFrame;
    bool allFramesMonoSafe;
} DirectSpriteFormatState;

typedef struct {
    char relativePath[320];
    char sourcePath[1024];
    uint32_t pathOffset;
    uint32_t dataOffset;
    uint32_t dataSize;
} DirectAssetPackEntry;

typedef struct {
    uint32_t roomIndex;
    uint32_t pageStart;
    uint32_t pageCount;
    uint32_t directSpriteStart;
    uint32_t directSpriteCount;
    uint32_t directBackgroundStart;
    uint32_t directBackgroundCount;
} N3DSRoomManifestEntry;

typedef struct {
    uint16_t pageIndex;
    uint8_t flags;
    uint8_t reserved;
} N3DSRoomManifestPageRef;

static bool fileExists(const char* path);
static bool directoryExists(const char* path);
static bool deleteDirectoryRecursive(const char* path);
static bool ensureDirRecursive(const char* path);
static char* dupParentDir(const char* path);
static bool getProgramDirPath(const char* argv0, char* outDir, size_t outDirSize);
static bool configureStagedOutputDir(Options* options, const char* argv0);
static bool configureSpriteReplacementDir(Options* options, const char* argv0);
static bool configureBorderAssetDir(Options* options, const char* argv0);
static bool syncStagedOutputToDestination(const Options* options);
static bool convertBorderAssets(const Options* options);
static void writeLe32(uint8_t* ptr, uint32_t value);

static void setDefaultOptions(Options* out) {
    memset(out, 0, sizeof(*out));
    out->keepPng = false;
    out->dumpPagePreviews = false;
    out->enableTargetedBattleDialogueMono = false;
    out->interactiveMode = false;
    out->atlasPageMode = N3DS_ATLAS_PAGE_MODE_AUTO;
    out->textureFormat = N3DS_TEXFMT_HYBRID;
    out->stageOutputLocally = true;
}

static void printUsage(const char* argv0) {
    fprintf(stderr,
        "Usage: %s <data.win> <output-dir> [--tex3ds <path>] [--keep-png]\n"
        "           [--atlas-page-mode auto|256|512]\n"
        "           [--texture-format etc1a4|rgba5551|indexed8|hybrid]\n"
        "           [--l4-dialogue-battle-sprites]\n"
        "           [--dump-page-previews]\n"
        "           [--page-format-overrides <file>]\n"
        "           [--sprite-replacements <dir>]\n"
        "           [--borders <dir>]\n"
        "\n"
        "Running with no arguments on Windows starts an interactive setup.\n"
        "\n"
        "Outputs:\n"
        "  <output-dir>/gfx/atlas.bin\n"
        "  <output-dir>/gfx/direct_assets.bin\n"
        "  <output-dir>/gfx/room_manifest.bin\n"
        "  <output-dir>/gfx/page_000.t3x ...\n"
        "  <output-dir>/audio/sound_bank.bin\n",
        argv0
    );
}

#if N3DS_PREPROCESS_HOST_WINDOWS
static bool getExecutableDir(char* out, size_t outSize) {
    if (out == NULL || outSize == 0) return false;
    DWORD len = GetModuleFileNameA(NULL, out, (DWORD) outSize);
    if (len == 0 || len >= outSize) return false;

    char* slash = strrchr(out, '\\');
    char* forward = strrchr(out, '/');
    if (forward != NULL && (slash == NULL || forward > slash)) slash = forward;
    if (slash == NULL) return false;
    *slash = '\0';
    return true;
}

static bool readRegistryString(HKEY root, const char* subKey, const char* valueName, char* out, DWORD outSize) {
    HKEY key = NULL;
    DWORD type = REG_SZ;
    DWORD size = outSize;
    if (RegOpenKeyExA(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    LONG status = RegQueryValueExA(key, valueName, NULL, &type, (LPBYTE) out, &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    out[outSize - 1] = '\0';
    return out[0] != '\0';
}
#endif

static void applyDefaultToolPaths(Options* out) {
    if (out == NULL) return;

#if N3DS_PREPROCESS_HOST_WINDOWS
    if (getExecutableDir(out->toolDirStorage, sizeof(out->toolDirStorage))) {
        char adjacentPath[1024];

        snprintf(adjacentPath, sizeof(adjacentPath), "%s\\tex3ds.exe", out->toolDirStorage);
        if (fileExists(adjacentPath)) {
            snprintf(out->tex3dsExeStorage, sizeof(out->tex3dsExeStorage), "%s", adjacentPath);
            out->tex3dsExe = out->tex3dsExeStorage;
        }
    }
#endif

    if (out->tex3dsExe == NULL) {
        const char* devkitpro = getenv("DEVKITPRO");
        if (devkitpro != NULL && devkitpro[0] != '\0') {
#if N3DS_PREPROCESS_HOST_WINDOWS
            snprintf(out->tex3dsExeStorage, sizeof(out->tex3dsExeStorage), "%s/tools/bin/tex3ds.exe", devkitpro);
#else
            snprintf(out->tex3dsExeStorage, sizeof(out->tex3dsExeStorage), "%s/tools/bin/tex3ds", devkitpro);
#endif
            out->tex3dsExe = out->tex3dsExeStorage;
        } else {
#if N3DS_PREPROCESS_HOST_WINDOWS
            out->tex3dsExe = "C:/devkitPro/tools/bin/tex3ds.exe";
#else
            out->tex3dsExe = "/opt/devkitpro/tools/bin/tex3ds";
#endif
        }
    }

}

static bool parseArgs(int argc, char** argv, Options* out) {
    setDefaultOptions(out);

    if (argc == 1) {
        out->interactiveMode = true;
        applyDefaultToolPaths(out);
        return true;
    }

    if (argc < 3) return false;

    out->inputPath = argv[1];
    out->outputDir = argv[2];
    out->pageFormatOverridesPath = NULL;
    applyDefaultToolPaths(out);

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--keep-png") == 0) {
            out->keepPng = true;
            continue;
        }
        if (strcmp(argv[i], "--dump-page-previews") == 0) {
            out->dumpPagePreviews = true;
            continue;
        }
        if (strcmp(argv[i], "--l4-dialogue-battle-sprites") == 0) {
            out->enableTargetedBattleDialogueMono = true;
            continue;
        }
        if (strcmp(argv[i], "--page-format-overrides") == 0 && i + 1 < argc) {
            out->pageFormatOverridesPath = argv[++i];
            out->dumpPagePreviews = true;
            continue;
        }
        if (strcmp(argv[i], "--sprite-replacements") == 0 && i + 1 < argc) {
            out->spriteReplacementDir = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--borders") == 0 && i + 1 < argc) {
            out->borderAssetDir = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--atlas-page-mode") == 0 && i + 1 < argc) {
            const char* mode = argv[++i];
            if (strcmp(mode, "auto") == 0) out->atlasPageMode = N3DS_ATLAS_PAGE_MODE_AUTO;
            else if (strcmp(mode, "256") == 0) out->atlasPageMode = N3DS_ATLAS_PAGE_MODE_FORCE_256;
            else if (strcmp(mode, "512") == 0) out->atlasPageMode = N3DS_ATLAS_PAGE_MODE_FORCE_512;
            else {
                fprintf(stderr, "Unknown atlas page mode: %s\n", mode);
                return false;
            }
            continue;
        }
        if (strcmp(argv[i], "--texture-format") == 0 && i + 1 < argc) {
            const char* format = argv[++i];
            if (strcmp(format, "etc1a4") == 0) out->textureFormat = N3DS_TEXFMT_ETC1A4;
            else if (strcmp(format, "rgba5551") == 0) out->textureFormat = N3DS_TEXFMT_RGBA5551;
            else if (strcmp(format, "indexed8") == 0) out->textureFormat = N3DS_TEXFMT_INDEXED8;
            else if (strcmp(format, "hybrid") == 0) out->textureFormat = N3DS_TEXFMT_HYBRID;
            else {
                fprintf(stderr, "Unknown texture format: %s\n", format);
                return false;
            }
            continue;
        }
        if (strcmp(argv[i], "--tex3ds") == 0 && i + 1 < argc) {
            out->tex3dsExe = argv[++i];
            continue;
        }
        fprintf(stderr, "Unknown argument: %s\n", argv[i]);
        return false;
    }

    return true;
}

static bool ensureDir(const char* path) {
    if (MKDIR(path) == 0) return true;
    return errno == EEXIST;
}

static bool ensureDirRecursive(const char* path) {
    if (path == NULL || path[0] == '\0') return false;
    if (ensureDir(path)) return true;

    char temp[1024];
    snprintf(temp, sizeof(temp), "%s", path);
    size_t len = strlen(temp);
    if (len == 0) return false;

    for (size_t i = 1; i < len; ++i) {
        char c = temp[i];
        if (c != '/' && c != '\\') continue;
        temp[i] = '\0';
        if (temp[0] != '\0' && !(i == 2 && temp[1] == ':') && !ensureDir(temp)) {
            return false;
        }
        temp[i] = c;
    }

    return ensureDir(temp);
}

static bool ensureOutputDirs(const char* outputDir) {
    if (!ensureDirRecursive(outputDir)) {
        fprintf(stderr, "Failed to create output dir: %s\n", outputDir);
        return false;
    }

    char gfxDir[1024];
    snprintf(gfxDir, sizeof(gfxDir), "%s/gfx", outputDir);
    if (!ensureDirRecursive(gfxDir)) {
        fprintf(stderr, "Failed to create gfx dir: %s\n", gfxDir);
        return false;
    }

    char audioDir[1024];
    snprintf(audioDir, sizeof(audioDir), "%s/audio", outputDir);
    if (!ensureDirRecursive(audioDir)) {
        fprintf(stderr, "Failed to create audio dir: %s\n", audioDir);
        return false;
    }

    char spritesDir[1024];
    snprintf(spritesDir, sizeof(spritesDir), "%s/gfx/sprites", outputDir);
    if (!ensureDirRecursive(spritesDir)) {
        fprintf(stderr, "Failed to create sprites dir: %s\n", spritesDir);
        return false;
    }

    char backgroundsDir[1024];
    snprintf(backgroundsDir, sizeof(backgroundsDir), "%s/gfx/backgrounds", outputDir);
    if (!ensureDirRecursive(backgroundsDir)) {
        fprintf(stderr, "Failed to create backgrounds dir: %s\n", backgroundsDir);
        return false;
    }

    char bordersDir[1024];
    snprintf(bordersDir, sizeof(bordersDir), "%s/gfx/borders", outputDir);
    if (!ensureDirRecursive(bordersDir)) {
        fprintf(stderr, "Failed to create borders dir: %s\n", bordersDir);
        return false;
    }

    char fontsDir[1024];
    snprintf(fontsDir, sizeof(fontsDir), "%s/gfx/fonts", outputDir);
    if (!ensureDirRecursive(fontsDir)) {
        fprintf(stderr, "Failed to create fonts dir: %s\n", fontsDir);
        return false;
    }
    return true;
}

static bool getProgramDirPath(const char* argv0, char* outDir, size_t outDirSize) {
    if (outDir == NULL || outDirSize == 0) return false;
    outDir[0] = '\0';

#if N3DS_PREPROCESS_HOST_WINDOWS
    if (getExecutableDir(outDir, outDirSize)) return true;
#else
    char exePath[1024];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0 && (size_t) len < sizeof(exePath)) {
        exePath[len] = '\0';
        char* slash = strrchr(exePath, '/');
        if (slash != NULL) {
            *slash = '\0';
            snprintf(outDir, outDirSize, "%s", exePath);
            return true;
        }
    }
#endif

    if (argv0 != NULL && argv0[0] != '\0') {
        char candidate[1024];
        snprintf(candidate, sizeof(candidate), "%s", argv0);
        char* slash = strrchr(candidate, '/');
        char* backslash = strrchr(candidate, '\\');
        if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
        if (slash != NULL) {
            *slash = '\0';
            if (candidate[0] != '\0') {
                snprintf(outDir, outDirSize, "%s", candidate);
                return true;
            }
        }
    }

#if N3DS_PREPROCESS_HOST_WINDOWS
    DWORD cwdLen = GetCurrentDirectoryA((DWORD) outDirSize, outDir);
    return cwdLen > 0 && cwdLen < outDirSize;
#else
    return getcwd(outDir, outDirSize) != NULL;
#endif
}

static bool filesAreIdentical(const char* pathA, const char* pathB) {
    if (pathA == NULL || pathB == NULL) return false;
    FILE* fileA = fopen(pathA, "rb");
    FILE* fileB = fopen(pathB, "rb");
    if (fileA == NULL || fileB == NULL) {
        if (fileA != NULL) fclose(fileA);
        if (fileB != NULL) fclose(fileB);
        return false;
    }

    bool identical = true;
    uint8_t* bufferA = safeMalloc(64u * 1024u);
    uint8_t* bufferB = safeMalloc(64u * 1024u);

    while (identical) {
        size_t readA = fread(bufferA, 1, 64u * 1024u, fileA);
        size_t readB = fread(bufferB, 1, 64u * 1024u, fileB);
        if (readA != readB) {
            identical = false;
            break;
        }
        if (readA == 0) {
            if (ferror(fileA) || ferror(fileB)) identical = false;
            break;
        }
        if (memcmp(bufferA, bufferB, readA) != 0) {
            identical = false;
            break;
        }
    }

    free(bufferA);
    free(bufferB);
    fclose(fileA);
    fclose(fileB);
    return identical;
}

static bool copyFileContents(const char* srcPath, const char* dstPath) {
    if (srcPath == NULL || dstPath == NULL) return false;

    char* parentDir = dupParentDir(dstPath);
    bool ok = ensureDirRecursive(parentDir);
    free(parentDir);
    if (!ok) return false;

    FILE* src = fopen(srcPath, "rb");
    if (src == NULL) return false;
    FILE* dst = fopen(dstPath, "wb");
    if (dst == NULL) {
        fclose(src);
        return false;
    }

    uint8_t* buffer = safeMalloc(64u * 1024u);
    ok = true;
    while (ok) {
        size_t bytesRead = fread(buffer, 1, 64u * 1024u, src);
        if (bytesRead > 0 && fwrite(buffer, 1, bytesRead, dst) != bytesRead) ok = false;
        if (bytesRead < 64u * 1024u) {
            if (ferror(src)) ok = false;
            break;
        }
    }

    free(buffer);
    fclose(src);
    fclose(dst);
    return ok;
}

static bool syncDirectoryTree(const char* srcDir, const char* dstDir, uint32_t* outUpdatedCount, uint32_t* outSkippedCount) {
    if (srcDir == NULL || dstDir == NULL) return false;
    if (!directoryExists(srcDir)) return false;
    if (!ensureDirRecursive(dstDir)) return false;

#if N3DS_PREPROCESS_HOST_WINDOWS
    char searchPath[1060];
    WIN32_FIND_DATAA findData;
    snprintf(searchPath, sizeof(searchPath), "%s\\*", srcDir);
    HANDLE findHandle = FindFirstFileA(searchPath, &findData);
    if (findHandle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to enumerate staged directory: %s\n", srcDir);
        return false;
    }

    bool ok = true;
    do {
        const char* name = findData.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char srcChild[1060];
        char dstChild[1060];
        snprintf(srcChild, sizeof(srcChild), "%s/%s", srcDir, name);
        snprintf(dstChild, sizeof(dstChild), "%s/%s", dstDir, name);

        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!syncDirectoryTree(srcChild, dstChild, outUpdatedCount, outSkippedCount)) {
                ok = false;
                break;
            }
            continue;
        }

        bool needsCopy = !fileExists(dstChild) || !filesAreIdentical(srcChild, dstChild);
        if (needsCopy) {
            if (!copyFileContents(srcChild, dstChild)) {
                fprintf(stderr, "Failed to copy staged asset: %s -> %s\n", srcChild, dstChild);
                ok = false;
                break;
            }
            if (outUpdatedCount != NULL) (*outUpdatedCount)++;
        } else if (outSkippedCount != NULL) {
            (*outSkippedCount)++;
        }
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);
    return ok;
#else
    DIR* dir = opendir(srcDir);
    if (dir == NULL) return false;

    bool ok = true;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char srcChild[1060];
        char dstChild[1060];
        snprintf(srcChild, sizeof(srcChild), "%s/%s", srcDir, name);
        snprintf(dstChild, sizeof(dstChild), "%s/%s", dstDir, name);

        if (directoryExists(srcChild)) {
            if (!syncDirectoryTree(srcChild, dstChild, outUpdatedCount, outSkippedCount)) {
                ok = false;
                break;
            }
            continue;
        }

        bool needsCopy = !fileExists(dstChild) || !filesAreIdentical(srcChild, dstChild);
        if (needsCopy) {
            if (!copyFileContents(srcChild, dstChild)) {
                fprintf(stderr, "Failed to copy staged asset: %s -> %s\n", srcChild, dstChild);
                ok = false;
                break;
            }
            if (outUpdatedCount != NULL) (*outUpdatedCount)++;
        } else if (outSkippedCount != NULL) {
            (*outSkippedCount)++;
        }
    }

    closedir(dir);
    return ok;
#endif
}

static bool configureStagedOutputDir(Options* options, const char* argv0) {
    if (options == NULL || options->outputDir == NULL || options->outputDir[0] == '\0') return false;
    if (!options->stageOutputLocally) return true;

    char programDir[1024];
    if (!getProgramDirPath(argv0, programDir, sizeof(programDir))) return false;

    snprintf(options->finalOutputDirStorage, sizeof(options->finalOutputDirStorage), "%s", options->outputDir);
#if N3DS_PREPROCESS_HOST_WINDOWS
    uint32_t pid = (uint32_t) GetCurrentProcessId();
#else
    uint32_t pid = (uint32_t) getpid();
#endif
    snprintf(options->stagingOutputDirStorage, sizeof(options->stagingOutputDirStorage), "%s/n3ds-preprocess-staging/run_%lu", programDir, (unsigned long) pid);
    if (directoryExists(options->stagingOutputDirStorage) && !deleteDirectoryRecursive(options->stagingOutputDirStorage)) {
        return false;
    }

    options->outputDir = options->stagingOutputDirStorage;
    return true;
}

static bool configureSpriteReplacementDir(Options* options, const char* argv0) {
    if (options == NULL) return false;

    if (options->spriteReplacementDir == NULL || options->spriteReplacementDir[0] == '\0') {
        bool foundDefaultDir = false;

#if defined(N3DS_PREPROCESS_TEXTURE_OVERRIDE_DIR)
        snprintf(
            options->spriteReplacementDirStorage,
            sizeof(options->spriteReplacementDirStorage),
            "%s",
            N3DS_PREPROCESS_TEXTURE_OVERRIDE_DIR
        );
        foundDefaultDir = directoryExists(options->spriteReplacementDirStorage);
#endif

        if (!foundDefaultDir) {
            char programDir[1024];
            if (options->toolDirStorage[0] != '\0') {
                snprintf(programDir, sizeof(programDir), "%s", options->toolDirStorage);
            } else if (!getProgramDirPath(argv0, programDir, sizeof(programDir))) {
                return false;
            }

            snprintf(
                options->spriteReplacementDirStorage,
                sizeof(options->spriteReplacementDirStorage),
                "%s/Sprite_replacements",
                programDir
            );
#if defined(N3DS_PREPROCESS_SOURCE_DIR)
            if (!directoryExists(options->spriteReplacementDirStorage)) {
                char sourceReplacementDir[1200];
                snprintf(
                    sourceReplacementDir,
                    sizeof(sourceReplacementDir),
                    "%s/Sprite_replacements",
                    N3DS_PREPROCESS_SOURCE_DIR
                );
                if (directoryExists(sourceReplacementDir)) {
                    snprintf(
                        options->spriteReplacementDirStorage,
                        sizeof(options->spriteReplacementDirStorage),
                        "%s",
                        sourceReplacementDir
                    );
                }
            }
#endif
        }
        options->spriteReplacementDir = options->spriteReplacementDirStorage;
    }

    options->spriteReplacementDirAvailable = directoryExists(options->spriteReplacementDir);
    if (options->spriteReplacementDirAvailable) {
        fprintf(stderr, "Using sprite replacements from: %s\n", options->spriteReplacementDir);
    }
    return true;
}

static bool configureBorderAssetDir(Options* options, const char* argv0) {
    if (options == NULL) return false;

    if (options->borderAssetDir == NULL || options->borderAssetDir[0] == '\0') {
        char programDir[1024];
        if (options->toolDirStorage[0] != '\0') {
            snprintf(programDir, sizeof(programDir), "%s", options->toolDirStorage);
        } else if (!getProgramDirPath(argv0, programDir, sizeof(programDir))) {
            return false;
        }

        snprintf(
            options->borderAssetDirStorage,
            sizeof(options->borderAssetDirStorage),
            "%s/Borders",
            programDir
        );
#if defined(N3DS_PREPROCESS_SOURCE_DIR)
        if (!directoryExists(options->borderAssetDirStorage)) {
            char sourceBorderDir[1200];
            snprintf(
                sourceBorderDir,
                sizeof(sourceBorderDir),
                "%s/Borders",
                N3DS_PREPROCESS_SOURCE_DIR
            );
            if (directoryExists(sourceBorderDir)) {
                snprintf(
                    options->borderAssetDirStorage,
                    sizeof(options->borderAssetDirStorage),
                    "%s",
                    sourceBorderDir
                );
            }
        }
#endif
        options->borderAssetDir = options->borderAssetDirStorage;
    }

    options->borderAssetDirAvailable = directoryExists(options->borderAssetDir);
    if (options->borderAssetDirAvailable) {
        fprintf(stderr, "Using border assets from: %s\n", options->borderAssetDir);
    }
    return true;
}

static bool syncStagedOutputToDestination(const Options* options) {
    if (options == NULL || !options->stageOutputLocally) return false;
    if (options->outputDir == NULL || options->outputDir[0] == '\0') return false;
    if (options->finalOutputDirStorage[0] == '\0') return false;

    if (!ensureOutputDirs(options->finalOutputDirStorage)) return false;

    uint32_t updatedCount = 0;
    uint32_t skippedCount = 0;
    if (!syncDirectoryTree(options->outputDir, options->finalOutputDirStorage, &updatedCount, &skippedCount)) {
        return false;
    }

    fprintf(
        stderr,
        "Synced staged assets to %s (updated=%u, unchanged=%u)\n",
        options->finalOutputDirStorage,
        updatedCount,
        skippedCount
    );
    return true;
}

static const char* getTex3dsFormatName(N3DSTextureFormat format) {
    switch (format) {
        case N3DS_TEXFMT_RGBA5551: return "rgba5551";
        case N3DS_TEXFMT_ETC1A4: return "etc1a4";
        case N3DS_TEXFMT_INDEXED8: return "rgba5551";
        case N3DS_TEXFMT_HYBRID: return "etc1a4";
        case N3DS_TEXFMT_L4: return "l4";
        case N3DS_TEXFMT_LA4: return "la4";
        default: return "etc1a4";
    }
}

static const char* getPageExtension(N3DSTextureFormat format) {
    return format == N3DS_TEXFMT_INDEXED8 ? "i8" : "t3x";
}

static const char* getTextureFormatLabel(N3DSTextureFormat format) {
    switch (format) {
        case N3DS_TEXFMT_RGBA5551: return "rgba5551";
        case N3DS_TEXFMT_ETC1A4: return "etc1a4";
        case N3DS_TEXFMT_INDEXED8: return "indexed8";
        case N3DS_TEXFMT_HYBRID: return "hybrid";
        case N3DS_TEXFMT_L4: return "l4";
        case N3DS_TEXFMT_LA4: return "la4";
        default: return "unknown";
    }
}

static bool tryParseTextureFormatLabel(const char* label, N3DSTextureFormat* outFormat) {
    if (label == NULL || outFormat == NULL) return false;
    if (strcmp(label, "rgba5551") == 0) { *outFormat = N3DS_TEXFMT_RGBA5551; return true; }
    if (strcmp(label, "etc1a4") == 0) { *outFormat = N3DS_TEXFMT_ETC1A4; return true; }
    if (strcmp(label, "indexed8") == 0) { *outFormat = N3DS_TEXFMT_INDEXED8; return true; }
    if (strcmp(label, "l4") == 0) { *outFormat = N3DS_TEXFMT_L4; return true; }
    if (strcmp(label, "la4") == 0) { *outFormat = N3DS_TEXFMT_LA4; return true; }
    if (strcmp(label, "hybrid") == 0) { *outFormat = N3DS_TEXFMT_HYBRID; return true; }
    return false;
}

static void sanitizeLabel(char* dst, size_t dstSize, const char* src) {
    if (dst == NULL || dstSize == 0) return;
    dst[0] = '\0';
    if (src == NULL || src[0] == '\0') {
        snprintf(dst, dstSize, "unnamed");
        return;
    }

    size_t written = 0;
    for (size_t i = 0; src[i] != '\0' && written + 1 < dstSize; ++i) {
        char c = src[i];
        bool alnum = (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9');
        if (alnum) {
            dst[written++] = c;
        } else if (written > 0 && dst[written - 1] != '_') {
            dst[written++] = '_';
        }
        if (written >= 40) break;
    }
    while (written > 0 && dst[written - 1] == '_') written--;
    if (written == 0) snprintf(dst, dstSize, "unnamed");
    else dst[written] = '\0';
}

static void appendDebugName(char* dst, size_t dstSize, const char* name) {
    if (dst == NULL || dstSize == 0 || name == NULL || name[0] == '\0') return;
    if (strstr(dst, name) != NULL) return;

    size_t len = strlen(dst);

    // FIX: If the buffer is already completely full, stop
    if (len >= dstSize - 1) return;

    if (len > 0) {
        snprintf(dst + len, dstSize - len, ", %s", name);
    } else {
        snprintf(dst, dstSize, "%s", name);
    }
}

static void appendPageDebugName(OutputPage* page, const char* name) {
    if (page == NULL || name == NULL || name[0] == '\0') return;
    appendDebugName(page->debugNames, sizeof(page->debugNames), name);
    if (page->previewLabel[0] == '\0') {
        sanitizeLabel(page->previewLabel, sizeof(page->previewLabel), name);
    }
}

static bool containsIgnoreCase(const char* haystack, const char* needle) {
    if (haystack == NULL || needle == NULL || needle[0] == '\0') return false;
    size_t haystackLen = strlen(haystack);
    size_t needleLen = strlen(needle);
    if (needleLen > haystackLen) return false;
    for (size_t i = 0; i + needleLen <= haystackLen; ++i) {
        size_t j = 0;
        while (j < needleLen) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
            if (a != b) break;
            ++j;
        }
        if (j == needleLen) return true;
    }
    return false;
}

static N3DSTextureFormat chooseHybridPageFormat(const PackedPage* packedPage) {
    if (packedPage == NULL || packedPage->pixels == NULL) return N3DS_TEXFMT_RGBA5551;

    ColorFreqMap* uniqueColors = NULL;
    uint32_t hardAlphaPixels = 0;
    uint64_t edgeEnergy = 0;
    uint32_t width = packedPage->width;
    uint32_t height = packedPage->height;

    repeat((size_t) width * (size_t) height, i) {
        const uint8_t* px = packedPage->pixels + (i * 4u);
        uint32_t colorKey =
            ((uint32_t) px[0] << 24) |
            ((uint32_t) px[1] << 16) |
            ((uint32_t) px[2] << 8) |
            (uint32_t) px[3];
        if (hmgeti(uniqueColors, colorKey) < 0) {
            hmput(uniqueColors, colorKey, 1u);
        }
        if (px[3] > 0u && px[3] < 255u) hardAlphaPixels++;
    }

    repeat(height, y) {
        repeat(width, x) {
            size_t idx = ((size_t) y * width + x) * 4u;
            const uint8_t* px = packedPage->pixels + idx;
            if (x + 1u < width) {
                const uint8_t* right = px + 4u;
                edgeEnergy += (uint64_t) abs((int) px[0] - (int) right[0]);
                edgeEnergy += (uint64_t) abs((int) px[1] - (int) right[1]);
                edgeEnergy += (uint64_t) abs((int) px[2] - (int) right[2]);
                edgeEnergy += (uint64_t) abs((int) px[3] - (int) right[3]);
            }
            if (y + 1u < height) {
                const uint8_t* down = packedPage->pixels + idx + ((size_t) width * 4u);
                edgeEnergy += (uint64_t) abs((int) px[0] - (int) down[0]);
                edgeEnergy += (uint64_t) abs((int) px[1] - (int) down[1]);
                edgeEnergy += (uint64_t) abs((int) px[2] - (int) down[2]);
                edgeEnergy += (uint64_t) abs((int) px[3] - (int) down[3]);
            }
        }
    }

    uint32_t uniqueCount = (uint32_t) hmlen(uniqueColors);
    hmfree(uniqueColors);

    uint64_t pixelCount = (uint64_t) width * (uint64_t) height;
    uint64_t avgEdgeEnergy = pixelCount > 0u ? edgeEnergy / pixelCount : 0u;

    if (hardAlphaPixels > (pixelCount / 64u)) return N3DS_TEXFMT_RGBA5551;
    if (uniqueCount <= 192u) return N3DS_TEXFMT_RGBA5551;
    if (avgEdgeEnergy >= 52u) return N3DS_TEXFMT_RGBA5551;
    return N3DS_TEXFMT_ETC1A4;
}

static bool spriteLooksLikeDialoguePortrait(const char* spriteName) {
    return containsIgnoreCase(spriteName, "face") ||
        containsIgnoreCase(spriteName, "portrait") ||
        containsIgnoreCase(spriteName, "mug") ||
        containsIgnoreCase(spriteName, "dialog") ||
        containsIgnoreCase(spriteName, "dial") ||
        containsIgnoreCase(spriteName, "textbox");
}

static bool* collectFontTPAGs(const DataWin* dataWin) {
    if (dataWin == NULL) return NULL;
    bool* fontTPAGs = safeCalloc(dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u, sizeof(bool));
    repeat(dataWin->font.count, i) {
        int32_t tpagIndex = dataWin->font.fonts[i].tpagIndex;
        if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
        fontTPAGs[tpagIndex] = true;
    }
    return fontTPAGs;
}

static bool* collectBackgroundTPAGs(const DataWin* dataWin) {
    if (dataWin == NULL) return NULL;
    bool* backgroundTPAGs = safeCalloc(dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u, sizeof(bool));
    repeat(dataWin->bgnd.count, i) {
        int32_t tpagIndex = dataWin->bgnd.backgrounds[i].tpagIndex;
        if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
        backgroundTPAGs[tpagIndex] = true;
    }
    return backgroundTPAGs;
}

static char** collectTPAGDebugNames(DataWin* dataWin) {
    if (dataWin == NULL) return NULL;
    char** names = safeCalloc(dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u, sizeof(char*));

    repeat(dataWin->sprt.count, i) {
        Sprite* sprite = &dataWin->sprt.sprites[i];
        repeat(sprite->textureCount, j) {
            int32_t tpagIndex = sprite->tpagIndices[j];
            if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
            if (names[tpagIndex] == NULL) names[tpagIndex] = safeCalloc(1u, 2048u);
            appendDebugName(names[tpagIndex], 2048u, sprite->name);
        }
    }

    repeat(dataWin->bgnd.count, i) {
        Background* bg = &dataWin->bgnd.backgrounds[i];
        if (bg->tpagIndex < 0 || (uint32_t) bg->tpagIndex >= dataWin->tpag.count) continue;
        if (names[bg->tpagIndex] == NULL) names[bg->tpagIndex] = safeCalloc(1u, 2048u);
        appendDebugName(names[bg->tpagIndex], 2048u, bg->name);
    }

    return names;
}

static void freeTPAGDebugNames(char** names, uint32_t count) {
    if (names == NULL) return;
    repeat(count, i) free(names[i]);
    free(names);
}

static int32_t* buildTPAGToSpriteIndexMap(const DataWin* dataWin) {
    if (dataWin == NULL) return NULL;
    size_t mapCount = dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u;
    int32_t* map = safeMalloc(mapCount * sizeof(int32_t));
    repeat(mapCount, i) map[i] = -1;

    repeat(dataWin->sprt.count, spriteIndex) {
        const Sprite* sprite = &dataWin->sprt.sprites[spriteIndex];
        repeat(sprite->textureCount, frameIndex) {
            int32_t tpagIndex = sprite->tpagIndices[frameIndex];
            if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
            if (map[tpagIndex] < 0) map[tpagIndex] = (int32_t) spriteIndex;
        }
    }
    return map;
}

static int32_t* buildTPAGToSpriteFrameMap(const DataWin* dataWin) {
    if (dataWin == NULL) return NULL;
    size_t mapCount = dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u;
    int32_t* map = safeMalloc(mapCount * sizeof(int32_t));
    repeat(mapCount, i) map[i] = -1;

    repeat(dataWin->sprt.count, spriteIndex) {
        const Sprite* sprite = &dataWin->sprt.sprites[spriteIndex];
        repeat(sprite->textureCount, frameIndex) {
            int32_t tpagIndex = sprite->tpagIndices[frameIndex];
            if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
            if (map[tpagIndex] < 0) map[tpagIndex] = (int32_t) frameIndex;
        }
    }
    return map;
}

static int32_t* buildTPAGToBackgroundIndexMap(const DataWin* dataWin) {
    if (dataWin == NULL) return NULL;
    size_t mapCount = dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u;
    int32_t* map = safeMalloc(mapCount * sizeof(int32_t));
    repeat(mapCount, i) map[i] = -1;

    repeat(dataWin->bgnd.count, bgIndex) {
        int32_t tpagIndex = dataWin->bgnd.backgrounds[bgIndex].tpagIndex;
        if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
        if (map[tpagIndex] < 0) map[tpagIndex] = (int32_t) bgIndex;
    }
    return map;
}

static int32_t* buildTPAGToFontIndexMap(const DataWin* dataWin) {
    if (dataWin == NULL) return NULL;
    size_t mapCount = dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u;
    int32_t* map = safeMalloc(mapCount * sizeof(int32_t));
    repeat(mapCount, i) map[i] = -1;

    repeat(dataWin->font.count, fontIndex) {
        int32_t tpagIndex = dataWin->font.fonts[fontIndex].tpagIndex;
        if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
        if (map[tpagIndex] < 0) map[tpagIndex] = (int32_t) fontIndex;
    }
    return map;
}

static int32_t* loadPageFormatOverrides(const char* path, uint32_t pageCount) {
    if (path == NULL || pageCount == 0) return NULL;
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "n3ds-preprocess: could not open page format overrides: %s\n", path);
        return NULL;
    }

    int32_t* overrides = safeMalloc(sizeof(int32_t) * pageCount);
    repeat(pageCount, i) overrides[i] = -1;

    char line[512];
    while (fgets(line, sizeof(line), file) != NULL) {
        char* cursor = line;
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (*cursor == '#' || *cursor == ';' || *cursor == '\r' || *cursor == '\n' || *cursor == '\0') continue;

        unsigned int pageIndex = 0;
        char formatName[64] = {0};
        if (sscanf(cursor, "page_%u = %63s", &pageIndex, formatName) != 2 &&
            sscanf(cursor, "page_%u:%63s", &pageIndex, formatName) != 2 &&
            sscanf(cursor, "%u = %63s", &pageIndex, formatName) != 2 &&
            sscanf(cursor, "%u:%63s", &pageIndex, formatName) != 2) {
            continue;
        }

        size_t formatLen = strlen(formatName);
        while (formatLen > 0 && (formatName[formatLen - 1] == '\r' || formatName[formatLen - 1] == '\n')) {
            formatName[--formatLen] = '\0';
        }

        N3DSTextureFormat parsedFormat;
        if (pageIndex < pageCount && tryParseTextureFormatLabel(formatName, &parsedFormat)) {
            overrides[pageIndex] = (int32_t) parsedFormat;
        }
    }

    fclose(file);
    return overrides;
}

static bool writePageFormatTemplate(const Options* options, const OutputPage* outputPages, uint32_t packedPageCount) {
    if (options == NULL || outputPages == NULL) return false;
    char templatePath[1024];
    snprintf(templatePath, sizeof(templatePath), "%s/gfx/page_formats.txt", options->outputDir);
    FILE* file = fopen(templatePath, "wb");
    if (file == NULL) return false;

    fprintf(file, "# Edit the format on the right and pass this file back with --page-format-overrides\n");
    fprintf(file, "# Supported formats: rgba5551, etc1a4, indexed8, l4, la4\n");
    repeat(packedPageCount, i) {
        const OutputPage* page = &outputPages[i];
        fprintf(
            file,
            "page_%03lu = %s ; %s\n",
            (unsigned long) i,
            getTextureFormatLabel((N3DSTextureFormat) page->textureFormat),
            page->debugNames[0] != '\0' ? page->debugNames : "unnamed"
        );
    }
    fclose(file);
    return true;
}

static bool pageCanUseMonoFormat(const OutputPage* page) {
    if (page == NULL) return false;
    return !page->containsFont && !page->containsBackground && !page->containsNonMonoContent;
}

static bool pageShouldForceRGBA5551(const OutputPage* page) {
    if (page == NULL) return false;
    if (page->containsSprite) return true;
    if (page->containsBackground) return true;
    const char* names = page->debugNames;
    if (names == NULL || names[0] == '\0') return false;

    return
        containsIgnoreCase(names, "spr_mainchara") ||
        containsIgnoreCase(names, "spr_f_mainchara") ||
        containsIgnoreCase(names, "spr_chara") ||
        containsIgnoreCase(names, "spr_heart") ||
        containsIgnoreCase(names, "heart_") ||
        containsIgnoreCase(names, "spr_soul") ||
        containsIgnoreCase(names, "soul");
}

static bool spriteShouldForceDirectRGBA5551(const char* spriteName) {
    if (spriteName == NULL || spriteName[0] == '\0') return false;
    return
        containsIgnoreCase(spriteName, "spr_mainchara") ||
        containsIgnoreCase(spriteName, "spr_f_mainchara") ||
        containsIgnoreCase(spriteName, "spr_chara") ||
        containsIgnoreCase(spriteName, "spr_heart") ||
        containsIgnoreCase(spriteName, "heart_") ||
        containsIgnoreCase(spriteName, "spr_soul") ||
        containsIgnoreCase(spriteName, "soul");
}

static bool objectLooksLikeBattleUI(const char* objectName) {
    return containsIgnoreCase(objectName, "battlecontroller") ||
        containsIgnoreCase(objectName, "writer") ||
        containsIgnoreCase(objectName, "border") ||
        containsIgnoreCase(objectName, "button") ||
        containsIgnoreCase(objectName, "blcon") ||
        containsIgnoreCase(objectName, "bullet") ||
        containsIgnoreCase(objectName, "soul") ||
        containsIgnoreCase(objectName, "heart") ||
        containsIgnoreCase(objectName, "menu") ||
        containsIgnoreCase(objectName, "target") ||
        containsIgnoreCase(objectName, "cursor") ||
        containsIgnoreCase(objectName, "slash");
}

static bool roomLooksLikeBattle(const Room* room, const DataWin* dataWin) {
    if (room == NULL || dataWin == NULL) return false;
    if (containsIgnoreCase(room->name, "battle")) return true;
    repeat(room->gameObjectCount, i) {
        int32_t objectIndex = room->gameObjects[i].objectDefinition;
        if (objectIndex < 0 || (uint32_t) objectIndex >= dataWin->objt.count) continue;
        const char* objectName = dataWin->objt.objects[objectIndex].name;
        if (containsIgnoreCase(objectName, "battlecontroller")) return true;
    }
    return false;
}

static void markSpriteTPAGs(const DataWin* dataWin, int32_t spriteIndex, bool* targetTPAGs) {
    if (dataWin == NULL || targetTPAGs == NULL) return;
    if (spriteIndex < 0 || (uint32_t) spriteIndex >= dataWin->sprt.count) return;
    Sprite* sprite = &dataWin->sprt.sprites[spriteIndex];
    repeat(sprite->textureCount, i) {
        int32_t tpagIndex = sprite->tpagIndices[i];
        if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
        targetTPAGs[tpagIndex] = true;
    }
}

static bool* collectTargetedDialogueBattleTPAGs(DataWin* dataWin) {
    if (dataWin == NULL) return NULL;
    bool* targetTPAGs = safeCalloc(dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u, sizeof(bool));

    repeat(dataWin->sprt.count, i) {
        if (spriteLooksLikeDialoguePortrait(dataWin->sprt.sprites[i].name)) {
            markSpriteTPAGs(dataWin, (int32_t) i, targetTPAGs);
        }
    }

    repeat(dataWin->room.count, i) {
        DataWin_loadRoomPayload(dataWin, (int32_t) i);
        Room* room = &dataWin->room.rooms[i];
        if (!roomLooksLikeBattle(room, dataWin)) continue;

        repeat(room->gameObjectCount, objIndex) {
            RoomGameObject* roomObject = &room->gameObjects[objIndex];
            if (roomObject->objectDefinition < 0 || (uint32_t) roomObject->objectDefinition >= dataWin->objt.count) continue;
            GameObject* object = &dataWin->objt.objects[roomObject->objectDefinition];
            if (object->spriteId < 0 || objectLooksLikeBattleUI(object->name)) continue;
            markSpriteTPAGs(dataWin, object->spriteId, targetTPAGs);
        }

        repeat(room->layerCount, layerIndex) {
            RoomLayer* layer = &room->layers[layerIndex];
            if (layer->assetsData == NULL) continue;
            repeat(layer->assetsData->spriteCount, spriteSlot) {
                int32_t spriteIndex = layer->assetsData->sprites[spriteSlot].spriteIndex;
                markSpriteTPAGs(dataWin, spriteIndex, targetTPAGs);
            }
        }
    }

    return targetTPAGs;
}

static N3DSTextureFormat chooseTargetedMonoItemFormat(
    const uint8_t* rgba,
    uint32_t stride,
    const TexturePageItem* item
) {
    if (rgba == NULL || item == NULL || item->sourceWidth == 0 || item->sourceHeight == 0) return N3DS_TEXFMT_RGBA5551;

    uint32_t visiblePixels = 0;
    uint32_t tintedPixels = 0;
    bool needsAlpha = false;

    repeat(item->sourceHeight, y) {
        repeat(item->sourceWidth, x) {
            const uint8_t* px = rgba + ((((size_t) item->sourceY + y) * stride + item->sourceX + x) * 4u);
            uint8_t alpha = px[3];
            if (alpha == 0u) {
                needsAlpha = true;
                continue;
            }
            visiblePixels++;
            if (alpha < 255u) needsAlpha = true;
            uint8_t maxRgb = px[0];
            if (px[1] > maxRgb) maxRgb = px[1];
            if (px[2] > maxRgb) maxRgb = px[2];
            uint8_t minRgb = px[0];
            if (minRgb > px[1]) minRgb = px[1];
            if (minRgb > px[2]) minRgb = px[2];
            if ((uint8_t) (maxRgb - minRgb) > 18u) tintedPixels++;
        }
    }

    if (visiblePixels == 0u) return N3DS_TEXFMT_RGBA5551;
    if (tintedPixels > (visiblePixels / 20u)) return N3DS_TEXFMT_RGBA5551;
    return needsAlpha ? N3DS_TEXFMT_LA4 : N3DS_TEXFMT_L4;
}

static bool pixelsLookMonochrome(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (rgba == NULL || width == 0 || height == 0) return false;

    uint32_t visiblePixels = 0;
    uint32_t tintedPixels = 0;

    repeat(height, y) {
        repeat(width, x) {
            const uint8_t* px = rgba + ((((size_t) y * width) + x) * 4u);
            if (px[3] == 0u) continue;

            visiblePixels++;

            uint8_t maxRgb = px[0];
            if (px[1] > maxRgb) maxRgb = px[1];
            if (px[2] > maxRgb) maxRgb = px[2];

            uint8_t minRgb = px[0];
            if (minRgb > px[1]) minRgb = px[1];
            if (minRgb > px[2]) minRgb = px[2];

            if ((uint8_t) (maxRgb - minRgb) > 18u) tintedPixels++;
        }
    }

    if (visiblePixels == 0u) return false;
    return tintedPixels <= (visiblePixels / 20u);
}

static bool runTex3ds(const char* tex3dsExe, const char* pngPath, const char* t3xPath, N3DSTextureFormat format) {
#if N3DS_PREPROCESS_HOST_WINDOWS
    char tex3dsExeNormalized[1024];
    char pngPathNormalized[1024];
    char t3xPathNormalized[1024];
    char command[4096];

    snprintf(tex3dsExeNormalized, sizeof(tex3dsExeNormalized), "%s", tex3dsExe);
    snprintf(pngPathNormalized, sizeof(pngPathNormalized), "%s", pngPath);
    snprintf(t3xPathNormalized, sizeof(t3xPathNormalized), "%s", t3xPath);
    for (char* cursor = tex3dsExeNormalized; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') *cursor = '\\';
    }
    for (char* cursor = pngPathNormalized; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') *cursor = '\\';
    }
    for (char* cursor = t3xPathNormalized; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') *cursor = '\\';
    }

    snprintf(
        command,
        sizeof(command),
        "\"%s\" -f %s -z none -o \"%s\" \"%s\"",
        tex3dsExeNormalized,
        getTex3dsFormatName(format),
        t3xPathNormalized,
        pngPathNormalized
    );
    intptr_t rc = spawnl(
        _P_WAIT,
        tex3dsExeNormalized,
        tex3dsExeNormalized,
        "-f",
        getTex3dsFormatName(format),
        "-z",
        "none",
        "-o",
        t3xPathNormalized,
        pngPathNormalized,
        NULL
    );
    if (rc != 0) {
        fprintf(stderr, "tex3ds failed (%ld): %s\n", (long) rc, command);
        return false;
    }
    return true;
#else
    char command[4096];
    snprintf(
        command,
        sizeof(command),
        "\"%s\" -f %s -z none -o \"%s\" \"%s\"",
        tex3dsExe,
        getTex3dsFormatName(format),
        t3xPath,
        pngPath
    );
    int rc = system(command);
    if (rc != 0) {
        fprintf(stderr, "tex3ds failed (%d): %s\n", rc, command);
        return false;
    }
    return true;
#endif
}

static bool runTex3dsAtlas(const char* tex3dsExe, const char* pngGlobPath, const char* t3xPath, N3DSTextureFormat format) {
#if N3DS_PREPROCESS_HOST_WINDOWS
    char tex3dsExeNormalized[1024];
    char pngGlobPathNormalized[1024];
    char t3xPathNormalized[1024];
    char command[4096];

    snprintf(tex3dsExeNormalized, sizeof(tex3dsExeNormalized), "%s", tex3dsExe);
    snprintf(pngGlobPathNormalized, sizeof(pngGlobPathNormalized), "%s", pngGlobPath);
    snprintf(t3xPathNormalized, sizeof(t3xPathNormalized), "%s", t3xPath);
    for (char* cursor = tex3dsExeNormalized; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') *cursor = '\\';
    }
    for (char* cursor = pngGlobPathNormalized; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') *cursor = '\\';
    }
    for (char* cursor = t3xPathNormalized; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') *cursor = '\\';
    }

    snprintf(
        command,
        sizeof(command),
        "\"%s\" --atlas -f %s -z none -o \"%s\" \"%s\"",
        tex3dsExeNormalized,
        getTex3dsFormatName(format),
        t3xPathNormalized,
        pngGlobPathNormalized
    );
    intptr_t rc = spawnl(
        _P_WAIT,
        tex3dsExeNormalized,
        tex3dsExeNormalized,
        "--atlas",
        "-f",
        getTex3dsFormatName(format),
        "-z",
        "none",
        "-o",
        t3xPathNormalized,
        pngGlobPathNormalized,
        NULL
    );
    if (rc != 0) {
        fprintf(stderr, "tex3ds atlas failed (%ld): %s\n", (long) rc, command);
        return false;
    }
    return true;
#else
    char command[4096];
    snprintf(
        command,
        sizeof(command),
        "\"%s\" --atlas -f %s -z none -o \"%s\" \"%s\"",
        tex3dsExe,
        getTex3dsFormatName(format),
        t3xPath,
        pngGlobPath
    );
    int rc = system(command);
    if (rc != 0) {
        fprintf(stderr, "tex3ds atlas failed (%d): %s\n", rc, command);
        return false;
    }
    return true;
#endif
}

static uint16_t rgbaToRgb5551(const uint8_t* rgba) {
    uint16_t r = (uint16_t) (rgba[0] >> 3);
    uint16_t g = (uint16_t) (rgba[1] >> 3);
    uint16_t b = (uint16_t) (rgba[2] >> 3);
    uint16_t a = rgba[3] >= 128 ? 1u : 0u;
    return (uint16_t) ((r << 11) | (g << 6) | (b << 1) | a);
}

static int compareColorFreqDesc(const void* lhs, const void* rhs) {
    const ColorFreqMap* a = (const ColorFreqMap*) lhs;
    const ColorFreqMap* b = (const ColorFreqMap*) rhs;
    if (a->value < b->value) return 1;
    if (a->value > b->value) return -1;
    if (a->key < b->key) return -1;
    if (a->key > b->key) return 1;
    return 0;
}

static int colorDistanceRgb5551(uint16_t a, uint16_t b) {
    int ar = (a >> 11) & 0x1F;
    int ag = (a >> 6) & 0x1F;
    int ab = (a >> 1) & 0x1F;
    int aa = a & 0x1;
    int br = (b >> 11) & 0x1F;
    int bg = (b >> 6) & 0x1F;
    int bb = (b >> 1) & 0x1F;
    int ba = b & 0x1;
    int dr = ar - br;
    int dg = ag - bg;
    int db = ab - bb;
    int da = aa - ba;
    return (dr * dr * 3) + (dg * dg * 4) + (db * db * 2) + (da * da * 2048);
}

static uint8_t findNearestPaletteIndex(uint16_t color, const uint16_t* palette, uint32_t paletteCount) {
    uint32_t bestIndex = 0;
    int bestDistance = INT32_MAX;
    repeat(paletteCount, i) {
        int distance = colorDistanceRgb5551(color, palette[i]);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = (uint32_t) i;
            if (distance == 0) break;
        }
    }
    return (uint8_t) bestIndex;
}

static bool writeIndexedPage(const char* outputPath, const PackedPage* packedPage) {
    if (outputPath == NULL || packedPage == NULL || packedPage->pixels == NULL) return false;

    size_t pixelCount = (size_t) packedPage->width * (size_t) packedPage->height;
    ColorFreqMap* colorFreqs = NULL;
    bool hasTransparent = false;

    repeat(pixelCount, i) {
        const uint8_t* px = packedPage->pixels + (i * 4u);
        uint16_t color = rgbaToRgb5551(px);
        if ((color & 0x1u) == 0u) hasTransparent = true;
        ptrdiff_t existing = hmgeti(colorFreqs, color);
        if (existing >= 0) colorFreqs[existing].value++;
        else hmput(colorFreqs, color, 1u);
    }

    size_t freqCount = arrlen(colorFreqs);
    qsort(colorFreqs, freqCount, sizeof(ColorFreqMap), compareColorFreqDesc);

    uint16_t palette[256] = {0};
    uint32_t paletteCount = 0;
    if (hasTransparent) palette[paletteCount++] = 0u;

    repeat(freqCount, i) {
        uint16_t color = (uint16_t) colorFreqs[i].key;
        if (hasTransparent && color == 0u) continue;
        if (paletteCount >= 256u) break;
        palette[paletteCount++] = color;
    }
    if (paletteCount == 0u) palette[paletteCount++] = 0u;

    size_t blobSize = 256u * sizeof(uint16_t) + pixelCount;
    uint8_t* blob = safeMalloc(blobSize);
    memset(blob, 0, blobSize);
    repeat(256u, i) {
        uint16_t color = palette[i];
        blob[i * 2u + 0u] = (uint8_t) (color & 0xFFu);
        blob[i * 2u + 1u] = (uint8_t) ((color >> 8) & 0xFFu);
    }

    uint8_t* indices = blob + (256u * sizeof(uint16_t));
    repeat(pixelCount, i) {
        const uint8_t* px = packedPage->pixels + (i * 4u);
        uint16_t color = rgbaToRgb5551(px);
        indices[i] = findNearestPaletteIndex(color, palette, paletteCount);
    }

    FILE* file = fopen(outputPath, "wb");
    if (file == NULL) {
        fprintf(stderr, "Failed to open indexed page output: %s\n", outputPath);
        hmfree(colorFreqs);
        free(blob);
        return false;
    }

    fwrite(blob, 1, blobSize, file);
    fclose(file);
    hmfree(colorFreqs);
    free(blob);
    return true;
}

static bool getFileSize32(const char* path, uint32_t* outSize) {
    if (path == NULL || outSize == NULL) return false;

    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fclose(file);
    if (size < 0) return false;

    *outSize = (uint32_t) size;
    return true;
}

static bool appendFileToStream(FILE* dst, const char* srcPath) {
    if (dst == NULL || srcPath == NULL) return false;

    FILE* src = fopen(srcPath, "rb");
    if (src == NULL) return false;

    uint8_t* buffer = safeMalloc(64u * 1024u);
    bool ok = true;
    while (ok) {
        size_t bytesRead = fread(buffer, 1, 64u * 1024u, src);
        if (bytesRead > 0 && fwrite(buffer, 1, bytesRead, dst) != bytesRead) ok = false;
        if (bytesRead < 64u * 1024u) {
            if (ferror(src)) ok = false;
            break;
        }
    }

    free(buffer);
    fclose(src);
    return ok;
}

static bool directAssetPackContainsPath(DirectAssetPackEntry* entries, const char* relativePath) {
    if (relativePath == NULL) return false;
    size_t entryCount = arrlenu(entries);
    repeat(entryCount, i) {
        if (strcmp(entries[i].relativePath, relativePath) == 0) return true;
    }
    return false;
}

static bool appendDirectAssetPackEntry(const Options* options, const char* relativePath, DirectAssetPackEntry** entries) {
    if (options == NULL || relativePath == NULL || entries == NULL) return false;
    if (directAssetPackContainsPath(*entries, relativePath)) return true;

    DirectAssetPackEntry entry = {0};
    snprintf(entry.relativePath, sizeof(entry.relativePath), "%s", relativePath);
    snprintf(entry.sourcePath, sizeof(entry.sourcePath), "%s/gfx/%s", options->outputDir, relativePath);
    if (!fileExists(entry.sourcePath)) return true;
    if (!getFileSize32(entry.sourcePath, &entry.dataSize)) {
        fprintf(stderr, "Failed to get direct asset size: %s\n", entry.sourcePath);
        return false;
    }

    arrput(*entries, entry);
    return true;
}

static bool collectDirectAssetPackEntries(const Options* options, const DataWin* dataWin, DirectAssetPackEntry** outEntries) {
    if (options == NULL || dataWin == NULL || outEntries == NULL) return false;

    DirectAssetPackEntry* entries = NULL;
    char relativePath[320];

    repeat(dataWin->sprt.count, spriteIndex) {
        const Sprite* sprite = &dataWin->sprt.sprites[spriteIndex];
        if (sprite->textureCount <= 0) continue;

        snprintf(relativePath, sizeof(relativePath), "sprites/spr_%05lu.t3x", (unsigned long) spriteIndex);
        if (!appendDirectAssetPackEntry(options, relativePath, &entries)) {
            arrfree(entries);
            return false;
        }

        repeat(sprite->textureCount, frameIndex) {
            snprintf(
                relativePath,
                sizeof(relativePath),
                "sprites/spr_%05lu_frame_%05zu.t3x",
                (unsigned long) spriteIndex,
                frameIndex
            );
            if (!appendDirectAssetPackEntry(options, relativePath, &entries)) {
                arrfree(entries);
                return false;
            }
        }
    }

    repeat(dataWin->bgnd.count, bgIndex) {
        snprintf(relativePath, sizeof(relativePath), "backgrounds/bg_%05lu.t3x", (unsigned long) bgIndex);
        if (!appendDirectAssetPackEntry(options, relativePath, &entries)) {
            arrfree(entries);
            return false;
        }
    }

    repeat(dataWin->font.count, fontIndex) {
        snprintf(relativePath, sizeof(relativePath), "fonts/font_%05lu.t3x", (unsigned long) fontIndex);
        if (!appendDirectAssetPackEntry(options, relativePath, &entries)) {
            arrfree(entries);
            return false;
        }
    }

    *outEntries = entries;
    return true;
}

static bool writePackedDirectTextureAssets(const Options* options, const DataWin* dataWin) {
    if (options == NULL || dataWin == NULL) return false;

    DirectAssetPackEntry* entries = NULL;
    if (!collectDirectAssetPackEntries(options, dataWin, &entries)) {
        return false;
    }

    char outputPath[1024];
    snprintf(outputPath, sizeof(outputPath), "%s/gfx/direct_assets.bin", options->outputDir);

    uint32_t entryCount = (uint32_t) arrlen(entries);
    if (entryCount == 0u) {
        remove(outputPath);
        arrfree(entries);
        return true;
    }

    uint32_t stringTableSize = 0u;
    repeat(entryCount, i) {
        entries[i].pathOffset = stringTableSize;
        stringTableSize += (uint32_t) strlen(entries[i].relativePath) + 1u;
    }

    uint32_t metadataSize = 16u + entryCount * 16u + stringTableSize;
    uint32_t dataOffset = metadataSize;
    repeat(entryCount, i) {
        entries[i].dataOffset = dataOffset;
        dataOffset += entries[i].dataSize;
    }

    FILE* file = fopen(outputPath, "wb");
    if (file == NULL) {
        fprintf(stderr, "Failed to open packed direct asset output: %s\n", outputPath);
        arrfree(entries);
        return false;
    }

    uint8_t header[16];
    writeLe32(header + 0u, N3DS_DIRECT_ASSET_MAGIC);
    writeLe32(header + 4u, N3DS_DIRECT_ASSET_VERSION);
    writeLe32(header + 8u, entryCount);
    writeLe32(header + 12u, stringTableSize);
    bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);

    repeat(entryCount, i) {
        if (!ok) break;
        uint8_t row[16];
        writeLe32(row + 0u, entries[i].pathOffset);
        writeLe32(row + 4u, entries[i].dataOffset);
        writeLe32(row + 8u, entries[i].dataSize);
        writeLe32(row + 12u, 0u);
        ok = fwrite(row, 1, sizeof(row), file) == sizeof(row);
    }

    repeat(entryCount, i) {
        if (!ok) break;
        size_t pathBytes = strlen(entries[i].relativePath) + 1u;
        ok = fwrite(entries[i].relativePath, 1, pathBytes, file) == pathBytes;
    }

    repeat(entryCount, i) {
        if (!ok) break;
        ok = appendFileToStream(file, entries[i].sourcePath);
    }

    fclose(file);

    if (!ok) {
        fprintf(stderr, "Failed to write packed direct asset file: %s\n", outputPath);
        arrfree(entries);
        return false;
    }

    repeat(entryCount, i) {
        if (!fileExists(entries[i].sourcePath)) continue;
        if (remove(entries[i].sourcePath) != 0) {
            fprintf(stderr, "Warning: failed to remove packed direct source file: %s\n", entries[i].sourcePath);
        }
    }

    arrfree(entries);
    fprintf(stderr, "n3ds-preprocess: packed %u direct texture assets into gfx/direct_assets.bin\n", entryCount);
    return true;
}

static bool writeAtlasFile(
    const char* outputDir,
    uint32_t pageCount,
    const OutputPage* pages,
    uint32_t itemCount,
    const OutputItem* items,
    uint32_t fragmentCount,
    const OutputFragment* fragments,
    uint32_t tileEntryCount,
    const OutputTileEntry* tileEntries,
    N3DSTextureFormat textureFormat
) {
    char atlasPath[1024];
    snprintf(atlasPath, sizeof(atlasPath), "%s/gfx/atlas.bin", outputDir);

    uint32_t* pageDataOffsets = safeCalloc(pageCount > 0 ? pageCount : 1u, sizeof(uint32_t));
    uint32_t* pageDataSizes = safeCalloc(pageCount > 0 ? pageCount : 1u, sizeof(uint32_t));
    uint16_t version = N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED;
    size_t metadataSize =
        24u +
        ((size_t) pageCount * 16u) +
        ((size_t) itemCount * 10u) +
        ((size_t) fragmentCount * 14u) +
        ((size_t) tileEntryCount * 26u);
    uint32_t currentOffset = (uint32_t) metadataSize;

    repeat(pageCount, i) {
        char pagePath[1024];
        snprintf(pagePath, sizeof(pagePath), "%s/gfx/%s", outputDir, pages[i].path);
        if (!getFileSize32(pagePath, &pageDataSizes[i])) {
            fprintf(stderr, "Failed to determine atlas page size: %s\n", pagePath);
            free(pageDataOffsets);
            free(pageDataSizes);
            return false;
        }
        pageDataOffsets[i] = currentOffset;
        currentOffset += pageDataSizes[i];
    }

    FILE* file = fopen(atlasPath, "wb");
    if (file == NULL) {
        fprintf(stderr, "Failed to open atlas output: %s\n", atlasPath);
        free(pageDataOffsets);
        free(pageDataSizes);
        return false;
    }

    uint32_t magic = N3DS_ATLAS_MAGIC;
    uint16_t pageCount16 = (uint16_t) pageCount;
    uint32_t textureFormat32 = (uint32_t) textureFormat;

    fwrite(&magic, sizeof(magic), 1, file);
    fwrite(&version, sizeof(version), 1, file);
    fwrite(&pageCount16, sizeof(pageCount16), 1, file);
    fwrite(&itemCount, sizeof(itemCount), 1, file);
    fwrite(&fragmentCount, sizeof(fragmentCount), 1, file);
    fwrite(&tileEntryCount, sizeof(tileEntryCount), 1, file);
    fwrite(&textureFormat32, sizeof(textureFormat32), 1, file);

    repeat(pageCount, i) {
        fwrite(&pages[i].width, sizeof(uint16_t), 1, file);
        fwrite(&pages[i].height, sizeof(uint16_t), 1, file);
        fwrite(&pages[i].textureFormat, sizeof(uint32_t), 1, file);
        fwrite(&pageDataOffsets[i], sizeof(uint32_t), 1, file);
        fwrite(&pageDataSizes[i], sizeof(uint32_t), 1, file);
    }

    repeat(itemCount, i) {
        fwrite(&items[i].width, sizeof(uint16_t), 1, file);
        fwrite(&items[i].height, sizeof(uint16_t), 1, file);
        fwrite(&items[i].fragmentStart, sizeof(uint32_t), 1, file);
        fwrite(&items[i].fragmentCount, sizeof(uint16_t), 1, file);
    }

    repeat(fragmentCount, i) {
        fwrite(&fragments[i].atlasId, sizeof(uint16_t), 1, file);
        fwrite(&fragments[i].x, sizeof(uint16_t), 1, file);
        fwrite(&fragments[i].y, sizeof(uint16_t), 1, file);
        fwrite(&fragments[i].width, sizeof(uint16_t), 1, file);
        fwrite(&fragments[i].height, sizeof(uint16_t), 1, file);
        fwrite(&fragments[i].sourceX, sizeof(uint16_t), 1, file);
        fwrite(&fragments[i].sourceY, sizeof(uint16_t), 1, file);
    }

    repeat(tileEntryCount, i) {
        fwrite(&tileEntries[i].bgDef, sizeof(int16_t), 1, file);
        fwrite(&tileEntries[i].srcX, sizeof(uint16_t), 1, file);
        fwrite(&tileEntries[i].srcY, sizeof(uint16_t), 1, file);
        fwrite(&tileEntries[i].srcW, sizeof(uint16_t), 1, file);
        fwrite(&tileEntries[i].srcH, sizeof(uint16_t), 1, file);
        fwrite(&tileEntries[i].atlasId, sizeof(uint16_t), 1, file);
        fwrite(&tileEntries[i].x, sizeof(uint16_t), 1, file);
        fwrite(&tileEntries[i].y, sizeof(uint16_t), 1, file);
        fwrite(&tileEntries[i].width, sizeof(uint16_t), 1, file);
        fwrite(&tileEntries[i].height, sizeof(uint16_t), 1, file);
        fwrite(&tileEntries[i].fragmentStart, sizeof(uint32_t), 1, file);
        fwrite(&tileEntries[i].fragmentCount, sizeof(uint16_t), 1, file);
    }

    repeat(pageCount, i) {
        char pagePath[1024];
        snprintf(pagePath, sizeof(pagePath), "%s/gfx/%s", outputDir, pages[i].path);
        if (!appendFileToStream(file, pagePath)) {
            fprintf(stderr, "Failed to append packed atlas page: %s\n", pagePath);
            fclose(file);
            free(pageDataOffsets);
            free(pageDataSizes);
            return false;
        }
    }

    fclose(file);
    free(pageDataOffsets);
    free(pageDataSizes);
    return true;
}

static PackedPage* createPackedPage(MAYBE_UNUSED const Options* options, OutputPage** pages, uint32_t* totalPageCount, uint32_t pageSize, N3DSTextureFormat pageFormat) {
    *pages = safeRealloc(*pages, (*totalPageCount + 1u) * sizeof(OutputPage));
    OutputPage* outputPage = &(*pages)[*totalPageCount];
    outputPage->width = (uint16_t) pageSize;
    outputPage->height = (uint16_t) pageSize;
    outputPage->textureFormat = (uint32_t) pageFormat;
    const char* extension = getPageExtension(pageFormat);
    snprintf(outputPage->path, sizeof(outputPage->path), "page_%03u.%s", *totalPageCount, extension);
    outputPage->containsFont = false;
    outputPage->containsBackground = false;
    outputPage->containsNonMonoContent = false;

    PackedPage* packedPage = safeCalloc(1, sizeof(PackedPage));
    packedPage->pageIndex = *totalPageCount;
    packedPage->width = pageSize;
    packedPage->height = pageSize;
    packedPage->pixels = safeCalloc((size_t) pageSize * pageSize, 4);
    (*totalPageCount)++;
    return packedPage;
}

static uint32_t choosePageSize(const Options* options, uint32_t width, uint32_t height) {
    if (options != NULL) {
        if (options->atlasPageMode == N3DS_ATLAS_PAGE_MODE_FORCE_256) return N3DS_SMALL_TEXTURE_SIZE;
        if (options->atlasPageMode == N3DS_ATLAS_PAGE_MODE_FORCE_512) return N3DS_LARGE_TEXTURE_SIZE;
    }
    return (width <= N3DS_SMALL_TEXTURE_SIZE && height <= N3DS_SMALL_TEXTURE_SIZE)
        ? N3DS_SMALL_TEXTURE_SIZE
        : N3DS_LARGE_TEXTURE_SIZE;
}

static bool packRect(const Options* options, PackedPage*** packedPages, uint32_t* packedPageCount, OutputPage** outputPages, uint32_t* totalPageCount, uint32_t width, uint32_t height, N3DSTextureFormat pageFormat, uint16_t* outPageIndex, uint16_t* outX, uint16_t* outY) {
    if (width > N3DS_LARGE_TEXTURE_SIZE || height > N3DS_LARGE_TEXTURE_SIZE) return false;
    uint32_t pageSize = choosePageSize(options, width, height);

    repeat(*packedPageCount, i) {
        PackedPage* page = (*packedPages)[i];
        if (page->width != pageSize || page->height != pageSize) continue;
        if ((N3DSTextureFormat) (*outputPages)[page->pageIndex].textureFormat != pageFormat) continue;
        if (page->cursorX + width > page->width) {
            page->cursorX = 0;
            page->cursorY += page->rowHeight;
            page->rowHeight = 0;
        }
        if (page->cursorY + height > page->height) continue;

        *outPageIndex = (uint16_t) page->pageIndex;
        *outX = (uint16_t) page->cursorX;
        *outY = (uint16_t) page->cursorY;
        page->cursorX += width;
        if (height > page->rowHeight) page->rowHeight = height;
        return true;
    }

    *packedPages = safeRealloc(*packedPages, (*packedPageCount + 1u) * sizeof(PackedPage*));
    PackedPage* page = createPackedPage(options, outputPages, totalPageCount, pageSize, pageFormat);
    (*packedPages)[(*packedPageCount)++] = page;

    *outPageIndex = (uint16_t) page->pageIndex;
    *outX = 0;
    *outY = 0;
    page->cursorX = width;
    page->rowHeight = height;
    return true;
}

static void blitRect(uint8_t* dst, uint32_t dstStride, uint32_t dstX, uint32_t dstY, const uint8_t* src, uint32_t srcStride, uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height) {
    repeat(height, row) {
        memcpy(
            dst + (((size_t) dstY + row) * dstStride + dstX) * 4u,
            src + (((size_t) srcY + row) * srcStride + srcX) * 4u,
            (size_t) width * 4u
        );
    }
}

static void sanitizeSpriteReplacementStem(const char* value, char* out, size_t outSize) {
    if (out == NULL || outSize == 0) return;
    out[0] = '\0';
    if (value == NULL || value[0] == '\0') return;

    size_t writeIndex = 0;
    for (size_t i = 0; value[i] != '\0' && writeIndex + 1 < outSize; ++i) {
        unsigned char c = (unsigned char) value[i];
        if (isalnum(c) || c == '_' || c == '-' || c == '.') {
            out[writeIndex++] = (char) c;
        } else {
            out[writeIndex++] = '_';
        }
    }
    out[writeIndex] = '\0';
}

static bool tryLoadSpriteReplacementCandidate(
    const Options* options,
    const char* filename,
    const Sprite* sprite,
    uint32_t spriteIndex,
    uint32_t frameIndex,
    uint32_t expectedWidth,
    uint32_t expectedHeight,
    uint8_t* outPixels
) {
    if (options == NULL || filename == NULL || outPixels == NULL) return false;
    if (options->spriteReplacementDir == NULL || options->spriteReplacementDir[0] == '\0') return false;

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", options->spriteReplacementDir, filename);
    if (!fileExists(path)) return false;

    int width = 0;
    int height = 0;
    int channels = 0;
    uint8_t* pixels = stbi_load(path, &width, &height, &channels, 4);
    if (pixels == NULL) {
        fprintf(stderr, "Warning: failed to load sprite replacement PNG: %s\n", path);
        return false;
    }

    if (width != (int) expectedWidth || height != (int) expectedHeight) {
        fprintf(
            stderr,
            "Warning: ignoring sprite replacement %s for sprite %05lu/%05lu (%s): expected %lux%lu, got %dx%d\n",
            path,
            (unsigned long) spriteIndex,
            (unsigned long) frameIndex,
            sprite != NULL && sprite->name != NULL ? sprite->name : "<unnamed>",
            (unsigned long) expectedWidth,
            (unsigned long) expectedHeight,
            width,
            height
        );
        stbi_image_free(pixels);
        return false;
    }

    memcpy(outPixels, pixels, (size_t) expectedWidth * expectedHeight * 4u);
    stbi_image_free(pixels);
    fprintf(
        stderr,
        "n3ds-preprocess: sprite replacement %05lu/%05lu <- %s\n",
        (unsigned long) spriteIndex,
        (unsigned long) frameIndex,
        path
    );
    return true;
}

static bool tryApplySpriteReplacement(
    const Options* options,
    const Sprite* sprite,
    uint32_t spriteIndex,
    uint32_t frameIndex,
    uint32_t expectedWidth,
    uint32_t expectedHeight,
    uint8_t* outPixels
) {
    if (options == NULL || sprite == NULL || outPixels == NULL) return false;
    if (!options->spriteReplacementDirAvailable) return false;

    char filename[320];
    char spriteStem[192];
    sanitizeSpriteReplacementStem(sprite->name, spriteStem, sizeof(spriteStem));

    if (spriteStem[0] != '\0') {
        snprintf(filename, sizeof(filename), "%s_frame_%lu.png", spriteStem, (unsigned long) frameIndex);
        if (tryLoadSpriteReplacementCandidate(options, filename, sprite, spriteIndex, frameIndex, expectedWidth, expectedHeight, outPixels)) return true;

        snprintf(filename, sizeof(filename), "%s_frame_%05lu.png", spriteStem, (unsigned long) frameIndex);
        if (tryLoadSpriteReplacementCandidate(options, filename, sprite, spriteIndex, frameIndex, expectedWidth, expectedHeight, outPixels)) return true;

        snprintf(filename, sizeof(filename), "%s_%lu.png", spriteStem, (unsigned long) frameIndex);
        if (tryLoadSpriteReplacementCandidate(options, filename, sprite, spriteIndex, frameIndex, expectedWidth, expectedHeight, outPixels)) return true;

        snprintf(filename, sizeof(filename), "%s_%05lu.png", spriteStem, (unsigned long) frameIndex);
        if (tryLoadSpriteReplacementCandidate(options, filename, sprite, spriteIndex, frameIndex, expectedWidth, expectedHeight, outPixels)) return true;

        if (sprite->textureCount <= 1) {
            snprintf(filename, sizeof(filename), "%s.png", spriteStem);
            if (tryLoadSpriteReplacementCandidate(options, filename, sprite, spriteIndex, frameIndex, expectedWidth, expectedHeight, outPixels)) return true;
        }
    }

    snprintf(filename, sizeof(filename), "spr_%05lu_frame_%lu.png", (unsigned long) spriteIndex, (unsigned long) frameIndex);
    if (tryLoadSpriteReplacementCandidate(options, filename, sprite, spriteIndex, frameIndex, expectedWidth, expectedHeight, outPixels)) return true;

    snprintf(filename, sizeof(filename), "spr_%05lu_frame_%05lu.png", (unsigned long) spriteIndex, (unsigned long) frameIndex);
    if (tryLoadSpriteReplacementCandidate(options, filename, sprite, spriteIndex, frameIndex, expectedWidth, expectedHeight, outPixels)) return true;

    snprintf(filename, sizeof(filename), "spr_%05lu_%lu.png", (unsigned long) spriteIndex, (unsigned long) frameIndex);
    if (tryLoadSpriteReplacementCandidate(options, filename, sprite, spriteIndex, frameIndex, expectedWidth, expectedHeight, outPixels)) return true;

    snprintf(filename, sizeof(filename), "spr_%05lu_%05lu.png", (unsigned long) spriteIndex, (unsigned long) frameIndex);
    if (tryLoadSpriteReplacementCandidate(options, filename, sprite, spriteIndex, frameIndex, expectedWidth, expectedHeight, outPixels)) return true;

    if (sprite->textureCount <= 1) {
        snprintf(filename, sizeof(filename), "spr_%05lu.png", (unsigned long) spriteIndex);
        if (tryLoadSpriteReplacementCandidate(options, filename, sprite, spriteIndex, frameIndex, expectedWidth, expectedHeight, outPixels)) return true;
    }

    return false;
}

static bool filenameHasPngExtension(const char* filename) {
    if (filename == NULL) return false;
    size_t len = strlen(filename);
    if (len < 5) return false;
    const char* ext = filename + len - 4;
    return tolower((unsigned char) ext[0]) == '.' &&
        tolower((unsigned char) ext[1]) == 'p' &&
        tolower((unsigned char) ext[2]) == 'n' &&
        tolower((unsigned char) ext[3]) == 'g';
}

static bool copyPngStem(char* out, size_t outSize, const char* filename) {
    if (out == NULL || outSize == 0 || !filenameHasPngExtension(filename)) return false;
    size_t stemLen = strlen(filename) - 4u;
    if (stemLen == 0 || stemLen >= outSize) return false;
    memcpy(out, filename, stemLen);
    out[stemLen] = '\0';
    return true;
}

static bool convertBorderAssetPng(const Options* options, const char* filename, uint32_t* convertedCount) {
    if (options == NULL || filename == NULL) return false;
    if (!filenameHasPngExtension(filename)) return true;

    char stem[256];
    if (!copyPngStem(stem, sizeof(stem), filename)) return true;

    char inputPath[1400];
    char outputPath[1400];
    snprintf(inputPath, sizeof(inputPath), "%s/%s", options->borderAssetDir, filename);
    snprintf(outputPath, sizeof(outputPath), "%s/gfx/borders/%s.t3x", options->outputDir, stem);

    fprintf(stderr, "n3ds-preprocess: border %s -> %s\n", inputPath, outputPath);
    if (!runTex3ds(options->tex3dsExe, inputPath, outputPath, N3DS_TEXFMT_RGBA5551)) {
        fprintf(stderr, "Failed to convert border asset: %s\n", inputPath);
        return false;
    }

    if (convertedCount != NULL) (*convertedCount)++;
    return true;
}

static bool convertBorderAssets(const Options* options) {
    if (options == NULL) return false;
    if (!options->borderAssetDirAvailable) return true;

    uint32_t convertedCount = 0;
    bool ok = true;

#if N3DS_PREPROCESS_HOST_WINDOWS
    char searchPath[1400];
    WIN32_FIND_DATAA findData;
    snprintf(searchPath, sizeof(searchPath), "%s\\*.png", options->borderAssetDir);
    HANDLE findHandle = FindFirstFileA(searchPath, &findData);
    if (findHandle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Border assets: converted=0\n");
        return true;
    }

    do {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        if (!convertBorderAssetPng(options, findData.cFileName, &convertedCount)) {
            ok = false;
            break;
        }
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);
#else
    DIR* dir = opendir(options->borderAssetDir);
    if (dir == NULL) {
        fprintf(stderr, "Border assets: converted=0\n");
        return true;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!convertBorderAssetPng(options, entry->d_name, &convertedCount)) {
            ok = false;
            break;
        }
    }

    closedir(dir);
#endif

    if (ok) fprintf(stderr, "Border assets: converted=%u\n", convertedCount);
    return ok;
}

static bool writeDirectTextureAsset(const Options* options, const char* relativePathNoExt, const uint8_t* rgba, uint32_t width, uint32_t height, N3DSTextureFormat format) {
    if (options == NULL || relativePathNoExt == NULL || rgba == NULL || width == 0 || height == 0) return false;

    char pngPath[1024];
    char t3xPath[1024];
    snprintf(pngPath, sizeof(pngPath), "%s/%s.png", options->outputDir, relativePathNoExt);
    snprintf(t3xPath, sizeof(t3xPath), "%s/%s.t3x", options->outputDir, relativePathNoExt);

    if (!stbi_write_png(pngPath, (int) width, (int) height, 4, rgba, (int) (width * 4u))) {
        fprintf(stderr, "Failed to write direct asset PNG: %s\n", pngPath);
        return false;
    }
    if (!runTex3ds(options->tex3dsExe, pngPath, t3xPath, format)) {
        return false;
    }
    if (!options->keepPng) remove(pngPath);
    return true;
}

static bool writeDirectTexturePng(const Options* options, const char* relativePathNoExt, const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (options == NULL || relativePathNoExt == NULL || rgba == NULL || width == 0 || height == 0) return false;

    char pngPath[1024];
    snprintf(pngPath, sizeof(pngPath), "%s/%s.png", options->outputDir, relativePathNoExt);
    if (!stbi_write_png(pngPath, (int) width, (int) height, 4, rgba, (int) (width * 4u))) {
        fprintf(stderr, "Failed to write direct asset PNG: %s\n", pngPath);
        return false;
    }
    return true;
}

static bool emitDirectSpriteFrameAsset(
    const Options* options,
    const Sprite* sprite,
    uint32_t spriteIndex,
    uint32_t frameIndex,
    const TexturePageItem* item,
    const uint8_t* rgba,
    uint32_t stride,
    DirectSpriteFormatState* spriteFormatStates
) {
    if (options == NULL || sprite == NULL || item == NULL || rgba == NULL) return false;
    if (sprite->name != NULL && sprite->name[0] != '\0') {
        fprintf(
            stderr,
            "n3ds-preprocess: sprite frame %05lu/%05lu (%s)\n",
            (unsigned long) spriteIndex,
            (unsigned long) frameIndex,
            sprite->name
        );
    } else {
        fprintf(
            stderr,
            "n3ds-preprocess: sprite frame %05lu/%05lu\n",
            (unsigned long) spriteIndex,
            (unsigned long) frameIndex
        );
    }

    uint32_t logicalW = sprite->width > 0 ? sprite->width : (item->boundingWidth > 0 ? item->boundingWidth : item->sourceWidth);
    uint32_t logicalH = sprite->height > 0 ? sprite->height : (item->boundingHeight > 0 ? item->boundingHeight : item->sourceHeight);
    if (logicalW == 0 || logicalH == 0) return true;
    if (logicalW > 1024 || logicalH > 1024) return true;

    uint8_t* framePixels = safeCalloc((size_t) logicalW * (size_t) logicalH, 4u);
    int32_t dstX = (int32_t) item->targetX;
    int32_t dstY = (int32_t) item->targetY;
    int32_t srcX = (int32_t) item->sourceX;
    int32_t srcY = (int32_t) item->sourceY;
    int32_t copyW = (int32_t) item->sourceWidth;
    int32_t copyH = (int32_t) item->sourceHeight;
    if (dstX < 0) {
        srcX -= dstX;
        copyW += dstX;
        dstX = 0;
    }
    if (dstY < 0) {
        srcY -= dstY;
        copyH += dstY;
        dstY = 0;
    }
    if (dstX + copyW > (int32_t) logicalW) copyW = (int32_t) logicalW - dstX;
    if (dstY + copyH > (int32_t) logicalH) copyH = (int32_t) logicalH - dstY;
    if (copyW > 0 && copyH > 0 && dstX >= 0 && dstY >= 0 && srcX >= 0 && srcY >= 0) {
        blitRect(framePixels, logicalW, (uint32_t) dstX, (uint32_t) dstY, rgba, stride, (uint32_t) srcX, (uint32_t) srcY, (uint32_t) copyW, (uint32_t) copyH);
    }

    tryApplySpriteReplacement(options, sprite, spriteIndex, frameIndex, logicalW, logicalH, framePixels);

    if (spriteFormatStates != NULL) {
        DirectSpriteFormatState* state = &spriteFormatStates[spriteIndex];
        bool frameMonoSafe = pixelsLookMonochrome(framePixels, logicalW, logicalH);
        if (!state->sawFrame) {
            state->sawFrame = true;
            state->allFramesMonoSafe = frameMonoSafe;
        } else if (state->allFramesMonoSafe && !frameMonoSafe) {
            state->allFramesMonoSafe = false;
        }
    }

    char spriteTempRoot[1024];
    char spriteTempDir[1024];
    char relativePath[256];
    snprintf(spriteTempRoot, sizeof(spriteTempRoot), "%s/gfx/sprites/__tmp", options->outputDir);
    snprintf(spriteTempDir, sizeof(spriteTempDir), "%s/spr_%05lu", spriteTempRoot, (unsigned long) spriteIndex);
    if (!ensureDir(spriteTempRoot) || !ensureDir(spriteTempDir)) {
        free(framePixels);
        return false;
    }

    snprintf(relativePath, sizeof(relativePath), "gfx/sprites/__tmp/spr_%05lu/frame_%05lu", (unsigned long) spriteIndex, (unsigned long) frameIndex);
    bool ok = writeDirectTexturePng(options, relativePath, framePixels, logicalW, logicalH);
    free(framePixels);
    return ok;
}

static bool finalizeDirectSpriteAssets(const Options* options, const DataWin* dataWin, const DirectSpriteFormatState* spriteFormatStates) {
    if (options == NULL || dataWin == NULL) return false;

    char spriteTempRoot[1024];
    snprintf(spriteTempRoot, sizeof(spriteTempRoot), "%s/gfx/sprites/__tmp", options->outputDir);

    fprintf(stderr, "n3ds-preprocess: finalizing direct sprite assets\n");

    repeat(dataWin->sprt.count, spriteIndex) {
        const Sprite* sprite = &dataWin->sprt.sprites[spriteIndex];
        if (sprite->textureCount <= 0) continue;

        char spriteTempDir[1024];
        char globPath[1024];
        char outPath[1024];
        char pngPath[1024];
        char frameOutPath[1024];
        snprintf(spriteTempDir, sizeof(spriteTempDir), "%s/spr_%05lu", spriteTempRoot, (unsigned long) spriteIndex);
        if (!directoryExists(spriteTempDir)) continue;
        snprintf(globPath, sizeof(globPath), "%s/spr_%05lu/*.png", spriteTempRoot, (unsigned long) spriteIndex);
        snprintf(outPath, sizeof(outPath), "%s/gfx/sprites/spr_%05lu.t3x", options->outputDir, (unsigned long) spriteIndex);
        bool forceFrameDirect = spriteShouldForceDirectRGBA5551(sprite->name);
        bool spriteMonoSafe =
            spriteFormatStates != NULL &&
            spriteFormatStates[spriteIndex].sawFrame &&
            spriteFormatStates[spriteIndex].allFramesMonoSafe;
        N3DSTextureFormat spriteFormat = spriteMonoSafe ? N3DS_TEXFMT_LA4 : N3DS_TEXFMT_RGBA5551;
        if (sprite->name != NULL && sprite->name[0] != '\0') {
            fprintf(
                stderr,
                "n3ds-preprocess: sprite %05lu -> %s [%s] (%s)\n",
                (unsigned long) spriteIndex,
                forceFrameDirect ? "frame files" : "atlas",
                getTextureFormatLabel(spriteFormat),
                sprite->name
            );
        } else {
            fprintf(
                stderr,
                "n3ds-preprocess: sprite %05lu -> %s [%s]\n",
                (unsigned long) spriteIndex,
                forceFrameDirect ? "frame files" : "atlas",
                getTextureFormatLabel(spriteFormat)
            );
        }
        if (!forceFrameDirect && !runTex3dsAtlas(options->tex3dsExe, globPath, outPath, spriteFormat)) {
            fprintf(stderr, "n3ds-preprocess: atlas build failed for sprite %05lu, falling back to per-frame t3x\n", (unsigned long) spriteIndex);
            repeat(sprite->textureCount, frameIndex) {
                snprintf(pngPath, sizeof(pngPath), "%s/spr_%05lu/frame_%05zu.png", spriteTempRoot, (unsigned long) spriteIndex, frameIndex);
                if (!fileExists(pngPath)) continue;
                snprintf(frameOutPath, sizeof(frameOutPath), "%s/gfx/sprites/spr_%05lu_frame_%05zu.t3x", options->outputDir, (unsigned long) spriteIndex, frameIndex);
                if (!runTex3ds(options->tex3dsExe, pngPath, frameOutPath, spriteFormat)) {
                    return false;
                }
            }
        } else if (forceFrameDirect) {
            repeat(sprite->textureCount, frameIndex) {
                snprintf(pngPath, sizeof(pngPath), "%s/spr_%05lu/frame_%05zu.png", spriteTempRoot, (unsigned long) spriteIndex, frameIndex);
                if (!fileExists(pngPath)) continue;
                snprintf(frameOutPath, sizeof(frameOutPath), "%s/gfx/sprites/spr_%05lu_frame_%05zu.t3x", options->outputDir, (unsigned long) spriteIndex, frameIndex);
                if (!runTex3ds(options->tex3dsExe, pngPath, frameOutPath, spriteFormat)) {
                    return false;
                }
            }
        }
        if (!options->keepPng) {
            if (!deleteDirectoryRecursive(spriteTempDir)) {
                fprintf(stderr, "Failed to remove temporary sprite directory: %s\n", spriteTempDir);
                return false;
            }
        }
    }

    if (!options->keepPng && directoryExists(spriteTempRoot)) {
        if (!deleteDirectoryRecursive(spriteTempRoot)) {
            fprintf(stderr, "Failed to remove temporary sprite root: %s\n", spriteTempRoot);
            return false;
        }
    }
    return true;
}

static bool emitDirectBackgroundAsset(
    const Options* options,
    uint32_t backgroundIndex,
    const TexturePageItem* item,
    const uint8_t* rgba,
    uint32_t stride
) {
    if (options == NULL || item == NULL || rgba == NULL || item->sourceWidth == 0 || item->sourceHeight == 0) return false;
    if (item->sourceWidth > 1024 || item->sourceHeight > 1024) return true;

    uint8_t* pixels = safeCalloc((size_t) item->sourceWidth * (size_t) item->sourceHeight, 4u);
    blitRect(pixels, item->sourceWidth, 0, 0, rgba, stride, item->sourceX, item->sourceY, item->sourceWidth, item->sourceHeight);

    char relativePath[256];
    snprintf(relativePath, sizeof(relativePath), "gfx/backgrounds/bg_%05lu", (unsigned long) backgroundIndex);
    bool ok = writeDirectTextureAsset(options, relativePath, pixels, item->sourceWidth, item->sourceHeight, N3DS_TEXFMT_RGBA5551);
    free(pixels);
    return ok;
}

static bool emitDirectFontAsset(
    const Options* options,
    uint32_t fontIndex,
    const TexturePageItem* item,
    const uint8_t* rgba,
    uint32_t stride
) {
    if (options == NULL || item == NULL || rgba == NULL || item->sourceWidth == 0 || item->sourceHeight == 0) return false;
    if (item->sourceWidth > 1024 || item->sourceHeight > 1024) return true;

    uint8_t* pixels = safeCalloc((size_t) item->sourceWidth * (size_t) item->sourceHeight, 4u);
    blitRect(pixels, item->sourceWidth, 0, 0, rgba, stride, item->sourceX, item->sourceY, item->sourceWidth, item->sourceHeight);

    char relativePath[256];
    snprintf(relativePath, sizeof(relativePath), "gfx/fonts/font_%05lu", (unsigned long) fontIndex);
    bool ok = writeDirectTextureAsset(options, relativePath, pixels, item->sourceWidth, item->sourceHeight, N3DS_TEXFMT_RGBA5551);
    free(pixels);
    return ok;
}

static PackedPage* findPackedPage(PackedPage** packedPages, uint32_t packedPageCount, uint16_t pageIndex) {
    repeat(packedPageCount, i) {
        if (packedPages[i]->pageIndex == pageIndex) return packedPages[i];
    }
    return NULL;
}

static void collectLegacyTileRequest(TileLookupKey key, TileLookupKey** outKeys, TileRequestMap** dedupe) {
    ptrdiff_t existing = hmgeti(*dedupe, key);
    if (existing >= 0) return;
    hmput(*dedupe, key, (uint32_t) arrlen(*outKeys));
    arrput(*outKeys, key);
}

static void collectTileLayerRequests(RoomLayerTilesData* tilesData, DataWin* dataWin, TileLookupKey** outKeys, TileRequestMap** dedupe) {
    if (tilesData == NULL || tilesData->tileData == NULL) return;
    if (tilesData->backgroundIndex < 0 || (uint32_t) tilesData->backgroundIndex >= dataWin->bgnd.count) return;

    Background* tileset = &dataWin->bgnd.backgrounds[tilesData->backgroundIndex];
    if (tileset->gms2TileWidth == 0 || tileset->gms2TileHeight == 0 || tileset->gms2TileColumns == 0) return;

    uint32_t tileW = tileset->gms2TileWidth;
    uint32_t tileH = tileset->gms2TileHeight;
    uint32_t borderX = tileset->gms2OutputBorderX;
    uint32_t borderY = tileset->gms2OutputBorderY;
    uint32_t columns = tileset->gms2TileColumns;
    uint32_t totalTiles = tilesData->tilesX * tilesData->tilesY;

    repeat(totalTiles, i) {
        uint32_t cell = tilesData->tileData[i];
        uint32_t tileIndex = cell & GMS2_TILE_INDEX_MASK;
        if (tileIndex == 0) continue;

        uint32_t tileSlot = tileIndex - 1u;
        uint32_t col = tileSlot % columns;
        uint32_t row = tileSlot / columns;
        collectLegacyTileRequest(
            (TileLookupKey) {
                .bgDef = (int16_t) tilesData->backgroundIndex,
                .srcX = (uint16_t) (col * (tileW + 2u * borderX) + borderX),
                .srcY = (uint16_t) (row * (tileH + 2u * borderY) + borderY),
                .srcW = (uint16_t) tileW,
                .srcH = (uint16_t) tileH,
            },
            outKeys,
            dedupe
        );
    }
}

static void collectBackgroundTilesetRequests(DataWin* dataWin, TileLookupKey** outKeys, TileRequestMap** dedupe) {
    repeat(dataWin->bgnd.count, i) {
        Background* bg = &dataWin->bgnd.backgrounds[i];
        if (bg->gms2TileWidth == 0 || bg->gms2TileHeight == 0 || bg->gms2TileColumns == 0 || bg->gms2TileCount == 0) continue;

        uint32_t tileW = bg->gms2TileWidth;
        uint32_t tileH = bg->gms2TileHeight;
        uint32_t borderX = bg->gms2OutputBorderX;
        uint32_t borderY = bg->gms2OutputBorderY;
        uint32_t columns = bg->gms2TileColumns;

        repeat(bg->gms2TileCount, tileSlot) {
            uint32_t col = (uint32_t) tileSlot % columns;
            uint32_t row = (uint32_t) tileSlot / columns;
            collectLegacyTileRequest(
                (TileLookupKey) {
                    .bgDef = (int16_t) i,
                    .srcX = (uint16_t) (col * (tileW + 2u * borderX) + borderX),
                    .srcY = (uint16_t) (row * (tileH + 2u * borderY) + borderY),
                    .srcW = (uint16_t) tileW,
                    .srcH = (uint16_t) tileH,
                },
                outKeys,
                dedupe
            );
        }
    }
}

static void collectLegacyTileRequestsFromRoom(Room* room, DataWin* dataWin, TileLookupKey** outKeys, TileRequestMap** dedupe) {
    if (room == NULL) return;

    repeat(room->tileCount, i) {
        RoomTile* tile = &room->tiles[i];
        if (tile->useSpriteDefinition || tile->backgroundDefinition < 0 || tile->width == 0 || tile->height == 0) continue;
        collectLegacyTileRequest(
            (TileLookupKey) {
                .bgDef = (int16_t) tile->backgroundDefinition,
                .srcX = (uint16_t) tile->sourceX,
                .srcY = (uint16_t) tile->sourceY,
                .srcW = (uint16_t) tile->width,
                .srcH = (uint16_t) tile->height,
            },
            outKeys,
            dedupe
        );
    }

    repeat(room->layerCount, i) {
        RoomLayer* layer = &room->layers[i];
        if (layer->tilesData != NULL) {
            collectTileLayerRequests(layer->tilesData, dataWin, outKeys, dedupe);
        }
        if (layer->assetsData == NULL) continue;
        repeat(layer->assetsData->legacyTileCount, j) {
            RoomTile* tile = &layer->assetsData->legacyTiles[j];
            if (tile->useSpriteDefinition || tile->backgroundDefinition < 0 || tile->width == 0 || tile->height == 0) continue;
            collectLegacyTileRequest(
                (TileLookupKey) {
                    .bgDef = (int16_t) tile->backgroundDefinition,
                    .srcX = (uint16_t) tile->sourceX,
                    .srcY = (uint16_t) tile->sourceY,
                    .srcW = (uint16_t) tile->width,
                    .srcH = (uint16_t) tile->height,
                },
                outKeys,
                dedupe
            );
        }
    }
}

static TileLookupKey* collectLegacyTileRequests(DataWin* dataWin) {
    TileLookupKey* keys = NULL;
    TileRequestMap* dedupe = NULL;
    uint32_t totalRoomTiles = 0;
    uint32_t totalLegacyAssetTiles = 0;
    uint32_t totalTileLayers = 0;
    repeat(dataWin->room.count, i) {
        DataWin_loadRoomPayload(dataWin, (int32_t) i);
        Room* room = &dataWin->room.rooms[i];
        totalRoomTiles += room->tileCount;
        repeat(room->layerCount, layerIndex) {
            RoomLayer* layer = &room->layers[layerIndex];
            if (layer->assetsData != NULL) totalLegacyAssetTiles += layer->assetsData->legacyTileCount;
            if (layer->tilesData != NULL) totalTileLayers++;
        }
        collectLegacyTileRequestsFromRoom(&dataWin->room.rooms[i], dataWin, &keys, &dedupe);
    }
    collectBackgroundTilesetRequests(dataWin, &keys, &dedupe);
    fprintf(
        stderr,
        "n3ds-preprocess: roomTiles=%u assetLegacyTiles=%u tileLayers=%u uniqueTileRects=%u\n",
        totalRoomTiles,
        totalLegacyAssetTiles,
        totalTileLayers,
        (unsigned) arrlen(keys)
    );
    hmfree(dedupe);
    return keys;
}

static void markRoomManifestOutputItemPages(
    uint32_t tpagIndex,
    uint32_t totalPageCount,
    const OutputItem* items,
    const OutputFragment* fragments,
    bool* seenPages
) {
    if (items == NULL || fragments == NULL || seenPages == NULL) return;
    const OutputItem* item = &items[tpagIndex];
    if (item->fragmentCount == 0 || item->fragmentStart == UINT32_MAX) return;

    repeat(item->fragmentCount, i) {
        uint32_t fragmentIndex = item->fragmentStart + (uint32_t) i;
        uint16_t pageIndex = fragments[fragmentIndex].atlasId;
        if (pageIndex < totalPageCount) seenPages[pageIndex] = true;
    }
}

static void markRoomManifestSpritePages(
    const DataWin* dataWin,
    int32_t spriteIndex,
    uint32_t totalPageCount,
    const OutputItem* items,
    const OutputFragment* fragments,
    bool* seenPages
) {
    if (dataWin == NULL || items == NULL || fragments == NULL || seenPages == NULL) return;
    if (spriteIndex < 0 || (uint32_t) spriteIndex >= dataWin->sprt.count) return;

    const Sprite* sprite = &dataWin->sprt.sprites[spriteIndex];
    repeat(sprite->textureCount, i) {
        int32_t tpagIndex = sprite->tpagIndices[i];
        if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
        markRoomManifestOutputItemPages((uint32_t) tpagIndex, totalPageCount, items, fragments, seenPages);
    }
}

static void markRoomManifestBackgroundPages(
    const DataWin* dataWin,
    int32_t backgroundIndex,
    uint32_t totalPageCount,
    const OutputItem* items,
    const OutputFragment* fragments,
    bool* seenPages
) {
    if (dataWin == NULL || items == NULL || fragments == NULL || seenPages == NULL) return;
    if (backgroundIndex < 0 || (uint32_t) backgroundIndex >= dataWin->bgnd.count) return;

    const Background* background = &dataWin->bgnd.backgrounds[backgroundIndex];
    if (background->tpagIndex < 0 || (uint32_t) background->tpagIndex >= dataWin->tpag.count) return;
    markRoomManifestOutputItemPages((uint32_t) background->tpagIndex, totalPageCount, items, fragments, seenPages);
}

static void markRoomManifestTileEntryPages(
    const OutputTileEntry* tileEntry,
    uint32_t totalPageCount,
    const OutputFragment* fragments,
    bool* seenPages
) {
    if (tileEntry == NULL || fragments == NULL || seenPages == NULL) return;

    if (tileEntry->atlasId != UINT16_MAX) {
        if (tileEntry->atlasId < totalPageCount) seenPages[tileEntry->atlasId] = true;
        return;
    }

    if (tileEntry->fragmentCount == 0 || tileEntry->fragmentStart == UINT32_MAX) return;
    repeat(tileEntry->fragmentCount, i) {
        uint32_t fragmentIndex = tileEntry->fragmentStart + (uint32_t) i;
        uint16_t pageIndex = fragments[fragmentIndex].atlasId;
        if (pageIndex < totalPageCount) seenPages[pageIndex] = true;
    }
}

static bool writeRoomManifestFile(
    const char* outputDir,
    const DataWin* dataWin,
    uint32_t totalPageCount,
    const OutputItem* items,
    const OutputFragment* fragments,
    const OutputTileEntry* packedTileEntries,
    uint32_t packedTileEntryCount
) {
    if (outputDir == NULL || dataWin == NULL || items == NULL || fragments == NULL) return false;

    char manifestPath[1024];
    snprintf(manifestPath, sizeof(manifestPath), "%s/gfx/room_manifest.bin", outputDir);

    TileRequestMap* tileEntryMap = NULL;
    repeat(packedTileEntryCount, i) {
        TileLookupKey key = {
            .bgDef = packedTileEntries[i].bgDef,
            .srcX = packedTileEntries[i].srcX,
            .srcY = packedTileEntries[i].srcY,
            .srcW = packedTileEntries[i].srcW,
            .srcH = packedTileEntries[i].srcH,
        };
        hmput(tileEntryMap, key, i);
    }

    N3DSRoomManifestEntry* entries = NULL;
    N3DSRoomManifestPageRef* pageRefs = NULL;
    uint32_t roomWithPagesCount = 0;

    repeat(dataWin->room.count, roomIndex) {
        Room* room = &dataWin->room.rooms[roomIndex];
        bool* seenPages = safeCalloc(totalPageCount > 0 ? totalPageCount : 1u, sizeof(bool));
        uint32_t pageStart = (uint32_t) arrlen(pageRefs);

        repeat(8u, i) {
            if (room->backgrounds == NULL || !room->backgrounds[i].enabled) continue;
            markRoomManifestBackgroundPages(dataWin, room->backgrounds[i].backgroundDefinition, totalPageCount, items, fragments, seenPages);
        }

        repeat(room->gameObjectCount, objIndex) {
            const RoomGameObject* roomObject = &room->gameObjects[objIndex];
            if (roomObject->objectDefinition < 0 || (uint32_t) roomObject->objectDefinition >= dataWin->objt.count) continue;
            const GameObject* object = &dataWin->objt.objects[roomObject->objectDefinition];
            markRoomManifestSpritePages(dataWin, object->spriteId, totalPageCount, items, fragments, seenPages);
        }

        repeat(room->layerCount, layerIndex) {
            RoomLayer* layer = &room->layers[layerIndex];
            if (layer->backgroundData != NULL) {
                markRoomManifestSpritePages(dataWin, layer->backgroundData->spriteIndex, totalPageCount, items, fragments, seenPages);
            }
            if (layer->assetsData != NULL) {
                repeat(layer->assetsData->spriteCount, spriteSlot) {
                    markRoomManifestSpritePages(
                        dataWin,
                        layer->assetsData->sprites[spriteSlot].spriteIndex,
                        totalPageCount,
                        items,
                        fragments,
                        seenPages
                    );
                }
            }
        }

        TileLookupKey* roomTileKeys = NULL;
        TileRequestMap* roomTileDedupe = NULL;
        collectLegacyTileRequestsFromRoom(room, (DataWin*) dataWin, &roomTileKeys, &roomTileDedupe);
        repeat(arrlen(roomTileKeys), tileKeyIndex) {
            ptrdiff_t packedTileIndex = hmgeti(tileEntryMap, roomTileKeys[tileKeyIndex]);
            if (packedTileIndex < 0) continue;
            markRoomManifestTileEntryPages(
                &packedTileEntries[tileEntryMap[packedTileIndex].value],
                totalPageCount,
                fragments,
                seenPages
            );
        }
        arrfree(roomTileKeys);
        hmfree(roomTileDedupe);

        repeat(totalPageCount, pageIndex) {
            if (!seenPages[pageIndex]) continue;
            N3DSRoomManifestPageRef ref = {
                .pageIndex = (uint16_t) pageIndex,
                .flags = 0,
                .reserved = 0,
            };
            arrput(pageRefs, ref);
        }

        uint32_t pageCount = (uint32_t) arrlen(pageRefs) - pageStart;
        if (pageCount > 0) roomWithPagesCount++;
        N3DSRoomManifestEntry entry = {
            .roomIndex = roomIndex,
            .pageStart = pageStart,
            .pageCount = pageCount,
            .directSpriteStart = 0,
            .directSpriteCount = 0,
            .directBackgroundStart = 0,
            .directBackgroundCount = 0,
        };
        arrput(entries, entry);
        free(seenPages);
    }

    FILE* file = fopen(manifestPath, "wb");
    if (file == NULL) {
        hmfree(tileEntryMap);
        arrfree(entries);
        arrfree(pageRefs);
        fprintf(stderr, "Failed to open room manifest output: %s\n", manifestPath);
        return false;
    }

    uint32_t header[5] = {
        N3DS_ROOM_MANIFEST_MAGIC,
        N3DS_ROOM_MANIFEST_VERSION,
        (uint32_t) arrlen(entries),
        (uint32_t) arrlen(pageRefs),
        dataWin->room.count,
    };
    bool ok = fwrite(header, sizeof(header), 1, file) == 1;
    if (ok && arrlen(entries) > 0) ok = fwrite(entries, sizeof(N3DSRoomManifestEntry), arrlen(entries), file) == arrlen(entries);
    if (ok && arrlen(pageRefs) > 0) ok = fwrite(pageRefs, sizeof(N3DSRoomManifestPageRef), arrlen(pageRefs), file) == arrlen(pageRefs);
    fclose(file);

    fprintf(
        stderr,
        "n3ds-preprocess: wrote room manifest (%u rooms, %u non-empty, %u page refs) -> %s\n",
        dataWin->room.count,
        roomWithPagesCount,
        (unsigned int) arrlen(pageRefs),
        manifestPath
    );

    hmfree(tileEntryMap);
    arrfree(entries);
    arrfree(pageRefs);
    return ok;
}

static bool flushPackedPages(const Options* options, OutputPage* outputPages, PackedPage** packedPages, uint32_t packedPageCount) {
    uint32_t rgba5551Pages = 0;
    uint32_t etc1a4Pages = 0;
    uint32_t indexed8Pages = 0;
    uint32_t l4Pages = 0;
    uint32_t la4Pages = 0;
    uint32_t monoRejectCount = 0;
    uint32_t monoRejectFontCount = 0;
    uint32_t monoRejectBackgroundCount = 0;
    uint32_t monoRejectNonMonoCount = 0;
    uint32_t monoRejectSamplePages[8] = {0};
    uint32_t monoRejectSampleCount = 0;
    uint32_t manualMonoRiskCount = 0;
    uint32_t manualMonoRiskSamplePages[8] = {0};
    uint32_t manualMonoRiskSampleCount = 0;
    int32_t* overrideFormats = loadPageFormatOverrides(options->pageFormatOverridesPath, packedPageCount);

    repeat(packedPageCount, i) {
        PackedPage* packedPage = packedPages[i];
        OutputPage* outputPage = &outputPages[packedPage->pageIndex];
        N3DSTextureFormat pageFormat = (N3DSTextureFormat) outputPage->textureFormat;
        if (options->textureFormat == N3DS_TEXFMT_HYBRID) {
            pageFormat = chooseHybridPageFormat(packedPage);
            outputPage->textureFormat = (uint32_t) pageFormat;
            snprintf(outputPage->path, sizeof(outputPage->path), "page_%03u.%s", packedPage->pageIndex, getPageExtension(pageFormat));
        }
        bool hasManualOverride = overrideFormats != NULL && overrideFormats[packedPage->pageIndex] >= 0;
        if (hasManualOverride) {
            pageFormat = (N3DSTextureFormat) overrideFormats[packedPage->pageIndex];
            outputPage->textureFormat = (uint32_t) pageFormat;
            snprintf(outputPage->path, sizeof(outputPage->path), "page_%03u.%s", packedPage->pageIndex, getPageExtension(pageFormat));
            if ((pageFormat == N3DS_TEXFMT_L4 || pageFormat == N3DS_TEXFMT_LA4) && !pageCanUseMonoFormat(outputPage)) {
                manualMonoRiskCount++;
                if (manualMonoRiskSampleCount < 8u) {
                    manualMonoRiskSamplePages[manualMonoRiskSampleCount++] = packedPage->pageIndex;
                }
            }
        }
        if (!hasManualOverride && pageShouldForceRGBA5551(outputPage)) {
            pageFormat = N3DS_TEXFMT_RGBA5551;
            outputPage->textureFormat = (uint32_t) pageFormat;
            snprintf(outputPage->path, sizeof(outputPage->path), "page_%03u.%s", packedPage->pageIndex, getPageExtension(pageFormat));
        }
        if (!hasManualOverride && (pageFormat == N3DS_TEXFMT_L4 || pageFormat == N3DS_TEXFMT_LA4) && !pageCanUseMonoFormat(outputPage)) {
            monoRejectCount++;
            if (outputPage->containsFont) monoRejectFontCount++;
            if (outputPage->containsBackground) monoRejectBackgroundCount++;
            if (outputPage->containsNonMonoContent) monoRejectNonMonoCount++;
            if (monoRejectSampleCount < 8u) {
                monoRejectSamplePages[monoRejectSampleCount++] = packedPage->pageIndex;
            }
            pageFormat = N3DS_TEXFMT_RGBA5551;
            outputPage->textureFormat = (uint32_t) pageFormat;
            snprintf(outputPage->path, sizeof(outputPage->path), "page_%03u.%s", packedPage->pageIndex, getPageExtension(pageFormat));
        }
        char pngPath[1024];
        char pagePath[1024];
        char previewPath[1024];
        snprintf(pngPath, sizeof(pngPath), "%s/gfx/page_%03u.png", options->outputDir, packedPage->pageIndex);
        snprintf(pagePath, sizeof(pagePath), "%s/gfx/%s", options->outputDir, outputPage->path);
        snprintf(
            previewPath,
            sizeof(previewPath),
            "%s/gfx/page_%03u__%s.png",
            options->outputDir,
            packedPage->pageIndex,
            outputPage->previewLabel[0] != '\0' ? outputPage->previewLabel : "unnamed"
        );

        if (pageFormat == N3DS_TEXFMT_INDEXED8) {
            if (!writeIndexedPage(pagePath, packedPage)) {
                free(overrideFormats);
                return false;
            }
        } else {
            if (!stbi_write_png(pngPath, packedPage->width, packedPage->height, 4, packedPage->pixels, packedPage->width * 4)) {
                fprintf(stderr, "Failed to write temporary PNG: %s\n", pngPath);
                free(overrideFormats);
                return false;
            }
            if (!runTex3ds(options->tex3dsExe, pngPath, pagePath, pageFormat)) {
                free(overrideFormats);
                return false;
            }
            if (options->dumpPagePreviews) {
                stbi_write_png(previewPath, packedPage->width, packedPage->height, 4, packedPage->pixels, packedPage->width * 4);
            }
            if (!options->keepPng) remove(pngPath);
        }
        if (pageFormat == N3DS_TEXFMT_INDEXED8 && options->dumpPagePreviews) {
            stbi_write_png(previewPath, packedPage->width, packedPage->height, 4, packedPage->pixels, packedPage->width * 4);
        }
        switch (pageFormat) {
            case N3DS_TEXFMT_RGBA5551: rgba5551Pages++; break;
            case N3DS_TEXFMT_ETC1A4: etc1a4Pages++; break;
            case N3DS_TEXFMT_INDEXED8: indexed8Pages++; break;
            case N3DS_TEXFMT_L4: l4Pages++; break;
            case N3DS_TEXFMT_LA4: la4Pages++; break;
            default: break;
        }
        fprintf(stderr, "Wrote packed atlas page %u (%s) -> %s\n", packedPage->pageIndex, getTextureFormatLabel(pageFormat), pagePath);
    }

    if (packedPageCount > 0) {
        fprintf(
            stderr,
            "n3ds-preprocess: atlas page formats: rgba5551=%u etc1a4=%u indexed8=%u l4=%u la4=%u total=%u\n",
            rgba5551Pages,
            etc1a4Pages,
            indexed8Pages,
            l4Pages,
            la4Pages,
            packedPageCount
        );
        if (monoRejectCount > 0) {
            fprintf(
                stderr,
                "n3ds-preprocess: rejected mono overrides: total=%u font=%u background=%u nonMono=%u",
                monoRejectCount,
                monoRejectFontCount,
                monoRejectBackgroundCount,
                monoRejectNonMonoCount
            );
            if (monoRejectSampleCount > 0) {
                fprintf(stderr, " samplePages=");
                repeat(monoRejectSampleCount, i) {
                    fprintf(stderr, "%s%03u", i == 0 ? "" : ",", monoRejectSamplePages[i]);
                }
            }
            fprintf(stderr, "\n");
        }
        if (manualMonoRiskCount > 0) {
            fprintf(stderr, "n3ds-preprocess: manual mono overrides flagged as risky: total=%u", manualMonoRiskCount);
            if (manualMonoRiskSampleCount > 0) {
                fprintf(stderr, " samplePages=");
                repeat(manualMonoRiskSampleCount, i) {
                    fprintf(stderr, "%s%03u", i == 0 ? "" : ",", manualMonoRiskSamplePages[i]);
                }
            }
            fprintf(stderr, "\n");
        }
    }
    writePageFormatTemplate(options, outputPages, packedPageCount);
    free(overrideFormats);
    return true;
}

static bool convertTextures(const Options* options, DataWin* dataWin) {
    fprintf(stderr, "n3ds-preprocess: processing textures and direct sprite assets\n");

    bool gm2022_5 = DataWin_isVersionAtLeast(dataWin, 2022, 5, 0, 0);
    OutputPage* pages = NULL;
    OutputItem* items = safeCalloc(dataWin->tpag.count, sizeof(OutputItem));
    OutputFragment* fragments = NULL;
    TileLookupKey* tileRequests = collectLegacyTileRequests(dataWin);
    size_t tileRequestCount = arrlen(tileRequests);
    OutputTileEntry* tileEntriesByRequest = safeCalloc(tileRequestCount > 0 ? tileRequestCount : 1u, sizeof(OutputTileEntry));
    bool* tileEntryWritten = safeCalloc(tileRequestCount > 0 ? tileRequestCount : 1u, sizeof(bool));
    OutputTileEntry* packedTileEntries = NULL;
    uint32_t totalPageCount = 0;
    PackedPage** packedPages = NULL;
    uint32_t packedPageCount = 0;
    bool* targetedMonoTPAGs = options->enableTargetedBattleDialogueMono ? collectTargetedDialogueBattleTPAGs(dataWin) : NULL;
    bool* fontTPAGs = collectFontTPAGs(dataWin);
    bool* backgroundTPAGs = collectBackgroundTPAGs(dataWin);
    char** tpagDebugNames = collectTPAGDebugNames(dataWin);
    int32_t* tpagToSpriteIndex = buildTPAGToSpriteIndexMap(dataWin);
    int32_t* tpagToSpriteFrameIndex = buildTPAGToSpriteFrameMap(dataWin);
    int32_t* tpagToBackgroundIndex = buildTPAGToBackgroundIndexMap(dataWin);
    int32_t* tpagToFontIndex = buildTPAGToFontIndexMap(dataWin);
    bool* emittedSpriteFrames = safeCalloc(dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u, sizeof(bool));
    DirectSpriteFormatState* directSpriteFormatStates = safeCalloc(dataWin->sprt.count > 0 ? dataWin->sprt.count : 1u, sizeof(DirectSpriteFormatState));
    bool* emittedBackgrounds = safeCalloc(dataWin->bgnd.count > 0 ? dataWin->bgnd.count : 1u, sizeof(bool));
    bool* emittedFonts = safeCalloc(dataWin->font.count > 0 ? dataWin->font.count : 1u, sizeof(bool));

    repeat(dataWin->tpag.count, i) {
        items[i].fragmentCount = UINT16_MAX;
        items[i].fragmentStart = UINT32_MAX;
    }

    repeat(dataWin->txtr.count, i) {
        Texture* texture = &dataWin->txtr.textures[i];
        if (texture->blobData == NULL || texture->blobSize == 0) {
            fprintf(stderr, "Texture page %zu has no embedded blob data; external textures are not supported by this preprocessor.\n", i);
            free(pages);
            return false;
        }

        int width = 0;
        int height = 0;
        uint8_t* rgba = ImageDecoder_decodeToRgba(texture->blobData, texture->blobSize, gm2022_5, &width, &height);
        if (rgba == NULL) {
            fprintf(stderr, "Failed to decode TXTR page %zu\n", i);
            free(pages);
            return false;
        }

        if (width <= 0 || height <= 0 || width > 65535 || height > 65535) {
            fprintf(stderr, "Invalid decoded size for TXTR page %zu: %dx%d\n", i, width, height);
            free(rgba);
            free(pages);
            return false;
        }

        repeat(dataWin->tpag.count, itemIndex) {
            TexturePageItem* item = &dataWin->tpag.items[itemIndex];
            if (item->texturePageId != (int16_t) i) continue;

            N3DSTextureFormat itemPageFormat = options->textureFormat;
            if (itemPageFormat == N3DS_TEXFMT_HYBRID) itemPageFormat = N3DS_TEXFMT_HYBRID;
            N3DSTextureFormat itemMonoFormat = chooseTargetedMonoItemFormat(rgba, (uint32_t) width, item);
            bool itemMonoSafe = itemMonoFormat == N3DS_TEXFMT_L4 || itemMonoFormat == N3DS_TEXFMT_LA4;
            if (targetedMonoTPAGs != NULL && targetedMonoTPAGs[itemIndex]) {
                if (itemMonoSafe) {
                    itemPageFormat = itemMonoFormat;
                }
            }

            uint32_t itemWidth = item->sourceWidth;
            uint32_t itemHeight = item->sourceHeight;
            if (itemWidth == 0 || itemHeight == 0) {
                items[itemIndex].width = 0;
                items[itemIndex].height = 0;
                items[itemIndex].fragmentStart = 0;
                items[itemIndex].fragmentCount = 0;
                continue;
            }
            uint32_t fragmentStart = arrlen(fragments);
            uint32_t itemChunkSize = choosePageSize(options, itemWidth, itemHeight);
            for (uint32_t chunkY = 0; chunkY < itemHeight; chunkY += itemChunkSize) {
                for (uint32_t chunkX = 0; chunkX < itemWidth; chunkX += itemChunkSize) {
                    uint32_t fragWidth = itemWidth - chunkX;
                    uint32_t fragHeight = itemHeight - chunkY;
                    if (fragWidth > itemChunkSize) fragWidth = itemChunkSize;
                    if (fragHeight > itemChunkSize) fragHeight = itemChunkSize;

                    uint16_t pageIndex = 0;
                    uint16_t dstX = 0;
                    uint16_t dstY = 0;
                    if (!packRect(options, &packedPages, &packedPageCount, &pages, &totalPageCount, fragWidth, fragHeight, itemPageFormat, &pageIndex, &dstX, &dstY)) {
                        fprintf(stderr, "Fragment pack failed for TPAG item %zu\n", itemIndex);
                        free(rgba);
                        free(items);
                        free(pages);
                        arrfree(fragments);
                        arrfree(tileRequests);
                        free(tileEntriesByRequest);
                        free(tileEntryWritten);
                        arrfree(packedTileEntries);
                        repeat(packedPageCount, freeIndex) {
                            free(packedPages[freeIndex]->pixels);
                            free(packedPages[freeIndex]);
                        }
                        free(packedPages);
                        return false;
                    }

                    PackedPage* dstPage = NULL;
                    repeat(packedPageCount, pageSlot) {
                        if (packedPages[pageSlot]->pageIndex == pageIndex) {
                            dstPage = packedPages[pageSlot];
                            break;
                        }
                    }
                    requireNotNull(dstPage);
                    OutputPage* outputPage = &pages[pageIndex];
                    if (tpagDebugNames != NULL && tpagDebugNames[itemIndex] != NULL) {
                        appendPageDebugName(outputPage, tpagDebugNames[itemIndex]);
                    }
                    if (tpagToSpriteIndex != NULL && tpagToSpriteIndex[itemIndex] >= 0) outputPage->containsSprite = true;
                    if (fontTPAGs != NULL && fontTPAGs[itemIndex]) outputPage->containsFont = true;
                    if (backgroundTPAGs != NULL && backgroundTPAGs[itemIndex]) outputPage->containsBackground = true;
                    if (!itemMonoSafe) outputPage->containsNonMonoContent = true;

                    blitRect(
                        dstPage->pixels,
                        dstPage->width,
                        dstX,
                        dstY,
                        rgba,
                        (uint32_t) width,
                        item->sourceX + chunkX,
                        item->sourceY + chunkY,
                        fragWidth,
                        fragHeight
                    );

                    OutputFragment fragment = {
                        .atlasId = pageIndex,
                        .x = dstX,
                        .y = dstY,
                        .width = (uint16_t) fragWidth,
                        .height = (uint16_t) fragHeight,
                        .sourceX = (uint16_t) chunkX,
                        .sourceY = (uint16_t) chunkY,
                    };
                    arrput(fragments, fragment);
                }
            }

            if (tpagToSpriteIndex != NULL && tpagToSpriteFrameIndex != NULL) {
                int32_t spriteIndex = tpagToSpriteIndex[itemIndex];
                int32_t frameIndex = tpagToSpriteFrameIndex[itemIndex];
                if (spriteIndex >= 0 && frameIndex >= 0 && (uint32_t) spriteIndex < dataWin->sprt.count && !emittedSpriteFrames[itemIndex]) {
                    if (!emitDirectSpriteFrameAsset(options, &dataWin->sprt.sprites[spriteIndex], (uint32_t) spriteIndex, (uint32_t) frameIndex, item, rgba, (uint32_t) width, directSpriteFormatStates)) {
                        fprintf(stderr, "Failed to emit direct sprite asset for sprite %d frame %d\n", spriteIndex, frameIndex);
                        free(rgba);
                        free(items);
                        free(pages);
                        arrfree(fragments);
                        arrfree(tileRequests);
                        free(tileEntriesByRequest);
                        free(tileEntryWritten);
                        arrfree(packedTileEntries);
                        free(targetedMonoTPAGs);
                        free(fontTPAGs);
                        free(backgroundTPAGs);
                        freeTPAGDebugNames(tpagDebugNames, dataWin->tpag.count);
                        free(tpagToSpriteIndex);
                        free(tpagToSpriteFrameIndex);
                        free(tpagToBackgroundIndex);
                        free(tpagToFontIndex);
                        free(emittedSpriteFrames);
                        free(directSpriteFormatStates);
                        free(emittedBackgrounds);
                        free(emittedFonts);
                        repeat(packedPageCount, freeIndex) {
                            free(packedPages[freeIndex]->pixels);
                            free(packedPages[freeIndex]);
                        }
                        free(packedPages);
                        return false;
                    }
                    emittedSpriteFrames[itemIndex] = true;
                }
            }

            if (tpagToBackgroundIndex != NULL) {
                int32_t backgroundIndex = tpagToBackgroundIndex[itemIndex];
                if (backgroundIndex >= 0 && (uint32_t) backgroundIndex < dataWin->bgnd.count && !emittedBackgrounds[backgroundIndex]) {
                    if (!emitDirectBackgroundAsset(options, (uint32_t) backgroundIndex, item, rgba, (uint32_t) width)) {
                        fprintf(stderr, "Failed to emit direct background asset for background %d\n", backgroundIndex);
                        free(rgba);
                        free(items);
                        free(pages);
                        arrfree(fragments);
                        arrfree(tileRequests);
                        free(tileEntriesByRequest);
                        free(tileEntryWritten);
                        arrfree(packedTileEntries);
                        free(targetedMonoTPAGs);
                        free(fontTPAGs);
                        free(backgroundTPAGs);
                        freeTPAGDebugNames(tpagDebugNames, dataWin->tpag.count);
                        free(tpagToSpriteIndex);
                        free(tpagToSpriteFrameIndex);
                        free(tpagToBackgroundIndex);
                        free(tpagToFontIndex);
                        free(emittedSpriteFrames);
                        free(directSpriteFormatStates);
                        free(emittedBackgrounds);
                        free(emittedFonts);
                        repeat(packedPageCount, freeIndex) {
                            free(packedPages[freeIndex]->pixels);
                            free(packedPages[freeIndex]);
                        }
                        free(packedPages);
                        return false;
                    }
                    emittedBackgrounds[backgroundIndex] = true;
                }
            }

            if (tpagToFontIndex != NULL) {
                int32_t fontIndex = tpagToFontIndex[itemIndex];
                if (fontIndex >= 0 && (uint32_t) fontIndex < dataWin->font.count && !emittedFonts[fontIndex]) {
                    if (!emitDirectFontAsset(options, (uint32_t) fontIndex, item, rgba, (uint32_t) width)) {
                        fprintf(stderr, "Failed to emit direct font asset for font %d\n", fontIndex);
                        free(rgba);
                        free(items);
                        free(pages);
                        arrfree(fragments);
                        arrfree(tileRequests);
                        free(tileEntriesByRequest);
                        free(tileEntryWritten);
                        arrfree(packedTileEntries);
                        free(targetedMonoTPAGs);
                        free(fontTPAGs);
                        free(backgroundTPAGs);
                        freeTPAGDebugNames(tpagDebugNames, dataWin->tpag.count);
                        free(tpagToSpriteIndex);
                        free(tpagToSpriteFrameIndex);
                        free(tpagToBackgroundIndex);
                        free(tpagToFontIndex);
                        free(emittedSpriteFrames);
                        free(directSpriteFormatStates);
                        free(emittedBackgrounds);
                        free(emittedFonts);
                        repeat(packedPageCount, freeIndex) {
                            free(packedPages[freeIndex]->pixels);
                            free(packedPages[freeIndex]);
                        }
                        free(packedPages);
                        return false;
                    }
                    emittedFonts[fontIndex] = true;
                }
            }

            items[itemIndex].width = item->sourceWidth;
            items[itemIndex].height = item->sourceHeight;
            items[itemIndex].fragmentStart = fragmentStart;
            items[itemIndex].fragmentCount = (uint16_t) (arrlen(fragments) - fragmentStart);
        }

        repeat(tileRequestCount, reqIndex) {
            if (tileEntryWritten[reqIndex]) continue;
            TileLookupKey key = tileRequests[reqIndex];
            if (key.bgDef < 0 || (uint32_t) key.bgDef >= dataWin->bgnd.count) continue;

            Background* bg = &dataWin->bgnd.backgrounds[key.bgDef];
            if (bg->tpagIndex < 0 || (uint32_t) bg->tpagIndex >= dataWin->tpag.count) continue;

            TexturePageItem* item = &dataWin->tpag.items[bg->tpagIndex];
            if (item->texturePageId != (int16_t) i) continue;

            uint8_t* tilePixels = safeCalloc((size_t) key.srcW * key.srcH, 4);
            uint8_t* candidateLogical = safeCalloc((size_t) key.srcW * key.srcH, 4);
            uint8_t* candidateContent = safeCalloc((size_t) key.srcW * key.srcH, 4);
            uint32_t bgLogicalW = item->boundingWidth > 0 ? item->boundingWidth : item->sourceWidth;
            uint32_t bgLogicalH = item->boundingHeight > 0 ? item->boundingHeight : item->sourceHeight;
            if (bgLogicalW > 0 && bgLogicalH > 0) {
                uint8_t* bgLogicalPixels = safeCalloc((size_t) bgLogicalW * bgLogicalH, 4);
                if ((uint32_t) item->targetX < bgLogicalW &&
                    (uint32_t) item->targetY < bgLogicalH &&
                    item->sourceWidth > 0 &&
                    item->sourceHeight > 0) {
                    uint32_t copyW = item->sourceWidth;
                    uint32_t copyH = item->sourceHeight;
                    if ((uint32_t) item->targetX + copyW > bgLogicalW) copyW = bgLogicalW - (uint32_t) item->targetX;
                    if ((uint32_t) item->targetY + copyH > bgLogicalH) copyH = bgLogicalH - (uint32_t) item->targetY;
                    blitRect(
                        bgLogicalPixels,
                        bgLogicalW,
                        item->targetX,
                        item->targetY,
                        rgba,
                        (uint32_t) width,
                        item->sourceX,
                        item->sourceY,
                        copyW,
                        copyH
                    );
                }

                if ((uint32_t) key.srcX < bgLogicalW && (uint32_t) key.srcY < bgLogicalH) {
                    uint32_t copyW = key.srcW;
                    uint32_t copyH = key.srcH;
                    if ((uint32_t) key.srcX + copyW > bgLogicalW) copyW = bgLogicalW - (uint32_t) key.srcX;
                    if ((uint32_t) key.srcY + copyH > bgLogicalH) copyH = bgLogicalH - (uint32_t) key.srcY;
                    blitRect(
                        candidateLogical,
                        key.srcW,
                        0,
                        0,
                        bgLogicalPixels,
                        bgLogicalW,
                        key.srcX,
                        key.srcY,
                        copyW,
                        copyH
                    );
                }

                free(bgLogicalPixels);
            }

            if ((uint32_t) key.srcX < item->sourceWidth && (uint32_t) key.srcY < item->sourceHeight) {
                uint32_t copyW = key.srcW;
                uint32_t copyH = key.srcH;
                if ((uint32_t) key.srcX + copyW > item->sourceWidth) copyW = item->sourceWidth - (uint32_t) key.srcX;
                if ((uint32_t) key.srcY + copyH > item->sourceHeight) copyH = item->sourceHeight - (uint32_t) key.srcY;
                blitRect(
                    candidateContent,
                    key.srcW,
                    0,
                    0,
                    rgba,
                    (uint32_t) width,
                    item->sourceX + key.srcX,
                    item->sourceY + key.srcY,
                    copyW,
                    copyH
                );
            }

            uint32_t logicalCoverage = 0;
            uint32_t contentCoverage = 0;
            repeat((size_t) key.srcW * key.srcH, pxIndex) {
                if (candidateLogical[pxIndex * 4u + 3u] != 0) logicalCoverage++;
                if (candidateContent[pxIndex * 4u + 3u] != 0) contentCoverage++;
            }
            memcpy(tilePixels, contentCoverage > logicalCoverage ? candidateContent : candidateLogical, (size_t) key.srcW * key.srcH * 4u);
            free(candidateLogical);
            free(candidateContent);

            uint16_t pageIndex = 0;
            uint16_t dstX = 0;
            uint16_t dstY = 0;
            if (packRect(options, &packedPages, &packedPageCount, &pages, &totalPageCount, key.srcW, key.srcH, options->textureFormat, &pageIndex, &dstX, &dstY)) {
                PackedPage* dstPage = findPackedPage(packedPages, packedPageCount, pageIndex);
                requireNotNull(dstPage);
                appendPageDebugName(&pages[pageIndex], bg->name);
                pages[pageIndex].containsBackground = true;
                pages[pageIndex].containsNonMonoContent = true;
                blitRect(dstPage->pixels, dstPage->width, dstX, dstY, tilePixels, key.srcW, 0, 0, key.srcW, key.srcH);

                tileEntriesByRequest[reqIndex] = (OutputTileEntry) {
                    .bgDef = key.bgDef,
                    .srcX = key.srcX,
                    .srcY = key.srcY,
                    .srcW = key.srcW,
                    .srcH = key.srcH,
                    .atlasId = pageIndex,
                    .x = dstX,
                    .y = dstY,
                    .width = key.srcW,
                    .height = key.srcH,
                    .fragmentStart = UINT32_MAX,
                    .fragmentCount = 0,
                };
            } else {
                uint32_t fragmentStart = arrlen(fragments);
                uint32_t tileChunkSize = choosePageSize(options, key.srcW, key.srcH);
                for (uint32_t chunkY = 0; chunkY < key.srcH; chunkY += tileChunkSize) {
                    uint32_t fragHeight = key.srcH - chunkY;
                    if (fragHeight > tileChunkSize) fragHeight = tileChunkSize;
                    for (uint32_t chunkX = 0; chunkX < key.srcW; chunkX += tileChunkSize) {
                        uint32_t fragWidth = key.srcW - chunkX;
                        if (fragWidth > tileChunkSize) fragWidth = tileChunkSize;

                        if (!packRect(options, &packedPages, &packedPageCount, &pages, &totalPageCount, (uint16_t) fragWidth, (uint16_t) fragHeight, options->textureFormat, &pageIndex, &dstX, &dstY)) {
                            fprintf(stderr, "Failed to fragment oversized tile rect bg=%d src=(%u,%u %ux%u) chunk=(%u,%u %ux%u)\n",
                                key.bgDef,
                                key.srcX,
                                key.srcY,
                                key.srcW,
                                key.srcH,
                                chunkX,
                                chunkY,
                                fragWidth,
                                fragHeight
                            );
                            free(tilePixels);
                            free(items);
                            free(pages);
                            arrfree(fragments);
                            arrfree(tileRequests);
                            free(tileEntriesByRequest);
                            free(tileEntryWritten);
                            arrfree(packedTileEntries);
                            repeat(packedPageCount, pageIt) {
                                free(packedPages[pageIt]->pixels);
                                free(packedPages[pageIt]);
                            }
                            free(packedPages);
                            return false;
                        }

                        PackedPage* dstPage = findPackedPage(packedPages, packedPageCount, pageIndex);
                        requireNotNull(dstPage);
                        appendPageDebugName(&pages[pageIndex], bg->name);
                        pages[pageIndex].containsBackground = true;
                        pages[pageIndex].containsNonMonoContent = true;
                        blitRect(dstPage->pixels, dstPage->width, dstX, dstY, tilePixels, key.srcW, chunkX, chunkY, fragWidth, fragHeight);

                        OutputFragment fragment = {
                            .atlasId = pageIndex,
                            .x = dstX,
                            .y = dstY,
                            .width = (uint16_t) fragWidth,
                            .height = (uint16_t) fragHeight,
                            .sourceX = (uint16_t) chunkX,
                            .sourceY = (uint16_t) chunkY,
                        };
                        arrput(fragments, fragment);
                    }
                }

                tileEntriesByRequest[reqIndex] = (OutputTileEntry) {
                    .bgDef = key.bgDef,
                    .srcX = key.srcX,
                    .srcY = key.srcY,
                    .srcW = key.srcW,
                    .srcH = key.srcH,
                    .atlasId = UINT16_MAX,
                    .x = 0,
                    .y = 0,
                    .width = 0,
                    .height = 0,
                    .fragmentStart = fragmentStart,
                    .fragmentCount = (uint16_t) (arrlen(fragments) - fragmentStart),
                };
            }

            free(tilePixels);
            tileEntryWritten[reqIndex] = true;
        }

        free(rgba);
    }

    repeat(dataWin->tpag.count, i) {
        if (items[i].fragmentCount == UINT16_MAX) {
            fprintf(stderr, "TPAG item %zu was not mapped to any output page.\n", i);
            free(items);
            free(pages);
            arrfree(fragments);
            arrfree(tileRequests);
            free(tileEntriesByRequest);
            free(tileEntryWritten);
            arrfree(packedTileEntries);
            return false;
        }
    }

    repeat(tileRequestCount, i) {
        if (!tileEntryWritten[i]) {
            fprintf(stderr, "Warning: no packed legacy tile entry for bg=%d src=(%u,%u %ux%u)\n",
                tileRequests[i].bgDef,
                tileRequests[i].srcX,
                tileRequests[i].srcY,
                tileRequests[i].srcW,
                tileRequests[i].srcH
            );
            continue;
        }
        arrput(packedTileEntries, tileEntriesByRequest[i]);
    }

    bool ok = flushPackedPages(options, pages, packedPages, packedPageCount);
    if (ok) ok = finalizeDirectSpriteAssets(options, dataWin, directSpriteFormatStates);
    if (ok) ok = writePackedDirectTextureAssets(options, dataWin);
    if (ok) ok = writeAtlasFile(options->outputDir, totalPageCount, pages, dataWin->tpag.count, items, arrlen(fragments), fragments, arrlen(packedTileEntries), packedTileEntries, options->textureFormat);
    if (ok) ok = writeRoomManifestFile(
        options->outputDir,
        dataWin,
        totalPageCount,
        items,
        fragments,
        packedTileEntries,
        (uint32_t) arrlen(packedTileEntries)
    );
    free(items);
    free(pages);
    arrfree(fragments);
    arrfree(tileRequests);
    free(tileEntriesByRequest);
    free(tileEntryWritten);
    free(targetedMonoTPAGs);
    free(fontTPAGs);
    free(backgroundTPAGs);
    freeTPAGDebugNames(tpagDebugNames, dataWin->tpag.count);
    free(tpagToSpriteIndex);
    free(tpagToSpriteFrameIndex);
    free(tpagToBackgroundIndex);
    free(tpagToFontIndex);
    free(emittedSpriteFrames);
    free(directSpriteFormatStates);
    free(emittedBackgrounds);
    free(emittedFonts);
    arrfree(packedTileEntries);
    repeat(packedPageCount, i) {
        free(packedPages[i]->pixels);
        free(packedPages[i]);
    }
    free(packedPages);
    return ok;
}

#define N3DS_AUDIO_SAMPLE_RATE 32000u

typedef struct {
    uint8_t predictorScale;
    int16_t yn1;
    int16_t yn2;
} EncodedContext;

typedef struct {
    uint32_t sampleRate;
    uint32_t sampleCount;
    uint32_t channelCount;
    int16_t* interleavedPcm;
} WavPcm16;

typedef struct {
    uint8_t* data;
    uint32_t dataSize;
    uint16_t coefs[16];
    EncodedContext startContext;
    EncodedContext loopContext;
} EncodedBcwavChannel;

typedef struct {
    uint16_t coefs[16];
    uint32_t dataOffset;
    uint32_t dataSize;
    EncodedContext startContext;
    EncodedContext loopContext;
} ParsedBcwavChannel;

typedef struct {
    uint32_t sampleRate;
    uint32_t sampleCount;
    uint32_t loopStart;
    uint32_t loopEnd;
    bool loop;
    uint8_t channelCount;
    ParsedBcwavChannel channels[2];
} ParsedBcwav;

typedef struct {
    int16_t coef1;
    int16_t coef2;
} DspPredictorPair;

static N3DS_PREPROCESS_MAYBE_UNUSED bool loadWavPcm16(const char* path, WavPcm16* out);
static N3DS_PREPROCESS_MAYBE_UNUSED bool loadWavPcm16FromMemory(const uint8_t* data, uint32_t dataSize, WavPcm16* out);
static N3DS_PREPROCESS_MAYBE_UNUSED bool loadVorbisPcm16(const char* path, WavPcm16* out);
static N3DS_PREPROCESS_MAYBE_UNUSED bool loadVorbisPcm16FromMemory(const uint8_t* data, uint32_t dataSize, WavPcm16* out);
static N3DS_PREPROCESS_MAYBE_UNUSED bool loadAudioPcm16(const char* path, WavPcm16* out);
static N3DS_PREPROCESS_MAYBE_UNUSED bool loadAudioPcm16FromMemory(const uint8_t* data, uint32_t dataSize, WavPcm16* out);
static N3DS_PREPROCESS_MAYBE_UNUSED bool resampleWavPcm16(WavPcm16* wav, uint32_t targetRate);
static N3DS_PREPROCESS_MAYBE_UNUSED void freeWavPcm16(WavPcm16* wav);
static N3DS_PREPROCESS_MAYBE_UNUSED bool writeBcwavFile(const char* path, const WavPcm16* wav);

static const DspPredictorPair N3DS_DSP_PREDICTORS[8] = {
    { 0, 0 },
    { 2048, 0 },
    { 1024, 0 },
    { 3072, -1024 },
    { 4096, -2048 },
    { 3584, -1536 },
    { 1536, 512 },
    { 2560, -512 },
};

static uint16_t readLe16(const uint8_t* ptr) {
    return (uint16_t) (ptr[0] | (ptr[1] << 8));
}

static uint32_t readLe32(const uint8_t* ptr) {
    return (uint32_t) ptr[0] |
        ((uint32_t) ptr[1] << 8) |
        ((uint32_t) ptr[2] << 16) |
        ((uint32_t) ptr[3] << 24);
}

static int16_t readLeS16(const uint8_t* ptr) {
    return (int16_t) readLe16(ptr);
}

static void writeLe16(uint8_t* ptr, uint16_t value) {
    ptr[0] = (uint8_t) (value & 0xFFu);
    ptr[1] = (uint8_t) ((value >> 8) & 0xFFu);
}

static void writeLe32(uint8_t* ptr, uint32_t value) {
    ptr[0] = (uint8_t) (value & 0xFFu);
    ptr[1] = (uint8_t) ((value >> 8) & 0xFFu);
    ptr[2] = (uint8_t) ((value >> 16) & 0xFFu);
    ptr[3] = (uint8_t) ((value >> 24) & 0xFFu);
}

static int signExtend4(int nibble) {
    return (nibble & 0x8) ? (nibble - 16) : nibble;
}

static uint32_t align32(uint32_t value) {
    return (value + 31u) & ~31u;
}

static int clampNibble(int value) {
    if (value < -8) return -8;
    if (value > 7) return 7;
    return value;
}

static int roundDivSigned(int numerator, int denominator) {
    if (denominator <= 0) return 0;
    if (numerator >= 0) return (numerator + denominator / 2) / denominator;
    return -(((-numerator) + denominator / 2) / denominator);
}

static char* dupParentDir(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    if (slash == NULL) return safeStrdup(".");
    size_t length = (size_t) (slash - path);
    char* result = safeMalloc(length + 1);
    memcpy(result, path, length);
    result[length] = '\0';
    return result;
}

static char* joinPath(const char* base, const char* relativePath) {
    size_t baseLen = strlen(base);
    size_t relLen = strlen(relativePath);
    bool needSlash = baseLen > 0 && base[baseLen - 1] != '/' && base[baseLen - 1] != '\\';
    char* result = safeMalloc(baseLen + relLen + (needSlash ? 2 : 1));
    memcpy(result, base, baseLen);
    size_t cursor = baseLen;
    if (needSlash) result[cursor++] = '/';
    memcpy(result + cursor, relativePath, relLen);
    result[cursor + relLen] = '\0';
    return result;
}

static bool fileExists(const char* path) {
    if (path == NULL || path[0] == '\0') return false;
#if N3DS_PREPROCESS_HOST_WINDOWS
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
#endif
}

static bool writeBytesToFile(const char* path, const uint8_t* data, uint32_t size) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = fwrite(data, 1, size, file) == size;
    fclose(file);
    return ok;
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool readBytesFromFile(const char* path, uint8_t** outData, uint32_t* outSize) {
    if (outData == NULL || outSize == NULL) return false;
    *outData = NULL;
    *outSize = 0;

    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long sizeLong = ftell(file);
    if (sizeLong <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    uint8_t* data = safeMalloc((size_t) sizeLong);
    bool ok = fread(data, 1, (size_t) sizeLong, file) == (size_t) sizeLong;
    fclose(file);
    if (!ok) {
        free(data);
        return false;
    }

    *outData = data;
    *outSize = (uint32_t) sizeLong;
    return true;
}

static bool pathHasExtension(const char* path, const char* extension) {
    if (path == NULL || extension == NULL) return false;
    size_t pathLen = strlen(path);
    size_t extLen = strlen(extension);
    if (pathLen < extLen) return false;
    const char* suffix = path + pathLen - extLen;
    repeat(extLen, i) {
        char a = suffix[i];
        char b = extension[i];
        if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool isBuiltinAudioInputPath(const char* path) {
    return pathHasExtension(path, ".wav") ||
        pathHasExtension(path, ".ogg") ||
        pathHasExtension(path, ".oga");
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool writeBcwavFromInputFile(const char* inputPath, const char* outputPath) {
    WavPcm16 wav;
    if (!loadAudioPcm16(inputPath, &wav)) {
        fprintf(stderr, "In-process BCWAV writer could not decode audio: %s\n", inputPath);
        return false;
    }

    bool ok = writeBcwavFile(outputPath, &wav);
    freeWavPcm16(&wav);
    if (!ok) {
        fprintf(stderr, "In-process BCWAV writer failed to write %s\n", outputPath);
        return false;
    }
    return true;
}

static const char* pickAudioTempExtension(const Sound* sound) {
    const char* candidates[2] = { sound != NULL ? sound->file : NULL, sound != NULL ? sound->name : NULL };
    repeat(2, i) {
        const char* value = candidates[i];
        if (value == NULL || value[0] == '\0') continue;
        const char* dot = strrchr(value, '.');
        if (dot != NULL && dot[1] != '\0') return dot;
    }
    return ".ogg";
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool loadWavPcm16(const char* path, WavPcm16* out) {
    uint8_t* blob = NULL;
    uint32_t blobSize = 0;
    bool ok = readBytesFromFile(path, &blob, &blobSize) &&
        loadWavPcm16FromMemory(blob, blobSize, out);
    free(blob);
    return ok;
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool loadWavPcm16FromMemory(const uint8_t* blob, uint32_t blobSize, WavPcm16* out) {
    if (blob == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));

    if (blobSize < 44 || memcmp(blob, "RIFF", 4) != 0 || memcmp(blob + 8, "WAVE", 4) != 0) return false;

    uint16_t formatTag = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint32_t sampleRate = 0;
    const uint8_t* sampleData = NULL;
    uint32_t sampleDataSize = 0;
    size_t offset = 12;
    while (offset + 8 <= blobSize) {
        const uint8_t* chunk = blob + offset;
        uint32_t chunkSize = readLe32(chunk + 4);
        size_t payloadOffset = offset + 8;
        size_t nextOffset = payloadOffset + chunkSize + (chunkSize & 1u);
        if (payloadOffset + chunkSize > blobSize) return false;

        if (memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
            formatTag = readLe16(blob + payloadOffset + 0);
            channels = readLe16(blob + payloadOffset + 2);
            sampleRate = readLe32(blob + payloadOffset + 4);
            bitsPerSample = readLe16(blob + payloadOffset + 14);
        } else if (memcmp(chunk, "data", 4) == 0) {
            sampleData = blob + payloadOffset;
            sampleDataSize = chunkSize;
        }

        offset = nextOffset;
    }

    if (formatTag != 1 || channels == 0 || channels > 2 || sampleRate == 0 || bitsPerSample != 16 || sampleData == NULL || sampleDataSize == 0) {
        return false;
    }

    size_t sampleCount = sampleDataSize / sizeof(int16_t);
    out->interleavedPcm = safeMalloc(sampleCount * sizeof(int16_t));
    memcpy(out->interleavedPcm, sampleData, sampleCount * sizeof(int16_t));
    out->sampleRate = sampleRate;
    out->channelCount = channels;
    out->sampleCount = (uint32_t) (sampleCount / channels);
    return true;
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool loadVorbisPcm16(const char* path, WavPcm16* out) {
    uint8_t* blob = NULL;
    uint32_t blobSize = 0;
    bool ok = readBytesFromFile(path, &blob, &blobSize) &&
        loadVorbisPcm16FromMemory(blob, blobSize, out);
    free(blob);
    return ok;
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool loadVorbisPcm16FromMemory(const uint8_t* data, uint32_t dataSize, WavPcm16* out) {
    if (data == NULL || out == NULL || dataSize > INT_MAX) return false;
    memset(out, 0, sizeof(*out));

    int channels = 0;
    int sampleRate = 0;
    short* decoded = NULL;
    int sampleCount = stb_vorbis_decode_memory(data, (int) dataSize, &channels, &sampleRate, &decoded);
    if (sampleCount <= 0 || decoded == NULL || channels <= 0 || channels > 2 || sampleRate <= 0) {
        free(decoded);
        return false;
    }

    out->channelCount = (uint32_t) channels;
    out->sampleRate = (uint32_t) sampleRate;
    out->sampleCount = (uint32_t) sampleCount;
    out->interleavedPcm = (int16_t*) decoded;
    return true;
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool loadAudioPcm16(const char* path, WavPcm16* out) {
    uint8_t* blob = NULL;
    uint32_t blobSize = 0;
    bool ok = readBytesFromFile(path, &blob, &blobSize) &&
        loadAudioPcm16FromMemory(blob, blobSize, out);
    free(blob);
    return ok;
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool loadAudioPcm16FromMemory(const uint8_t* data, uint32_t dataSize, WavPcm16* out) {
    if (data == NULL || out == NULL || dataSize < 4) return false;
    if (dataSize >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0) {
        return loadWavPcm16FromMemory(data, dataSize, out);
    }
    if (memcmp(data, "OggS", 4) == 0) {
        return loadVorbisPcm16FromMemory(data, dataSize, out);
    }
    return loadVorbisPcm16FromMemory(data, dataSize, out);
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool resampleWavPcm16(WavPcm16* wav, uint32_t targetRate) {
    if (wav == NULL || wav->interleavedPcm == NULL || wav->sampleCount == 0 || wav->channelCount == 0 || targetRate == 0) return false;
    if (wav->sampleRate == 0) wav->sampleRate = targetRate;
    if (wav->sampleRate == targetRate) return true;

    uint64_t dstFrames64 = ((uint64_t) wav->sampleCount * targetRate + wav->sampleRate / 2u) / wav->sampleRate;
    if (dstFrames64 == 0 || dstFrames64 > UINT32_MAX) return false;

    uint32_t dstFrames = (uint32_t) dstFrames64;
    size_t dstSamples = (size_t) dstFrames * wav->channelCount;
    int16_t* dst = safeMalloc(dstSamples * sizeof(int16_t));

    repeat(dstFrames, dstFrame) {
        uint64_t srcPos = (uint64_t) dstFrame * wav->sampleRate;
        uint32_t srcFrame = (uint32_t) (srcPos / targetRate);
        uint32_t frac = (uint32_t) (srcPos % targetRate);
        if (srcFrame >= wav->sampleCount) srcFrame = wav->sampleCount - 1u;
        uint32_t nextFrame = srcFrame + 1u < wav->sampleCount ? srcFrame + 1u : srcFrame;

        repeat(wav->channelCount, channel) {
            int a = wav->interleavedPcm[(size_t) srcFrame * wav->channelCount + channel];
            int b = wav->interleavedPcm[(size_t) nextFrame * wav->channelCount + channel];
            int value = a + (int) (((int64_t) (b - a) * frac + targetRate / 2u) / targetRate);
            if (value < -32768) value = -32768;
            if (value > 32767) value = 32767;
            dst[(size_t) dstFrame * wav->channelCount + channel] = (int16_t) value;
        }
    }

    free(wav->interleavedPcm);
    wav->interleavedPcm = dst;
    wav->sampleCount = dstFrames;
    wav->sampleRate = targetRate;
    return true;
}

static N3DS_PREPROCESS_MAYBE_UNUSED void freeWavPcm16(WavPcm16* wav) {
    free(wav->interleavedPcm);
    memset(wav, 0, sizeof(*wav));
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool parseBcwavInfo(const uint8_t* info, uint32_t infoSize, uint32_t dataBlockOffset, uint32_t fileSize, ParsedBcwav* out) {
    memset(out, 0, sizeof(*out));
    if (infoSize < 0x20 || memcmp(info, "INFO", 4) != 0) return false;

    uint8_t encoding = info[0x08];
    if (encoding != 2) return false;

    out->loop = info[0x09] != 0;
    out->sampleRate = readLe32(info + 0x0C);
    out->loopStart = readLe32(info + 0x10);
    out->loopEnd = readLe32(info + 0x14);
    out->sampleCount = out->loopEnd;

    uint32_t tableOffset = 0x1C;
    uint32_t channelCount = readLe32(info + tableOffset);
    if (channelCount == 0 || channelCount > 2) return false;
    if (infoSize < tableOffset + 4 + channelCount * 8) return false;

    out->channelCount = (uint8_t) channelCount;
    uint32_t encodedBytes = ((out->sampleCount + 13u) / 14u) * 8u;

    repeat(channelCount, i) {
        const uint8_t* channelRef = info + tableOffset + 4 + i * 8;
        uint32_t channelInfoOffset = tableOffset + readLe32(channelRef + 4);
        if (channelInfoOffset + 0x14 > infoSize) return false;

        const uint8_t* channelInfo = info + channelInfoOffset;
        uint32_t sampleOffset = readLe32(channelInfo + 4);
        uint32_t adpcmInfoOffset = channelInfoOffset + readLe32(channelInfo + 12);
        if (adpcmInfoOffset + 0x2E > infoSize) return false;

        ParsedBcwavChannel* channel = &out->channels[i];
        channel->dataOffset = dataBlockOffset + 8 + sampleOffset;
        channel->dataSize = encodedBytes;
        if (channel->dataOffset + channel->dataSize > fileSize) return false;

        const uint8_t* adpcmInfo = info + adpcmInfoOffset;
        repeat(16, coef) {
            channel->coefs[coef] = readLe16(adpcmInfo + coef * 2);
        }
        channel->startContext.predictorScale = adpcmInfo[0x20];
        channel->startContext.yn1 = readLeS16(adpcmInfo + 0x22);
        channel->startContext.yn2 = readLeS16(adpcmInfo + 0x24);
        channel->loopContext.predictorScale = adpcmInfo[0x26];
        channel->loopContext.yn1 = readLeS16(adpcmInfo + 0x28);
        channel->loopContext.yn2 = readLeS16(adpcmInfo + 0x2A);
    }

    return true;
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool parseBcwavBlob(const uint8_t* data, uint32_t size, ParsedBcwav* out) {
    if (data == NULL || out == NULL) return false;
    if (size < 0x40 || memcmp(data, "CWAV", 4) != 0) return false;
    uint32_t infoOffset = readLe32(data + 0x18);
    uint32_t infoSize = readLe32(data + 0x1C);
    uint32_t dataOffset = readLe32(data + 0x24);
    if (infoOffset + infoSize > size || dataOffset + 8 > size) return false;
    return parseBcwavInfo(data + infoOffset, infoSize, dataOffset, size, out);
}

static void decodeBcwavFrame(const ParsedBcwavChannel* channel, const uint8_t* frame, int16_t* hist1, int16_t* hist2, int16_t outSamples[14]) {
    int predictor = frame[0] >> 4;
    int scale = 1 << (frame[0] & 0x0F);
    int coef1 = (int16_t) channel->coefs[predictor * 2 + 0];
    int coef2 = (int16_t) channel->coefs[predictor * 2 + 1];

    int sampleIndex = 0;
    for (int byteIndex = 1; byteIndex < 8; ++byteIndex) {
        int hi = signExtend4(frame[byteIndex] >> 4);
        int lo = signExtend4(frame[byteIndex] & 0x0F);
        int nibbles[2] = { hi, lo };
        repeat(2, nibbleIndex) {
            int sample = (nibbles[nibbleIndex] * scale) << 11;
            sample += 1024 + coef1 * (*hist1) + coef2 * (*hist2);
            sample >>= 11;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            outSamples[sampleIndex++] = (int16_t) sample;
            *hist2 = *hist1;
            *hist1 = (int16_t) sample;
        }
    }
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool decodeBcwavToPcm(const uint8_t* fileData, const ParsedBcwav* bcwav, WavPcm16* out) {
    if (fileData == NULL || bcwav == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));

    size_t totalSamples = (size_t) bcwav->sampleCount * bcwav->channelCount;
    int16_t* pcm = safeMalloc(totalSamples * sizeof(int16_t));
    if (pcm == NULL) return false;

    int16_t frameSamples[2][14];
    int16_t hist1[2] = {
        bcwav->channels[0].startContext.yn1,
        bcwav->channels[1].startContext.yn1,
    };
    int16_t hist2[2] = {
        bcwav->channels[0].startContext.yn2,
        bcwav->channels[1].startContext.yn2,
    };

    uint32_t frameCount = (bcwav->sampleCount + 13u) / 14u;
    uint32_t sampleCursor = 0;
    repeat(frameCount, frameIndex) {
        repeat(bcwav->channelCount, channelIndex) {
            const uint8_t* frame = fileData + bcwav->channels[channelIndex].dataOffset + frameIndex * 8u;
            decodeBcwavFrame(&bcwav->channels[channelIndex], frame, &hist1[channelIndex], &hist2[channelIndex], frameSamples[channelIndex]);
        }

        uint32_t samplesThisFrame = bcwav->sampleCount - sampleCursor;
        if (samplesThisFrame > 14u) samplesThisFrame = 14u;
        repeat(samplesThisFrame, sampleIndex) {
            if (bcwav->channelCount == 1) {
                pcm[sampleCursor + sampleIndex] = frameSamples[0][sampleIndex];
            } else {
                size_t outIndex = ((size_t) sampleCursor + sampleIndex) * 2u;
                pcm[outIndex + 0] = frameSamples[0][sampleIndex];
                pcm[outIndex + 1] = frameSamples[1][sampleIndex];
            }
        }
        sampleCursor += samplesThisFrame;
    }

    out->sampleRate = bcwav->sampleRate;
    out->sampleCount = bcwav->sampleCount;
    out->channelCount = bcwav->channelCount;
    out->interleavedPcm = pcm;
    return true;
}

static bool stringContainsIgnoreCase(const char* haystack, const char* needle) {
    if (haystack == NULL || needle == NULL || needle[0] == '\0') return false;
    size_t needleLen = strlen(needle);
    for (const char* cursor = haystack; *cursor != '\0'; ++cursor) {
        size_t matched = 0;
        while (matched < needleLen) {
            char a = cursor[matched];
            if (a == '\0') return false;
            char b = needle[matched];
            if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
            if (a != b) break;
            matched++;
        }
        if (matched == needleLen) return true;
    }
    return false;
}

static bool nameLooksLikeSfx(const char* value) {
    if (value == NULL || value[0] == '\0') return false;
    const char* base = strrchr(value, '/');
    const char* backslash = strrchr(value, '\\');
    if (backslash != NULL && (base == NULL || backslash > base)) base = backslash;
    base = (base != NULL) ? base + 1 : value;

    return strncmp(base, "mus_sfx", 7) == 0 ||
        strncmp(base, "sfx_", 4) == 0 ||
        strncmp(base, "snd_", 4) == 0 ||
        stringContainsIgnoreCase(base, "_sfx") ||
        stringContainsIgnoreCase(base, "sfx_");
}

static bool nameLooksLikeMusic(const char* value) {
    if (value == NULL || value[0] == '\0') return false;
    const char* base = strrchr(value, '/');
    const char* backslash = strrchr(value, '\\');
    if (backslash != NULL && (base == NULL || backslash > base)) base = backslash;
    base = (base != NULL) ? base + 1 : value;
    if (nameLooksLikeSfx(base)) return false;
    if (strncmp(base, "mus_", 4) == 0 || strncmp(base, "bgm_", 4) == 0) return true;
    return stringContainsIgnoreCase(base, "music");
}

static bool soundLooksLikeSfx(const Sound* sound) {
    if (sound == NULL) return false;
    return nameLooksLikeSfx(sound->name) ||
        nameLooksLikeSfx(sound->file) ||
        stringContainsIgnoreCase(sound->type, "sfx") ||
        stringContainsIgnoreCase(sound->type, "effect");
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool soundLooksLikeMusic(const Sound* sound) {
    if (sound == NULL) return false;
    if (soundLooksLikeSfx(sound)) return false;
    return nameLooksLikeMusic(sound->name) ||
        nameLooksLikeMusic(sound->file) ||
        stringContainsIgnoreCase(sound->type, "music") ||
        stringContainsIgnoreCase(sound->type, "stream");
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool extractBaseNameNoExt(const char* value, char* out, size_t outSize) {
    if (out == NULL || outSize == 0) return false;
    out[0] = '\0';
    if (value == NULL || value[0] == '\0') return false;

    const char* base = strrchr(value, '/');
    const char* backslash = strrchr(value, '\\');
    if (backslash != NULL && (base == NULL || backslash > base)) base = backslash;
    base = (base != NULL) ? base + 1 : value;
    if (base[0] == '\0') return false;

    const char* dot = strrchr(base, '.');
    size_t len = (dot != NULL && dot > base) ? (size_t) (dot - base) : strlen(base);
    if (len == 0 || len + 1 > outSize) return false;
    memcpy(out, base, len);
    out[len] = '\0';
    return true;
}

static bool directoryExists(const char* path) {
    if (path == NULL || path[0] == '\0') return false;
#if N3DS_PREPROCESS_HOST_WINDOWS
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

#if N3DS_PREPROCESS_HOST_WINDOWS
static bool deleteDirectoryRecursive(const char* path) {
    if (path == NULL || path[0] == '\0') return false;
    if (!directoryExists(path)) return true;

    char searchPath[1060];
    WIN32_FIND_DATAA findData;
    snprintf(searchPath, sizeof(searchPath), "%s\\*", path);
    HANDLE findHandle = FindFirstFileA(searchPath, &findData);
    if (findHandle != INVALID_HANDLE_VALUE) {
        do {
            const char* name = findData.cFileName;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

            char childPath[1060];
            snprintf(childPath, sizeof(childPath), "%s\\%s", path, name);
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (!deleteDirectoryRecursive(childPath)) {
                    FindClose(findHandle);
                    return false;
                }
            } else {
                if (!DeleteFileA(childPath)) {
                    FindClose(findHandle);
                    return false;
                }
            }
        } while (FindNextFileA(findHandle, &findData));
        FindClose(findHandle);
    }

    return RemoveDirectoryA(path) != 0;
}
#else
static bool deleteDirectoryRecursive(const char* path) {
    if (path == NULL || path[0] == '\0') return false;
    if (!directoryExists(path)) return true;

    DIR* dir = opendir(path);
    if (dir == NULL) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char childPath[1060];
        snprintf(childPath, sizeof(childPath), "%s/%s", path, name);
        if (directoryExists(childPath)) {
            if (!deleteDirectoryRecursive(childPath)) {
                closedir(dir);
                return false;
            }
        } else if (remove(childPath) != 0) {
            closedir(dir);
            return false;
        }
    }

    closedir(dir);
    return rmdir(path) == 0;
}
#endif

static void trimWhitespace(char* value) {
    if (value == NULL) return;

    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == '\r' || value[len - 1] == '\n' || isspace((unsigned char) value[len - 1]))) {
        value[--len] = '\0';
    }

    size_t start = 0;
    while (value[start] != '\0' && isspace((unsigned char) value[start])) start++;
    if (start > 0) memmove(value, value + start, strlen(value + start) + 1);
}

static void stripOptionalQuotes(char* value) {
    if (value == NULL) return;
    trimWhitespace(value);
    size_t len = strlen(value);
    if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
        memmove(value, value + 1, len - 2);
        value[len - 2] = '\0';
    }
}

static bool promptLine(const char* prompt, char* out, size_t outSize) {
    if (out == NULL || outSize == 0) return false;
    fprintf(stdout, "%s", prompt);
    fflush(stdout);
    if (fgets(out, (int) outSize, stdin) == NULL) return false;
    trimWhitespace(out);
    stripOptionalQuotes(out);
    return true;
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool getQuotedField(const char* line, int fieldIndex, char* out, size_t outSize) {
    if (line == NULL || out == NULL || outSize == 0 || fieldIndex < 0) return false;
    int currentField = 0;
    const char* cursor = line;
    while (*cursor != '\0') {
        while (*cursor != '\0' && *cursor != '"') cursor++;
        if (*cursor == '\0') return false;
        cursor++;

        char buffer[1024];
        size_t written = 0;
        while (*cursor != '\0' && *cursor != '"') {
            char c = *cursor++;
            if (c == '\\' && *cursor == '\\') c = *cursor++;
            if (written + 1 < sizeof(buffer)) buffer[written++] = c;
        }
        if (*cursor != '"') return false;
        cursor++;
        buffer[written] = '\0';

        if (currentField == fieldIndex) {
            snprintf(out, outSize, "%s", buffer);
            return true;
        }
        currentField++;
    }
    return false;
}

static bool pathLooksLikeRootDrive(const char* path) {
    return path != NULL &&
        ((strlen(path) == 2 && path[1] == ':') ||
            (strlen(path) == 3 && path[1] == ':' && (path[2] == '\\' || path[2] == '/')));
}

static bool resolveUndertaleDataWinPath(const char* candidate, char* dataWinPath, size_t dataWinPathSize) {
    if (candidate == NULL || candidate[0] == '\0') return false;

    if (fileExists(candidate)) {
        if (stringContainsIgnoreCase(candidate, "data.win")) {
            snprintf(dataWinPath, dataWinPathSize, "%s", candidate);
            return true;
        }
        return false;
    }

    if (!directoryExists(candidate)) return false;

    char directPath[1024];
#if N3DS_PREPROCESS_HOST_WINDOWS
    snprintf(directPath, sizeof(directPath), "%s\\data.win", candidate);
#else
    snprintf(directPath, sizeof(directPath), "%s/data.win", candidate);
#endif
    if (fileExists(directPath)) {
        snprintf(dataWinPath, dataWinPathSize, "%s", directPath);
        return true;
    }

#if N3DS_PREPROCESS_HOST_WINDOWS
    snprintf(directPath, sizeof(directPath), "%s\\UNDERTALE.exe", candidate);
#else
    snprintf(directPath, sizeof(directPath), "%s/UNDERTALE.exe", candidate);
#endif
    if (fileExists(directPath)) {
#if N3DS_PREPROCESS_HOST_WINDOWS
        snprintf(dataWinPath, dataWinPathSize, "%s\\data.win", candidate);
#else
        snprintf(dataWinPath, dataWinPathSize, "%s/data.win", candidate);
#endif
        if (fileExists(dataWinPath)) return true;
    }

    return false;
}

static bool normalizeInputDataWinPath(Options* options) {
    if (options == NULL || options->inputPath == NULL || options->inputPath[0] == '\0') return false;
    if (!resolveUndertaleDataWinPath(options->inputPath, options->inputPathStorage, sizeof(options->inputPathStorage))) {
        return false;
    }
    options->inputPath = options->inputPathStorage;
    return true;
}

#if N3DS_PREPROCESS_HOST_WINDOWS
static bool findUndertaleFromSteamRoot(const char* steamRoot, char* outDataWinPath, size_t outDataWinPathSize) {
    if (steamRoot == NULL || steamRoot[0] == '\0') return false;

    char undertaleDir[1024];
    snprintf(undertaleDir, sizeof(undertaleDir), "%s\\steamapps\\common\\Undertale", steamRoot);
    if (resolveUndertaleDataWinPath(undertaleDir, outDataWinPath, outDataWinPathSize)) return true;

    char libraryVdfPath[1024];
    snprintf(libraryVdfPath, sizeof(libraryVdfPath), "%s\\steamapps\\libraryfolders.vdf", steamRoot);
    FILE* file = fopen(libraryVdfPath, "rb");
    if (file == NULL) return false;

    char line[2048];
    while (fgets(line, sizeof(line), file) != NULL) {
        char first[1024];
        char second[1024];
        if (!getQuotedField(line, 0, first, sizeof(first))) continue;
        if (!getQuotedField(line, 1, second, sizeof(second))) continue;

        bool looksLikePathField = strcmp(first, "path") == 0;
        bool looksLikeLegacyLibraryField = true;
        for (size_t i = 0; first[i] != '\0'; ++i) {
            if (!isdigit((unsigned char) first[i])) {
                looksLikeLegacyLibraryField = false;
                break;
            }
        }
        if (!looksLikePathField && !looksLikeLegacyLibraryField) continue;

        char candidateDir[1024];
        snprintf(candidateDir, sizeof(candidateDir), "%s\\steamapps\\common\\Undertale", second);
        if (resolveUndertaleDataWinPath(candidateDir, outDataWinPath, outDataWinPathSize)) {
            fclose(file);
            return true;
        }
    }

    fclose(file);
    return false;
}

static bool autoDetectUndertaleDataWin(char* outDataWinPath, size_t outDataWinPathSize) {
    char steamRoot[1024];
    if (readRegistryString(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", steamRoot, sizeof(steamRoot))) {
        if (findUndertaleFromSteamRoot(steamRoot, outDataWinPath, outDataWinPathSize)) return true;
    }
    if (readRegistryString(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath", steamRoot, sizeof(steamRoot))) {
        if (findUndertaleFromSteamRoot(steamRoot, outDataWinPath, outDataWinPathSize)) return true;
    }

    const char* programFilesX86 = getenv("ProgramFiles(x86)");
    const char* programFiles = getenv("ProgramFiles");
    const char* fallbacks[] = {
        programFilesX86,
        programFiles,
        "C:\\Program Files (x86)",
        "C:\\Program Files",
    };
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i) {
        if (fallbacks[i] == NULL || fallbacks[i][0] == '\0') continue;
        snprintf(steamRoot, sizeof(steamRoot), "%s\\Steam", fallbacks[i]);
        if (findUndertaleFromSteamRoot(steamRoot, outDataWinPath, outDataWinPathSize)) return true;
    }

    return false;
}
#endif

static bool buildSdOutputDir(const char* sdInputPath, char* outOutputDir, size_t outOutputDirSize) {
    if (sdInputPath == NULL || sdInputPath[0] == '\0') return false;

    char temp[1024];
    snprintf(temp, sizeof(temp), "%s", sdInputPath);
    stripOptionalQuotes(temp);

    size_t len = strlen(temp);
    while (len > 0 && (temp[len - 1] == '/' || temp[len - 1] == '\\')) {
        if (pathLooksLikeRootDrive(temp) && len <= 3) break;
        temp[--len] = '\0';
    }

    if (pathLooksLikeRootDrive(temp)) {
        char driveRoot[4];
        snprintf(driveRoot, sizeof(driveRoot), "%c:", temp[0]);
        snprintf(
            outOutputDir,
            outOutputDirSize,
            "%s" N3DS_PREPROCESS_PATH_SEP "3ds" N3DS_PREPROCESS_PATH_SEP "cinnamon",
            driveRoot
        );
        return true;
    }

    const char* lastSlash = strrchr(temp, '/');
    const char* lastBackslash = strrchr(temp, '\\');
    const char* base = lastSlash;
    if (lastBackslash != NULL && (base == NULL || lastBackslash > base)) base = lastBackslash;
    base = (base != NULL) ? base + 1 : temp;

    if (stringContainsIgnoreCase(temp, "\\3ds\\cinnamon") || stringContainsIgnoreCase(temp, "/3ds/cinnamon")) {
        snprintf(outOutputDir, outOutputDirSize, "%s", temp);
        return true;
    }
    if (strcmp(base, "cinnamon") == 0) {
        snprintf(outOutputDir, outOutputDirSize, "%s", temp);
        return true;
    }
    if (strcmp(base, "3ds") == 0) {
        snprintf(outOutputDir, outOutputDirSize, "%s" N3DS_PREPROCESS_PATH_SEP "cinnamon", temp);
        return true;
    }

    snprintf(outOutputDir, outOutputDirSize, "%s" N3DS_PREPROCESS_PATH_SEP "3ds" N3DS_PREPROCESS_PATH_SEP "cinnamon", temp);
    return true;
}

static bool promptInteractivePaths(Options* options) {
    if (options == NULL) return false;

    fprintf(stdout, "Nintendo 3DS asset preprocessor\n\n");

#if N3DS_PREPROCESS_HOST_WINDOWS
    if (autoDetectUndertaleDataWin(options->inputPathStorage, sizeof(options->inputPathStorage))) {
        fprintf(stdout, "Detected Undertale at: %s\n", options->inputPathStorage);
    } else {
        char manualPath[1024];
        while (true) {
            if (!promptLine("Undertale was not auto-detected. Enter your Undertale folder or data.win path: ", manualPath, sizeof(manualPath))) {
                return false;
            }
            if (resolveUndertaleDataWinPath(manualPath, options->inputPathStorage, sizeof(options->inputPathStorage))) break;
            fprintf(stderr, "Could not find data.win there. Try the Undertale install folder or data.win directly.\n");
        }
    }
#else
    char manualPath[1024];
    while (true) {
        if (!promptLine("Enter your Undertale folder or data.win path: ", manualPath, sizeof(manualPath))) {
            return false;
        }
        if (resolveUndertaleDataWinPath(manualPath, options->inputPathStorage, sizeof(options->inputPathStorage))) break;
        fprintf(stderr, "Could not find data.win there. Try again.\n");
    }
#endif

    options->inputPath = options->inputPathStorage;

    char sdPath[1024];
    while (true) {
        if (!promptLine("Enter your SD card path (for example E:\\ or the mounted SD folder): ", sdPath, sizeof(sdPath))) {
            return false;
        }
        if (sdPath[0] == '\0') {
            fprintf(stderr, "Please enter a path.\n");
            continue;
        }
        if (!directoryExists(sdPath)) {
            fprintf(stderr, "That path does not exist. Enter the SD root or an existing folder on the SD card.\n");
            continue;
        }
        if (!buildSdOutputDir(sdPath, options->outputDirStorage, sizeof(options->outputDirStorage))) {
            fprintf(stderr, "Could not build an output path from that SD path.\n");
            continue;
        }
        break;
    }

    options->outputDir = options->outputDirStorage;

    fprintf(stdout, "Writing assets to: %s\n", options->outputDir);
    fprintf(stdout, "\n");
    return true;
}

static void encodeFrame(
    const int16_t* channelSamples,
    uint32_t sampleCount,
    uint32_t frameIndex,
    const DspPredictorPair* predictor,
    uint8_t shift,
    int16_t startHist1,
    int16_t startHist2,
    uint8_t outFrame[8],
    int16_t* outHist1,
    int16_t* outHist2,
    uint64_t* outError
) {
    int scale = 1 << shift;
    int16_t hist1 = startHist1;
    int16_t hist2 = startHist2;
    uint8_t frame[8];
    memset(frame, 0, sizeof(frame));
    frame[0] = (uint8_t) (((int) (predictor - N3DS_DSP_PREDICTORS) << 4) | (shift & 0x0F));
    uint64_t totalError = 0;

    repeat(14, i) {
        uint32_t sampleIndex = frameIndex * 14u + (uint32_t) i;
        int target = sampleIndex < sampleCount ? channelSamples[sampleIndex] : 0;
        int predicted = (1024 + predictor->coef1 * hist1 + predictor->coef2 * hist2) >> 11;
        int residual = target - predicted;
        int nibble = clampNibble(roundDivSigned(residual, scale));
        int decoded = (((nibble * scale) << 11) + 1024 + predictor->coef1 * hist1 + predictor->coef2 * hist2) >> 11;
        int error = target - decoded;
        totalError += (uint64_t) (error * error);

        hist2 = hist1;
        hist1 = (int16_t) decoded;

        uint8_t encodedNibble = (uint8_t) (nibble < 0 ? nibble + 16 : nibble);
        uint32_t byteIndex = 1u + (uint32_t) i / 2u;
        if ((i & 1) == 0) {
            frame[byteIndex] = (uint8_t) (encodedNibble << 4);
        } else {
            frame[byteIndex] |= encodedNibble;
        }
    }

    memcpy(outFrame, frame, sizeof(frame));
    *outHist1 = hist1;
    *outHist2 = hist2;
    *outError = totalError;
}

static bool encodeChannelDspAdpcm(const int16_t* channelSamples, uint32_t sampleCount, EncodedBcwavChannel* out) {
    memset(out, 0, sizeof(*out));
    uint32_t frameCount = (sampleCount + 13u) / 14u;
    out->dataSize = frameCount * 8u;
    out->data = safeMalloc(out->dataSize);
    out->startContext.predictorScale = 0;
    out->startContext.yn1 = 0;
    out->startContext.yn2 = 0;
    out->loopContext = out->startContext;

    repeat(8, i) {
        out->coefs[i * 2 + 0] = (uint16_t) N3DS_DSP_PREDICTORS[i].coef1;
        out->coefs[i * 2 + 1] = (uint16_t) N3DS_DSP_PREDICTORS[i].coef2;
    }

    int16_t hist1 = 0;
    int16_t hist2 = 0;
    repeat(frameCount, frameIndex) {
        uint8_t bestFrame[8];
        int16_t bestHist1 = hist1;
        int16_t bestHist2 = hist2;
        uint64_t bestError = UINT64_MAX;
        bool found = false;

        repeat(8, predictorIndex) {
            repeat(14, shift) {
                uint8_t frame[8];
                int16_t nextHist1 = hist1;
                int16_t nextHist2 = hist2;
                uint64_t error = 0;
                encodeFrame(
                    channelSamples,
                    sampleCount,
                    (uint32_t) frameIndex,
                    &N3DS_DSP_PREDICTORS[predictorIndex],
                    (uint8_t) shift,
                    hist1,
                    hist2,
                    frame,
                    &nextHist1,
                    &nextHist2,
                    &error
                );
                if (!found || error < bestError) {
                    memcpy(bestFrame, frame, sizeof(bestFrame));
                    bestHist1 = nextHist1;
                    bestHist2 = nextHist2;
                    bestError = error;
                    found = true;
                }
            }
        }

        if (!found) return false;
        memcpy(out->data + (size_t) frameIndex * 8u, bestFrame, 8u);
        if (frameIndex == 0) {
            out->startContext.predictorScale = bestFrame[0];
            out->loopContext = out->startContext;
        }
        hist1 = bestHist1;
        hist2 = bestHist2;
    }

    return true;
}

static void freeEncodedChannel(EncodedBcwavChannel* channel) {
    free(channel->data);
    memset(channel, 0, sizeof(*channel));
}

static N3DS_PREPROCESS_MAYBE_UNUSED bool writeBcwavFile(const char* path, const WavPcm16* wav) {
    EncodedBcwavChannel channels[2];
    memset(channels, 0, sizeof(channels));
    bool ok = false;
    uint32_t encodedSampleCount = ((wav->sampleCount + 13u) / 14u) * 14u;
    if (encodedSampleCount == 0) return false;

    repeat(wav->channelCount, channelIndex) {
        int16_t* mono = safeCalloc(encodedSampleCount, sizeof(int16_t));
        repeat(wav->sampleCount, sampleIndex) {
            mono[sampleIndex] = wav->interleavedPcm[(size_t) sampleIndex * wav->channelCount + channelIndex];
        }
        if (!encodeChannelDspAdpcm(mono, encodedSampleCount, &channels[channelIndex])) {
            free(mono);
            goto cleanup;
        }
        free(mono);
    }

    uint32_t headerSize = 0x40u;
    uint32_t infoOffset = headerSize;
    uint32_t channelInfoOffset = 0x20u + wav->channelCount * 8u;
    uint32_t adpcmInfoOffset = channelInfoOffset + wav->channelCount * 0x14u;
    uint32_t infoSize = align32(adpcmInfoOffset + wav->channelCount * 0x2Eu);
    uint32_t sampleDataSize = 0;
    repeat(wav->channelCount, i) {
        sampleDataSize += channels[i].dataSize;
    }
    uint32_t dataOffset = infoOffset + infoSize;
    uint32_t dataPayloadOffset = align32(dataOffset + 8u);
    uint32_t dataPayloadPadding = dataPayloadOffset - (dataOffset + 8u);
    uint32_t dataSize = 8u + dataPayloadPadding + sampleDataSize;
    uint32_t fileSize = dataOffset + dataSize;
    uint32_t sampleRate = wav->sampleRate != 0 ? wav->sampleRate : N3DS_AUDIO_SAMPLE_RATE;

    uint8_t* blob = safeCalloc(1, fileSize);
    memcpy(blob, "CWAV", 4);
    writeLe16(blob + 4, 0xFEFFu);
    writeLe16(blob + 6, (uint16_t) headerSize);
    writeLe32(blob + 8, CWAV_VERSION);
    writeLe32(blob + 0x0C, fileSize);
    writeLe16(blob + 0x10, 2u);
    writeLe16(blob + 0x14, CWAV_REF_INFO_BLOCK);
    writeLe32(blob + 0x18, infoOffset);
    writeLe32(blob + 0x1C, infoSize);
    writeLe16(blob + 0x20, CWAV_REF_DATA_BLOCK);
    writeLe32(blob + 0x24, dataOffset);
    writeLe32(blob + 0x28, dataSize);

    uint8_t* info = blob + infoOffset;
    memcpy(info, "INFO", 4);
    writeLe32(info + 4, infoSize);
    info[0x08] = 2;
    info[0x09] = 0;
    writeLe32(info + 0x0C, sampleRate);
    writeLe32(info + 0x10, 0);
    writeLe32(info + 0x14, encodedSampleCount);
    writeLe32(info + 0x1C, wav->channelCount);

    uint32_t sampleOffset = 0;
    repeat(wav->channelCount, channelIndex) {
        uint8_t* ref = info + 0x20u + channelIndex * 8u;
        uint32_t channelInfoPos = channelInfoOffset + channelIndex * 0x14u;
        uint32_t adpcmInfoPos = adpcmInfoOffset + channelIndex * 0x2Eu;
        writeLe16(ref + 0, CWAV_REF_CHANNEL_INFO);
        writeLe32(ref + 4, channelInfoPos - 0x1Cu);

        uint8_t* channelInfo = info + channelInfoPos;
        writeLe16(channelInfo + 0, CWAV_REF_SAMPLE_DATA);
        writeLe32(channelInfo + 4, dataPayloadPadding + sampleOffset);
        writeLe16(channelInfo + 8, CWAV_REF_DSP_ADPCM_INFO);
        writeLe32(channelInfo + 12, adpcmInfoPos - channelInfoPos);

        uint8_t* adpcmInfo = info + adpcmInfoPos;
        repeat(16, coefIndex) {
            writeLe16(adpcmInfo + coefIndex * 2u, channels[channelIndex].coefs[coefIndex]);
        }
        adpcmInfo[0x20] = channels[channelIndex].startContext.predictorScale;
        writeLe16(adpcmInfo + 0x22, (uint16_t) channels[channelIndex].startContext.yn1);
        writeLe16(adpcmInfo + 0x24, (uint16_t) channels[channelIndex].startContext.yn2);
        adpcmInfo[0x26] = channels[channelIndex].loopContext.predictorScale;
        writeLe16(adpcmInfo + 0x28, (uint16_t) channels[channelIndex].loopContext.yn1);
        writeLe16(adpcmInfo + 0x2A, (uint16_t) channels[channelIndex].loopContext.yn2);

        sampleOffset += channels[channelIndex].dataSize;
    }

    uint8_t* dataBlock = blob + dataOffset;
    memcpy(dataBlock, "DATA", 4);
    writeLe32(dataBlock + 4, dataSize);
    uint32_t dataCursor = dataPayloadOffset;
    repeat(wav->channelCount, channelIndex) {
        memcpy(blob + dataCursor, channels[channelIndex].data, channels[channelIndex].dataSize);
        dataCursor += channels[channelIndex].dataSize;
    }

    ok = writeBytesToFile(path, blob, fileSize);
    free(blob);

cleanup:
    repeat(2, i) freeEncodedChannel(&channels[i]);
    return ok;
}

static char* resolveExternalSoundPath(const Options* options, const Sound* sound) {
    char* inputDir = dupParentDir(options->inputPath);
    const char* candidates[2] = { sound->file, sound->name };

    repeat(2, candidateIndex) {
        const char* base = candidates[candidateIndex];
        if (base == NULL || base[0] == '\0') continue;

        char* direct = joinPath(inputDir, base);
        if (fileExists(direct)) {
            free(inputDir);
            return direct;
        }
        free(direct);

        if (strchr(base, '.') == NULL) {
            const char* exts[] = { ".ogg", ".wav", ".mp3" };
            repeat(3, i) {
                char relative[1024];
                snprintf(relative, sizeof(relative), "%s%s", base, exts[i]);
                char* candidate = joinPath(inputDir, relative);
                if (fileExists(candidate)) {
                    free(inputDir);
                    return candidate;
                }
                free(candidate);
            }
        }
    }

    free(inputDir);
    return NULL;
}

static bool loadPcmForSoundBank(
    const Options* options,
    const Sound* sound,
    const uint8_t* embeddedData,
    uint32_t embeddedDataSize,
    size_t soundIndex,
    WavPcm16* outPcm
) {
    if (options == NULL || sound == NULL || outPcm == NULL) return false;
    memset(outPcm, 0, sizeof(*outPcm));

    char* externalSourcePath = resolveExternalSoundPath(options, sound);
    const char* inputPath = externalSourcePath;
    char tempInputPath[1024];
    bool wroteTempInput = false;

    if (inputPath == NULL && embeddedData != NULL && embeddedDataSize > 0) {
        if (loadAudioPcm16FromMemory(embeddedData, embeddedDataSize, outPcm)) {
            free(externalSourcePath);
            return true;
        }

        const char* tempExt = pickAudioTempExtension(sound);
        snprintf(tempInputPath, sizeof(tempInputPath), "%s/audio/__tmp_bank_input_%05zu%s", options->outputDir, soundIndex, tempExt);
        remove(tempInputPath);
        if (!writeBytesToFile(tempInputPath, embeddedData, embeddedDataSize)) {
            free(externalSourcePath);
            return false;
        }
        inputPath = tempInputPath;
        wroteTempInput = true;
    }

    if (inputPath == NULL) {
        free(externalSourcePath);
        return false;
    }

    bool ok = loadAudioPcm16(inputPath, outPcm);

    if (wroteTempInput) remove(tempInputPath);
    free(externalSourcePath);
    return ok;
}

static bool buildPackedSoundBank(Options* options, DataWin* dataWin) {
    if (options == NULL || dataWin == NULL) return false;

    uint32_t soundCount = (uint32_t) dataWin->sond.count;
    uint32_t headerSize = 16u + soundCount * 20u;
    uint8_t* header = safeCalloc(1, headerSize);
    uint32_t* offsets = safeCalloc(soundCount, sizeof(uint32_t));
    uint32_t* sizes = safeCalloc(soundCount, sizeof(uint32_t));
    uint32_t* sampleRates = safeCalloc(soundCount, sizeof(uint32_t));
    uint32_t* sampleCounts = safeCalloc(soundCount, sizeof(uint32_t));
    uint32_t* flags = safeCalloc(soundCount, sizeof(uint32_t));
    char bankPath[1024];
    FILE* bankFile = NULL;
    bool ok = false;
    uint32_t packedCount = 0;
    uint32_t packedBytes = 0;

    snprintf(bankPath, sizeof(bankPath), "%s/audio/sound_bank.bin", options->outputDir);
    bankFile = fopen(bankPath, "wb");
    if (bankFile == NULL) goto cleanup;
    if (fseek(bankFile, (long) headerSize, SEEK_SET) != 0) goto cleanup;

    uint32_t cursor = headerSize;
    repeat(dataWin->sond.count, soundIndex) {
        const Sound* sound = &dataWin->sond.sounds[soundIndex];
        if (soundLooksLikeMusic(sound)) continue;

        WavPcm16 pcm;
        const uint8_t* sourceData = NULL;
        uint32_t sourceSize = 0;
        if (sound->audioFile >= 0 && (uint32_t) sound->audioFile < dataWin->audo.count) {
            AudioEntry* entry = &dataWin->audo.entries[sound->audioFile];
            sourceData = entry->data;
            sourceSize = entry->dataSize;
        }

        if (!loadPcmForSoundBank(
            options,
            sound,
            sourceData,
            sourceSize,
            soundIndex,
            &pcm
        )) {
            if (sound->name != NULL && sound->name[0] != '\0') {
                fprintf(stderr, "Skipping sound bank entry %zu (%s): unsupported or missing audio source\n", soundIndex, sound->name);
            }
            continue;
        }

        if (!resampleWavPcm16(&pcm, N3DS_AUDIO_SAMPLE_RATE)) {
            fprintf(stderr, "Skipping sound bank entry %zu (%s): failed to resample audio to %u Hz\n", soundIndex, sound->name != NULL ? sound->name : "<unnamed>", N3DS_AUDIO_SAMPLE_RATE);
            freeWavPcm16(&pcm);
            continue;
        }

        uint32_t sampleRate = pcm.sampleRate != 0 ? pcm.sampleRate : N3DS_AUDIO_SAMPLE_RATE;
        uint32_t sampleCount = pcm.sampleCount;
        uint32_t channelCount = pcm.channelCount;
        uint32_t pcmBytes = (uint32_t) ((size_t) pcm.sampleCount * pcm.channelCount * sizeof(int16_t));
        if (fwrite(pcm.interleavedPcm, 1, pcmBytes, bankFile) != pcmBytes) {
            freeWavPcm16(&pcm);
            goto cleanup;
        }
        freeWavPcm16(&pcm);

        offsets[soundIndex] = cursor;
        sizes[soundIndex] = pcmBytes;
        sampleRates[soundIndex] = sampleRate;
        sampleCounts[soundIndex] = sampleCount;
        flags[soundIndex] =
            channelCount |
            N3DS_SOUND_BANK_ENTRY_FLAG_PCM16 |
            0u;
        cursor += pcmBytes;
        packedCount += 1u;
        packedBytes += pcmBytes;
    }

    writeLe32(header + 0u, N3DS_SOUND_BANK_MAGIC);
    writeLe32(header + 4u, N3DS_SOUND_BANK_VERSION);
    writeLe32(header + 8u, soundCount);
    writeLe32(header + 12u, headerSize);
    repeat(soundCount, soundIndex) {
        uint32_t entryOffset = 16u + soundIndex * 20u;
        writeLe32(header + entryOffset + 0u, offsets[soundIndex]);
        writeLe32(header + entryOffset + 4u, sizes[soundIndex]);
        writeLe32(header + entryOffset + 8u, sampleRates[soundIndex]);
        writeLe32(header + entryOffset + 12u, sampleCounts[soundIndex]);
        writeLe32(header + entryOffset + 16u, flags[soundIndex]);
    }

    if (fseek(bankFile, 0, SEEK_SET) != 0) goto cleanup;
    if (fwrite(header, 1, headerSize, bankFile) != headerSize) goto cleanup;

    fprintf(
        stderr,
        "Packed %u sounds into %s (%u bytes)\n",
        packedCount,
        bankPath,
        packedBytes
    );
    ok = true;

cleanup:
    free(header);
    free(offsets);
    free(sizes);
    free(sampleRates);
    free(sampleCounts);
    free(flags);
    if (bankFile != NULL) fclose(bankFile);
    return ok;
}

static bool convertStreamedMusicBcwavs(Options* options, DataWin* dataWin) {
    if (options == NULL || dataWin == NULL) return false;

    bool allOk = true;
    uint32_t convertedCount = 0;
    uint32_t preservedCount = 0;
    uint32_t skippedCount = 0;

    repeat(dataWin->sond.count, soundIndex) {
        const Sound* sound = &dataWin->sond.sounds[soundIndex];
        if (!soundLooksLikeMusic(sound)) continue;

        char baseName[256];
        bool haveBaseName = extractBaseNameNoExt(sound->file, baseName, sizeof(baseName));
        if (!haveBaseName) haveBaseName = extractBaseNameNoExt(sound->name, baseName, sizeof(baseName));
        if (!haveBaseName) snprintf(baseName, sizeof(baseName), "sound_%05zu", soundIndex);

        char outPath[1024];
        snprintf(outPath, sizeof(outPath), "%s/%s.bcwav", options->outputDir, baseName);

        char* externalSourcePath = resolveExternalSoundPath(options, sound);
        if (externalSourcePath == NULL) {
            if (fileExists(outPath)) {
                preservedCount++;
                continue;
            }
            if (sound->name != NULL && sound->name[0] != '\0') {
                fprintf(stderr, "Skipping streamed music %zu (%s): no external audio source found\n", soundIndex, sound->name);
            }
            skippedCount++;
            continue;
        }

        remove(outPath);
        fprintf(
            stderr,
            "n3ds-preprocess: music %05zu -> %s [%s]\n",
            soundIndex,
            outPath,
            externalSourcePath
        );

        bool ok = writeBcwavFromInputFile(externalSourcePath, outPath);
        free(externalSourcePath);

        if (!ok) {
            fprintf(stderr, "Failed to write streamed music BCWAV for sound %zu\n", soundIndex);
            allOk = false;
            continue;
        }

        convertedCount++;
    }

    fprintf(
        stderr,
        "Streamed music BCWAVs: converted=%u preserved=%u skipped=%u\n",
        convertedCount,
        preservedCount,
        skippedCount
    );
    return allOk;
}

static bool convertAudio(Options* options, DataWin* dataWin) {
    fprintf(stderr, "n3ds-preprocess: processing audio\n");
    fprintf(stderr, "Using in-process audio conversion (WAV and Ogg Vorbis supported; other codecs unavailable).\n");

    bool ok = convertStreamedMusicBcwavs(options, dataWin);
    if (ok) ok = buildPackedSoundBank(options, dataWin);
    if (ok) fprintf(stderr, "Packed SFX audio into PCM16 sound_bank.bin at %u Hz\n", N3DS_AUDIO_SAMPLE_RATE);
    return ok;
}

int main(int argc, char** argv) {
    Options options;
    if (!parseArgs(argc, argv, &options)) {
        printUsage(argv[0]);
        return 1;
    }

    if (options.interactiveMode && !promptInteractivePaths(&options)) {
        fprintf(stderr, "Interactive setup was cancelled or failed.\n");
        return 1;
    }

    if (options.inputPath == NULL || options.outputDir == NULL) {
        printUsage(argv[0]);
        return 1;
    }

    if (!normalizeInputDataWinPath(&options)) {
        fprintf(stderr, "Could not resolve data.win from: %s\n", options.inputPath);
        fprintf(stderr, "Pass either the Undertale folder or the full path to data.win.\n");
        return 1;
    }

    if (!configureStagedOutputDir(&options, argv[0])) {
        fprintf(stderr, "Failed to configure local staging output directory.\n");
        return 1;
    }
    if (!configureSpriteReplacementDir(&options, argv[0])) {
        fprintf(stderr, "Failed to configure sprite replacement directory.\n");
        return 1;
    }
    if (!configureBorderAssetDir(&options, argv[0])) {
        fprintf(stderr, "Failed to configure border asset directory.\n");
        return 1;
    }
    if (options.stageOutputLocally) {
        fprintf(stderr, "Staging generated assets in: %s\n", options.outputDir);
        fprintf(stderr, "Final destination: %s\n", options.finalOutputDirStorage);
    }

    if (!ensureOutputDirs(options.outputDir)) return 1;
    if (!convertBorderAssets(&options)) return 1;

    DataWin* dataWin = DataWin_parse(
        options.inputPath,
        (DataWinParserOptions) {
            .parseGen8 = true,
            .parseSond = true,
            .parseSprt = true,
            .parseBgnd = true,
            .parseFont = true,
            .parseRoom = true,
            .parseTpag = true,
            .parseStrg = true,
            .parseTxtr = true,
            .parseAudo = true,
            .skipLoadingPreciseMasksForNonPreciseSprites = true,
        }
    );
    if (dataWin == NULL) {
        fprintf(stderr, "Failed to parse %s\n", options.inputPath);
        return 1;
    }

    bool ok = convertTextures(&options, dataWin);
    if (ok) ok = convertAudio(&options, dataWin);
    DataWin_free(dataWin);

    bool syncedStaging = true;
    if (options.stageOutputLocally) {
        syncedStaging = ok && syncStagedOutputToDestination(&options);
        ok = syncedStaging;
    }
    if (syncedStaging && options.stageOutputLocally && options.stagingOutputDirStorage[0] != '\0' && directoryExists(options.stagingOutputDirStorage)) {
        if (!deleteDirectoryRecursive(options.stagingOutputDirStorage)) {
            fprintf(stderr, "Warning: failed to remove staging directory: %s\n", options.stagingOutputDirStorage);
        }
    } else if (options.stageOutputLocally && options.stagingOutputDirStorage[0] != '\0' && directoryExists(options.stagingOutputDirStorage)) {
        fprintf(stderr, "Preserving staging directory after failure: %s\n", options.stagingOutputDirStorage);
    }

    if (!ok) return 1;
    const char* finalOutputDir = options.stageOutputLocally ? options.finalOutputDirStorage : options.outputDir;
    fprintf(stderr, "Wrote 3DS assets to %s/gfx and %s/audio\n", finalOutputDir, finalOutputDir);
    return 0;
}
