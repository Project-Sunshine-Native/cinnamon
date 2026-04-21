#include "n3ds_audio_system.h"

#include "../data_win.h"
#include "../utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PCM_BYTES_PER_FRAME_STEREO 4
#define PCM_BYTES_PER_FRAME_MONO   2
#define N3DS_DEFAULT_SAMPLE_RATE 44100
#define N3DS_DEFAULT_CHANNEL_COUNT 12

#define AUDIO_LOG(...) do { fprintf(stderr, __VA_ARGS__); printf(__VA_ARGS__); } while(0)
#define AUDIO_ERR(...) do { fprintf(stderr, __VA_ARGS__); printf(__VA_ARGS__); } while(0)

#ifndef N3DS_AUDIO_DEBUG_PLAY
#define N3DS_AUDIO_DEBUG_PLAY 0
#endif

#if N3DS_AUDIO_DEBUG_PLAY
#define AUDIO_DBG_PLAY(...) AUDIO_LOG(__VA_ARGS__)
#else
#define AUDIO_DBG_PLAY(...) do {} while(0)
#endif

static N3DSSoundInstance* findInstanceById(N3DSAudioSystem* n3ds, int32_t instanceId) {
    int32_t slot = instanceId - N3DS_SOUND_INSTANCE_ID_BASE;
    if (slot < 0 || slot >= N3DS_MAX_SOUND_INSTANCES) return NULL;
    N3DSSoundInstance* inst = &n3ds->instances[slot];
    if (!inst->active || inst->instanceId != instanceId) return NULL;
    return inst;
}

static bool isBufferQueued(const ndspWaveBuf* wb) {
    return wb->status == NDSP_WBUF_QUEUED || wb->status == NDSP_WBUF_PLAYING;
}

static void updateChannelMix(N3DSAudioSystem* n3ds, N3DSSoundInstance* inst) {
    if (!inst || inst->channel < 0 || inst->channel >= N3DS_MAX_AUDIO_CHANNELS) return;

    float mix[12] = {0.0f};
    float gain = n3ds->masterGain * inst->currentGain * inst->sondVolume;
    if (gain < 0.0f) gain = 0.0f;
    mix[0] = gain;
    mix[1] = gain;
    ndspChnSetMix((u32) inst->channel, mix);
}

static void updateChannelRate(N3DSSoundInstance* inst) {
    if (!inst || inst->channel < 0 || inst->channel >= N3DS_MAX_AUDIO_CHANNELS) return;

    float baseRate = (inst->sampleRate > 0) ? (float)inst->sampleRate : (float)N3DS_DEFAULT_SAMPLE_RATE;
    float rate = baseRate * inst->pitch * inst->sondPitch;
    if (rate < 1000.0f) rate = 1000.0f;
    if (rate > 96000.0f) rate = 96000.0f;
    ndspChnSetRate((u32) inst->channel, rate);
}

static void stopInstance(N3DSAudioSystem* n3ds, N3DSSoundInstance* inst) {
    if (!inst || !inst->active) return;

    if (inst->channel >= 0 && inst->channel < N3DS_MAX_AUDIO_CHANNELS) {
        ndspChnWaveBufClear((u32) inst->channel);
        ndspChnReset((u32) inst->channel);
        n3ds->channelOwner[inst->channel] = -1;
    }

    if (inst->file) {
        fclose(inst->file);
        inst->file = NULL;
    }

    // inst->buffers[] point into the pre-allocated streamBufPool — do NOT linearFree them.
    // inst->cacheData points into sfxCache — do NOT free it.
    memset(inst, 0, sizeof(*inst));
    inst->channel = -1;
}

static int32_t selectVictimInstance(N3DSAudioSystem* n3ds) {
    int32_t victim = -1;
    int32_t victimPriority = 0x7FFFFFFF;

    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        N3DSSoundInstance* inst = &n3ds->instances[i];
        if (!inst->active) continue;
        if (inst->priority < victimPriority) {
            victimPriority = inst->priority;
            victim = i;
        }
    }

    return victim;
}

static int32_t allocateChannel(N3DSAudioSystem* n3ds) {
    for (int32_t ch = 0; ch < n3ds->maxChannels; ch++) {
        if (n3ds->channelOwner[ch] < 0) return ch;
    }
    return -1;
}

static N3DSSoundInstance* allocateInstanceSlot(N3DSAudioSystem* n3ds, int32_t priority) {
    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        if (!n3ds->instances[i].active) return &n3ds->instances[i];
    }

    int32_t victim = selectVictimInstance(n3ds);
    if (victim < 0) return NULL;

    if (n3ds->instances[victim].priority > priority) {
        return NULL;
    }

    stopInstance(n3ds, &n3ds->instances[victim]);
    return &n3ds->instances[victim];
}

