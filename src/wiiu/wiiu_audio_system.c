#include "wiiu_audio_system.h"

#include "../data_win.h"
#include "../utils.h"

#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../vendor/stb/vorbis/stb_vorbis.c"

static void WiiUAudio_bootLog(const char* message);
static bool WiiUAudio_decodeSoundData(WiiUAudioSystem* wiiu, const uint8_t* data, size_t dataSize, WiiUDecodedSound* out);

__attribute__((weak)) void WiiUAudio_platformBootLog(const char* message) {
    (void) message;
}

static void WiiUAudio_bootLog(const char* message) {
    WiiUAudio_platformBootLog(message);
}

static uint16_t WiiUAudio_readLe16(const uint8_t* data) {
    return (uint16_t) ((uint16_t) data[0] | ((uint16_t) data[1] << 8));
}

static uint32_t WiiUAudio_readLe32(const uint8_t* data) {
    return
        ((uint32_t) data[0]) |
        ((uint32_t) data[1] << 8) |
        ((uint32_t) data[2] << 16) |
        ((uint32_t) data[3] << 24);
}

static float WiiUAudio_clampSample(float sample) {
    if (sample < -1.0f) return -1.0f;
    if (sample > 1.0f) return 1.0f;
    return sample;
}

static void WiiUAudio_resetInstance(WiiUSoundInstance* inst) {
    if (inst->audioStream != NULL) {
        SDL_FreeAudioStream(inst->audioStream);
    }
    if (inst->vorbisStream != NULL) {
        stb_vorbis_close(inst->vorbisStream);
    }
    free(inst->streamDecodeBuffer);
    free(inst->streamMixBuffer);
    memset(inst, 0, sizeof(*inst));
}

static bool WiiUAudio_shouldStreamSound(const Sound* sound) {
    if (sound == NULL || sound->file == NULL) return false;
    if ((sound->flags & 0x01) != 0) return false;
    return strncmp(sound->file, "mus_", 4) == 0;
}

static bool WiiUAudio_ensureStreamScratch(WiiUAudioSystem* wiiu, uint32_t sampleCount) {
    if (wiiu->streamScratchSamples < sampleCount) {
        free(wiiu->streamScratch);
        wiiu->streamScratch = safeMalloc((size_t) sampleCount * sizeof(float));
        wiiu->streamScratchSamples = sampleCount;
    }
    return wiiu->streamScratch != NULL;
}

static bool WiiUAudio_ensureStreamMixCapacity(WiiUSoundInstance* inst, uint32_t frameCapacity, uint32_t channelCount) {
    if (inst->streamMixCapacity >= frameCapacity) return true;
    uint32_t newCapacity = inst->streamMixCapacity > 0 ? inst->streamMixCapacity : 1024;
    while (newCapacity < frameCapacity) newCapacity *= 2;
    float* newBuffer = realloc(inst->streamMixBuffer, (size_t) newCapacity * (size_t) channelCount * sizeof(float));
    if (newBuffer == NULL) return false;
    inst->streamMixBuffer = newBuffer;
    inst->streamMixCapacity = newCapacity;
    return true;
}

static bool WiiUAudio_tryOpenVorbisStream(WiiUAudioSystem* wiiu, const char* path, WiiUSoundInstance* slot) {
    int error = 0;
    stb_vorbis* vorbis = stb_vorbis_open_filename(path, &error, NULL);
    if (vorbis == NULL) return false;

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    if (info.channels <= 0 || info.sample_rate <= 0) {
        stb_vorbis_close(vorbis);
        return false;
    }

    SDL_AudioStream* stream = SDL_NewAudioStream(
        AUDIO_F32SYS,
        (Uint8) info.channels,
        info.sample_rate,
        AUDIO_F32SYS,
        (Uint8) wiiu->audioSpec.channels,
        wiiu->audioSpec.freq
    );
    if (stream == NULL) {
        stb_vorbis_close(vorbis);
        return false;
    }

    uint32_t decodeFrames = 2048;
    float* decodeBuffer = safeMalloc((size_t) decodeFrames * (size_t) info.channels * sizeof(float));

    slot->streaming = true;
    slot->streamEof = false;
    slot->streamSourceChannels = info.channels;
    slot->streamSourceRate = info.sample_rate;
    slot->audioStream = stream;
    slot->vorbisStream = vorbis;
    slot->streamDecodeBuffer = decodeBuffer;
    slot->streamDecodeFrames = decodeFrames;
    return true;
}

static bool WiiUAudio_tryOpenMusicStream(WiiUAudioSystem* wiiu, const Sound* sound, WiiUSoundInstance* slot) {
    if (!WiiUAudio_shouldStreamSound(sound)) return false;

    const char* candidates[3] = { sound->file, NULL, NULL };
    char oggName[512];
    char wavName[512];
    if (strchr(sound->file, '.') == NULL) {
        snprintf(oggName, sizeof(oggName), "%s.ogg", sound->file);
        snprintf(wavName, sizeof(wavName), "%s.wav", sound->file);
        candidates[1] = oggName;
        candidates[2] = wavName;
    }

    repeat(3, i) {
        const char* candidate = candidates[i];
        if (candidate == NULL) continue;

        char* path = wiiu->fileSystem->vtable->resolvePath(wiiu->fileSystem, candidate);
        if (path != NULL) {
            bool ok = WiiUAudio_tryOpenVorbisStream(wiiu, path, slot);
            free(path);
            if (ok) return true;
        }

        char contentPath[640];
        snprintf(contentPath, sizeof(contentPath), "/vol/content/%s", candidate);
        if (WiiUAudio_tryOpenVorbisStream(wiiu, contentPath, slot)) return true;

        snprintf(contentPath, sizeof(contentPath), "./content/%s", candidate);
        if (WiiUAudio_tryOpenVorbisStream(wiiu, contentPath, slot)) return true;
    }

    return false;
}

