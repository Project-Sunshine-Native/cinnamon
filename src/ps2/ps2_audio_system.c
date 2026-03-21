#include "ps2_audio_system.h"
#include "ps2_utils.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <audsrv.h>

// ===[ IMA ADPCM Tables ]===

static const int16_t IMA_STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14,
    16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

static const int8_t IMA_INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

// ===[ IMA ADPCM Decoder ]===

// Decode a block of IMA ADPCM data, updating predictor/stepIndex state in place
// Returns the number of samples written to outPcm (2 per input byte)
static uint32_t imaAdpcmDecodeBlock(const uint8_t* adpcmData, uint32_t adpcmSize, int16_t* outPcm, int32_t* predictor, int32_t* stepIndex) {
    uint32_t samplesWritten = 0;

    repeat(adpcmSize, i) {
        uint8_t byte = adpcmData[i];

        // Process two nibbles per byte (low nibble first, then high)
        for (int nibbleIdx = 0; 2 > nibbleIdx; nibbleIdx++) {
            uint8_t nibble = (nibbleIdx == 0) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);

            int32_t step = IMA_STEP_TABLE[*stepIndex];
            int32_t delta = step >> 3;
            if (nibble & 1) delta += step >> 2;
            if (nibble & 2) delta += step >> 1;
            if (nibble & 4) delta += step;
            if (nibble & 8) delta = -delta;

            *predictor += delta;
            if (*predictor > 32767) *predictor = 32767;
            if (-32768 > *predictor) *predictor = -32768;

            *outPcm++ = (int16_t) *predictor;
            samplesWritten++;

            *stepIndex += IMA_INDEX_TABLE[nibble];
            if (0 > *stepIndex) *stepIndex = 0;
            if (*stepIndex > 88) *stepIndex = 88;
        }
    }

    return samplesWritten;
}

// Convenience wrapper for full-buffer decode (SFX path)
static void imaAdpcmDecode(const uint8_t* adpcmData, uint32_t adpcmSize, int16_t* outPcm) {
    int32_t predictor = 0;
    int32_t stepIndex = 0;
    imaAdpcmDecodeBlock(adpcmData, adpcmSize, outPcm, &predictor, &stepIndex);
}

// ===[ SOUNDBNK.BIN Parser ]===

static void parseSoundBank(Ps2AudioSystem* ps2) {
    char* path = PS2Utils_createDevicePath("SOUNDBNK.BIN");

    FILE* f = fopen(path, "rb");
    free(path);
    if (f == nullptr) {
        fprintf(stderr, "PS2AudioSystem: Could not open SOUNDBNK.BIN\n");
        return;
    }

    // Header: version(u8) + sondEntryCount(u16) + audoEntryCount(u16) = 5 bytes
    uint8_t version;
    fread(&version, 1, 1, f);
    fread(&ps2->sondEntryCount, 2, 1, f);
    fread(&ps2->audoEntryCount, 2, 1, f);

    fprintf(stderr, "PS2AudioSystem: SOUNDBNK v%d, %d SOND entries, %d AUDO entries\n", version, ps2->sondEntryCount, ps2->audoEntryCount);

    // Parse SOND entries (12 bytes each)
    ps2->sondEntries = safeMalloc(ps2->sondEntryCount * sizeof(Ps2SondEntry));
    for (int i = 0; ps2->sondEntryCount > i; i++) {
        uint8_t buf[12];
        fread(buf, 12, 1, f);
        ps2->sondEntries[i].audoIndex = *(uint16_t*) &buf[0];
        ps2->sondEntries[i].flags     = *(uint32_t*) &buf[2];
        ps2->sondEntries[i].volume    = *(int16_t*)  &buf[6];
        ps2->sondEntries[i].pitch     = *(int16_t*)  &buf[8];
        // buf[10..11] reserved
    }

    // Parse AUDO entries (16 bytes each)
    ps2->audoEntries = safeMalloc(ps2->audoEntryCount * sizeof(Ps2AudoEntry));
    for (int i = 0; ps2->audoEntryCount > i; i++) {
        uint8_t buf[16];
        fread(buf, 16, 1, f);
        ps2->audoEntries[i].dataOffset    = *(uint32_t*) &buf[0];
        ps2->audoEntries[i].dataSize      = *(uint32_t*) &buf[4];
        ps2->audoEntries[i].sampleRate    = *(uint16_t*) &buf[8];
        ps2->audoEntries[i].channels      = buf[10];
        ps2->audoEntries[i].bitsPerSample = buf[11];
        ps2->audoEntries[i].format        = buf[12];
        // buf[13..15] reserved
    }

    fclose(f);
}

// ===[ SOUNDS.BIN File Handle ]===

static void openSoundsBin(Ps2AudioSystem* ps2) {
    // SOUNDS.BIN is always alongside the ELF on the boot device, not in the FileSystem mappings
    // We keep the file handle open and read on demand to avoid loading everything into EE RAM
    char* path = PS2Utils_createDevicePath("SOUNDS.BIN");

    ps2->soundsFile = fopen(path, "rb");
    if (ps2->soundsFile == nullptr) {
        fprintf(stderr, "PS2AudioSystem: Could not open SOUNDS.BIN at %s\n", path);
        free(path);
        return;
    }

    fprintf(stderr, "PS2AudioSystem: Opened SOUNDS.BIN for streaming (%s)\n", path);
    free(path);
}

// ===[ LRU Decoded PCM Cache (SFX) ]===

