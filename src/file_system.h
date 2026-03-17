#pragma once

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
} FileSystemVtable;

struct FileSystem {
    FileSystemVtable* vtable;
};