static void WiiUAudio_fillMusicStream(WiiUSoundInstance* inst, uint32_t neededBytes) {
    if (!inst->streaming || inst->audioStream == NULL || inst->vorbisStream == NULL) return;

    while ((uint32_t) SDL_AudioStreamAvailable(inst->audioStream) < neededBytes) {
        int decodedFrames = stb_vorbis_get_samples_float_interleaved(
            inst->vorbisStream,
            inst->streamSourceChannels,
            inst->streamDecodeBuffer,
            (int) (inst->streamDecodeFrames * (uint32_t) inst->streamSourceChannels)
        );
        if (decodedFrames <= 0) {
            if (inst->loop) {
                stb_vorbis_seek_start(inst->vorbisStream);
                continue;
            }
            inst->streamEof = true;
            SDL_AudioStreamFlush(inst->audioStream);
            break;
        }

        SDL_AudioStreamPut(
            inst->audioStream,
            inst->streamDecodeBuffer,
            decodedFrames * inst->streamSourceChannels * (int) sizeof(float)
        );
    }
}

static uint8_t* WiiUAudio_readFileBinary(const char* path, size_t* outSize) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return NULL;

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        fclose(file);
        return NULL;
    }

    uint8_t* data = safeMalloc((size_t) size);
    size_t bytesRead = fread(data, 1, (size_t) size, file);
    fclose(file);
    if (bytesRead != (size_t) size) {
        free(data);
        return NULL;
    }

    *outSize = (size_t) size;
    return data;
}

static bool WiiUAudio_tryDecodeFile(WiiUAudioSystem* wiiu, const char* path, WiiUDecodedSound* out) {
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "wiiu_audio: try path=%s", path);
    WiiUAudio_bootLog(buffer);

    size_t encodedSize = 0;
    uint8_t* encodedData = WiiUAudio_readFileBinary(path, &encodedSize);
    if (encodedData == NULL) return false;

    bool ok = WiiUAudio_decodeSoundData(wiiu, encodedData, encodedSize, out);
    free(encodedData);
    if (ok) WiiUAudio_bootLog("wiiu_audio: external decode ok");
    return ok;
}

static bool WiiUAudio_convertPcmS16ToDevice(
    WiiUAudioSystem* wiiu,
    const int16_t* pcm,
    uint32_t frameCount,
    int32_t srcChannels,
    int32_t srcSampleRate,
    WiiUDecodedSound* out
) {
    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(
            &cvt,
            AUDIO_S16SYS,
            (Uint8) srcChannels,
            srcSampleRate,
            AUDIO_F32SYS,
            (Uint8) wiiu->audioSpec.channels,
            wiiu->audioSpec.freq) < 0) {
        return false;
    }

    int srcBytes = (int) ((size_t) frameCount * srcChannels * sizeof(int16_t));
    cvt.len = srcBytes;
    cvt.buf = safeMalloc((size_t) cvt.len * cvt.len_mult);
    memcpy(cvt.buf, pcm, (size_t) srcBytes);

    if (SDL_ConvertAudio(&cvt) < 0) {
        free(cvt.buf);
        return false;
    }

    out->samples = safeMalloc((size_t) cvt.len_cvt);
    memcpy(out->samples, cvt.buf, (size_t) cvt.len_cvt);
    free(cvt.buf);

    out->sampleCount = (uint32_t) ((size_t) cvt.len_cvt / sizeof(float));
    out->channels = wiiu->audioSpec.channels;
    out->sampleRate = wiiu->audioSpec.freq;
    out->loaded = true;
    return true;
}

static bool WiiUAudio_decodeVorbis(WiiUAudioSystem* wiiu, const uint8_t* encodedData, size_t encodedSize, WiiUDecodedSound* out) {
    int channels = 0;
    int sampleRate = 0;
    short* decoded = NULL;
    int frameCount = stb_vorbis_decode_memory(encodedData, (int) encodedSize, &channels, &sampleRate, &decoded);
    if (frameCount <= 0 || decoded == NULL || channels <= 0 || sampleRate <= 0) {
        free(decoded);
        return false;
    }

    bool ok = WiiUAudio_convertPcmS16ToDevice(wiiu, decoded, (uint32_t) frameCount, channels, sampleRate, out);
    free(decoded);
    return ok;
}