// Lazy-load a small sound into regular heap RAM so subsequent plays use
// memcpy instead of fopen+fread without consuming scarce linear RAM.
// dataOffset: byte position in the file where audio data starts (skip BCWAV header).
static N3DSSoundCacheEntry* findOrAddSfxCache(N3DSAudioSystem* n3ds, int32_t soundIndex, FILE* f,
                                              uint32_t dataOffset, uint32_t bytes,
                                              uint32_t sampleRate, uint8_t channels, uint8_t bytesPerFrame) {
    for (int32_t i = 0; i < n3ds->sfxCacheCount; i++) {
        if (n3ds->sfxCache[i].soundIndex == soundIndex) return &n3ds->sfxCache[i];
    }
    if (n3ds->sfxCacheCount >= N3DS_SFX_CACHE_MAX) return NULL;

    int16_t* buf = malloc(bytes);
    if (!buf) return NULL;

    fseek(f, (long)dataOffset, SEEK_SET);
    if (fread(buf, 1, bytes, f) != bytes) {
        free(buf);
        return NULL;
    }

    N3DSSoundCacheEntry* entry = &n3ds->sfxCache[n3ds->sfxCacheCount++];
    entry->soundIndex = soundIndex;
    entry->data       = buf;
    entry->byteSize   = bytes;
    entry->sampleRate = sampleRate;
    entry->channels = channels;
    entry->bytesPerFrame = bytesPerFrame;
    AUDIO_LOG("N3DSAudio: cached SFX idx=%d size=%lu B\n", (int)soundIndex, (unsigned long)bytes);
    return entry;
}

static bool fillAndQueueBuffer(N3DSSoundInstance* inst, int32_t bufferIndex) {
    if (!inst || (!inst->file && !inst->cacheData)) return false;

    ndspWaveBuf* wb = &inst->waveBufs[bufferIndex];
    if (isBufferQueued(wb)) return true;

    uint32_t bytesPerFrame = (inst->bytesPerFrame > 0) ? inst->bytesPerFrame : PCM_BYTES_PER_FRAME_STEREO;
    size_t want = (size_t)N3DS_STREAM_BUFFER_FRAMES * bytesPerFrame;
    size_t got;

    if (inst->cacheData) {
        // Memory-based: memcpy from the cached PCM buffer
        uint32_t remaining = inst->cacheTotalBytes - inst->cachePosByte;
        got = (remaining >= want) ? want : (size_t)remaining;
        if (got == 0 && inst->loop) {
            inst->cachePosByte = 0;
            remaining = inst->cacheTotalBytes;
            got = (remaining >= want) ? want : (size_t)remaining;
        }
        if (got == 0) { inst->endOfStream = true; return false; }
        got -= got % bytesPerFrame;
        if (got == 0) { inst->endOfStream = true; return false; }
        memcpy(inst->buffers[bufferIndex], (const uint8_t*)inst->cacheData + inst->cachePosByte, got);
        inst->cachePosByte += (uint32_t)got;
    } else {
        // File-based streaming
        got = fread(inst->buffers[bufferIndex], 1, want, inst->file);
        if (got == 0 && inst->loop) {
            // Seek back to the loop start point (dataOffset + loopStartSample * bpf).
            long loopByte = (long)inst->dataOffset
                          + (long)((uint64_t)inst->loopStartSample * bytesPerFrame);
            fseek(inst->file, loopByte, SEEK_SET);
            got = fread(inst->buffers[bufferIndex], 1, want, inst->file);
        }
        if (got == 0) { inst->endOfStream = true; return false; }
        got -= got % bytesPerFrame;
        if (got == 0) { inst->endOfStream = true; return false; }
    }

    memset(wb, 0, sizeof(*wb));
    wb->data_pcm16 = inst->buffers[bufferIndex];
    wb->nsamples = (u32)(got / bytesPerFrame);
    DSP_FlushDataCache(inst->buffers[bufferIndex], (u32) got);
    ndspChnWaveBufAdd((u32) inst->channel, wb);
    return true;
}

static bool seekAndPrime(N3DSSoundInstance* inst, uint32_t framePos) {
    if (!inst || (!inst->file && !inst->cacheData)) return false;

    uint32_t bytesPerFrame = (inst->bytesPerFrame > 0) ? inst->bytesPerFrame : PCM_BYTES_PER_FRAME_STEREO;
    if (inst->cacheData) {
        inst->cachePosByte = framePos * bytesPerFrame;
    } else {
        // File seek is relative to the audio data start, not the file start.
        long seekPos = (long)inst->dataOffset + (long)((uint64_t)framePos * bytesPerFrame);
        if (fseek(inst->file, seekPos, SEEK_SET) != 0) return false;
    }

    inst->playedFrames = framePos;
    inst->endOfStream  = false;
    ndspChnWaveBufClear((u32)inst->channel);
    memset(&inst->waveBufs[0], 0, sizeof(inst->waveBufs[0]));
    memset(&inst->waveBufs[1], 0, sizeof(inst->waveBufs[1]));

    bool first  = fillAndQueueBuffer(inst, 0);
    bool second = fillAndQueueBuffer(inst, 1);
    return first || second;
}

static bool buildCwavPathFromName(const Sound* sound, char* outPath, size_t outSize) {
    if (!sound || !sound->name || sound->name[0] == '\0' || !outPath || outSize == 0) return false;
    int n = snprintf(outPath, outSize, "romfs:/audio/%s.cwav", sound->name);
    return n > 0 && (size_t) n < outSize;
}