static DecodedPcmEntry* cacheGet(Ps2AudioSystem* ps2, int32_t audoIndex) {
    for (int i = 0; LRU_CACHE_SIZE > i; i++) {
        if (ps2->cacheEntries[i].audoIndex == audoIndex) {
            ps2->cacheEntries[i].lastAccessCounter = ++ps2->cacheAccessCounter;
            return &ps2->cacheEntries[i];
        }
    }
    return nullptr;
}

static bool cacheIsInUse(Ps2AudioSystem* ps2, int32_t audoIndex) {
    repeat(MAX_PS2_SOUND_INSTANCES, i) {
        if (ps2->instances[i].active && ps2->instances[i].audoIndex == audoIndex) {
            return true;
        }
    }
    return false;
}

static DecodedPcmEntry* cacheInsert(Ps2AudioSystem* ps2, int32_t audoIndex) {
    Ps2AudoEntry* audo = &ps2->audoEntries[audoIndex];

    // Compute decoded sample count: IMA ADPCM = 2 samples per byte
    uint32_t sampleCount = audo->dataSize * 2;
    uint32_t pcmBytes = sampleCount * sizeof(int16_t);

    // Find a free slot
    DecodedPcmEntry* slot = nullptr;
    for (int i = 0; LRU_CACHE_SIZE > i; i++) {
        if (0 > ps2->cacheEntries[i].audoIndex) {
            slot = &ps2->cacheEntries[i];
            break;
        }
    }

    // No free slot: evict LRU entry not in active use
    if (slot == nullptr) {
        uint32_t oldestAccess = UINT32_MAX;
        for (int i = 0; LRU_CACHE_SIZE > i; i++) {
            DecodedPcmEntry* entry = &ps2->cacheEntries[i];
            if (!cacheIsInUse(ps2, entry->audoIndex) && oldestAccess > entry->lastAccessCounter) {
                oldestAccess = entry->lastAccessCounter;
                slot = entry;
            }
        }

        if (slot == nullptr) {
            // fprintf(stderr, "PS2AudioSystem: Cache full, all entries in use! Cannot decode audoIndex %" PRId32 "\n", audoIndex);
            return nullptr;
        }

        // Free the evicted entry's PCM data
        free(slot->pcmData);
    }

    // Read compressed ADPCM data from SOUNDS.BIN on demand
    uint8_t* adpcmBuf = safeMalloc(audo->dataSize);
    fseek(ps2->soundsFile, (long) audo->dataOffset, SEEK_SET);
    fread(adpcmBuf, 1, audo->dataSize, ps2->soundsFile);

    // Decode IMA ADPCM into new PCM buffer
    slot->pcmData = safeMalloc(pcmBytes);
    imaAdpcmDecode(adpcmBuf, audo->dataSize, slot->pcmData);
    free(adpcmBuf);

    slot->audoIndex = audoIndex;
    slot->pcmSampleCount = sampleCount;
    slot->pcmDataBytes = pcmBytes;
    slot->lastAccessCounter = ++ps2->cacheAccessCounter;

    return slot;
}

// ===[ Streaming Music ]===

// Fill one buffer of a music stream by reading ADPCM from SOUNDS.BIN and decoding
static void streamFillBuffer(Ps2AudioSystem* ps2, Ps2MusicStream* stream, int bufferIndex) {
    if (stream->fileOffset >= stream->fileEndOffset) {
        // No more data to read
        stream->bufferSampleCount[bufferIndex] = 0;
        return;
    }

    // How many ADPCM bytes remain for this track?
    uint32_t remaining = stream->fileEndOffset - stream->fileOffset;
    uint32_t toRead = STREAM_ADPCM_CHUNK_BYTES;
    if (toRead > remaining) toRead = remaining;

    // Read ADPCM chunk from disc
    uint8_t adpcmBuf[STREAM_ADPCM_CHUNK_BYTES];
    fseek(ps2->soundsFile, (long) stream->fileOffset, SEEK_SET);
    size_t bytesRead = fread(adpcmBuf, 1, toRead, ps2->soundsFile);
    stream->fileOffset += (uint32_t) bytesRead;

    // Decode into the target buffer, continuing decoder state
    uint32_t samples = imaAdpcmDecodeBlock(adpcmBuf, (uint32_t) bytesRead, stream->buffers[bufferIndex], &stream->decoderPredictor, &stream->decoderStepIndex);
    stream->bufferSampleCount[bufferIndex] = samples;

    if (stream->fileOffset >= stream->fileEndOffset) {
        stream->endOfTrack = true;
    }
}

// Reset a stream to the beginning of its track (for looping)
static void streamResetToStart(Ps2AudioSystem* ps2, Ps2MusicStream* stream) {
    stream->fileOffset = stream->fileStartOffset;
    stream->decoderPredictor = 0;
    stream->decoderStepIndex = 0;
    stream->endOfTrack = false;
    stream->readPosition = 0;
    stream->readPositionFrac = 0;

    // Fill both buffers from the start
    streamFillBuffer(ps2, stream, 0);
    streamFillBuffer(ps2, stream, 1);
    stream->activeBuffer = 0;
    stream->needsRefill = false;
}

// ===[ SFX Instance Helpers ]===

static Ps2SoundInstance* findFreeSlot(Ps2AudioSystem* ps2) {
    // First pass: find an inactive slot
    repeat(MAX_PS2_SOUND_INSTANCES, i) {
        if (!ps2->instances[i].active) {
            return &ps2->instances[i];
        }
    }

    // Second pass: evict the lowest-priority ended sound
    Ps2SoundInstance* best = nullptr;
    repeat(MAX_PS2_SOUND_INSTANCES, i) {
        Ps2SoundInstance* inst = &ps2->instances[i];
        if (inst->positionInt >= inst->totalSamples && !inst->loop) {
            if (best == nullptr || best->priority > inst->priority) {
                best = inst;
            }
        }
    }

    if (best != nullptr) {
        best->active = false;
    }

    return best;
}