static bool WiiUAudio_decodeWav(WiiUAudioSystem* wiiu, const uint8_t* encodedData, size_t encodedSize, WiiUDecodedSound* out) {
    if (encodedSize < 44) return false;
    if (memcmp(encodedData, "RIFF", 4) != 0 || memcmp(encodedData + 8, "WAVE", 4) != 0) return false;

    uint16_t formatTag = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    const uint8_t* sampleData = NULL;
    uint32_t sampleDataSize = 0;

    size_t offset = 12;
    while (offset + 8 <= encodedSize) {
        const uint8_t* chunk = encodedData + offset;
        uint32_t chunkSize = WiiUAudio_readLe32(chunk + 4);
        size_t payloadOffset = offset + 8;
        size_t paddedSize = (size_t) chunkSize + ((chunkSize & 1U) ? 1U : 0U);
        if (payloadOffset + chunkSize > encodedSize) return false;

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunkSize < 16) return false;
            formatTag = WiiUAudio_readLe16(encodedData + payloadOffset + 0);
            channels = WiiUAudio_readLe16(encodedData + payloadOffset + 2);
            sampleRate = WiiUAudio_readLe32(encodedData + payloadOffset + 4);
            bitsPerSample = WiiUAudio_readLe16(encodedData + payloadOffset + 14);
        } else if (memcmp(chunk, "data", 4) == 0) {
            sampleData = encodedData + payloadOffset;
            sampleDataSize = chunkSize;
        }

        offset = payloadOffset + paddedSize;
    }

    if (formatTag != 1 || channels == 0 || sampleRate == 0 || sampleData == NULL || sampleDataSize == 0) {
        return false;
    }
    if (bitsPerSample != 8 && bitsPerSample != 16) {
        return false;
    }

    uint32_t frameCount = bitsPerSample == 8
        ? sampleDataSize / channels
        : sampleDataSize / ((uint32_t) channels * 2U);
    if (frameCount == 0) return false;

    int16_t* interleaved = safeMalloc((size_t) frameCount * channels * sizeof(int16_t));
    if (bitsPerSample == 8) {
        repeat(frameCount, i) {
            repeat(channels, ch) {
                uint8_t sample = sampleData[(size_t) i * channels + ch];
                interleaved[(size_t) i * channels + ch] = (int16_t) ((((int32_t) sample) - 128) << 8);
            }
        }
    } else {
        repeat(frameCount, i) {
            repeat(channels, ch) {
                const uint8_t* samplePtr = sampleData + (((size_t) i * channels + ch) * 2U);
                interleaved[(size_t) i * channels + ch] = (int16_t) WiiUAudio_readLe16(samplePtr);
            }
        }
    }

    bool ok = WiiUAudio_convertPcmS16ToDevice(wiiu, interleaved, frameCount, channels, sampleRate, out);
    free(interleaved);
    return ok;
}

static bool WiiUAudio_decodeSoundData(WiiUAudioSystem* wiiu, const uint8_t* data, size_t dataSize, WiiUDecodedSound* out) {
    memset(out, 0, sizeof(*out));
    if (WiiUAudio_decodeWav(wiiu, data, dataSize, out)) return true;
    if (WiiUAudio_decodeVorbis(wiiu, data, dataSize, out)) return true;
    return false;
}

