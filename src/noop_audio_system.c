#include "noop_audio_system.h"

#include <stdlib.h>

static void noopInit(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED DataWin* dataWin, MAYBE_UNUSED FileSystem* fileSystem) {}

static void noopDestroy(AudioSystem* audio) {
    free(audio);
}

static void noopUpdate(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED float deltaTime) {}

static int32_t noopPlaySound(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundIndex, MAYBE_UNUSED int32_t priority, MAYBE_UNUSED bool loop) {
    return -1;
}

static void noopStopSound(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance) {}

static void noopStopAll(MAYBE_UNUSED AudioSystem* audio) {}

static bool noopIsPlaying(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance) {
    return false;
}

static void noopPauseSound(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance) {}

static void noopResumeSound(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance) {}

static void noopPauseAll(MAYBE_UNUSED AudioSystem* audio) {}

static void noopResumeAll(MAYBE_UNUSED AudioSystem* audio) {}

static void noopSetSoundGain(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance, MAYBE_UNUSED float gain, MAYBE_UNUSED uint32_t timeMs) {}

static float noopGetSoundGain(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance) {
    return 1.0f;
}

static void noopSetSoundPitch(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance, MAYBE_UNUSED float pitch) {}

static float noopGetSoundPitch(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance) {
    return 1.0f;
}

static float noopGetTrackPosition(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance) {
    return 0.0f;
}

static void noopSetTrackPosition(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance, MAYBE_UNUSED float positionSeconds) {}

static void noopSetMasterGain(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED float gain) {}

static void noopSetChannelCount(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t count) {}

static void noopGroupLoad(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t groupIndex) {}

static bool noopGroupIsLoaded(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t groupIndex) {
    return true;
}

static int32_t noopCreateStream(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED const char* filename) {
    return -1;
}

static bool noopDestroyStream(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t streamIndex) {
    return false;
}

static AudioSystemVtable noopVtable = {
    .init = noopInit,
    .destroy = noopDestroy,
    .update = noopUpdate,
    .playSound = noopPlaySound,
    .stopSound = noopStopSound,
    .stopAll = noopStopAll,
    .isPlaying = noopIsPlaying,
    .pauseSound = noopPauseSound,
    .resumeSound = noopResumeSound,
    .pauseAll = noopPauseAll,
    .resumeAll = noopResumeAll,
    .setSoundGain = noopSetSoundGain,
    .getSoundGain = noopGetSoundGain,
    .setSoundPitch = noopSetSoundPitch,
    .getSoundPitch = noopGetSoundPitch,
    .getTrackPosition = noopGetTrackPosition,
    .setTrackPosition = noopSetTrackPosition,
    .setMasterGain = noopSetMasterGain,
    .setChannelCount = noopSetChannelCount,
    .groupLoad = noopGroupLoad,
    .groupIsLoaded = noopGroupIsLoaded,
    .createStream = noopCreateStream,
    .destroyStream = noopDestroyStream,
};

NoopAudioSystem* NoopAudioSystem_create(void) {
    NoopAudioSystem* audio = calloc(1, sizeof(NoopAudioSystem));
    audio->base.vtable = &noopVtable;
    return audio;
}
