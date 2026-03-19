#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "data_win.h"
#include "file_system.h"

// ===[ AudioSystem Vtable ]===

typedef struct AudioSystem AudioSystem;

typedef struct {
    void (*init)(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem);
    void (*destroy)(AudioSystem* audio);
    void (*update)(AudioSystem* audio, float deltaTime);
    int32_t (*playSound)(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop);
    void (*stopSound)(AudioSystem* audio, int32_t soundOrInstance);
    void (*stopAll)(AudioSystem* audio);
    bool (*isPlaying)(AudioSystem* audio, int32_t soundOrInstance);
    void (*pauseSound)(AudioSystem* audio, int32_t soundOrInstance);
    void (*resumeSound)(AudioSystem* audio, int32_t soundOrInstance);
    void (*pauseAll)(AudioSystem* audio);
    void (*resumeAll)(AudioSystem* audio);
    void (*setSoundGain)(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs);
    float (*getSoundGain)(AudioSystem* audio, int32_t soundOrInstance);
    void (*setSoundPitch)(AudioSystem* audio, int32_t soundOrInstance, float pitch);
    float (*getSoundPitch)(AudioSystem* audio, int32_t soundOrInstance);
    float (*getTrackPosition)(AudioSystem* audio, int32_t soundOrInstance);
    void (*setTrackPosition)(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds);
    void (*setMasterGain)(AudioSystem* audio, float gain);
    void (*setChannelCount)(AudioSystem* audio, int32_t count);
    void (*groupLoad)(AudioSystem* audio, int32_t groupIndex);
    bool (*groupIsLoaded)(AudioSystem* audio, int32_t groupIndex);
} AudioSystemVtable;

// ===[ AudioSystem Base Struct ]===

struct AudioSystem {
    AudioSystemVtable* vtable;
    DataWin* dataWin;
};