static Ps2SoundInstance* findSfxInstanceById(Ps2AudioSystem* ps2, int32_t instanceId) {
    int32_t slotIndex = instanceId - PS2_SOUND_INSTANCE_ID_BASE;
    if (0 > slotIndex || slotIndex >= MAX_PS2_SOUND_INSTANCES) return nullptr;
    Ps2SoundInstance* inst = &ps2->instances[slotIndex];
    if (!inst->active || inst->instanceId != instanceId) return nullptr;
    return inst;
}

// Find a music stream by instance ID
static Ps2MusicStream* findMusicStreamById(Ps2AudioSystem* ps2, int32_t instanceId) {
    repeat(MAX_MUSIC_STREAMS, i) {
        if (ps2->musicStreams[i].active && ps2->musicStreams[i].instanceId == instanceId) {
            return &ps2->musicStreams[i];
        }
    }
    return nullptr;
}

// ===[ Software Mixer ]===

static void mixAudio(Ps2AudioSystem* ps2, int16_t* outBuf, int32_t samplePairs) {
    memset(outBuf, 0, samplePairs * 2 * sizeof(int16_t));

    for (int32_t s = 0; samplePairs > s; s++) {
        int32_t accumL = 0;
        int32_t accumR = 0;

        // Mix SFX instances (from LRU cache)
        repeat(MAX_PS2_SOUND_INSTANCES, i) {
            Ps2SoundInstance* inst = &ps2->instances[i];
            if (!inst->active || inst->paused) continue;

            DecodedPcmEntry* cache = cacheGet(ps2, inst->audoIndex);
            if (cache == nullptr) continue;

            if (inst->positionInt >= inst->totalSamples) {
                if (inst->loop) {
                    inst->positionInt = 0;
                    inst->positionFrac = 0;
                } else {
                    inst->active = false;
                    continue;
                }
            }

            // Linear interpolation between samples
            int32_t idx0 = (int32_t) inst->positionInt;
            int32_t idx1 = idx0 + 1;
            if ((uint32_t) idx1 >= inst->totalSamples) idx1 = idx0;

            int32_t s0 = cache->pcmData[idx0];
            int32_t s1 = cache->pcmData[idx1];
            int32_t frac = (int32_t) (inst->positionFrac >> 16);
            int32_t sample = s0 + ((s1 - s0) * frac >> 16);

            float gain = inst->currentGain * inst->sondVolume * ps2->masterGain;
            int32_t scaled = (int32_t) (sample * gain);
            accumL += scaled;
            accumR += scaled;

            // Advance position
            Ps2AudoEntry* audo = &ps2->audoEntries[inst->audoIndex];
            float stepRate = inst->pitch * inst->sondPitch * ((float) audo->sampleRate / (float) AUDSRV_OUTPUT_FREQ);
            uint32_t stepInt = (uint32_t) stepRate;
            uint32_t stepFrac = (uint32_t) ((stepRate - (float) stepInt) * 4294967296.0f);
            uint32_t oldFrac = inst->positionFrac;
            inst->positionFrac += stepFrac;
            if (oldFrac > inst->positionFrac) inst->positionInt++;
            inst->positionInt += stepInt;

            if (inst->positionInt >= inst->totalSamples) {
                if (inst->loop) {
                    inst->positionInt = inst->positionInt % inst->totalSamples;
                    inst->positionFrac = 0;
                } else {
                    inst->active = false;
                }
            }
        }

        // Mix streaming music instances
        repeat(MAX_MUSIC_STREAMS, i) {
            Ps2MusicStream* stream = &ps2->musicStreams[i];
            if (!stream->active || stream->paused) continue;

            uint32_t bufSamples = stream->bufferSampleCount[stream->activeBuffer];

            // Check if we've exhausted the active buffer
            if (stream->readPosition >= bufSamples) {
                if (stream->needsRefill && stream->endOfTrack) {
                    // Both buffers consumed and track is done
                    if (stream->loop) {
                        streamResetToStart(ps2, stream);
                        bufSamples = stream->bufferSampleCount[stream->activeBuffer];
                    } else {
                        stream->active = false;
                        continue;
                    }
                } else {
                    // Swap to the back buffer (which should have been refilled)
                    stream->activeBuffer ^= 1;
                    stream->readPosition = 0;
                    stream->readPositionFrac = 0;
                    stream->needsRefill = true;
                    bufSamples = stream->bufferSampleCount[stream->activeBuffer];

                    if (bufSamples == 0) {
                        // Back buffer is also empty
                        if (stream->loop) {
                            streamResetToStart(ps2, stream);
                            bufSamples = stream->bufferSampleCount[stream->activeBuffer];
                        } else {
                            stream->active = false;
                            continue;
                        }
                    }
                }
            }

            // Linear interpolation between samples
            int32_t idx0 = (int32_t) stream->readPosition;
            int32_t idx1 = idx0 + 1;
            if ((uint32_t) idx1 >= bufSamples) idx1 = idx0;

            int32_t s0 = stream->buffers[stream->activeBuffer][idx0];
            int32_t s1 = stream->buffers[stream->activeBuffer][idx1];
            int32_t frac = (int32_t) (stream->readPositionFrac >> 16);
            int32_t sample = s0 + ((s1 - s0) * frac >> 16);

            float gain = stream->currentGain * stream->sondVolume * ps2->masterGain;
            int32_t scaled = (int32_t) (sample * gain);
            accumL += scaled;
            accumR += scaled;

            // Advance read position by pitch-adjusted step (32.32 fixed-point)
            Ps2AudoEntry* audo = &ps2->audoEntries[stream->audoIndex];
            float stepRate = stream->pitch * stream->sondPitch * ((float) audo->sampleRate / (float) AUDSRV_OUTPUT_FREQ);
            uint32_t stepInt = (uint32_t) stepRate;
            uint32_t stepFrac = (uint32_t) ((stepRate - (float) stepInt) * 4294967296.0f);
            uint32_t oldFrac = stream->readPositionFrac;
            stream->readPositionFrac += stepFrac;
            if (oldFrac > stream->readPositionFrac) stream->readPosition++;
            stream->readPosition += stepInt;
        }

        // Clamp and write interleaved stereo
        if (accumL > 32767) accumL = 32767;
        if (-32768 > accumL) accumL = -32768;
        if (accumR > 32767) accumR = 32767;
        if (-32768 > accumR) accumR = -32768;

        outBuf[s * 2]     = (int16_t) accumL;
        outBuf[s * 2 + 1] = (int16_t) accumR;
    }
}