static void WiiUAudio_callback(void* userdata, Uint8* stream, int len) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) userdata;
    uint32_t sampleCount = (uint32_t) (len / (int) sizeof(float));

    if (wiiu->mixBufferSamples < sampleCount) {
        free(wiiu->mixBuffer);
        wiiu->mixBuffer = safeMalloc((size_t) sampleCount * sizeof(float));
        wiiu->mixBufferSamples = sampleCount;
    }

    memset(wiiu->mixBuffer, 0, (size_t) sampleCount * sizeof(float));

    repeat(MAX_WIIU_SOUND_INSTANCES, i) {
        WiiUSoundInstance* inst = &wiiu->instances[i];
        if (!inst->active || inst->paused) continue;

        float gain = inst->currentGain * inst->sondVolume * wiiu->masterGain;
        double step = (double) inst->pitch * (double) inst->sondPitch;
        if (step <= 0.0) step = 1.0;

        if (inst->streaming) {
            uint32_t channelCount = (uint32_t) (wiiu->audioSpec.channels > 0 ? wiiu->audioSpec.channels : 2);
            uint32_t outFrames = sampleCount / channelCount;
            uint32_t neededFrames = (uint32_t) (outFrames * step) + 4;
            if (!WiiUAudio_ensureStreamMixCapacity(inst, inst->streamMixFrames + neededFrames, channelCount)) continue;

            uint32_t wantedFrames = neededFrames;
            while (inst->streamMixFrames < wantedFrames) {
                uint32_t missingFrames = wantedFrames - inst->streamMixFrames;
                uint32_t missingBytes = missingFrames * channelCount * (uint32_t) sizeof(float);
                WiiUAudio_fillMusicStream(inst, missingBytes);
                int availBytes = SDL_AudioStreamAvailable(inst->audioStream);
                if (availBytes <= 0) break;
                uint32_t freeFrames = inst->streamMixCapacity - inst->streamMixFrames;
                uint32_t pullFrames = (uint32_t) availBytes / (channelCount * (uint32_t) sizeof(float));
                if (pullFrames > freeFrames) pullFrames = freeFrames;
                if (pullFrames == 0) break;
                int gotBytes = SDL_AudioStreamGet(
                    inst->audioStream,
                    inst->streamMixBuffer + ((size_t) inst->streamMixFrames * channelCount),
                    (int) (pullFrames * channelCount * (uint32_t) sizeof(float))
                );
                if (gotBytes <= 0) break;
                inst->streamMixFrames += (uint32_t) gotBytes / (channelCount * (uint32_t) sizeof(float));
            }

            uint32_t mixedFrames = 0;
            for (uint32_t outFrame = 0; outFrame < outFrames; outFrame++) {
                double position = inst->position;
                uint32_t frameIndex = (uint32_t) position;
                if (frameIndex >= inst->streamMixFrames) {
                    if (inst->streamEof && SDL_AudioStreamAvailable(inst->audioStream) == 0) {
                        WiiUAudio_resetInstance(inst);
                    }
                    break;
                }

                uint32_t nextFrameIndex = frameIndex + 1;
                if (nextFrameIndex >= inst->streamMixFrames) {
                    nextFrameIndex = frameIndex;
                }

                float frac = (float) (position - (double) frameIndex);
                uint32_t srcBase0 = frameIndex * channelCount;
                uint32_t srcBase1 = nextFrameIndex * channelCount;
                uint32_t outBase = outFrame * channelCount;
                repeat(channelCount, ch) {
                    float s0 = inst->streamMixBuffer[srcBase0 + ch];
                    float s1 = inst->streamMixBuffer[srcBase1 + ch];
                    float sample = s0 + (s1 - s0) * frac;
                    wiiu->mixBuffer[outBase + ch] += sample * gain;
                }
                inst->position = position + step;
                mixedFrames = outFrame + 1;
            }

            uint32_t consumedFrames = (uint32_t) inst->position;
            if (consumedFrames > inst->streamMixFrames) consumedFrames = inst->streamMixFrames;
            if (consumedFrames > 0) {
                uint32_t remainingFrames = inst->streamMixFrames - consumedFrames;
                if (remainingFrames > 0) {
                    memmove(
                        inst->streamMixBuffer,
                        inst->streamMixBuffer + ((size_t) consumedFrames * channelCount),
                        (size_t) remainingFrames * channelCount * sizeof(float)
                    );
                }
                inst->streamMixFrames = remainingFrames;
                inst->position -= (double) consumedFrames;
                if (inst->position < 0.0) inst->position = 0.0;
            }

            (void) mixedFrames;
            continue;
        }

        if (inst->decoded == NULL || inst->decoded->samples == NULL) continue;
        WiiUDecodedSound* decoded = inst->decoded;
        uint32_t channelCount = (uint32_t) (decoded->channels > 0 ? decoded->channels : 1);
        uint32_t frameCount = decoded->sampleCount / channelCount;
        if (frameCount == 0) {
            WiiUAudio_resetInstance(inst);
            continue;
        }

        for (uint32_t outPos = 0; outPos + channelCount <= sampleCount; outPos += channelCount) {
            double position = inst->position;
            uint32_t frameIndex = (uint32_t) position;
            if (frameIndex >= frameCount) {
                if (inst->loop) {
                    inst->position = 0.0;
                    position = 0.0;
                    frameIndex = 0;
                } else {
                    WiiUAudio_resetInstance(inst);
                    break;
                }
            }

            uint32_t nextFrameIndex = frameIndex + 1;
            if (nextFrameIndex >= frameCount) {
                nextFrameIndex = inst->loop ? 0 : frameIndex;
            }

            float frac = (float) (position - (double) frameIndex);
            uint32_t srcBase0 = frameIndex * channelCount;
            uint32_t srcBase1 = nextFrameIndex * channelCount;
            repeat(channelCount, ch) {
                float s0 = decoded->samples[srcBase0 + ch];
                float s1 = decoded->samples[srcBase1 + ch];
                float sample = s0 + (s1 - s0) * frac;
                wiiu->mixBuffer[outPos + ch] += sample * gain;
            }
            inst->position = position + step;
        }
    }

    float* out = (float*) stream;
    repeat(sampleCount, i) {
        out[i] = WiiUAudio_clampSample(wiiu->mixBuffer[i]);
    }
}

