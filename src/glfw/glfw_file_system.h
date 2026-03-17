#pragma once

#include "file_system.h"

typedef struct {
    FileSystem base;
    char* basePath; // directory containing data.win, with trailing separator
} GlfwFileSystem;

// Creates a GlfwFileSystem from the path to the data.win file
// The basePath is derived by stripping the filename from dataWinPath.
GlfwFileSystem* GlfwFileSystem_create(const char* dataWinPath);
void GlfwFileSystem_destroy(GlfwFileSystem* fs);
