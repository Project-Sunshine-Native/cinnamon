// Include stb_vorbis BEFORE miniaudio so that STB_VORBIS_INCLUDE_STB_VORBIS_H is defined,
// which enables miniaudio's built-in OGG Vorbis decoding support
#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "ma_audio_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===[ Helpers ]===

static SoundInstance* findFreeSlot(MaAudioSystem* ma) {
    // First pass: find an inactive slot
    repeat(MAX_SOUND_INSTANCES, i) {
        if (!ma->instances[i].active) {
            return &ma->instances[i];
        }
    }

    // Second pass: evict the lowest-priority ended sound
    SoundInstance* best = nullptr;
    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (!ma_sound_is_playing(&inst->maSound)) {
            if (best == nullptr || best->priority > inst->priority) {
                best = inst;
            }
        }
    }

    if (best != nullptr) {
        ma_sound_uninit(&best->maSound);
        if (best->ownsDecoder) {
            ma_decoder_uninit(&best->decoder);
        }
        best->active = false;
    }

    return best;
}

static SoundInstance* findInstanceById(MaAudioSystem* ma, int32_t instanceId) {
    int32_t slotIndex = instanceId - SOUND_INSTANCE_ID_BASE;
    if (0 > slotIndex || slotIndex >= MAX_SOUND_INSTANCES) return nullptr;
    SoundInstance* inst = &ma->instances[slotIndex];
    if (!inst->active || inst->instanceId != instanceId) return nullptr;
    return inst;
}

// Helper: resolve external audio file path from Sound entry
static char* resolveExternalPath(MaAudioSystem* ma, Sound* sound) {
    const char* file = sound->file;
    if (file == nullptr || file[0] == '\0') return nullptr;

    // If the filename has no extension, append ".ogg"
    bool hasExtension = (strchr(file, '.') != nullptr);

    char filename[512];
    if (hasExtension) {
        snprintf(filename, sizeof(filename), "%s", file);
    } else {
        snprintf(filename, sizeof(filename), "%s.ogg", file);
    }

    return ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
}

// ===[ Vtable Implementations ]===

static void maInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;
    ma->base.dataWin = dataWin;
    ma->fileSystem = fileSystem;

    ma_engine_config config = ma_engine_config_init();
    ma_result result = ma_engine_init(&config, &ma->engine);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "Audio: Failed to initialize miniaudio engine (error %d)\n", result);
        return;
    }

    memset(ma->instances, 0, sizeof(ma->instances));
    ma->nextInstanceCounter = 0;

    fprintf(stderr, "Audio: miniaudio engine initialized\n");
}

static void maDestroy(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    // Uninit all active sound instances
    repeat(MAX_SOUND_INSTANCES, i) {
        if (ma->instances[i].active) {
            ma_sound_uninit(&ma->instances[i].maSound);
            if (ma->instances[i].ownsDecoder) {
                ma_decoder_uninit(&ma->instances[i].decoder);
            }
            ma->instances[i].active = false;
        }
    }

    ma_engine_uninit(&ma->engine);
    free(ma);
}

static void maUpdate(AudioSystem* audio, float deltaTime) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (!inst->active) continue;

        // Handle gain fading (for cases where we do manual fading)
        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (0.0f >= inst->fadeTimeRemaining) {
                inst->fadeTimeRemaining = 0.0f;
                inst->currentGain = inst->targetGain;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
            ma_sound_set_volume(&inst->maSound, inst->currentGain);
        }

        // Clean up ended non-looping sounds
        if (!ma_sound_is_playing(&inst->maSound) && !ma_sound_is_looping(&inst->maSound)) {
            ma_sound_uninit(&inst->maSound);
            if (inst->ownsDecoder) {
                ma_decoder_uninit(&inst->decoder);
            }
            inst->active = false;
        }
    }
}