static bool WiiUAudio_decodeSound(WiiUAudioSystem* wiiu, Sound* sound, WiiUDecodedSound* out) {
    bool isEmbedded = (sound->flags & 0x01) != 0;
    char buffer[256];
    snprintf(
        buffer,
        sizeof(buffer),
        "wiiu_audio: decodeSound begin embedded=%s flags=0x%08X group=%d audioFile=%d file=%s",
        isEmbedded ? "true" : "false",
        sound->flags,
        sound->audioGroup,
        sound->audioFile,
        sound->file != NULL ? sound->file : "<null>"
    );
    WiiUAudio_bootLog(buffer);

    if (sound->audioFile >= 0 && (uint32_t) sound->audioFile < wiiu->dataWin->audo.count) {
        AudioEntry* entry = &wiiu->dataWin->audo.entries[sound->audioFile];
        snprintf(
            buffer,
            sizeof(buffer),
            "wiiu_audio: audo entry idx=%d off=%u size=%u loaded=%s",
            sound->audioFile,
            entry->dataOffset,
            entry->dataSize,
            entry->data != NULL ? "true" : "false"
        );
        WiiUAudio_bootLog(buffer);
        if (entry->data != NULL && entry->dataSize >= 4) {
            snprintf(
                buffer,
                sizeof(buffer),
                "wiiu_audio: audo head %02X %02X %02X %02X",
                entry->data[0],
                entry->data[1],
                entry->data[2],
                entry->data[3]
            );
            WiiUAudio_bootLog(buffer);
        }
    }

    if (isEmbedded) {
        if (sound->audioFile < 0 || (uint32_t) sound->audioFile >= wiiu->dataWin->audo.count) return false;
        AudioEntry* entry = &wiiu->dataWin->audo.entries[sound->audioFile];
        if (entry->data == NULL) return false;
        if (!WiiUAudio_decodeSoundData(wiiu, entry->data, entry->dataSize, out)) {
            WiiUAudio_bootLog("wiiu_audio: embedded decode failed");
            return false;
        }
        return true;
    }

    if (sound->file != NULL && sound->file[0] != '\0') {
        const char* candidates[3] = { sound->file, NULL, NULL };
        char oggName[512];
        char wavName[512];
        if (strchr(sound->file, '.') == NULL) {
            snprintf(oggName, sizeof(oggName), "%s.ogg", sound->file);
            snprintf(wavName, sizeof(wavName), "%s.wav", sound->file);
            candidates[1] = oggName;
            candidates[2] = wavName;
        }

        repeat(3, i) {
            const char* candidate = candidates[i];
            if (candidate == NULL) continue;
            char* path = wiiu->fileSystem->vtable->resolvePath(wiiu->fileSystem, candidate);
            if (path != NULL) {
                bool ok = WiiUAudio_tryDecodeFile(wiiu, path, out);
                free(path);
                if (ok) return true;
            }

            char contentPath[640];
            snprintf(contentPath, sizeof(contentPath), "/vol/content/%s", candidate);
            if (WiiUAudio_tryDecodeFile(wiiu, contentPath, out)) return true;

            snprintf(contentPath, sizeof(contentPath), "./content/%s", candidate);
            if (WiiUAudio_tryDecodeFile(wiiu, contentPath, out)) return true;
        }
    }

    if (sound->audioFile >= 0 && (uint32_t) sound->audioFile < wiiu->dataWin->audo.count) {
        AudioEntry* entry = &wiiu->dataWin->audo.entries[sound->audioFile];
        if (entry->data != NULL && entry->dataSize > 0) {
            WiiUAudio_bootLog("wiiu_audio: trying AUDO fallback");
            if (WiiUAudio_decodeSoundData(wiiu, entry->data, entry->dataSize, out)) {
                WiiUAudio_bootLog("wiiu_audio: AUDO fallback decode ok");
                return true;
            }
        }
    }

    WiiUAudio_bootLog("wiiu_audio: external decode failed");
    return false;
}

static WiiUSoundInstance* WiiUAudio_findInstanceById(WiiUAudioSystem* wiiu, int32_t instanceId) {
    int32_t slotIndex = instanceId - WIIU_SOUND_INSTANCE_ID_BASE;
    if (slotIndex < 0 || slotIndex >= MAX_WIIU_SOUND_INSTANCES) return NULL;
    WiiUSoundInstance* inst = &wiiu->instances[slotIndex];
    if (!inst->active || inst->instanceId != instanceId) return NULL;
    return inst;
}

static WiiUSoundInstance* WiiUAudio_findFreeSlot(WiiUAudioSystem* wiiu) {
    repeat(MAX_WIIU_SOUND_INSTANCES, i) {
        if (!wiiu->instances[i].active) return &wiiu->instances[i];
    }

    WiiUSoundInstance* best = NULL;
    repeat(MAX_WIIU_SOUND_INSTANCES, i) {
        WiiUSoundInstance* inst = &wiiu->instances[i];
        if (!inst->loop) {
            if (best == NULL || best->priority > inst->priority) best = inst;
        }
    }

    if (best != NULL) {
        WiiUAudio_resetInstance(best);
    }
    return best;
}

static void WiiUAudioSystem_init(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    WiiUAudio_bootLog("wiiu_audio: init begin");
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    wiiu->dataWin = dataWin;
    wiiu->fileSystem = fileSystem;
    wiiu->masterGain = 1.0f;
    wiiu->decodedSounds = safeCalloc(dataWin->sond.count, sizeof(WiiUDecodedSound));
    wiiu->loadedGroups = safeCalloc(dataWin->agrp.count > 0 ? dataWin->agrp.count : 1, sizeof(bool));

    if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "wiiu_audio: SDL_InitSubSystem failed: %s", SDL_GetError());
            WiiUAudio_bootLog(buffer);
            return;
        }
    }

    SDL_AudioSpec desired;
    SDL_zero(desired);
    desired.freq = 44100;
    desired.format = AUDIO_F32SYS;
    desired.channels = 2;
    desired.samples = 1024;
    desired.callback = WiiUAudio_callback;
    desired.userdata = wiiu;

    wiiu->deviceId = SDL_OpenAudioDevice(NULL, 0, &desired, &wiiu->audioSpec, 0);
    if (wiiu->deviceId == 0) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "wiiu_audio: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        WiiUAudio_bootLog(buffer);
        return;
    }

    SDL_PauseAudioDevice(wiiu->deviceId, 0);
    wiiu->initialized = true;
    WiiUAudio_bootLog("wiiu_audio: init end");
}

