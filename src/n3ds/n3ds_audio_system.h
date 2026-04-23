#pragma once

#include "../audio_system.h"

#include <3ds.h>
#include <stdio.h>

#define N3DS_MAX_SOUND_INSTANCES  32
#define N3DS_MAX_AUDIO_CHANNELS   24
#define N3DS_SOUND_INSTANCE_ID_BASE 100000
#define N3DS_STREAM_BUFFER_FRAMES 4096

// Sounds smaller than this are fully loaded into regular heap RAM on first play;
// subsequent plays memcpy from that cache instead of fopen+fread.
// Raised to 10 MB so music tracks can be cached too.
#define N3DS_SFX_CACHE_SIZE_LIMIT (10 * 1024 * 1024)
#define N3DS_SFX_CACHE_MAX        64

typedef struct {
    int32_t  soundIndex;   // -1 = empty slot
    int16_t* data;         // heap-allocated full PCM content
    uint32_t byteSize;
    uint32_t sampleRate;
    uint8_t  channels;
    uint8_t  bytesPerFrame;
} N3DSSoundCacheEntry;

typedef struct {
    bool active;
    bool paused;
    bool loop;
    bool endOfStream;

    int32_t soundIndex;
    int32_t instanceId;
    int32_t priority;
    int32_t channel;

    // Streaming path (large files, e.g. music)
    FILE*    file;

    // Cache path (short SFX loaded into heap RAM)
    const int16_t* cacheData;
    uint32_t       cacheTotalBytes;
    uint32_t       cachePosByte;
    uint32_t       sampleRate;
    uint8_t        channels;
    uint8_t        bytesPerFrame;

    uint32_t totalFrames;
    uint32_t playedFrames;
    uint32_t dataOffset;      // byte offset in file to first audio sample (BCWAV: after header)
    uint32_t loopStartSample; // sample frame to loop back to (from BCWAV loopStart field)

    float currentGain;
    float targetGain;
    float startGain;
    float fadeTimeRemaining;
    float fadeTotalTime;

    float pitch;
    float sondVolume;
    float sondPitch;

    // Pointers into the N3DSAudioSystem stream buffer pool (never individually freed)
    int16_t*   buffers[2];
    ndspWaveBuf waveBufs[2];
} N3DSSoundInstance;

typedef struct {
    AudioSystem base;
    FileSystem* fileSystem;

    bool initialized;
    int32_t maxChannels;
    int32_t nextInstanceCounter;
    float masterGain;

    // Pre-allocated wave data buffers: one pair per instance slot.
    // Avoids linearAlloc/linearFree on every sound play call.
    int16_t* streamBufPool;  // linearAlloc'd, size = MAX_INSTANCES*2*FRAMES*PCM_BPF

    // Lazy-loaded RAM cache of short sound files
    N3DSSoundCacheEntry sfxCache[N3DS_SFX_CACHE_MAX];
    int32_t             sfxCacheCount;

    int32_t channelOwner[N3DS_MAX_AUDIO_CHANNELS];
    N3DSSoundInstance instances[N3DS_MAX_SOUND_INSTANCES];

    bool* loadedGroups;
    uint32_t loadedGroupCount;
} N3DSAudioSystem;

N3DSAudioSystem* N3DSAudioSystem_create(void);