static int32_t maPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;
    DataWin* dw = ma->base.dataWin;

    if (0 > soundIndex || (uint32_t) soundIndex >= dw->sond.count) {
        fprintf(stderr, "Audio: Invalid sound index %d\n", soundIndex);
        return -1;
    }

    Sound* sound = &dw->sond.sounds[soundIndex];

    SoundInstance* slot = findFreeSlot(ma);
    if (slot == nullptr) {
        fprintf(stderr, "Audio: No free sound slots for '%s'\n", sound->name);
        return -1;
    }

    int32_t slotIndex = (int32_t) (slot - ma->instances);
    bool isEmbedded = (sound->flags & 0x01) != 0;
    ma_result result;

    if (isEmbedded) {
        // Embedded audio: decode from AUDO chunk memory
        if (0 > sound->audioFile || (uint32_t) sound->audioFile >= dw->audo.count) {
            fprintf(stderr, "Audio: Invalid audio file index %d for sound '%s'\n", sound->audioFile, sound->name);
            return -1;
        }

        AudioEntry* entry = &dw->audo.entries[sound->audioFile];

        ma_decoder_config decoderConfig = ma_decoder_config_init_default();
        result = ma_decoder_init_memory(entry->data, entry->dataSize, &decoderConfig, &slot->decoder);
        if (result != MA_SUCCESS) {
            fprintf(stderr, "Audio: Failed to init decoder for '%s' (error %d)\n", sound->name, result);
            return -1;
        }
        slot->ownsDecoder = true;

        result = ma_sound_init_from_data_source(&ma->engine, &slot->decoder, 0, nullptr, &slot->maSound);
        if (result != MA_SUCCESS) {
            fprintf(stderr, "Audio: Failed to init sound from decoder for '%s' (error %d)\n", sound->name, result);
            ma_decoder_uninit(&slot->decoder);
            return -1;
        }
    } else {
        // External audio: load from file
        char* path = resolveExternalPath(ma, sound);
        if (path == nullptr) {
            fprintf(stderr, "Audio: Could not resolve path for sound '%s'\n", sound->name);
            return -1;
        }

        result = ma_sound_init_from_file(&ma->engine, path, 0, nullptr, nullptr, &slot->maSound);
        if (result != MA_SUCCESS) {
            fprintf(stderr, "Audio: Failed to load file for '%s' at '%s' (error %d)\n", sound->name, path, result);
            free(path);
            return -1;
        }
        free(path);
        slot->ownsDecoder = false;
    }

    // Apply SOND properties
    float volume = sound->volume;
    float pitch = sound->pitch;
    ma_sound_set_volume(&slot->maSound, volume);
    if (pitch != 1.0f) {
        ma_sound_set_pitch(&slot->maSound, pitch);
    }
    ma_sound_set_looping(&slot->maSound, loop);

    // Set up instance tracking
    slot->active = true;
    slot->soundIndex = soundIndex;
    slot->instanceId = SOUND_INSTANCE_ID_BASE + slotIndex;
    slot->currentGain = volume;
    slot->targetGain = volume;
    slot->fadeTimeRemaining = 0.0f;
    slot->fadeTotalTime = 0.0f;
    slot->startGain = volume;
    slot->priority = priority;

    // Track unique IDs for disambiguation
    ma->nextInstanceCounter++;

    ma_sound_start(&slot->maSound);

    return slot->instanceId;
}

static void maStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        // Stop specific instance
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_stop(&inst->maSound);
            ma_sound_uninit(&inst->maSound);
            if (inst->ownsDecoder) {
                ma_decoder_uninit(&inst->decoder);
            }
            inst->active = false;
        }
    } else {
        // Stop all instances of this sound resource
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_stop(&inst->maSound);
                ma_sound_uninit(&inst->maSound);
                if (inst->ownsDecoder) {
                    ma_decoder_uninit(&inst->decoder);
                }
                inst->active = false;
            }
        }
    }
}

static void maStopAll(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active) {
            ma_sound_stop(&inst->maSound);
            ma_sound_uninit(&inst->maSound);
            if (inst->ownsDecoder) {
                ma_decoder_uninit(&inst->decoder);
            }
            inst->active = false;
        }
    }
}

static bool maIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        return inst != nullptr && ma_sound_is_playing(&inst->maSound);
    } else {
        // Check if any instance of this sound resource is playing
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance && ma_sound_is_playing(&inst->maSound)) {
                return true;
            }
        }
        return false;
    }
}

static void maPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_stop(&inst->maSound);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_stop(&inst->maSound);
            }
        }
    }
}

static void maResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_start(&inst->maSound);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_start(&inst->maSound);
            }
        }
    }
}