static bool buildCwavPathFromFile(const Sound* sound, char* outPath, size_t outSize) {
    if (!sound || !sound->file || sound->file[0] == '\0' || !outPath || outSize == 0) return false;

    const char* base = sound->file;
    const char* slash = strrchr(base, '/');
    if (slash) base = slash + 1;
    const char* bslash = strrchr(base, '\\');
    if (bslash) base = bslash + 1;

    char stem[256];
    size_t len = strlen(base);
    if (len >= sizeof(stem)) len = sizeof(stem) - 1;
    memcpy(stem, base, len);
    stem[len] = '\0';

    char* dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

    int n = snprintf(outPath, outSize, "romfs:/audio/%s.cwav", stem);
    return n > 0 && (size_t) n < outSize;
}

static bool buildSdmcCwavPathFromStem(const char* stem, char* outPath, size_t outSize) {
    if (!stem || !stem[0] || !outPath || outSize == 0) return false;
    // Prefer sdmc:/cinnamon/audio/ but also allow directly under sdmc:/cinnamon/.
    int n = snprintf(outPath, outSize, "sdmc:/cinnamon/audio/%s.cwav", stem);
    return n > 0 && (size_t)n < outSize;
}

// Minimal BCWAV (bannertool makecwav) header parser.
// Only reads the fields the runtime actually needs: sample rate, channels,
// loop info, and the byte offset within the file where PCM16 audio starts.
// Offsets in BCWAV block-level references are ABSOLUTE file offsets.
// Offsets in inner references are self-relative from the offset field itself.
//
// Verified against bannertool v1.2.0 output for mono and stereo PCM16 files.
typedef struct {
    uint8_t  encoding;        // 0=PCM8, 1=PCM16, 2=DSP_ADPCM, 3=IMA_ADPCM
    uint8_t  loop;            // 1 if the CWAV was created with -l true
    uint8_t  channels;        // 1 or 2
    uint32_t sampleRate;      // Hz
    uint32_t loopStartSample; // sample frame to resume at when looping
    uint32_t totalSamples;    // total playable samples per channel (= loopEnd field)
    uint32_t dataOffset;      // absolute file byte offset to first audio sample
    uint32_t dataSize;        // total audio bytes (all channels combined)
} BcwavInfo;

static bool parseBcwavHeader(FILE* f, BcwavInfo* out) {
    uint8_t hdr[64];
    fseek(f, 0, SEEK_SET);
    if (fread(hdr, 1, 64, f) != 64) return false;
    if (hdr[0]!='C' || hdr[1]!='W' || hdr[2]!='A' || hdr[3]!='V') return false;
    // BOM: little-endian files have 0xFEFF stored as bytes {0xFF, 0xFE}
    uint16_t bom = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
    if (bom != 0xFEFF) return false;

    // Block offsets are absolute file offsets (at header bytes 0x18 and 0x24).
    uint32_t infoOff = (uint32_t)hdr[0x18] | ((uint32_t)hdr[0x19]<<8)
                     | ((uint32_t)hdr[0x1A]<<16) | ((uint32_t)hdr[0x1B]<<24);
    uint32_t dataOff = (uint32_t)hdr[0x24] | ((uint32_t)hdr[0x25]<<8)
                     | ((uint32_t)hdr[0x26]<<16) | ((uint32_t)hdr[0x27]<<24);

    // Read INFO block (at least 32 bytes: magic + blockSize + fixed fields).
    uint8_t info[32];
    fseek(f, (long)infoOff, SEEK_SET);
    if (fread(info, 1, 32, f) != 32) return false;
    if (info[0]!='I' || info[1]!='N' || info[2]!='F' || info[3]!='O') return false;

    memset(out, 0, sizeof(*out));
    out->encoding        = info[0x08];
    out->loop            = info[0x09];
    out->sampleRate      = (uint32_t)info[0x0C] | ((uint32_t)info[0x0D]<<8)
                         | ((uint32_t)info[0x0E]<<16) | ((uint32_t)info[0x0F]<<24);
    out->loopStartSample = (uint32_t)info[0x10] | ((uint32_t)info[0x11]<<8)
                         | ((uint32_t)info[0x12]<<16) | ((uint32_t)info[0x13]<<24);
    out->totalSamples    = (uint32_t)info[0x14] | ((uint32_t)info[0x15]<<8)
                         | ((uint32_t)info[0x16]<<16) | ((uint32_t)info[0x17]<<24);
    uint32_t channelCount = (uint32_t)info[0x1C] | ((uint32_t)info[0x1D]<<8)
                          | ((uint32_t)info[0x1E]<<16) | ((uint32_t)info[0x1F]<<24);
    out->channels = (channelCount >= 2) ? 2 : 1;

    // Read DATA block size (4 bytes at dataOff+4) to compute audio extent.
    uint8_t dataSzBuf[4];
    fseek(f, (long)(dataOff + 4), SEEK_SET);
    if (fread(dataSzBuf, 1, 4, f) != 4) return false;
    uint32_t dataBlockSize = (uint32_t)dataSzBuf[0] | ((uint32_t)dataSzBuf[1]<<8)
                           | ((uint32_t)dataSzBuf[2]<<16) | ((uint32_t)dataSzBuf[3]<<24);

    // Audio samples begin 0x20 bytes into the DATA block
    // (8 bytes block header + 24 bytes reserved/padding).
    out->dataOffset = dataOff + 0x20;
    out->dataSize   = (dataBlockSize > 0x20) ? (dataBlockSize - 0x20) : 0;

    if (out->sampleRate < 8000 || out->sampleRate > 96000) return false;
    if (out->dataSize == 0) return false;
    return true;
}