// ===[ Vtable Implementations ]===

static void ps2Init(AudioSystem* audio, [[maybe_unused]] DataWin* dataWin, [[maybe_unused]] FileSystem* fileSystem) {
    Ps2AudioSystem* ps2 = (Ps2AudioSystem*) audio;

    // Parse sound bank index
    parseSoundBank(ps2);
    if (ps2->sondEntries == nullptr || ps2->audoEntries == nullptr) {
        fprintf(stderr, "PS2AudioSystem: Failed to parse SOUNDBNK.BIN, audio disabled\n");
        return;
    }

    // Open SOUNDS.BIN for streaming (kept open for on-demand reads)
    openSoundsBin(ps2);
    if (ps2->soundsFile == nullptr) {
        fprintf(stderr, "PS2AudioSystem: Failed to open SOUNDS.BIN, audio disabled\n");
        return;
    }

    // Initialize LRU cache (all slots empty)
    for (int i = 0; LRU_CACHE_SIZE > i; i++) {
        ps2->cacheEntries[i].audoIndex = -1;
        ps2->cacheEntries[i].pcmData = nullptr;
    }

    // Initialize sound instances
    memset(ps2->instances, 0, sizeof(ps2->instances));
    memset(ps2->musicStreams, 0, sizeof(ps2->musicStreams));
    ps2->nextInstanceCounter = 0;
    ps2->masterGain = 1.0f;

    // Initialize audsrv
    int ret = audsrv_init();
    if (ret != 0) {
        fprintf(stderr, "PS2AudioSystem: audsrv_init failed (%d)\n", ret);
        return;
    }

    struct audsrv_fmt_t format;
    format.freq = AUDSRV_OUTPUT_FREQ;
    format.bits = 16;
    format.channels = 2;

    ret = audsrv_set_format(&format);
    if (ret != 0) {
        fprintf(stderr, "PS2AudioSystem: audsrv_set_format failed (%d)\n", ret);
        audsrv_quit();
        return;
    }

    audsrv_set_volume(MAX_VOLUME);

    ps2->initialized = true;
    fprintf(stderr, "PS2AudioSystem: Initialized (output: %d Hz, 16-bit, stereo)\n", AUDSRV_OUTPUT_FREQ);
}

static void ps2Destroy(AudioSystem* audio) {
    Ps2AudioSystem* ps2 = (Ps2AudioSystem*) audio;

    if (ps2->initialized) {
        audsrv_stop_audio();
        audsrv_quit();
    }

    // Close SOUNDS.BIN file handle
    if (ps2->soundsFile != nullptr) {
        fclose(ps2->soundsFile);
    }

    // Free all cached PCM data
    for (int i = 0; LRU_CACHE_SIZE > i; i++) {
        free(ps2->cacheEntries[i].pcmData);
    }

    // Free sound bank entries
    free(ps2->sondEntries);
    free(ps2->audoEntries);

    free(ps2);
}

static void ps2Update(AudioSystem* audio, float deltaTime) {
    Ps2AudioSystem* ps2 = (Ps2AudioSystem*) audio;
    if (!ps2->initialized) return;

    // Cap deltaTime to prevent large fades on lag spikes
    if (deltaTime > 0.1f) deltaTime = 0.1f;

    // Update gain fading on SFX instances
    repeat(MAX_PS2_SOUND_INSTANCES, i) {
        Ps2SoundInstance* inst = &ps2->instances[i];
        if (!inst->active) continue;

        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (0.0f >= inst->fadeTimeRemaining) {
                inst->fadeTimeRemaining = 0.0f;
                inst->currentGain = inst->targetGain;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
        }
    }

    // Update gain fading on music streams
    repeat(MAX_MUSIC_STREAMS, i) {
        Ps2MusicStream* stream = &ps2->musicStreams[i];
        if (!stream->active) continue;

        if (stream->fadeTimeRemaining > 0.0f) {
            stream->fadeTimeRemaining -= deltaTime;
            if (0.0f >= stream->fadeTimeRemaining) {
                stream->fadeTimeRemaining = 0.0f;
                stream->currentGain = stream->targetGain;
            } else {
                float t = 1.0f - (stream->fadeTimeRemaining / stream->fadeTotalTime);
                stream->currentGain = stream->startGain + (stream->targetGain - stream->startGain) * t;
            }
        }
    }

    // Refill music stream back buffers (do disc I/O here, outside the mixer loop)
    repeat(MAX_MUSIC_STREAMS, i) {
        Ps2MusicStream* stream = &ps2->musicStreams[i];
        if (!stream->active || !stream->needsRefill) continue;
        // fprintf(stderr, "PS2AudioSystem: Filling music stream %d back buffers...\n", stream->soundIndex);

        int backBuffer = stream->activeBuffer ^ 1;
        streamFillBuffer(ps2, stream, backBuffer);
        stream->needsRefill = false;
    }

    // Fill audsrv ring buffer
    int32_t chunkBytes = MIX_BUFFER_SAMPLES * 2 * (int32_t) sizeof(int16_t);
    while (audsrv_available() >= chunkBytes) {
        // fprintf(stderr, "PS2AudioSystem: Filling audsrv ring buffer... audsrv_available: %d, chunkBytes: %d\n", audsrv_available(), chunkBytes);
        mixAudio(ps2, ps2->mixBuffer, MIX_BUFFER_SAMPLES);
        audsrv_play_audio((char*) ps2->mixBuffer, chunkBytes);
    }

    // fprintf(stderr, "PS2AudioSystem: Finished ticking the audio system\n");
}