static void WiiUAudioSystem_destroy(AudioSystem* audio) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (wiiu->deviceId != 0) {
        SDL_CloseAudioDevice(wiiu->deviceId);
        wiiu->deviceId = 0;
    }
    if (wiiu->decodedSounds != NULL) {
        repeat(wiiu->dataWin->sond.count, i) {
            free(wiiu->decodedSounds[i].samples);
        }
        free(wiiu->decodedSounds);
    }
    free(wiiu->loadedGroups);
    free(wiiu->mixBuffer);
    free(wiiu->streamScratch);
    free(audio);
}

static void WiiUAudioSystem_update(AudioSystem* audio, float deltaTime) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (wiiu->deviceId == 0) return;

    SDL_LockAudioDevice(wiiu->deviceId);
    repeat(MAX_WIIU_SOUND_INSTANCES, i) {
        WiiUSoundInstance* inst = &wiiu->instances[i];
        if (!inst->active) continue;

        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (inst->fadeTimeRemaining <= 0.0f) {
                inst->fadeTimeRemaining = 0.0f;
                inst->currentGain = inst->targetGain;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
        }
    }
    SDL_UnlockAudioDevice(wiiu->deviceId);
}

static int32_t WiiUAudioSystem_playSound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    DataWin* dw = wiiu->dataWin;
    char buffer[192];
    snprintf(buffer, sizeof(buffer), "wiiu_audio: playSound begin sound=%d priority=%d loop=%s", soundIndex, priority, loop ? "true" : "false");
    WiiUAudio_bootLog(buffer);

    if (soundIndex < 0 || (uint32_t) soundIndex >= dw->sond.count) return -1;
    if (wiiu->deviceId == 0) {
        WiiUAudio_bootLog("wiiu_audio: SDL device unavailable");
        return -1;
    }

    WiiUSoundInstance* slot = WiiUAudio_findFreeSlot(wiiu);
    if (slot == NULL) {
        WiiUAudio_bootLog("wiiu_audio: no free slot");
        return -1;
    }

    Sound* sound = &dw->sond.sounds[soundIndex];

    SDL_LockAudioDevice(wiiu->deviceId);
    WiiUAudio_resetInstance(slot);
    slot->active = true;
    slot->loop = loop;
    slot->soundIndex = soundIndex;
    slot->instanceId = WIIU_SOUND_INSTANCE_ID_BASE + (int32_t) (slot - wiiu->instances);
    slot->priority = priority;
    slot->position = 0.0;
    slot->currentGain = 1.0f;
    slot->targetGain = 1.0f;
    slot->startGain = 1.0f;
    slot->pitch = 1.0f;
    slot->sondVolume = sound->volume;
    slot->sondPitch = sound->pitch <= 0.0f ? 1.0f : sound->pitch;

    bool streamOk = WiiUAudio_tryOpenMusicStream(wiiu, sound, slot);
    if (!streamOk) {
        WiiUDecodedSound* decoded = &wiiu->decodedSounds[soundIndex];
        if (!decoded->loaded) {
            if (!WiiUAudio_decodeSound(wiiu, sound, decoded)) {
                SDL_UnlockAudioDevice(wiiu->deviceId);
                WiiUAudio_resetInstance(slot);
                WiiUAudio_bootLog("wiiu_audio: resolveDecodedSound failed");
                return -1;
            }
        }
        slot->decoded = decoded;
    }
    SDL_UnlockAudioDevice(wiiu->deviceId);

    snprintf(buffer, sizeof(buffer), "wiiu_audio: playSound end sound=%d instance=%d", soundIndex, slot->instanceId);
    WiiUAudio_bootLog(buffer);
    return slot->instanceId;
}

static void WiiUAudioSystem_stopSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (wiiu->deviceId == 0) return;

    SDL_LockAudioDevice(wiiu->deviceId);
    if (soundOrInstance >= WIIU_SOUND_INSTANCE_ID_BASE) {
        WiiUSoundInstance* inst = WiiUAudio_findInstanceById(wiiu, soundOrInstance);
        if (inst != NULL) WiiUAudio_resetInstance(inst);
    } else {
        repeat(MAX_WIIU_SOUND_INSTANCES, i) {
            WiiUSoundInstance* inst = &wiiu->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) WiiUAudio_resetInstance(inst);
        }
    }
    SDL_UnlockAudioDevice(wiiu->deviceId);
}

static void WiiUAudioSystem_stopAll(AudioSystem* audio) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (wiiu->deviceId == 0) return;
    SDL_LockAudioDevice(wiiu->deviceId);
    repeat(MAX_WIIU_SOUND_INSTANCES, i) { WiiUAudio_resetInstance(&wiiu->instances[i]); }
    SDL_UnlockAudioDevice(wiiu->deviceId);
}