static void n3dsInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;
    n3ds->dataWin = dataWin;
    n3ds->fileSystem = fileSystem;
    n3ds->masterGain = 1.0f;

    if (ndspInit() != 0) {
        AUDIO_ERR("N3DSAudio: ndspInit failed, audio disabled\n");
        n3ds->initialized = false;
        return;
    }

    // Global DSP output mode is shared by all channels.
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    // Initialize every hardware channel with a safe baseline state.
    for (int32_t i = 0; i < N3DS_MAX_AUDIO_CHANNELS; i++) {
        n3ds->channelOwner[i] = -1;
        ndspChnReset((u32) i);
        ndspChnSetInterp((u32) i, NDSP_INTERP_LINEAR);
        ndspChnSetFormat((u32) i, NDSP_FORMAT_STEREO_PCM16);
        ndspChnSetRate((u32) i, (float)N3DS_DEFAULT_SAMPLE_RATE);
    }

    // Mark all software instance slots as free.
    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        n3ds->instances[i].channel = -1;
    }

    n3ds->maxChannels = N3DS_DEFAULT_CHANNEL_COUNT;
    if (n3ds->maxChannels > N3DS_MAX_AUDIO_CHANNELS) n3ds->maxChannels = N3DS_MAX_AUDIO_CHANNELS;

    // Pre-allocate one pair of streaming buffers per instance slot to avoid
    // linearAlloc/linearFree on every audio_play_sound call.
    size_t poolBytes = (size_t)N3DS_MAX_SOUND_INSTANCES * 2 * N3DS_STREAM_BUFFER_FRAMES * PCM_BYTES_PER_FRAME_STEREO;
    n3ds->streamBufPool = linearAlloc(poolBytes);
    if (!n3ds->streamBufPool) {
        AUDIO_ERR("N3DSAudio: failed to alloc stream buffer pool (%lu KB)\n", (unsigned long)(poolBytes / 1024));
        ndspExit();
        n3ds->initialized = false;
        return;
    }

    // Init SFX RAM cache
    for (int32_t i = 0; i < N3DS_SFX_CACHE_MAX; i++) {
        n3ds->sfxCache[i].soundIndex = -1;
        n3ds->sfxCache[i].data       = NULL;
        n3ds->sfxCache[i].byteSize   = 0;
    }
    n3ds->sfxCacheCount = 0;

    // Audio groups gate logical availability of sounds (GM convention).
    n3ds->loadedGroupCount = dataWin->agrp.count;
    if (n3ds->loadedGroupCount > 0) {
        n3ds->loadedGroups = safeCalloc(n3ds->loadedGroupCount, sizeof(bool));
        n3ds->loadedGroups[0] = true; // GM group 0 is effectively always available.
    }

    n3ds->initialized = true;
    AUDIO_LOG("N3DSAudio: initialized (%ld channels)\n", (long) n3ds->maxChannels);
}

static void n3dsDestroy(AudioSystem* audio) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;

    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        stopInstance(n3ds, &n3ds->instances[i]);
    }

    // Free SFX RAM cache
    for (int32_t i = 0; i < n3ds->sfxCacheCount; i++) {
        if (n3ds->sfxCache[i].data) {
            free(n3ds->sfxCache[i].data);
            n3ds->sfxCache[i].data = NULL;
        }
    }
    n3ds->sfxCacheCount = 0;

    // Free pre-allocated stream buffer pool
    if (n3ds->streamBufPool) {
        linearFree(n3ds->streamBufPool);
        n3ds->streamBufPool = NULL;
    }

    free(n3ds->loadedGroups);
    n3ds->loadedGroups = NULL;
    n3ds->loadedGroupCount = 0;

    if (n3ds->initialized) {
        ndspExit();
        n3ds->initialized = false;
    }

    free(n3ds);
}

static void n3dsUpdate(AudioSystem* audio, float deltaTime) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;
    if (!n3ds->initialized) return;

    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        N3DSSoundInstance* inst = &n3ds->instances[i];
        if (!inst->active) continue;

        // Apply in-progress gain fades and push updated mix to NDSP.
        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (inst->fadeTimeRemaining <= 0.0f) {
                inst->fadeTimeRemaining = 0.0f;
                inst->currentGain = inst->targetGain;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
            updateChannelMix(n3ds, inst);
        }

        // Refill whichever stream buffers NDSP has finished consuming.
        for (int32_t b = 0; b < 2; b++) {
            if (inst->waveBufs[b].status == NDSP_WBUF_DONE) {
                inst->playedFrames += inst->waveBufs[b].nsamples;
                memset(&inst->waveBufs[b], 0, sizeof(inst->waveBufs[b]));
                fillAndQueueBuffer(inst, b);
            }
        }

        // Fully stop one-shots once stream data is exhausted and nothing is queued.
        bool anyQueued = isBufferQueued(&inst->waveBufs[0]) || isBufferQueued(&inst->waveBufs[1]);
        if (inst->endOfStream && !anyQueued) {
            stopInstance(n3ds, inst);
        }
    }
}