static int32_t ps2PlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    // fprintf(stderr, "PS2AudioSystem: Attempting to play sound index %d with priority %d, should loop? %d\n", soundIndex, priority, loop);
    Ps2AudioSystem* ps2 = (Ps2AudioSystem*) audio;
    if (!ps2->initialized) return -1;

    if (0 > soundIndex || (uint16_t) soundIndex >= ps2->sondEntryCount) {
        // fprintf(stderr, "PS2AudioSystem: Invalid sound index %" PRId32 "\n", soundIndex);
        return -1;
    }

    Ps2SondEntry* sond = &ps2->sondEntries[soundIndex];

    // 0xFFFF = unmapped sound (no audio data)
    if (sond->audoIndex == 0xFFFF) {
        return -1;
    }

    if (sond->audoIndex >= ps2->audoEntryCount) {
        // fprintf(stderr, "PS2AudioSystem: Invalid audo index %d for sound %" PRId32 "\n", sond->audoIndex, soundIndex);
        return -1;
    }

    // SOND volume and pitch are fixed-point * 256
    // A pitch of 0 means "default" (1.0) in GameMaker
    float sondVolume = (float) sond->volume / 256.0f;
    float sondPitch = (sond->pitch == 0) ? 1.0f : (float) sond->pitch / 256.0f;

    bool isEmbedded = (sond->flags & 0x01) != 0;

    if (!isEmbedded) {
        // ===[ Streaming music path ]===
        // Find a free music stream slot
        Ps2MusicStream* stream = nullptr;
        int streamSlot = -1;
        repeat(MAX_MUSIC_STREAMS, i) {
            if (!ps2->musicStreams[i].active) {
                stream = &ps2->musicStreams[i];
                streamSlot = i;
                break;
            }
        }

        if (stream == nullptr) {
            // fprintf(stderr, "PS2AudioSystem: No free music stream slots for sound %" PRId32 "\n", soundIndex);
            return -1;
        }

        Ps2AudoEntry* audo = &ps2->audoEntries[sond->audoIndex];

        // Use a separate ID range for music streams (offset by MAX_PS2_SOUND_INSTANCES)
        int32_t instanceId = PS2_SOUND_INSTANCE_ID_BASE + MAX_PS2_SOUND_INSTANCES + streamSlot;

        memset(stream, 0, sizeof(Ps2MusicStream));
        stream->active = true;
        stream->soundIndex = soundIndex;
        stream->audoIndex = sond->audoIndex;
        stream->instanceId = instanceId;
        stream->priority = priority;
        stream->loop = loop;
        stream->paused = false;
        stream->currentGain = sondVolume;
        stream->targetGain = sondVolume;
        stream->startGain = sondVolume;
        stream->sondVolume = sondVolume;
        stream->pitch = 1.0f;
        stream->sondPitch = sondPitch;

        // Set up file streaming state
        stream->fileStartOffset = audo->dataOffset;
        stream->fileEndOffset = audo->dataOffset + audo->dataSize;
        stream->fileOffset = audo->dataOffset;
        stream->decoderPredictor = 0;
        stream->decoderStepIndex = 0;
        stream->endOfTrack = false;

        // Fill both buffers initially
        streamFillBuffer(ps2, stream, 0);
        streamFillBuffer(ps2, stream, 1);
        stream->activeBuffer = 0;
        stream->readPosition = 0;
        stream->needsRefill = false;

        // fprintf(stderr, "PS2AudioSystem: Streaming music soundIndex=%" PRId32 " audoIndex=%d, size=%" PRIu32 " bytes, instanceId=%" PRId32 "\n", soundIndex, sond->audoIndex, audo->dataSize, instanceId);

        return instanceId;
    }

    // ===[ Cached SFX path ]===
    // Ensure decoded PCM is in cache
    DecodedPcmEntry* cached = cacheGet(ps2, sond->audoIndex);
    if (cached == nullptr) {
        cached = cacheInsert(ps2, sond->audoIndex);
        if (cached == nullptr) {
            // fprintf(stderr, "PS2AudioSystem: Failed to cache decoded audio for sound %" PRId32 "\n", soundIndex);
            return -1;
        }
    }

    // Find a free SFX instance slot
    Ps2SoundInstance* slot = findFreeSlot(ps2);
    if (slot == nullptr) {
        // fprintf(stderr, "PS2AudioSystem: No free sound slots for sound %" PRId32 "\n", soundIndex);
        return -1;
    }

    int32_t slotIndex = (int32_t) (slot - ps2->instances);

    slot->active = true;
    slot->soundIndex = soundIndex;
    slot->audoIndex = sond->audoIndex;
    slot->instanceId = PS2_SOUND_INSTANCE_ID_BASE + slotIndex;
    slot->priority = priority;
    slot->loop = loop;
    slot->paused = false;
    slot->positionInt = 0;
    slot->positionFrac = 0;
    slot->totalSamples = cached->pcmSampleCount;
    slot->pitch = 1.0f;
    slot->sondPitch = sondPitch;
    slot->currentGain = sondVolume;
    slot->targetGain = sondVolume;
    slot->startGain = sondVolume;
    slot->fadeTimeRemaining = 0.0f;
    slot->fadeTotalTime = 0.0f;
    slot->sondVolume = sondVolume;

    ps2->nextInstanceCounter++;

    return slot->instanceId;
}