static bool WiiUAudioSystem_isPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (soundOrInstance >= WIIU_SOUND_INSTANCE_ID_BASE) {
        WiiUSoundInstance* inst = WiiUAudio_findInstanceById(wiiu, soundOrInstance);
        return inst != NULL && inst->active && !inst->paused;
    }

    repeat(MAX_WIIU_SOUND_INSTANCES, i) {
        WiiUSoundInstance* inst = &wiiu->instances[i];
        if (inst->active && !inst->paused && inst->soundIndex == soundOrInstance) return true;
    }
    return false;
}

static void WiiUAudioSystem_pauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (wiiu->deviceId == 0) return;
    SDL_LockAudioDevice(wiiu->deviceId);
    if (soundOrInstance >= WIIU_SOUND_INSTANCE_ID_BASE) {
        WiiUSoundInstance* inst = WiiUAudio_findInstanceById(wiiu, soundOrInstance);
        if (inst != NULL) inst->paused = true;
    } else {
        repeat(MAX_WIIU_SOUND_INSTANCES, i) {
            WiiUSoundInstance* inst = &wiiu->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) inst->paused = true;
        }
    }
    SDL_UnlockAudioDevice(wiiu->deviceId);
}

static void WiiUAudioSystem_resumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (wiiu->deviceId == 0) return;
    SDL_LockAudioDevice(wiiu->deviceId);
    if (soundOrInstance >= WIIU_SOUND_INSTANCE_ID_BASE) {
        WiiUSoundInstance* inst = WiiUAudio_findInstanceById(wiiu, soundOrInstance);
        if (inst != NULL) inst->paused = false;
    } else {
        repeat(MAX_WIIU_SOUND_INSTANCES, i) {
            WiiUSoundInstance* inst = &wiiu->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) inst->paused = false;
        }
    }
    SDL_UnlockAudioDevice(wiiu->deviceId);
}

static void WiiUAudioSystem_pauseAll(AudioSystem* audio) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (wiiu->deviceId == 0) return;
    SDL_LockAudioDevice(wiiu->deviceId);
    repeat(MAX_WIIU_SOUND_INSTANCES, i) {
        if (wiiu->instances[i].active) wiiu->instances[i].paused = true;
    }
    SDL_UnlockAudioDevice(wiiu->deviceId);
}

static void WiiUAudioSystem_resumeAll(AudioSystem* audio) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (wiiu->deviceId == 0) return;
    SDL_LockAudioDevice(wiiu->deviceId);
    repeat(MAX_WIIU_SOUND_INSTANCES, i) {
        if (wiiu->instances[i].active) wiiu->instances[i].paused = false;
    }
    SDL_UnlockAudioDevice(wiiu->deviceId);
}

static void WiiUAudioSystem_setSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (wiiu->deviceId == 0) return;
    SDL_LockAudioDevice(wiiu->deviceId);
    if (soundOrInstance >= WIIU_SOUND_INSTANCE_ID_BASE) {
        WiiUSoundInstance* inst = WiiUAudio_findInstanceById(wiiu, soundOrInstance);
        if (inst != NULL) {
            if (timeMs == 0) {
                inst->currentGain = gain;
                inst->targetGain = gain;
                inst->fadeTimeRemaining = 0.0f;
            } else {
                inst->startGain = inst->currentGain;
                inst->targetGain = gain;
                inst->fadeTotalTime = (float) timeMs / 1000.0f;
                inst->fadeTimeRemaining = inst->fadeTotalTime;
            }
        }
    } else {
        repeat(MAX_WIIU_SOUND_INSTANCES, i) {
            WiiUSoundInstance* inst = &wiiu->instances[i];
            if (!inst->active || inst->soundIndex != soundOrInstance) continue;
            if (timeMs == 0) {
                inst->currentGain = gain;
                inst->targetGain = gain;
                inst->fadeTimeRemaining = 0.0f;
            } else {
                inst->startGain = inst->currentGain;
                inst->targetGain = gain;
                inst->fadeTotalTime = (float) timeMs / 1000.0f;
                inst->fadeTimeRemaining = inst->fadeTotalTime;
            }
        }
    }
    SDL_UnlockAudioDevice(wiiu->deviceId);
}

static float WiiUAudioSystem_getSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (soundOrInstance >= WIIU_SOUND_INSTANCE_ID_BASE) {
        WiiUSoundInstance* inst = WiiUAudio_findInstanceById(wiiu, soundOrInstance);
        return inst != NULL ? inst->currentGain : 0.0f;
    }
    repeat(MAX_WIIU_SOUND_INSTANCES, i) {
        WiiUSoundInstance* inst = &wiiu->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) return inst->currentGain;
    }
    return 0.0f;
}

static void WiiUAudioSystem_setSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (wiiu->deviceId == 0) return;
    if (pitch <= 0.0f) pitch = 1.0f;

    SDL_LockAudioDevice(wiiu->deviceId);
    if (soundOrInstance >= WIIU_SOUND_INSTANCE_ID_BASE) {
        WiiUSoundInstance* inst = WiiUAudio_findInstanceById(wiiu, soundOrInstance);
        if (inst != NULL) inst->pitch = pitch;
        SDL_UnlockAudioDevice(wiiu->deviceId);
        return;
    }
    repeat(MAX_WIIU_SOUND_INSTANCES, i) {
        WiiUSoundInstance* inst = &wiiu->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) inst->pitch = pitch;
    }
    SDL_UnlockAudioDevice(wiiu->deviceId);
}

