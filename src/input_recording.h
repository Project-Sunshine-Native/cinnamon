#pragma once

#include "common.h"
#include <stdint.h>

#include "runner_keyboard.h"

typedef struct InputRecording {
    bool isRecording;
    bool isPlayback;
    const char* recordFilePath;

    // Recording: stb_ds array of stb_ds int32_t arrays (one per frame)
    int32_t** recordedFrames;

    // Playback: same structure, loaded from JSON
    int32_t** playbackFrames;
    int32_t playbackFrameCount;
    bool playbackEnded;
    bool previousKeyDown[GML_KEY_COUNT];
} InputRecording;

// Create a recorder that snapshots keyboard state each frame
InputRecording* InputRecording_createRecorder(const char* filePath);

// Create a player that loads recorded input from a JSON file.
// If recordFilePath is non-null, also enables recording (playback + record mode).
InputRecording* InputRecording_createPlayer(const char* playbackFilePath, const char* recordFilePath);

// Free all resources
void InputRecording_free(InputRecording* recording);

// Called each frame: plays back recorded input (if active), then snapshots keyboard state (if recording)
void InputRecording_processFrame(InputRecording* recording, RunnerKeyboardState* kb, int frameNumber);

// Write recorded frames to the JSON file (returns true on success)
bool InputRecording_save(InputRecording* recording);

// Null-safe check: returns true if recording is non-null and playback hasn't ended yet
bool InputRecording_isPlaybackActive(InputRecording* recording);