// ===[ Helper: Apply action to SFX instance, music stream, or all matching by soundIndex ]===
// These helpers handle the dual SFX/music lookup needed by stop/pause/resume/gain/pitch/etc.

// Find either a SFX instance or music stream by instanceId or soundIndex
// For instanceId lookups (>= PS2_SOUND_INSTANCE_ID_BASE), returns at most one match.
// For soundIndex lookups, iterates all matches via callback.

typedef void (*InstanceAction)(Ps2SoundInstance* sfx, Ps2MusicStream* music, void* userData);

static void forEachInstance(Ps2AudioSystem* ps2, int32_t soundOrInstance, InstanceAction action, void* userData) {
    if (soundOrInstance >= PS2_SOUND_INSTANCE_ID_BASE) {
        // Lookup by instance ID
        Ps2SoundInstance* sfx = findSfxInstanceById(ps2, soundOrInstance);
        if (sfx != nullptr) {
            action(sfx, nullptr, userData);
            return;
        }
        Ps2MusicStream* music = findMusicStreamById(ps2, soundOrInstance);
        if (music != nullptr) {
            action(nullptr, music, userData);
            return;
        }
    } else {
        // Lookup by sound index -- apply to all matching
        repeat(MAX_PS2_SOUND_INSTANCES, i) {
            if (ps2->instances[i].active && ps2->instances[i].soundIndex == soundOrInstance) {
                action(&ps2->instances[i], nullptr, userData);
            }
        }
        repeat(MAX_MUSIC_STREAMS, i) {
            if (ps2->musicStreams[i].active && ps2->musicStreams[i].soundIndex == soundOrInstance) {
                action(nullptr, &ps2->musicStreams[i], userData);
            }
        }
    }
}

static void actionStop(Ps2SoundInstance* sfx, Ps2MusicStream* music, [[maybe_unused]] void* userData) {
    if (sfx != nullptr) sfx->active = false;
    if (music != nullptr) music->active = false;
}

static void actionPause(Ps2SoundInstance* sfx, Ps2MusicStream* music, [[maybe_unused]] void* userData) {
    if (sfx != nullptr) sfx->paused = true;
    if (music != nullptr) music->paused = true;
}

static void actionResume(Ps2SoundInstance* sfx, Ps2MusicStream* music, [[maybe_unused]] void* userData) {
    if (sfx != nullptr) sfx->paused = false;
    if (music != nullptr) music->paused = false;
}

typedef struct {
    float gain;
    uint32_t timeMs;
} GainParams;

static void actionSetGain(Ps2SoundInstance* sfx, Ps2MusicStream* music, void* userData) {
    GainParams* params = (GainParams*) userData;
    float gain = params->gain;
    uint32_t timeMs = params->timeMs;

    if (sfx != nullptr) {
        if (timeMs == 0) {
            sfx->currentGain = gain;
            sfx->targetGain = gain;
            sfx->fadeTimeRemaining = 0.0f;
        } else {
            sfx->startGain = sfx->currentGain;
            sfx->targetGain = gain;
            sfx->fadeTotalTime = (float) timeMs / 1000.0f;
            sfx->fadeTimeRemaining = sfx->fadeTotalTime;
        }
    }
    if (music != nullptr) {
        if (timeMs == 0) {
            music->currentGain = gain;
            music->targetGain = gain;
            music->fadeTimeRemaining = 0.0f;
        } else {
            music->startGain = music->currentGain;
            music->targetGain = gain;
            music->fadeTotalTime = (float) timeMs / 1000.0f;
            music->fadeTimeRemaining = music->fadeTotalTime;
        }
    }
}

static void actionSetPitch(Ps2SoundInstance* sfx, Ps2MusicStream* music, void* userData) {
    float pitch = *(float*) userData;
    if (sfx != nullptr) sfx->pitch = pitch;
    if (music != nullptr) music->pitch = pitch;
}

// ===[ Vtable: Stop/Pause/Resume/Gain/Pitch ]===

static void ps2StopSound(AudioSystem* audio, int32_t soundOrInstance) {
    // fprintf(stderr, "PS2AudioSystem: Stopping sound %d\n", soundOrInstance);
    forEachInstance((Ps2AudioSystem*) audio, soundOrInstance, actionStop, nullptr);
}

static void ps2StopAll(AudioSystem* audio) {
    // fprintf(stderr, "PS2AudioSystem: Stopping all audios!\n");
    Ps2AudioSystem* ps2 = (Ps2AudioSystem*) audio;
    repeat(MAX_PS2_SOUND_INSTANCES, i) {
        ps2->instances[i].active = false;
    }
    repeat(MAX_MUSIC_STREAMS, i) {
        ps2->musicStreams[i].active = false;
    }
}