static int32_t n3dsPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;
    DataWin* dw = n3ds->dataWin;

    if (!n3ds->initialized) return -1;
    // Fully muted: reject play requests before any file I/O/cache allocations.
    if (n3ds->masterGain <= 0.0001f) return -1;
    if (soundIndex < 0 || (uint32_t) soundIndex >= dw->sond.count) {
        AUDIO_DBG_PLAY("N3DSAudio[play]: reject invalid soundIndex=%ld\n", (long) soundIndex);
        return -1;
    }

    Sound* sound = &dw->sond.sounds[soundIndex];
    AUDIO_DBG_PLAY("N3DSAudio[play]: request idx=%ld name='%s' file='%s' prio=%ld loop=%d\n",
            (long) soundIndex,
            sound->name ? sound->name : "",
            sound->file ? sound->file : "",
            (long) priority,
            loop ? 1 : 0);

    // Reserve a hardware channel, evicting lower-priority audio if required.
    int32_t channel = allocateChannel(n3ds);
    if (channel < 0) {
        int32_t victimIdx = selectVictimInstance(n3ds);
        AUDIO_DBG_PLAY("N3DSAudio[play]: no free channel, victimSlot=%ld\n", (long) victimIdx);
        if (victimIdx < 0 || n3ds->instances[victimIdx].priority > priority) {
            AUDIO_DBG_PLAY("N3DSAudio[play]: reject due to priority (incoming=%ld victimPrio=%ld)\n",
                    (long) priority,
                    victimIdx >= 0 ? (long) n3ds->instances[victimIdx].priority : -1L);
            return -1;
        }
        stopInstance(n3ds, &n3ds->instances[victimIdx]);
        channel = allocateChannel(n3ds);
        if (channel < 0) {
            AUDIO_DBG_PLAY("N3DSAudio[play]: failed to allocate channel after eviction\n");
            return -1;
        }
    }
    AUDIO_DBG_PLAY("N3DSAudio[play]: using channel=%ld\n", (long) channel);

    // Reserve a software instance slot tied to that hardware channel.
    N3DSSoundInstance* inst = allocateInstanceSlot(n3ds, priority);
    if (!inst) {
        AUDIO_DBG_PLAY("N3DSAudio[play]: no free instance slot for prio=%ld\n", (long) priority);
        return -1;
    }

    // Check SFX RAM cache first (avoids fopen/fread for frequently played sounds)
    N3DSSoundCacheEntry* cachedEntry = NULL;
    for (int32_t ci = 0; ci < n3ds->sfxCacheCount; ci++) {
        if (n3ds->sfxCache[ci].soundIndex == soundIndex) {
            cachedEntry = &n3ds->sfxCache[ci];
            break;
        }
    }
    if (cachedEntry) {
        AUDIO_DBG_PLAY("N3DSAudio[play]: cache hit idx=%ld bytes=%lu\n",
                (long) soundIndex,
                (unsigned long) cachedEntry->byteSize);
    }

    FILE* file = NULL;
    uint32_t sampleRate = N3DS_DEFAULT_SAMPLE_RATE;
    uint8_t channels = 1;
    uint8_t bytesPerFrame = PCM_BYTES_PER_FRAME_MONO;
    uint32_t dataOffset = 0;
    uint32_t dataSize = 0;
    uint32_t loopStartSample = 0;

    if (!cachedEntry) {
        // Open from romfs using the basename stem from sound->file.
        // Example: "mus/mus_story.ogg" -> "romfs:/audio/mus_story.cwav"
        char filePath[512] = {0};
        bool hasFilePath = buildCwavPathFromFile(sound, filePath, sizeof(filePath));
        AUDIO_DBG_PLAY("N3DSAudio[play]: cache miss, resolvedPath='%s'\n", hasFilePath ? filePath : "<none>");
        file = hasFilePath ? fopen(filePath, "rb") : NULL;

        // Fallback: allow loading from SD card to support modded audio / external packs.
        if (!file && sound && sound->file && sound->file[0]) {
            // Re-derive the same stem we used in buildCwavPathFromFile
            const char* base = sound->file;
            const char* slash = strrchr(base, '/');
            if (slash) base = slash + 1;
            const char* bslash = strrchr(base, '\\');
            if (bslash) base = bslash + 1;

            char stem[256];
            size_t len = strlen(base);
            if (len >= sizeof(stem)) len = sizeof(stem) - 1;
            memcpy(stem, base, len);
            stem[len] = '\0';
            char* dot = strrchr(stem, '.');
            if (dot) *dot = '\0';

            char sdPath[512] = {0};
            if (buildSdmcCwavPathFromStem(stem, sdPath, sizeof(sdPath))) {
                file = fopen(sdPath, "rb");
                if (file) {
                    AUDIO_DBG_PLAY("N3DSAudio[play]: opened SD CWAV '%s'\n", sdPath);
                }
            }
        }

        if (!file) {
            AUDIO_ERR("N3DSAudio: CWAV not found by file stem: sound='%s' file='%s'\n",
                    sound->name ? sound->name : "<null>",
                    sound->file ? sound->file : "<null>");
            return -1;
        }

        BcwavInfo cwav;
        if (!parseBcwavHeader(file, &cwav)) {
            AUDIO_ERR("N3DSAudio: BCWAV parse failed for '%s'\n", filePath);
            fclose(file);
            return -1;
        }

        sampleRate      = cwav.sampleRate;
        channels        = cwav.channels;
        bytesPerFrame   = (channels == 1) ? PCM_BYTES_PER_FRAME_MONO : PCM_BYTES_PER_FRAME_STEREO;
        dataOffset      = cwav.dataOffset;
        dataSize        = cwav.dataSize;
        loopStartSample = cwav.loopStartSample;
        AUDIO_DBG_PLAY("N3DSAudio[play]: CWAV sr=%lu ch=%u enc=%u off=%lu size=%lu\n",
            (unsigned long)sampleRate, (unsigned)channels, (unsigned)cwav.encoding,
            (unsigned long)dataOffset, (unsigned long)dataSize);

        // Cache entirely if small enough — skips fopen on subsequent plays.
        if (dataSize <= N3DS_SFX_CACHE_SIZE_LIMIT) {
            cachedEntry = findOrAddSfxCache(n3ds, soundIndex, file, dataOffset, dataSize,
                                            sampleRate, channels, bytesPerFrame);
            if (cachedEntry) {
                AUDIO_DBG_PLAY("N3DSAudio[play]: promoted to cache idx=%ld\n", (long) soundIndex);
                fclose(file);
                file = NULL;
            }
        }
    } else {
        sampleRate    = cachedEntry->sampleRate > 0 ? cachedEntry->sampleRate : N3DS_DEFAULT_SAMPLE_RATE;
        channels      = (cachedEntry->channels == 1) ? 1 : 2;
        bytesPerFrame = (cachedEntry->bytesPerFrame == PCM_BYTES_PER_FRAME_MONO)
                            ? PCM_BYTES_PER_FRAME_MONO : PCM_BYTES_PER_FRAME_STEREO;
        // dataOffset/loopStartSample are 0/0 for cached entries (cache holds only audio bytes).
    }

    // Populate runtime playback state for this instance.
    memset(inst, 0, sizeof(*inst));
    inst->active          = true;
    inst->loop            = loop;
    inst->soundIndex      = soundIndex;
    inst->priority        = priority;
    inst->channel         = channel;
    inst->instanceId      = N3DS_SOUND_INSTANCE_ID_BASE + (int32_t)(inst - n3ds->instances);
    inst->pitch           = 1.0f;
    inst->sondVolume      = sound->volume;
    inst->sondPitch       = sound->pitch;
    if (inst->sondPitch <= 0.0f) inst->sondPitch = 1.0f;
    inst->currentGain     = sound->volume;
    inst->targetGain      = sound->volume;
    inst->sampleRate      = sampleRate;
    inst->channels        = channels;
    inst->bytesPerFrame   = bytesPerFrame;
    inst->dataOffset      = dataOffset;
    inst->loopStartSample = loopStartSample;

    // Assign pre-allocated streaming buffers from pool (no linearAlloc per call)
    size_t slotIdx = (size_t)(inst - n3ds->instances);
    inst->buffers[0] = n3ds->streamBufPool + (slotIdx * 2 + 0) * (size_t)N3DS_STREAM_BUFFER_FRAMES * 2;
    inst->buffers[1] = n3ds->streamBufPool + (slotIdx * 2 + 1) * (size_t)N3DS_STREAM_BUFFER_FRAMES * 2;

    // Bind either RAM-cached audio (SFX) or file-backed stream (music/long audio).
    if (cachedEntry) {
        inst->cacheData        = cachedEntry->data;
        inst->cacheTotalBytes  = cachedEntry->byteSize;
        inst->cachePosByte     = 0;
        inst->totalFrames      = cachedEntry->byteSize / bytesPerFrame;
    } else {
        inst->file        = file;
        inst->totalFrames = dataSize / bytesPerFrame;
    }

    n3ds->channelOwner[channel] = (int32_t)(inst - n3ds->instances);

    ndspChnReset((u32)channel);
    ndspChnSetInterp((u32)channel, NDSP_INTERP_LINEAR);
    ndspChnSetFormat((u32)channel, (channels == 1) ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetPaused((u32)channel, false);

    // Apply initial per-sound pitch/gain and enqueue the first two stream buffers.
    updateChannelRate(inst);
    updateChannelMix(n3ds, inst);

    if (!seekAndPrime(inst, 0)) {
        AUDIO_DBG_PLAY("N3DSAudio[play]: seekAndPrime failed idx=%ld ch=%ld\n",
            (long) soundIndex,
            (long) channel);
        stopInstance(n3ds, inst);
        return -1;
    }

        AUDIO_DBG_PLAY("N3DSAudio[play]: started instanceId=%ld ch=%ld source=%s totalFrames=%lu\n",
            (long) inst->instanceId,
            (long) channel,
            cachedEntry ? "cache" : "file",
            (unsigned long) inst->totalFrames);

    n3ds->nextInstanceCounter++;
    return inst->instanceId;
}

