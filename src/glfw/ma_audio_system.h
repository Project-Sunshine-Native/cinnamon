#pragma once

#include "audio_system.h"
#include "miniaudio.h"

#define MAX_SOUND_INSTANCES 128
#define SOUND_INSTANCE_ID_BASE 100000

typedef struct {
    bool active;
    int32_t soundIndex; // SOND resource that spawned this
    int32_t instanceId; // unique ID returned to GML
    ma_sound maSound; // miniaudio sound object
    ma_decoder decoder; // decoder for memory-based audio
    bool ownsDecoder; // true if decoder needs uninit
    float targetGain;
    float currentGain;
    float fadeTimeRemaining;
    float fadeTotalTime;
    float startGain;
    int32_t priority;
} SoundInstance;

typedef struct {
    AudioSystem base;
    ma_engine engine;
    SoundInstance instances[MAX_SOUND_INSTANCES];
    int32_t nextInstanceCounter;
    FileSystem* fileSystem;
} MaAudioSystem;

MaAudioSystem* MaAudioSystem_create(void);