static bool ps2IsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    Ps2AudioSystem* ps2 = (Ps2AudioSystem*) audio;

    if (soundOrInstance >= PS2_SOUND_INSTANCE_ID_BASE) {
        Ps2SoundInstance* sfx = findSfxInstanceById(ps2, soundOrInstance);
        if (sfx != nullptr) return !sfx->paused;
        Ps2MusicStream* music = findMusicStreamById(ps2, soundOrInstance);
        if (music != nullptr) return !music->paused;
        return false;
    } else {
        repeat(MAX_PS2_SOUND_INSTANCES, i) {
            Ps2SoundInstance* inst = &ps2->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance && !inst->paused) return true;
        }
        repeat(MAX_MUSIC_STREAMS, i) {
            Ps2MusicStream* stream = &ps2->musicStreams[i];
            if (stream->active && stream->soundIndex == soundOrInstance && !stream->paused) return true;
        }
        return false;
    }
}

static void ps2PauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    // fprintf(stderr, "PS2AudioSystem: Pausing sound %d\n", soundOrInstance);
    forEachInstance((Ps2AudioSystem*) audio, soundOrInstance, actionPause, nullptr);
}

static void ps2ResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    // fprintf(stderr, "PS2AudioSystem: Resuming sound %d\n", soundOrInstance);
    forEachInstance((Ps2AudioSystem*) audio, soundOrInstance, actionResume, nullptr);
}

static void ps2PauseAll(AudioSystem* audio) {
    // fprintf(stderr, "PS2AudioSystem: Pausing all sounds!\n");
    Ps2AudioSystem* ps2 = (Ps2AudioSystem*) audio;
    repeat(MAX_PS2_SOUND_INSTANCES, i) {
        if (ps2->instances[i].active) ps2->instances[i].paused = true;
    }
    repeat(MAX_MUSIC_STREAMS, i) {
        if (ps2->musicStreams[i].active) ps2->musicStreams[i].paused = true;
    }
}

static void ps2ResumeAll(AudioSystem* audio) {
    // fprintf(stderr, "PS2AudioSystem: Resuming all sounds!\n");
    Ps2AudioSystem* ps2 = (Ps2AudioSystem*) audio;
    repeat(MAX_PS2_SOUND_INSTANCES, i) {
        if (ps2->instances[i].active) ps2->instances[i].paused = false;
    }
    repeat(MAX_MUSIC_STREAMS, i) {
        if (ps2->musicStreams[i].active) ps2->musicStreams[i].paused = false;
    }
}

static void ps2SetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    GainParams params = { .gain = gain, .timeMs = timeMs };
    forEachInstance((Ps2AudioSystem*) audio, soundOrInstance, actionSetGain, &params);
}

static float ps2GetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    Ps2AudioSystem* ps2 = (Ps2AudioSystem*) audio;
    if (soundOrInstance >= PS2_SOUND_INSTANCE_ID_BASE) {
        Ps2SoundInstance* sfx = findSfxInstanceById(ps2, soundOrInstance);
        if (sfx != nullptr) return sfx->currentGain;
        Ps2MusicStream* music = findMusicStreamById(ps2, soundOrInstance);
        if (music != nullptr) return music->currentGain;
    } else {
        repeat(MAX_PS2_SOUND_INSTANCES, i) {
            if (ps2->instances[i].active && ps2->instances[i].soundIndex == soundOrInstance) return ps2->instances[i].currentGain;
        }
        repeat(MAX_MUSIC_STREAMS, i) {
            if (ps2->musicStreams[i].active && ps2->musicStreams[i].soundIndex == soundOrInstance) return ps2->musicStreams[i].currentGain;
        }
    }
    return 0.0f;
}

static void ps2SetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    // fprintf(stderr, "PS2AudioSystem: Setting pitch of sound %d to %f\n", soundOrInstance, pitch);
    forEachInstance((Ps2AudioSystem*) audio, soundOrInstance, actionSetPitch, &pitch);
}

static float ps2GetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    Ps2AudioSystem* ps2 = (Ps2AudioSystem*) audio;
    if (soundOrInstance >= PS2_SOUND_INSTANCE_ID_BASE) {
        Ps2SoundInstance* sfx = findSfxInstanceById(ps2, soundOrInstance);
        if (sfx != nullptr) return sfx->pitch;
        Ps2MusicStream* music = findMusicStreamById(ps2, soundOrInstance);
        if (music != nullptr) return music->pitch;
    } else {
        repeat(MAX_PS2_SOUND_INSTANCES, i) {
            if (ps2->instances[i].active && ps2->instances[i].soundIndex == soundOrInstance) return ps2->instances[i].pitch;
        }
        repeat(MAX_MUSIC_STREAMS, i) {
            if (ps2->musicStreams[i].active && ps2->musicStreams[i].soundIndex == soundOrInstance) return ps2->musicStreams[i].pitch;
        }
    }
    return 1.0f;
}

static float ps2GetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    Ps2AudioSystem* ps2 = (Ps2AudioSystem*) audio;

    if (soundOrInstance >= PS2_SOUND_INSTANCE_ID_BASE) {
        Ps2SoundInstance* sfx = findSfxInstanceById(ps2, soundOrInstance);
        if (sfx != nullptr && sfx->audoIndex < ps2->audoEntryCount) {
            return (float) sfx->positionInt / (float) ps2->audoEntries[sfx->audoIndex].sampleRate;
        }
        Ps2MusicStream* music = findMusicStreamById(ps2, soundOrInstance);
        if (music != nullptr && music->audoIndex < ps2->audoEntryCount) {
            // Approximate: bytes consumed from file, converted to samples, then to seconds
            uint32_t bytesConsumed = music->fileOffset - music->fileStartOffset;
            uint32_t samplesConsumed = bytesConsumed * 2; // IMA ADPCM: 2 samples/byte
            return (float) samplesConsumed / (float) ps2->audoEntries[music->audoIndex].sampleRate;
        }
    } else {
        repeat(MAX_PS2_SOUND_INSTANCES, i) {
            Ps2SoundInstance* inst = &ps2->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance && inst->audoIndex < ps2->audoEntryCount) {
                return (float) inst->positionInt / (float) ps2->audoEntries[inst->audoIndex].sampleRate;
            }
        }
        repeat(MAX_MUSIC_STREAMS, i) {
            Ps2MusicStream* stream = &ps2->musicStreams[i];
            if (stream->active && stream->soundIndex == soundOrInstance && stream->audoIndex < ps2->audoEntryCount) {
                uint32_t bytesConsumed = stream->fileOffset - stream->fileStartOffset;
                uint32_t samplesConsumed = bytesConsumed * 2;
                return (float) samplesConsumed / (float) ps2->audoEntries[stream->audoIndex].sampleRate;
            }
        }
    }
    return 0.0f;
}