static float WiiUAudioSystem_getSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (soundOrInstance >= WIIU_SOUND_INSTANCE_ID_BASE) {
        WiiUSoundInstance* inst = WiiUAudio_findInstanceById(wiiu, soundOrInstance);
        return inst != NULL ? inst->pitch : 1.0f;
    }
    repeat(MAX_WIIU_SOUND_INSTANCES, i) {
        WiiUSoundInstance* inst = &wiiu->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) return inst->pitch;
    }
    return 1.0f;
}

static float WiiUAudioSystem_getTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    WiiUSoundInstance* inst = NULL;
    if (soundOrInstance >= WIIU_SOUND_INSTANCE_ID_BASE) {
        inst = WiiUAudio_findInstanceById(wiiu, soundOrInstance);
    } else {
        repeat(MAX_WIIU_SOUND_INSTANCES, i) {
            if (wiiu->instances[i].active && wiiu->instances[i].soundIndex == soundOrInstance) {
                inst = &wiiu->instances[i];
                break;
            }
        }
    }
    if (inst != NULL && inst->decoded != NULL && inst->decoded->sampleRate > 0 && inst->decoded->channels > 0) {
        return (float) inst->position / (float) inst->decoded->sampleRate;
    }
    return 0.0f;
}

static void WiiUAudioSystem_setTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    if (wiiu->deviceId == 0) return;
    SDL_LockAudioDevice(wiiu->deviceId);
    WiiUSoundInstance* inst = NULL;
    if (soundOrInstance >= WIIU_SOUND_INSTANCE_ID_BASE) {
        inst = WiiUAudio_findInstanceById(wiiu, soundOrInstance);
    } else {
        repeat(MAX_WIIU_SOUND_INSTANCES, i) {
            if (wiiu->instances[i].active && wiiu->instances[i].soundIndex == soundOrInstance) {
                inst = &wiiu->instances[i];
                break;
            }
        }
    }
    if (inst != NULL && inst->decoded != NULL && inst->decoded->channels > 0) {
        uint32_t frame = (uint32_t) (positionSeconds * (float) inst->decoded->sampleRate);
        uint32_t frameCount = inst->decoded->sampleCount / (uint32_t) inst->decoded->channels;
        inst->position = (double) frame;
        if ((uint32_t) inst->position >= frameCount) inst->position = 0.0;
    }
    SDL_UnlockAudioDevice(wiiu->deviceId);
}

static void WiiUAudioSystem_setMasterGain(AudioSystem* audio, float gain) {
    WiiUAudioSystem* wiiu = (WiiUAudioSystem*) audio;
    wiiu->masterGain = gain;
}

static void WiiUAudioSystem_setChannelCount(AudioSystem* audio, int32_t count) {
    (void) audio;
    (void) count;
}

static void WiiUAudioSystem_groupLoad(AudioSystem* audio, int32_t groupIndex) {
    (void) audio;
    (void) groupIndex;
}

static bool WiiUAudioSystem_groupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    (void) audio;
    (void) groupIndex;
    return true;
}

static AudioSystemVtable WiiUAudioSystemVtable = {
    .init = WiiUAudioSystem_init,
    .destroy = WiiUAudioSystem_destroy,
    .update = WiiUAudioSystem_update,
    .playSound = WiiUAudioSystem_playSound,
    .stopSound = WiiUAudioSystem_stopSound,
    .stopAll = WiiUAudioSystem_stopAll,
    .isPlaying = WiiUAudioSystem_isPlaying,
    .pauseSound = WiiUAudioSystem_pauseSound,
    .resumeSound = WiiUAudioSystem_resumeSound,
    .pauseAll = WiiUAudioSystem_pauseAll,
    .resumeAll = WiiUAudioSystem_resumeAll,
    .setSoundGain = WiiUAudioSystem_setSoundGain,
    .getSoundGain = WiiUAudioSystem_getSoundGain,
    .setSoundPitch = WiiUAudioSystem_setSoundPitch,
    .getSoundPitch = WiiUAudioSystem_getSoundPitch,
    .getTrackPosition = WiiUAudioSystem_getTrackPosition,
    .setTrackPosition = WiiUAudioSystem_setTrackPosition,
    .setMasterGain = WiiUAudioSystem_setMasterGain,
    .setChannelCount = WiiUAudioSystem_setChannelCount,
    .groupLoad = WiiUAudioSystem_groupLoad,
    .groupIsLoaded = WiiUAudioSystem_groupIsLoaded,
};

WiiUAudioSystem* WiiUAudioSystem_create(void) {
    WiiUAudio_bootLog("wiiu_audio: create begin");
    WiiUAudioSystem* audio = safeCalloc(1, sizeof(WiiUAudioSystem));
    audio->base.vtable = &WiiUAudioSystemVtable;
    audio->masterGain = 1.0f;
    WiiUAudio_bootLog("wiiu_audio: create end");
    return audio;
}