static void n3dsStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;

    if (soundOrInstance >= N3DS_SOUND_INSTANCE_ID_BASE) {
        N3DSSoundInstance* inst = findInstanceById(n3ds, soundOrInstance);
        if (inst) stopInstance(n3ds, inst);
        return;
    }

    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        if (n3ds->instances[i].active && n3ds->instances[i].soundIndex == soundOrInstance) {
            stopInstance(n3ds, &n3ds->instances[i]);
        }
    }
}

static void n3dsStopAll(AudioSystem* audio) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;
    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        stopInstance(n3ds, &n3ds->instances[i]);
    }
}

static bool n3dsIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;

    if (soundOrInstance >= N3DS_SOUND_INSTANCE_ID_BASE) {
        return findInstanceById(n3ds, soundOrInstance) != NULL;
    }

    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        if (n3ds->instances[i].active && n3ds->instances[i].soundIndex == soundOrInstance) {
            return true;
        }
    }

    return false;
}

static void n3dsPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;

    if (soundOrInstance >= N3DS_SOUND_INSTANCE_ID_BASE) {
        N3DSSoundInstance* inst = findInstanceById(n3ds, soundOrInstance);
        if (inst) {
            inst->paused = true;
            ndspChnSetPaused((u32) inst->channel, true);
        }
        return;
    }

    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        N3DSSoundInstance* inst = &n3ds->instances[i];
        if (!inst->active || inst->soundIndex != soundOrInstance) continue;
        inst->paused = true;
        ndspChnSetPaused((u32) inst->channel, true);
    }
}

