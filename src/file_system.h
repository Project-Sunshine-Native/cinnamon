#pragma once

#include "common.h"
#include <stdint.h>
// ===[ FileSystem Vtable ]===
// Platform-agnostic file system interface

typedef struct FileSystem FileSystem;

typedef struct {
    // Resolve a game-relative path to a full platform path (caller frees result)
    char* (*resolvePath)(FileSystem* fs, const char* relativePath);
    // Check if a file exists
    bool (*fileExists)(FileSystem* fs, const char* relativePath);
    // Read entire file contents into a string (caller frees result), returns nullptr if not found
    char* (*readFileText)(FileSystem* fs, const char* relativePath);
    // Write string contents to a file (creates/overwrites), returns true on success
    bool (*writeFileText)(FileSystem* fs, const char* relativePath, const char* contents);
    // Delete a file, returns true on success
    bool (*deleteFile)(FileSystem* fs, const char* relativePath);
    // Read entire file as binary data (caller frees *outData), returns true on success
    bool (*readFileBinary)(FileSystem* fs, const char* relativePath, uint8_t** outData, int32_t* outSize);
    // Write binary data to a file (creates/overwrites), returns true on success
    bool (*writeFileBinary)(FileSystem* fs, const char* relativePath, const uint8_t* data, int32_t size);
} FileSystemVtable;

struct FileSystem {
    FileSystemVtable* vtable;
};
