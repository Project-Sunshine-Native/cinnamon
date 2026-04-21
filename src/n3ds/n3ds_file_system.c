#include "n3ds_file_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ===[ Helpers ]===

static char* buildFullPath(N3DSFileSystem* fs, const char* relativePath) {
    if (strncmp(relativePath, "sdmc:/", 6) == 0)
        return strdup(relativePath);
    size_t baseLen = strlen(fs->basePath);
    size_t relLen = strlen(relativePath);
    char* fullPath = safeMalloc(baseLen + relLen + 1);
    memcpy(fullPath, fs->basePath, baseLen);
    memcpy(fullPath + baseLen, relativePath, relLen);
    fullPath[baseLen + relLen] = '\0';
    return fullPath;
}

// ===[ Vtable Implementations ]===

static char* n3dsResolvePath(FileSystem* fs, const char* relativePath) {
    return buildFullPath((N3DSFileSystem*) fs, relativePath);
}

static bool n3dsFileExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((N3DSFileSystem*) fs, relativePath);
    struct stat st;
    bool exists = (stat(fullPath, &st) == 0);
    free(fullPath);
    return exists;
}

static char* n3dsReadFileText(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((N3DSFileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);
    if (f == NULL)
        return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* content = safeMalloc((size_t) size + 1);
    if (!content) {
        fclose(f);
        printf("malloc failed for file\n");
        return NULL;
    }
    size_t bytesRead = fread(content, 1, (size_t) size, f);
    content[bytesRead] = '\0';
    fclose(f);
    printf("%.20s\n", content);
    printf("%02X %02X %02X %c\n",
        (unsigned char)content[0],
        (unsigned char)content[1],
        (unsigned char)content[2],
        content[3]);
    return content;
}

static bool n3dsWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    char* fullPath = buildFullPath((N3DSFileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);
    if (f == NULL)
        return false;

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);
    return written == len;
}

static bool n3dsDeleteFile(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((N3DSFileSystem*) fs, relativePath);
    int result = remove(fullPath);
    free(fullPath);
    return result == 0;
}

// ===[ Vtable ]===

static FileSystemVtable N3DSFileSystemVtable = {
    .resolvePath = n3dsResolvePath,
    .fileExists = n3dsFileExists,
    .readFileText = n3dsReadFileText,
    .writeFileText = n3dsWriteFileText,
    .deleteFile = n3dsDeleteFile,
};

// ===[ Lifecycle ]===

N3DSFileSystem* N3DSFileSystem_create(const char* dataWinPath) {
    N3DSFileSystem* fs = safeCalloc(1, sizeof(N3DSFileSystem));
    fs->base.vtable = &N3DSFileSystemVtable;

    // Derive basePath by stripping the filename from dataWinPath
    const char* lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash != NULL) {
        size_t dirLen = (size_t) (lastSlash - dataWinPath + 1); // include the trailing /
        fs->basePath = safeMalloc(dirLen + 1);
        memcpy(fs->basePath, dataWinPath, dirLen);
        fs->basePath[dirLen] = '\0';
    } else {
        // data.win is in current directory
        fs->basePath = strdup("./");
    }

    return fs;
}

void N3DSFileSystem_destroy(N3DSFileSystem* fs) {
    if (fs == NULL) return;
    free(fs->basePath);
    free(fs);
}
