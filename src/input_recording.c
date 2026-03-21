#include "input_recording.h"
#include "json_reader.h"
#include "json_writer.h"

#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_ds.h"

InputRecording* InputRecording_createRecorder(const char* filePath) {
    InputRecording* rec = safeCalloc(1, sizeof(InputRecording));
    rec->isRecording = true;
    rec->recordFilePath = filePath;
    return rec;
}

InputRecording* InputRecording_createPlayer(const char* playbackFilePath, const char* recordFilePath) {
    // Read the file contents
    FILE* f = fopen(playbackFilePath, "r");
    if (f == nullptr) {
        fprintf(stderr, "Error: Could not open input recording file '%s'\n", playbackFilePath);
        exit(1);
    }

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* contents = safeMalloc(fileSize + 1);
    fread(contents, 1, fileSize, f);
    contents[fileSize] = '\0';
    fclose(f);

    // Parse JSON
    JsonValue* root = JsonReader_parse(contents);
    free(contents);

    if (root == nullptr || !JsonReader_isObject(root)) {
        fprintf(stderr, "Error: Invalid JSON in input recording file '%s'\n", playbackFilePath);
        exit(1);
    }

    // Find the highest frame number to determine array size
    int objectLen = JsonReader_objectLength(root);
    int32_t maxFrame = -1;
    repeat(objectLen, i) {
        const char* key = JsonReader_getObjectKey(root, i);
        int32_t frameNum = (int32_t) strtol(key, nullptr, 10);
        if (frameNum > maxFrame) maxFrame = frameNum;
    }

    InputRecording* rec = safeCalloc(1, sizeof(InputRecording));
    rec->isPlayback = true;
    rec->playbackFrameCount = maxFrame + 1;

    // If a record path was provided, also enable recording
    if (recordFilePath != nullptr) {
        rec->isRecording = true;
        rec->recordFilePath = recordFilePath;
    }

    // Allocate playbackFrames array (one stb_ds int32_t array per frame)
    rec->playbackFrames = safeCalloc(rec->playbackFrameCount, sizeof(int32_t*));

    repeat(objectLen, i) {
        const char* key = JsonReader_getObjectKey(root, i);
        JsonValue* val = JsonReader_getObjectValue(root, i);
        int32_t frameNum = (int32_t) strtol(key, nullptr, 10);

        if (JsonReader_isArray(val)) {
            int keyCount = JsonReader_arrayLength(val);
            int32_t* keys = nullptr;
            repeat(keyCount, k) {
                JsonValue* keyVal = JsonReader_getArrayElement(val, k);
                arrput(keys, (int32_t) JsonReader_getInt(keyVal));
            }
            rec->playbackFrames[frameNum] = keys;
        }
    }

    JsonReader_free(root);
    fprintf(stderr, "InputRecording: Loaded %d frames from '%s'\n", rec->playbackFrameCount, playbackFilePath);
    return rec;
}

void InputRecording_free(InputRecording* recording) {
    if (recording == nullptr) return;

    if (recording->recordedFrames != nullptr) {
        int32_t count = (int32_t) arrlen(recording->recordedFrames);
        repeat(count, i) {
            arrfree(recording->recordedFrames[i]);
        }
        arrfree(recording->recordedFrames);
    }

    if (recording->playbackFrames != nullptr) {
        repeat(recording->playbackFrameCount, i) {
            arrfree(recording->playbackFrames[i]);
        }
        free(recording->playbackFrames);
    }

    free(recording);
}

void InputRecording_processFrame(InputRecording* recording, RunnerKeyboardState* kb, int frameNumber) {
    if (recording == nullptr) return;

    // Playback: overwrite keyboard state from recorded data (while frames remain)
    if (recording->isPlayback) {
        if (recording->playbackFrameCount > frameNumber) {
            int32_t* frameKeys = recording->playbackFrames[frameNumber];
            int32_t keyCount = (int32_t) arrlen(frameKeys);

            // Build a temporary "current held" array for this frame
            bool currentKeyDown[GML_KEY_COUNT];
            memset(currentKeyDown, 0, sizeof(currentKeyDown));
            repeat(keyCount, i) {
                int32_t key = frameKeys[i];
                if (GML_KEY_COUNT > key && key >= 0) {
                    currentKeyDown[key] = true;
                }
            }

            // Derive transitions by comparing against previousKeyDown
            repeat(GML_KEY_COUNT, key) {
                kb->keyDown[key] = currentKeyDown[key];
                kb->keyPressed[key] = currentKeyDown[key] && !recording->previousKeyDown[key];
                kb->keyReleased[key] = !currentKeyDown[key] && recording->previousKeyDown[key];
                if (kb->keyPressed[key]) {
                    kb->lastKey = (int32_t) key;
                }
            }

            memcpy(recording->previousKeyDown, currentKeyDown, sizeof(currentKeyDown));
        } else {
            // Past the end of recorded data: release everything, then let real input through
            if (!recording->playbackEnded) {
                fprintf(stderr, "InputRecording: Playback ended at frame %d (recorded %d frames)\n", frameNumber, recording->playbackFrameCount);
                recording->playbackEnded = true;

                repeat(GML_KEY_COUNT, key) {
                    kb->keyReleased[key] = recording->previousKeyDown[key];
                    kb->keyDown[key] = false;
                    kb->keyPressed[key] = false;
                }
                memset(recording->previousKeyDown, 0, sizeof(recording->previousKeyDown));
            }
            // After the first "ended" frame, real keyboard input flows through naturally
        }
    }

    // Recording: snapshot whatever the current keyboard state is (from real input or playback)
    if (recording->isRecording) {
        int32_t* heldKeys = nullptr;
        repeat(GML_KEY_COUNT, key) {
            if (kb->keyDown[key]) {
                arrput(heldKeys, (int32_t) key);
            }
        }
        arrput(recording->recordedFrames, heldKeys);
    }
}

bool InputRecording_save(InputRecording* recording) {
    if (recording == nullptr || !recording->isRecording) return false;

    int32_t frameCount = (int32_t) arrlen(recording->recordedFrames);

    JsonWriter w = JsonWriter_create();
    JsonWriter_beginObject(&w);

    repeat(frameCount, f) {
        // Frame number as string key
        char frameKey[16];
        snprintf(frameKey, sizeof(frameKey), "%d", (int) f);
        JsonWriter_key(&w, frameKey);

        JsonWriter_beginArray(&w);
        int32_t* keys = recording->recordedFrames[f];
        int32_t keyCount = (int32_t) arrlen(keys);
        repeat(keyCount, k) {
            JsonWriter_int(&w, keys[k]);
        }
        JsonWriter_endArray(&w);
    }

    JsonWriter_endObject(&w);

    FILE* f = fopen(recording->recordFilePath, "w");
    if (f == nullptr) {
        fprintf(stderr, "Error: Could not write input recording to '%s'\n", recording->recordFilePath);
        JsonWriter_free(&w);
        return false;
    }

    const char* output = JsonWriter_getOutput(&w);
    fwrite(output, 1, JsonWriter_getLength(&w), f);
    fputc('\n', f);
    fclose(f);

    fprintf(stderr, "InputRecording: Saved %d frames to '%s'\n", frameCount, recording->recordFilePath);
    JsonWriter_free(&w);
    return true;
}

bool InputRecording_isPlaybackActive(InputRecording* recording) {
    return recording != nullptr && recording->isPlayback && !recording->playbackEnded;
}
