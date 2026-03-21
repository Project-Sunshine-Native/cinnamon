#pragma once

#include "../audio_system.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define MAX_PS2_SOUND_INSTANCES 64
#define PS2_SOUND_INSTANCE_ID_BASE 100000
#define LRU_CACHE_SIZE 64
#define MIX_BUFFER_SAMPLES 512
#define AUDSRV_OUTPUT_FREQ 22050

// Streaming music: decode ADPCM in chunks, double-buffered
// Each buffer holds STREAM_DECODE_SAMPLES decoded PCM samples
#define MAX_MUSIC_STREAMS 4
#define STREAM_ADPCM_CHUNK_BYTES 4096
#define STREAM_DECODE_SAMPLES (STREAM_ADPCM_CHUNK_BYTES * 2) // 2 samples per ADPCM byte

// ===[ SOUNDBNK.BIN Structs ]===

typedef struct {
    uint16_t audoIndex; // index into AUDO table, 0xFFFF = unmapped
    uint32_t flags;
    int16_t volume;     // fixed-point: original float * 256
    int16_t pitch;      // fixed-point: original float * 256
} Ps2SondEntry;

typedef struct {
    uint32_t dataOffset; // byte offset in SOUNDS.BIN
    uint32_t dataSize;   // bytes in SOUNDS.BIN
    uint16_t sampleRate;
    uint8_t channels;
    uint8_t bitsPerSample;
    uint8_t format;      // 0=PCM, 1=IMA ADPCM
} Ps2AudoEntry;

// ===[ LRU Decoded PCM Cache ]===

typedef struct {
    int32_t audoIndex;         // -1 = empty slot
    int16_t* pcmData;
    uint32_t pcmSampleCount;   // number of mono samples
    uint32_t pcmDataBytes;
    uint32_t lastAccessCounter;
} DecodedPcmEntry;

// ===[ Sound Instance ]===

typedef struct {
    bool active;
    int32_t soundIndex;   // SOND resource index
    int32_t audoIndex;    // AUDO resource index
    int32_t instanceId;   // unique ID returned to GML
    int32_t priority;
    bool loop;
    bool paused;

    // Playback position (32.32 fixed-point for fractional sample stepping)
    uint32_t positionInt;
    uint32_t positionFrac;
    uint32_t totalSamples;

    // Pitch
    float pitch;          // runtime pitch set by GML
    float sondPitch;      // SOND resource pitch (fixed-point / 256)

    // Gain / volume
    float currentGain;
    float targetGain;
    float startGain;
    float fadeTimeRemaining;
    float fadeTotalTime;
    float sondVolume;     // SOND resource volume (fixed-point / 256)
} Ps2SoundInstance;

// ===[ Streaming Music Instance ]===

typedef struct {
    bool active;
    int32_t soundIndex;
    int32_t audoIndex;
    int32_t instanceId;
    int32_t priority;
    bool loop;
    bool paused;

    // Gain / volume (same fields as Ps2SoundInstance)
    float currentGain;
    float targetGain;
    float startGain;
    float fadeTimeRemaining;
    float fadeTotalTime;
    float sondVolume;
    float pitch;
    float sondPitch;

    // ADPCM file streaming state
    uint32_t fileOffset;      // current read position in SOUNDS.BIN
    uint32_t fileStartOffset; // start offset of this track in SOUNDS.BIN
    uint32_t fileEndOffset;   // end offset (fileStartOffset + dataSize)

    // IMA ADPCM decoder state (persists across chunks)
    int32_t decoderPredictor;
    int32_t decoderStepIndex;

    // Double-buffered decoded PCM
    int16_t buffers[2][STREAM_DECODE_SAMPLES];
    uint32_t bufferSampleCount[2]; // actual samples decoded in each buffer (may be < STREAM_DECODE_SAMPLES at end of track)
    int activeBuffer;              // which buffer the mixer is reading from (0 or 1)
    uint32_t readPosition;         // integer sample position within the active buffer
    uint32_t readPositionFrac;     // fractional part (32-bit) for pitch resampling
    bool needsRefill;              // true when the back buffer needs to be filled
    bool endOfTrack;               // true when we've read all ADPCM data
} Ps2MusicStream;

// ===[ PS2 Audio System ]===

typedef struct {
    AudioSystem base;

    // SOUNDBNK.BIN index
    uint16_t sondEntryCount;
    uint16_t audoEntryCount;
    Ps2SondEntry* sondEntries;
    Ps2AudoEntry* audoEntries;

    // SOUNDS.BIN file handle (streamed on demand, not loaded into RAM)
    FILE* soundsFile;

    // LRU decoded PCM cache (for short embedded SFX)
    DecodedPcmEntry cacheEntries[LRU_CACHE_SIZE];
    uint32_t cacheAccessCounter;

    // SFX instance slots (embedded sounds, fully decoded in LRU cache)
    Ps2SoundInstance instances[MAX_PS2_SOUND_INSTANCES];
    int32_t nextInstanceCounter;

    // Streaming music slots (non-embedded sounds, double-buffered from disc)
    Ps2MusicStream musicStreams[MAX_MUSIC_STREAMS];

    // Mixer output buffer (stereo interleaved)
    int16_t mixBuffer[MIX_BUFFER_SAMPLES * 2];

    float masterGain;
    bool initialized;
} Ps2AudioSystem;

Ps2AudioSystem* Ps2AudioSystem_create(void);
