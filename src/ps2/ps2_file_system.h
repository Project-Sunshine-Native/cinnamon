#pragma once

#include "../file_system.h"
#include "../json_reader.h"

// Creates a PS2 file system that maps game-relative file names to PS2 device paths
// using the "fileSystem" object from a parsed CONFIG.JSN root
//
// configRoot: parsed JSON root of CONFIG.JSN (caller retains ownership, not freed here)
// gameTitle: the game's display name (used for icon.sys on memory card saves)
FileSystem* Ps2FileSystem_create(JsonValue* configRoot, const char* gameTitle);
void Ps2FileSystem_destroy(FileSystem* fs);
