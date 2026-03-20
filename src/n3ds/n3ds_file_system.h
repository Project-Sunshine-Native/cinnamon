#pragma once

#include "file_system.h"

typedef struct {
    FileSystem base;
    char* basePath; // directory containing data.win, with trailing separator
} N3DSFileSystem;

// Creates a N3DSFileSystem from the path to the data.win file
// The basePath is derived by stripping the filename from dataWinPath.
N3DSFileSystem* N3DSFileSystem_create(const char* dataWinPath);
void N3DSFileSystem_destroy(N3DSFileSystem* fs);
