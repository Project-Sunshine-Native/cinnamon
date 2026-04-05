#include "wiiu_file_system.h"
#include "../utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

__attribute__((weak)) void WiiUFileSystem_platformBootLog(const char* message) {
    (void) message;
}

static void WiiUFileSystem_bootLog(const char* message) {
    WiiUFileSystem_platformBootLog(message);
}

static char* WiiUFileSystem_buildFullPath(WiiUFileSystem* fs, const char* relativePath) {
    if (relativePath == NULL) return NULL;
    if (strncmp(relativePath, "fs:/", 4) == 0 || strncmp(relativePath, "/vol/", 5) == 0) {
        return safeStrdup(relativePath);
    }

    size_t baseLen = strlen(fs->basePath);
    size_t relLen = strlen(relativePath);
    char* fullPath = safeMalloc(baseLen + relLen + 1);
    memcpy(fullPath, fs->basePath, baseLen);
    memcpy(fullPath + baseLen, relativePath, relLen);
    fullPath[baseLen + relLen] = '\0';
    return fullPath;
}

static char* WiiUFileSystem_resolvePath(FileSystem* fs, const char* relativePath) {
    return WiiUFileSystem_buildFullPath((WiiUFileSystem*) fs, relativePath);
}

static bool WiiUFileSystem_fileExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = WiiUFileSystem_buildFullPath((WiiUFileSystem*) fs, relativePath);
    struct stat st;
    bool exists = stat(fullPath, &st) == 0;
    free(fullPath);
    return exists;
}

static char* WiiUFileSystem_readFileText(FileSystem* fs, const char* relativePath) {
    char* fullPath = WiiUFileSystem_buildFullPath((WiiUFileSystem*) fs, relativePath);
    FILE* file = fopen(fullPath, "rb");
    free(fullPath);
    if (file == NULL) return NULL;

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        return NULL;
    }

    char* text = safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(text, 1, (size_t) size, file);
    text[bytesRead] = '\0';
    fclose(file);
    return text;
}

static bool WiiUFileSystem_writeFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    char* fullPath = WiiUFileSystem_buildFullPath((WiiUFileSystem*) fs, relativePath);
    FILE* file = fopen(fullPath, "wb");
    free(fullPath);
    if (file == NULL) return false;

    size_t length = strlen(contents);
    size_t written = fwrite(contents, 1, length, file);
    bool ok = written == length;
    if (ok) {
        ok = fflush(file) == 0;
    }
    if (ok) {
        ok = fsync(fileno(file)) == 0;
    }
    bool closeOk = fclose(file) == 0;
    return ok && closeOk;
}

static bool WiiUFileSystem_deleteFile(FileSystem* fs, const char* relativePath) {
    char* fullPath = WiiUFileSystem_buildFullPath((WiiUFileSystem*) fs, relativePath);
    int result = remove(fullPath);
    free(fullPath);
    return result == 0;
}

static FileSystemVtable WiiUFileSystemVtable = {
    .resolvePath = WiiUFileSystem_resolvePath,
    .fileExists = WiiUFileSystem_fileExists,
    .readFileText = WiiUFileSystem_readFileText,
    .writeFileText = WiiUFileSystem_writeFileText,
    .deleteFile = WiiUFileSystem_deleteFile,
};

WiiUFileSystem* WiiUFileSystem_create(const char* dataWinPath) {
    WiiUFileSystem_bootLog("wiiu_fs: create begin");
    WiiUFileSystem* fs = safeCalloc(1, sizeof(WiiUFileSystem));
    fs->base.vtable = &WiiUFileSystemVtable;

    const char* lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash != NULL) {
        size_t dirLen = (size_t) (lastSlash - dataWinPath + 1);
        fs->basePath = safeMalloc(dirLen + 1);
        memcpy(fs->basePath, dataWinPath, dirLen);
        fs->basePath[dirLen] = '\0';
    } else {
        fs->basePath = safeStrdup("./");
    }

    WiiUFileSystem_bootLog("wiiu_fs: create end");
    return fs;
}

void WiiUFileSystem_destroy(WiiUFileSystem* fs) {
    if (fs == NULL) return;
    free(fs->basePath);
    free(fs);
}