static void maPauseAll(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active && ma_sound_is_playing(&inst->maSound)) {
            ma_sound_stop(&inst->maSound);
        }
    }
}

static void maResumeAll(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active) {
            ma_sound_start(&inst->maSound);
        }
    }
}

static void maSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            if (timeMs == 0) {
                inst->currentGain = gain;
                inst->targetGain = gain;
                inst->fadeTimeRemaining = 0.0f;
                ma_sound_set_volume(&inst->maSound, gain);
            } else {
                inst->startGain = inst->currentGain;
                inst->targetGain = gain;
                inst->fadeTotalTime = (float) timeMs / 1000.0f;
                inst->fadeTimeRemaining = inst->fadeTotalTime;
            }
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                if (timeMs == 0) {
                    inst->currentGain = gain;
                    inst->targetGain = gain;
                    inst->fadeTimeRemaining = 0.0f;
                    ma_sound_set_volume(&inst->maSound, gain);
                } else {
                    inst->startGain = inst->currentGain;
                    inst->targetGain = gain;
                    inst->fadeTotalTime = (float) timeMs / 1000.0f;
                    inst->fadeTimeRemaining = inst->fadeTotalTime;
                }
            }
        }
    }
}

static float maGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) return inst->currentGain;
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                return inst->currentGain;
            }
        }
    }
    return 0.0f;
}

static void maSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_set_pitch(&inst->maSound, pitch);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_set_pitch(&inst->maSound, pitch);
            }
        }
    }
}

static float maGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) return ma_sound_get_pitch(&inst->maSound);
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                return ma_sound_get_pitch(&inst->maSound);
            }
        }
    }
    return 1.0f;
}

static float maGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            float cursor;
            ma_result result = ma_sound_get_cursor_in_seconds(&inst->maSound, &cursor);
            if (result == MA_SUCCESS) return cursor;
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                float cursor;
                ma_result result = ma_sound_get_cursor_in_seconds(&inst->maSound, &cursor);
                if (result == MA_SUCCESS) return cursor;
            }
        }
    }
    return 0.0f;
}

static void maSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_seek_to_pcm_frame(&inst->maSound, (ma_uint64) (positionSeconds * 44100.0f));
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_seek_to_pcm_frame(&inst->maSound, (ma_uint64) (positionSeconds * 44100.0f));
            }
        }
    }
}

static void maSetMasterGain(AudioSystem* audio, float gain) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;
    ma_engine_set_volume(&ma->engine, gain);
}

static void maSetChannelCount([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t count) {
    // miniaudio handles channel management internally, this is a no-op
}

static void maGroupLoad([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t groupIndex) {
    // Group 0 is already loaded (AUDO chunk), external sounds are loaded on demand
    // Full audiogroup*.dat parsing can be added later if needed
}

static bool maGroupIsLoaded([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t groupIndex) {
    // Always report loaded -- group 0 is embedded, external files load on demand
    return true;
}

// ===[ Vtable ]===

static AudioSystemVtable maAudioSystemVtable = {
    .init = maInit,
    .destroy = maDestroy,
    .update = maUpdate,
    .playSound = maPlaySound,
    .stopSound = maStopSound,
    .stopAll = maStopAll,
    .isPlaying = maIsPlaying,
    .pauseSound = maPauseSound,
    .resumeSound = maResumeSound,
    .pauseAll = maPauseAll,
    .resumeAll = maResumeAll,
    .setSoundGain = maSetSoundGain,
    .getSoundGain = maGetSoundGain,
    .setSoundPitch = maSetSoundPitch,
    .getSoundPitch = maGetSoundPitch,
    .getTrackPosition = maGetTrackPosition,
    .setTrackPosition = maSetTrackPosition,
    .setMasterGain = maSetMasterGain,
    .setChannelCount = maSetChannelCount,
    .groupLoad = maGroupLoad,
    .groupIsLoaded = maGroupIsLoaded,
};

// ===[ Lifecycle ]===

MaAudioSystem* MaAudioSystem_create(void) {
    MaAudioSystem* ma = safeCalloc(1, sizeof(MaAudioSystem));
    ma->base.vtable = &maAudioSystemVtable;
    return ma;
}