static void n3dsResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;

    if (soundOrInstance >= N3DS_SOUND_INSTANCE_ID_BASE) {
        N3DSSoundInstance* inst = findInstanceById(n3ds, soundOrInstance);
        if (inst) {
            inst->paused = false;
            ndspChnSetPaused((u32) inst->channel, false);
        }
        return;
    }

    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        N3DSSoundInstance* inst = &n3ds->instances[i];
        if (!inst->active || inst->soundIndex != soundOrInstance) continue;
        inst->paused = false;
        ndspChnSetPaused((u32) inst->channel, false);
    }
}

static void n3dsPauseAll(AudioSystem* audio) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;
    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        N3DSSoundInstance* inst = &n3ds->instances[i];
        if (!inst->active) continue;
        inst->paused = true;
        ndspChnSetPaused((u32) inst->channel, true);
    }
}

static void n3dsResumeAll(AudioSystem* audio) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;
    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        N3DSSoundInstance* inst = &n3ds->instances[i];
        if (!inst->active) continue;
        inst->paused = false;
        ndspChnSetPaused((u32) inst->channel, false);
    }
}

static void n3dsSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;
    if (gain < 0.0f) gain = 0.0f;

    N3DSSoundInstance* target = NULL;
    if (soundOrInstance >= N3DS_SOUND_INSTANCE_ID_BASE) {
        target = findInstanceById(n3ds, soundOrInstance);
        if (!target) return;

        target->targetGain = gain;
        if (timeMs == 0) {
            target->currentGain = gain;
            target->fadeTimeRemaining = 0.0f;
            target->fadeTotalTime = 0.0f;
            updateChannelMix(n3ds, target);
        } else {
            target->startGain = target->currentGain;
            target->fadeTotalTime = (float) timeMs / 1000.0f;
            target->fadeTimeRemaining = target->fadeTotalTime;
        }
        return;
    }

    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        N3DSSoundInstance* inst = &n3ds->instances[i];
        if (!inst->active || inst->soundIndex != soundOrInstance) continue;

        inst->targetGain = gain;
        if (timeMs == 0) {
            inst->currentGain = gain;
            inst->fadeTimeRemaining = 0.0f;
            inst->fadeTotalTime = 0.0f;
            updateChannelMix(n3ds, inst);
        } else {
            inst->startGain = inst->currentGain;
            inst->fadeTotalTime = (float) timeMs / 1000.0f;
            inst->fadeTimeRemaining = inst->fadeTotalTime;
        }
    }
}

static float n3dsGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;

    if (soundOrInstance >= N3DS_SOUND_INSTANCE_ID_BASE) {
        N3DSSoundInstance* inst = findInstanceById(n3ds, soundOrInstance);
        return inst ? inst->currentGain : 0.0f;
    }

    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        N3DSSoundInstance* inst = &n3ds->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) return inst->currentGain;
    }

    return 0.0f;
}

static void n3dsSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;
    if (pitch <= 0.0f) pitch = 0.01f;

    if (soundOrInstance >= N3DS_SOUND_INSTANCE_ID_BASE) {
        N3DSSoundInstance* inst = findInstanceById(n3ds, soundOrInstance);
        if (!inst) return;
        inst->pitch = pitch;
        updateChannelRate(inst);
        return;
    }

    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        N3DSSoundInstance* inst = &n3ds->instances[i];
        if (!inst->active || inst->soundIndex != soundOrInstance) continue;
        inst->pitch = pitch;
        updateChannelRate(inst);
    }
}

static float n3dsGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;

    if (soundOrInstance >= N3DS_SOUND_INSTANCE_ID_BASE) {
        N3DSSoundInstance* inst = findInstanceById(n3ds, soundOrInstance);
        return inst ? inst->pitch : 1.0f;
    }

    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        N3DSSoundInstance* inst = &n3ds->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) return inst->pitch;
    }

    return 1.0f;
}

