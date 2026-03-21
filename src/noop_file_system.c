#include "noop_file_system.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

#include "stb_ds.h"

// ===[ In-Memory File Storage ]===

typedef struct {
    char* key; // file path
    char* value; // file contents
} MemoryFileEntry;

typedef struct {
    FileSystem base;
    MemoryFileEntry* files; // stb_ds string hashmap
} NoopFileSystem;

// ===[ Vtable Implementations ]===

static char* noopResolvePath([[maybe_unused]] FileSystem* fs, [[maybe_unused]] const char* relativePath) {
    return safeStrdup("./");
}

static bool noopFileExists(FileSystem* fs, const char* relativePath) {
    NoopFileSystem* nfs = (NoopFileSystem*) fs;
    return shgeti(nfs->files, relativePath) >= 0;
}

static char* noopReadFileText(FileSystem* fs, const char* relativePath) {
    NoopFileSystem* nfs = (NoopFileSystem*) fs;
    ptrdiff_t idx = shgeti(nfs->files, relativePath);
    if (0 > idx)
        return nullptr;
    return safeStrdup(nfs->files[idx].value);
}

static bool noopWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    NoopFileSystem* nfs = (NoopFileSystem*) fs;

    // If the key already exists, free the old value before overwriting
    ptrdiff_t idx = shgeti(nfs->files, relativePath);
    if (idx >= 0) {
        free(nfs->files[idx].value);
        nfs->files[idx].value = safeStrdup(contents);
    } else {
        shput(nfs->files, relativePath, safeStrdup(contents));
    }

    return true;
}

static bool noopDeleteFile(FileSystem* fs, const char* relativePath) {
    NoopFileSystem* nfs = (NoopFileSystem*) fs;
    ptrdiff_t idx = shgeti(nfs->files, relativePath);
    if (0 > idx)
        return false;

    free(nfs->files[idx].value);
    shdel(nfs->files, relativePath);
    return true;
}

// ===[ Vtable ]===

static FileSystemVtable noopFileSystemVtable = {
    .resolvePath = noopResolvePath,
    .fileExists = noopFileExists,
    .readFileText = noopReadFileText,
    .writeFileText = noopWriteFileText,
    .deleteFile = noopDeleteFile,
};

// ===[ Lifecycle ]===

FileSystem* NoopFileSystem_create(void) {
    NoopFileSystem* nfs = safeCalloc(1, sizeof(NoopFileSystem));
    nfs->base.vtable = &noopFileSystemVtable;
    nfs->files = nullptr;
    sh_new_strdup(nfs->files);
    return (FileSystem*) nfs;
}

void NoopFileSystem_destroy(FileSystem* fs) {
    NoopFileSystem* nfs = (NoopFileSystem*) fs;
    repeat(shlen(nfs->files), i) {
        free(nfs->files[i].value);
    }
    shfree(nfs->files);
    free(nfs);
}
