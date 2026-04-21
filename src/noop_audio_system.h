#pragma once

#include "common.h"
#include "audio_system.h"

// A no-op audio system that silently ignores all audio calls.
// Useful for headless mode or platforms without audio support.

typedef struct {
    AudioSystem base;
} NoopAudioSystem;

NoopAudioSystem* NoopAudioSystem_create(void);
