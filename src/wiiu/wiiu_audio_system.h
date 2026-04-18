#pragma once

#include "../audio_system.h"

#include <SDL2/SDL.h>

typedef struct stb_vorbis stb_vorbis;

typedef struct {
    bool loaded;
    float* samples;
    uint32_t sampleCount;
    int32_t channels;
    int32_t sampleRate;
} WiiUDecodedSound;

typedef struct {
    bool active;
    bool paused;
    bool loop;
    int32_t soundIndex;
    int32_t instanceId;
    int32_t priority;
    double position;
    float currentGain;
    float targetGain;
    float startGain;
    float fadeTimeRemaining;
    float fadeTotalTime;
    float pitch;
    float sondVolume;
    float sondPitch;
    WiiUDecodedSound* decoded;
    bool streaming;
    bool streamEof;
    int32_t streamSourceChannels;
    int32_t streamSourceRate;
    SDL_AudioStream* audioStream;
    stb_vorbis* vorbisStream;
    float* streamDecodeBuffer;
    uint32_t streamDecodeFrames;
    float* streamMixBuffer;
    uint32_t streamMixFrames;
    uint32_t streamMixCapacity;
} WiiUSoundInstance;

#define MAX_WIIU_SOUND_INSTANCES 64
#define WIIU_SOUND_INSTANCE_ID_BASE 100000

typedef struct {
    AudioSystem base;
    FileSystem* fileSystem;
    float masterGain;
    bool initialized;
    int32_t nextInstanceCounter;
    WiiUDecodedSound* decodedSounds;
    SDL_AudioDeviceID deviceId;
    SDL_AudioSpec audioSpec;
    uint32_t debugUpdateCounter;
    bool* loadedGroups;
    float* mixBuffer;
    uint32_t mixBufferSamples;
    float* streamScratch;
    uint32_t streamScratchSamples;
    WiiUSoundInstance instances[MAX_WIIU_SOUND_INSTANCES];
} WiiUAudioSystem;

WiiUAudioSystem* WiiUAudioSystem_create(void);