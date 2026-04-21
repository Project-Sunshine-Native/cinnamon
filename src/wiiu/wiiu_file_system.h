#pragma once

#include "../file_system.h"

typedef struct {
    FileSystem base;
    char* basePath;
} WiiUFileSystem;

WiiUFileSystem* WiiUFileSystem_create(const char* dataWinPath);
void WiiUFileSystem_destroy(WiiUFileSystem* fs);