static float n3dsGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;

    N3DSSoundInstance* inst = NULL;
    if (soundOrInstance >= N3DS_SOUND_INSTANCE_ID_BASE) {
        inst = findInstanceById(n3ds, soundOrInstance);
    } else {
        for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
            if (n3ds->instances[i].active && n3ds->instances[i].soundIndex == soundOrInstance) {
                inst = &n3ds->instances[i];
                break;
            }
        }
    }

    if (!inst) return 0.0f;
    uint32_t rate = inst->sampleRate > 0 ? inst->sampleRate : N3DS_DEFAULT_SAMPLE_RATE;
    return (float)inst->playedFrames / (float)rate;
}

static void n3dsSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;

    N3DSSoundInstance* inst = NULL;
    if (soundOrInstance >= N3DS_SOUND_INSTANCE_ID_BASE) {
        inst = findInstanceById(n3ds, soundOrInstance);
    } else {
        for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
            if (n3ds->instances[i].active && n3ds->instances[i].soundIndex == soundOrInstance) {
                inst = &n3ds->instances[i];
                break;
            }
        }
    }

    if (!inst || !inst->file || inst->totalFrames == 0) return;

    if (positionSeconds < 0.0f) positionSeconds = 0.0f;
    uint32_t rate = inst->sampleRate > 0 ? inst->sampleRate : N3DS_DEFAULT_SAMPLE_RATE;
    uint32_t framePos = (uint32_t)(positionSeconds * (float)rate);
    if (framePos >= inst->totalFrames) {
        framePos = inst->loop ? (framePos % inst->totalFrames) : (inst->totalFrames - 1);
    }

    seekAndPrime(inst, framePos);
    ndspChnSetPaused((u32) inst->channel, inst->paused);
}

static void n3dsSetMasterGain(AudioSystem* audio, float gain) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;
    if (gain < 0.0f) gain = 0.0f;
    n3ds->masterGain = gain;

    // When muted, stop active streams so background file reads also cease.
    if (gain <= 0.0001f) {
        n3dsStopAll(audio);
        return;
    }

    for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
        if (n3ds->instances[i].active) {
            updateChannelMix(n3ds, &n3ds->instances[i]);
        }
    }
}

static void n3dsSetChannelCount(AudioSystem* audio, int32_t count) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;
    if (count < 1) count = 1;
    if (count > N3DS_MAX_AUDIO_CHANNELS) count = N3DS_MAX_AUDIO_CHANNELS;

    if (count < n3ds->maxChannels) {
        for (int32_t i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) {
            if (!n3ds->instances[i].active) continue;
            if (n3ds->instances[i].channel >= count) {
                stopInstance(n3ds, &n3ds->instances[i]);
            }
        }
    }

    n3ds->maxChannels = count;
}

static void n3dsGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;
    if (groupIndex < 0 || (uint32_t) groupIndex >= n3ds->loadedGroupCount || !n3ds->loadedGroups) return;
    n3ds->loadedGroups[groupIndex] = true;
}

static bool n3dsGroupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    N3DSAudioSystem* n3ds = (N3DSAudioSystem*) audio;
    if (groupIndex < 0 || (uint32_t) groupIndex >= n3ds->loadedGroupCount || !n3ds->loadedGroups) return false;
    return n3ds->loadedGroups[groupIndex];
}

static AudioSystemVtable N3DS_AUDIO_VTABLE = {
    .init = n3dsInit,
    .destroy = n3dsDestroy,
    .update = n3dsUpdate,
    .playSound = n3dsPlaySound,
    .stopSound = n3dsStopSound,
    .stopAll = n3dsStopAll,
    .isPlaying = n3dsIsPlaying,
    .pauseSound = n3dsPauseSound,
    .resumeSound = n3dsResumeSound,
    .pauseAll = n3dsPauseAll,
    .resumeAll = n3dsResumeAll,
    .setSoundGain = n3dsSetSoundGain,
    .getSoundGain = n3dsGetSoundGain,
    .setSoundPitch = n3dsSetSoundPitch,
    .getSoundPitch = n3dsGetSoundPitch,
    .getTrackPosition = n3dsGetTrackPosition,
    .setTrackPosition = n3dsSetTrackPosition,
    .setMasterGain = n3dsSetMasterGain,
    .setChannelCount = n3dsSetChannelCount,
    .groupLoad = n3dsGroupLoad,
    .groupIsLoaded = n3dsGroupIsLoaded,
};

N3DSAudioSystem* N3DSAudioSystem_create(void) {
    N3DSAudioSystem* n3ds = safeCalloc(1, sizeof(N3DSAudioSystem));
    n3ds->base.vtable = &N3DS_AUDIO_VTABLE;
    n3ds->masterGain = 1.0f;
    n3ds->maxChannels = N3DS_DEFAULT_CHANNEL_COUNT;
    for (int i = 0; i < N3DS_MAX_AUDIO_CHANNELS; i++) n3ds->channelOwner[i] = -1;
    for (int i = 0; i < N3DS_MAX_SOUND_INSTANCES; i++) n3ds->instances[i].channel = -1;
    return n3ds;
}