static void ps2SetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    // fprintf(stderr, "PS2AudioSystem: Setting track position of sound %d to %f\n", soundOrInstance, positionSeconds);
    Ps2AudioSystem* ps2 = (Ps2AudioSystem*) audio;

    // SFX track position
    if (soundOrInstance >= PS2_SOUND_INSTANCE_ID_BASE) {
        Ps2SoundInstance* sfx = findSfxInstanceById(ps2, soundOrInstance);
        if (sfx != nullptr && sfx->audoIndex < ps2->audoEntryCount) {
            float sampleRate = (float) ps2->audoEntries[sfx->audoIndex].sampleRate;
            sfx->positionInt = (uint32_t) (positionSeconds * sampleRate);
            sfx->positionFrac = 0;
            if (sfx->positionInt >= sfx->totalSamples) {
                sfx->positionInt = sfx->totalSamples > 0 ? sfx->totalSamples - 1 : 0;
            }
            return;
        }
        // Music stream seek: reset decoder and re-seek
        Ps2MusicStream* music = findMusicStreamById(ps2, soundOrInstance);
        if (music != nullptr && music->audoIndex < ps2->audoEntryCount) {
            float sampleRate = (float) ps2->audoEntries[music->audoIndex].sampleRate;
            uint32_t targetSample = (uint32_t) (positionSeconds * sampleRate);
            // Convert sample position to ADPCM byte offset (2 samples per byte)
            uint32_t byteOffset = targetSample / 2;
            uint32_t maxBytes = music->fileEndOffset - music->fileStartOffset;
            if (byteOffset > maxBytes) byteOffset = maxBytes;

            // Reset decoder state and seek
            music->fileOffset = music->fileStartOffset + byteOffset;
            music->decoderPredictor = 0;
            music->decoderStepIndex = 0;
            music->endOfTrack = false;
            music->readPosition = 0;

            // Re-fill both buffers from the new position
            streamFillBuffer(ps2, music, 0);
            streamFillBuffer(ps2, music, 1);
            music->activeBuffer = 0;
            music->needsRefill = false;
        }
    } else {
        // By sound index (apply to first match)
        repeat(MAX_PS2_SOUND_INSTANCES, i) {
            Ps2SoundInstance* inst = &ps2->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance && inst->audoIndex < ps2->audoEntryCount) {
                float sampleRate = (float) ps2->audoEntries[inst->audoIndex].sampleRate;
                inst->positionInt = (uint32_t) (positionSeconds * sampleRate);
                inst->positionFrac = 0;
                if (inst->positionInt >= inst->totalSamples) {
                    inst->positionInt = inst->totalSamples > 0 ? inst->totalSamples - 1 : 0;
                }
                return;
            }
        }
    }
}

static void ps2SetMasterGain(AudioSystem* audio, float gain) {
    // fprintf(stderr, "PS2AudioSystem: Setting master gain to %f\n", gain);
    Ps2AudioSystem* ps2 = (Ps2AudioSystem*) audio;
    ps2->masterGain = gain;
}

static void ps2SetChannelCount([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t count) {
    // No-op: software mixer handles all channels internally
}

static void ps2GroupLoad([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t groupIndex) {
    // No-op: all audio is available from SOUNDS.BIN
}

static bool ps2GroupIsLoaded([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t groupIndex) {
    return true;
}

// ===[ Vtable ]===

static AudioSystemVtable ps2AudioSystemVtable = {
    .init = ps2Init,
    .destroy = ps2Destroy,
    .update = ps2Update,
    .playSound = ps2PlaySound,
    .stopSound = ps2StopSound,
    .stopAll = ps2StopAll,
    .isPlaying = ps2IsPlaying,
    .pauseSound = ps2PauseSound,
    .resumeSound = ps2ResumeSound,
    .pauseAll = ps2PauseAll,
    .resumeAll = ps2ResumeAll,
    .setSoundGain = ps2SetSoundGain,
    .getSoundGain = ps2GetSoundGain,
    .setSoundPitch = ps2SetSoundPitch,
    .getSoundPitch = ps2GetSoundPitch,
    .getTrackPosition = ps2GetTrackPosition,
    .setTrackPosition = ps2SetTrackPosition,
    .setMasterGain = ps2SetMasterGain,
    .setChannelCount = ps2SetChannelCount,
    .groupLoad = ps2GroupLoad,
    .groupIsLoaded = ps2GroupIsLoaded,
};

// ===[ Lifecycle ]===

Ps2AudioSystem* Ps2AudioSystem_create(void) {
    Ps2AudioSystem* ps2 = safeCalloc(1, sizeof(Ps2AudioSystem));
    ps2->base.vtable = &ps2AudioSystemVtable;
    ps2->masterGain = 1.0f;
    return ps2;
}
