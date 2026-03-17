#pragma once

#include "file_system.h"

// Creates an in-memory FileSystem backed by a hashmap instead of real disk I/O
// Files written via writeFileText are kept in memory and can be read back
// Use this as a fallback while you don't have a proper file system implementation for your target!
FileSystem* NoopFileSystem_create(void);
void NoopFileSystem_destroy(FileSystem* fs);
