#include "../data_win.h"
#include "../vm.h"
#include "../runner.h"
#include "../runner_keyboard.h"

#include "wiiu_file_system.h"
#include "wiiu_renderer.h"
#include "wiiu_audio_system.h"
#include "pos_col_gsh.h"

#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/swap.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <vpad/input.h>
#include <whb/gfx.h>
#include <whb/proc.h>
#include <whb/sdcard.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int gBootLogFd = -1;

typedef struct {
    float progress;
    int32_t lastChunkIndex;
} WiiULoadingState;

typedef struct {
    float x;
    float y;
    float z;
    float w;
    float r;
    float g;
    float b;
    float a;
} WiiULoadingVertex;

typedef struct {
    WHBGfxShaderGroup shaderGroup;
    GX2RBuffer vertexBuffer;
    WiiULoadingVertex* vertices;
    bool ready;
} WiiULoadingGpu;

typedef struct {
    uint32_t vpadButton;
    int32_t gmlKey;
} WiiUKeyMap;

static WiiULoadingGpu gLoadingGpu;

static const WiiUKeyMap WIIU_KEY_MAPS[] = {
    { VPAD_BUTTON_UP, VK_UP },
    { VPAD_BUTTON_DOWN, VK_DOWN },
    { VPAD_BUTTON_LEFT, VK_LEFT },
    { VPAD_BUTTON_RIGHT, VK_RIGHT },
    { VPAD_BUTTON_A, 'Z' },
    { VPAD_BUTTON_B, 'X' },
    { VPAD_BUTTON_X, 'C' },
    { VPAD_BUTTON_Y, 'C' },
    { VPAD_BUTTON_PLUS, VK_ENTER },
    { VPAD_BUTTON_MINUS, VK_BACKSPACE },
    { VPAD_BUTTON_L, VK_PAGEDOWN },
    { VPAD_BUTTON_R, VK_PAGEUP },
    { VPAD_BUTTON_ZL, VK_SHIFT },
};

static void bootLog(const char* message) {
    if (gBootLogFd < 0 || message == NULL) return;

    if (strncmp(message, "runner:", 7) == 0) return;
    if (strncmp(message, "vm:", 3) == 0) return;

    write(gBootLogFd, message, strlen(message));
    write(gBootLogFd, "\n", 1);

    if (
        strncmp(message, "stage:", 6) == 0 ||
        strncmp(message, "datawin:", 8) == 0 ||
        strncmp(message, "wiiu_", 5) == 0 ||
        strncmp(message, "perf:", 5) == 0
    ) {
        fsync(gBootLogFd);
    }
}

static void openBootLog(void) {
    if (!WHBMountSdCard()) return;

    const char* mountPath = WHBGetSdCardMountPath();
    if (mountPath == NULL) return;

    char logPath[512];
    snprintf(logPath, sizeof(logPath), "%s/wiiu/apps/cinnamon/bootlog.txt", mountPath);
    gBootLogFd = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (gBootLogFd < 0) return;

    bootLog("stage: sd mounted");
}

static char* duplicateDirname(const char* path) {
    const char* lastSlash = strrchr(path, '/');
    if (lastSlash == NULL) return strdup(".");
    size_t length = (size_t) (lastSlash - path);
    char* dir = malloc(length + 1);
    memcpy(dir, path, length);
    dir[length] = '\0';
    return dir;
}

static bool fileExistsAtPath(const char* path) {
    if (path == NULL) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

static char* buildDefaultDataWinPath(const char* argv0) {
    const char* mountPath = WHBGetSdCardMountPath();
    if (mountPath != NULL) {
        char sdAppPath[512];
        snprintf(sdAppPath, sizeof(sdAppPath), "%s/wiiu/apps/cinnamon/data.win", mountPath);
        if (fileExistsAtPath(sdAppPath)) return strdup(sdAppPath);
    }

    if (fileExistsAtPath("/vol/content/data.win")) {
        return strdup("/vol/content/data.win");
    }

    char* dir = duplicateDirname(argv0);
    size_t dirLen = strlen(dir);
    const char suffix[] = "/data.win";
    char* result = malloc(dirLen + sizeof(suffix));
    memcpy(result, dir, dirLen);
    memcpy(result + dirLen, suffix, sizeof(suffix));
    free(dir);
    return result;
}

static int32_t clampRenderDimension(int32_t value, int32_t fallback, int32_t maxValue) {
    if (value <= 0) value = fallback;
    if (value > maxValue) value = maxValue;
    return value;
}

static double elapsedMs(const struct timespec* start, const struct timespec* end) {
    double seconds = (double) (end->tv_sec - start->tv_sec);
    double nanos = (double) (end->tv_nsec - start->tv_nsec);
    return seconds * 1000.0 + nanos / 1000000.0;
}

static void syncKeyState(RunnerKeyboardState* keyboard, int32_t gmlKey, bool isHeld) {
    bool wasHeld = RunnerKeyboard_check(keyboard, gmlKey);
    if (isHeld && !wasHeld) {
        RunnerKeyboard_onKeyDown(keyboard, gmlKey);
    } else if (!isHeld && wasHeld) {
        RunnerKeyboard_onKeyUp(keyboard, gmlKey);
    }
}

static void syncButtonsToKeyboard(RunnerKeyboardState* keyboard, uint32_t held) {
    repeat(sizeof(WIIU_KEY_MAPS) / sizeof(WIIU_KEY_MAPS[0]), i) {
        int32_t gmlKey = WIIU_KEY_MAPS[i].gmlKey;
        if (gmlKey == VK_LEFT || gmlKey == VK_RIGHT || gmlKey == VK_UP || gmlKey == VK_DOWN) {
            continue;
        }
        syncKeyState(keyboard, gmlKey, (held & WIIU_KEY_MAPS[i].vpadButton) != 0);
    }
}

static void syncDirectionalInputToKeyboard(RunnerKeyboardState* keyboard, uint32_t held, const VPADStatus* status) {
    const float deadzone = 0.35f;
    bool leftHeld = (held & VPAD_BUTTON_LEFT) != 0 || status->leftStick.x <= -deadzone;
    bool rightHeld = (held & VPAD_BUTTON_RIGHT) != 0 || status->leftStick.x >= deadzone;
    bool upHeld = (held & VPAD_BUTTON_UP) != 0 || status->leftStick.y >= deadzone;
    bool downHeld = (held & VPAD_BUTTON_DOWN) != 0 || status->leftStick.y <= -deadzone;

    syncKeyState(keyboard, VK_LEFT, leftHeld);
    syncKeyState(keyboard, VK_RIGHT, rightHeld);
    syncKeyState(keyboard, VK_UP, upHeld);
    syncKeyState(keyboard, VK_DOWN, downHeld);
}

void Runner_platformBootLog(const char* message) { bootLog(message); }
void VM_platformBootLog(const char* message) { bootLog(message); }
void DataWin_platformBootLog(const char* message) { bootLog(message); }
void WiiUFileSystem_platformBootLog(const char* message) { bootLog(message); }
void WiiUAudio_platformBootLog(const char* message) { bootLog(message); }
void WiiURenderer_platformBootLog(const char* message) { bootLog(message); }

static void loadingWriteRect(
    WiiULoadingVertex* vertices,
    uint32_t surfaceWidth,
    uint32_t surfaceHeight,
    float x,
    float y,
    float w,
    float h,
    float r,
    float g,
    float b,
    float a
) {
    float x0 = (x / (float) surfaceWidth) * 2.0f - 1.0f;
    float x1 = ((x + w) / (float) surfaceWidth) * 2.0f - 1.0f;
    float y0 = 1.0f - (y / (float) surfaceHeight) * 2.0f;
    float y1 = 1.0f - ((y + h) / (float) surfaceHeight) * 2.0f;

    vertices[0] = (WiiULoadingVertex){ x0, y1, 0.0f, 1.0f, r, g, b, a };
    vertices[1] = (WiiULoadingVertex){ x1, y1, 0.0f, 1.0f, r, g, b, a };
    vertices[2] = (WiiULoadingVertex){ x0, y0, 0.0f, 1.0f, r, g, b, a };
    vertices[3] = (WiiULoadingVertex){ x0, y0, 0.0f, 1.0f, r, g, b, a };
    vertices[4] = (WiiULoadingVertex){ x1, y1, 0.0f, 1.0f, r, g, b, a };
    vertices[5] = (WiiULoadingVertex){ x1, y0, 0.0f, 1.0f, r, g, b, a };
}

static void loadingDrawRect(
    GX2ContextState* context,
    GX2ColorBuffer* buffer,
    float x,
    float y,
    float w,
    float h,
    float r,
    float g,
    float b,
    float a
) {
    if (context == NULL || buffer == NULL || !gLoadingGpu.ready || gLoadingGpu.vertices == NULL) return;

    loadingWriteRect(gLoadingGpu.vertices, buffer->surface.width, buffer->surface.height, x, y, w, h, r, g, b, a);
    GX2SetContextState(context);
    GX2RInvalidateBuffer(&gLoadingGpu.vertexBuffer, 0);
    GX2RSetAttributeBuffer(&gLoadingGpu.vertexBuffer, 0, sizeof(WiiULoadingVertex), offsetof(WiiULoadingVertex, x));
    GX2RSetAttributeBuffer(&gLoadingGpu.vertexBuffer, 1, sizeof(WiiULoadingVertex), offsetof(WiiULoadingVertex, r));
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, 0, 1);
}

static bool initLoadingGpu(void) {
    memset(&gLoadingGpu, 0, sizeof(gLoadingGpu));

    if (!WHBGfxLoadGFDShaderGroup(&gLoadingGpu.shaderGroup, 0, resources_wiiu_shaders_pos_col_gsh)) {
        bootLog("stage: loading shader group load failed");
        return false;
    }

    if (!WHBGfxInitShaderAttribute(&gLoadingGpu.shaderGroup, "aPosition", 0, offsetof(WiiULoadingVertex, x), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32)) {
        bootLog("stage: loading aPosition init failed");
        WHBGfxFreeShaderGroup(&gLoadingGpu.shaderGroup);
        memset(&gLoadingGpu.shaderGroup, 0, sizeof(gLoadingGpu.shaderGroup));
        return false;
    }

    if (!WHBGfxInitShaderAttribute(&gLoadingGpu.shaderGroup, "aColour", 0, offsetof(WiiULoadingVertex, r), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32)) {
        bootLog("stage: loading aColour init failed");
        WHBGfxFreeShaderGroup(&gLoadingGpu.shaderGroup);
        memset(&gLoadingGpu.shaderGroup, 0, sizeof(gLoadingGpu.shaderGroup));
        return false;
    }

    if (!WHBGfxInitFetchShader(&gLoadingGpu.shaderGroup)) {
        bootLog("stage: loading fetch shader init failed");
        WHBGfxFreeShaderGroup(&gLoadingGpu.shaderGroup);
        memset(&gLoadingGpu.shaderGroup, 0, sizeof(gLoadingGpu.shaderGroup));
        return false;
    }

    memset(&gLoadingGpu.vertexBuffer, 0, sizeof(gLoadingGpu.vertexBuffer));
    gLoadingGpu.vertexBuffer.flags =
        GX2R_RESOURCE_BIND_VERTEX_BUFFER |
        GX2R_RESOURCE_USAGE_CPU_WRITE |
        GX2R_RESOURCE_USAGE_GPU_READ |
        GX2R_RESOURCE_USAGE_FORCE_MEM2;
    gLoadingGpu.vertexBuffer.elemSize = sizeof(WiiULoadingVertex);
    gLoadingGpu.vertexBuffer.elemCount = 6;
    if (!GX2RCreateBuffer(&gLoadingGpu.vertexBuffer)) {
        bootLog("stage: loading vertex buffer create failed");
        WHBGfxFreeShaderGroup(&gLoadingGpu.shaderGroup);
        memset(&gLoadingGpu.shaderGroup, 0, sizeof(gLoadingGpu.shaderGroup));
        return false;
    }

    gLoadingGpu.vertices = (WiiULoadingVertex*) GX2RLockBufferEx(&gLoadingGpu.vertexBuffer, 0);
    if (gLoadingGpu.vertices == NULL) {
        bootLog("stage: loading vertex buffer lock failed");
        GX2RDestroyBufferEx(&gLoadingGpu.vertexBuffer, 0);
        memset(&gLoadingGpu.vertexBuffer, 0, sizeof(gLoadingGpu.vertexBuffer));
        WHBGfxFreeShaderGroup(&gLoadingGpu.shaderGroup);
        memset(&gLoadingGpu.shaderGroup, 0, sizeof(gLoadingGpu.shaderGroup));
        return false;
    }

    gLoadingGpu.ready = true;
    return true;
}

static void shutdownLoadingGpu(void) {
    if (gLoadingGpu.vertices != NULL && GX2RBufferExists(&gLoadingGpu.vertexBuffer)) {
        GX2RUnlockBufferEx(&gLoadingGpu.vertexBuffer, 0);
    }
    if (GX2RBufferExists(&gLoadingGpu.vertexBuffer)) {
        GX2RDestroyBufferEx(&gLoadingGpu.vertexBuffer, 0);
    }
    if (gLoadingGpu.shaderGroup.vertexShader != NULL ||
        gLoadingGpu.shaderGroup.pixelShader != NULL ||
        gLoadingGpu.shaderGroup.fetchShader.program != NULL) {
        WHBGfxFreeShaderGroup(&gLoadingGpu.shaderGroup);
    }
    memset(&gLoadingGpu, 0, sizeof(gLoadingGpu));
}

static void drawLoadingBarToBuffer(GX2ContextState* context, GX2ColorBuffer* buffer, float progress) {
    if (context == NULL || buffer == NULL || !gLoadingGpu.ready) return;

    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    uint32_t width = buffer->surface.width;
    uint32_t height = buffer->surface.height;

    uint32_t barW = width * 2u / 3u;
    uint32_t barH = height / 18u;
    if (barW < 64u) barW = width > 64u ? width - 16u : width;
    if (barH < 8u) barH = 8u;

    uint32_t barX = (width > barW) ? (width - barW) / 2u : 0u;
    uint32_t barY = (height * 4u) / 5u;
    if (barY + barH >= height) {
        barY = height > (barH + 8u) ? height - barH - 8u : 0u;
    }

    uint32_t border = 3u;
    uint32_t trackX = barX + border;
    uint32_t trackY = barY + border;
    uint32_t trackW = barW > border * 2u ? barW - border * 2u : barW;
    uint32_t trackH = barH > border * 2u ? barH - border * 2u : barH;
    uint32_t fillW = (uint32_t) ((float) trackW * progress);
    if (progress > 0.0f && fillW == 0u) fillW = 1u;
    if (fillW > trackW) fillW = trackW;

    GX2SetContextState(context);
    GX2SetColorBuffer(buffer, GX2_RENDER_TARGET_0);
    GX2SetViewport(0.0f, 0.0f, (float) width, (float) height, 0.0f, 1.0f);
    GX2SetScissor(0, 0, width, height);
    GX2ClearColor(buffer, 0.02f, 0.02f, 0.04f, 1.0f);

    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
    GX2SetShaderModeEx(
        GX2_SHADER_MODE_UNIFORM_BLOCK,
        GX2GetVertexShaderGPRs(gLoadingGpu.shaderGroup.vertexShader),
        GX2GetVertexShaderStackEntries(gLoadingGpu.shaderGroup.vertexShader),
        0,
        0,
        GX2GetPixelShaderGPRs(gLoadingGpu.shaderGroup.pixelShader),
        GX2GetPixelShaderStackEntries(gLoadingGpu.shaderGroup.pixelShader)
    );
    GX2SetFetchShader(&gLoadingGpu.shaderGroup.fetchShader);
    GX2SetVertexShader(gLoadingGpu.shaderGroup.vertexShader);
    GX2SetPixelShader(gLoadingGpu.shaderGroup.pixelShader);
    GX2SetStreamOutEnable(FALSE);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x0, FALSE, TRUE);
    GX2SetBlendControl(
        GX2_RENDER_TARGET_0,
        GX2_BLEND_MODE_ONE,
        GX2_BLEND_MODE_ZERO,
        GX2_BLEND_COMBINE_MODE_ADD,
        GX2_DISABLE,
        GX2_BLEND_MODE_ONE,
        GX2_BLEND_MODE_ZERO,
        GX2_BLEND_COMBINE_MODE_ADD
    );
    GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, 0, 0, 0, 0, 0, 0, 0);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);

    loadingDrawRect(context, buffer, (float) barX, (float) barY, (float) barW, (float) barH, 0.82f, 0.64f, 0.20f, 1.0f);
    loadingDrawRect(context, buffer, (float) trackX, (float) trackY, (float) trackW, (float) trackH, 0.10f, 0.10f, 0.14f, 1.0f);

    // Draw the loading fill.
    if (fillW > 0u) {
        loadingDrawRect(context, buffer, (float) trackX, (float) trackY, (float) fillW, (float) trackH, 0.15f, 0.78f, 0.96f, 1.0f);
    }
}



static void presentStartupFrame(uint8_t r, uint8_t g, uint8_t b) {
    GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
    GX2ColorBuffer* drc = WHBGfxGetDRCColourBuffer();
    GX2ContextState* tvContext = WHBGfxGetTVContextState();
    GX2ContextState* drcContext = WHBGfxGetDRCContextState();
    if (tv == NULL || drc == NULL || tvContext == NULL || drcContext == NULL) {
        bootLog("stage: startup present skipped");
        return;
    }

    float rf = (float) r / 255.0f;
    float gf = (float) g / 255.0f;
    float bf = (float) b / 255.0f;

    GX2SetContextState(tvContext);
    GX2ClearColor(tv, rf, gf, bf, 1.0f);
    GX2CopyColorBufferToScanBuffer(tv, GX2_SCAN_TARGET_TV);

    GX2SetContextState(drcContext);
    GX2ClearColor(drc, rf, gf, bf, 1.0f);
    GX2CopyColorBufferToScanBuffer(drc, GX2_SCAN_TARGET_DRC);

    GX2Flush();
    GX2SwapScanBuffers();
    GX2DrawDone();
}

static void presentLoadingProgress(float progress) {
    GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
    GX2ColorBuffer* drc = WHBGfxGetDRCColourBuffer();
    GX2ContextState* tvContext = WHBGfxGetTVContextState();
    GX2ContextState* drcContext = WHBGfxGetDRCContextState();
    if (tv == NULL || drc == NULL || tvContext == NULL || drcContext == NULL) {
        bootLog("stage: loading present skipped");
        return;
    }

    drawLoadingBarToBuffer(tvContext, tv, progress);
    GX2CopyColorBufferToScanBuffer(tv, GX2_SCAN_TARGET_TV);

    drawLoadingBarToBuffer(drcContext, drc, progress);
    GX2CopyColorBufferToScanBuffer(drc, GX2_SCAN_TARGET_DRC);

    GX2Flush();
    GX2SwapScanBuffers();
    GX2DrawDone();
}

static void dataWinProgressCallback(
    const char* chunkName,
    int chunkIndex,
    int totalChunks,
    DataWin* dataWin,
    void* userData
) {
    (void) dataWin;
    WiiULoadingState* state = (WiiULoadingState*) userData;
    if (state == NULL) return;
    if (chunkIndex == state->lastChunkIndex) return;
    state->lastChunkIndex = chunkIndex;

    float parseProgress = totalChunks > 0 ? (float) (chunkIndex + 1) / (float) totalChunks : 0.0f;
    if (parseProgress < 0.0f) parseProgress = 0.0f;
    if (parseProgress > 1.0f) parseProgress = 1.0f;

    if (chunkName != NULL) {
        char parseLog[96];
        snprintf(parseLog, sizeof(parseLog), "datawin: parse %.4s (%d/%d)", chunkName, chunkIndex + 1, totalChunks > 0 ? totalChunks : 1);
        bootLog(parseLog);
    }

    state->progress = 0.08f + parseProgress * 0.74f;
    presentLoadingProgress(state->progress);
}

int main(int argc, char* argv[]) {
    WHBProcInit();
    openBootLog();
    bootLog("stage: after WHBProcInit");

    if (!WHBGfxInit()) {
        bootLog("stage: WHBGfxInit failed");
        if (gBootLogFd >= 0) close(gBootLogFd);
        WHBProcShutdown();
        return 1;
    }
    bootLog("stage: after WHBGfxInit");
    presentStartupFrame(0, 0, 0);
    bootLog("stage: startup frame presented");
    presentLoadingProgress(0.02f);

    VPADInit();
    WPADInit();
    KPADInit();
    bootLog("stage: input init complete");
    presentLoadingProgress(0.05f);

    if (initLoadingGpu()) {
        bootLog("stage: loading gpu init complete");
    } else {
        bootLog("stage: loading gpu init failed");
    }
    presentLoadingProgress(0.06f);

    char* dataWinPath = argc > 1 ? strdup(argv[1]) : buildDefaultDataWinPath(argv[0]);
    if (gBootLogFd >= 0) {
        char pathBuffer[768];
        snprintf(pathBuffer, sizeof(pathBuffer), "data.win path: %s", dataWinPath);
        bootLog(pathBuffer);
    }

    WiiULoadingState loadingState = {
        .progress = 0.05f,
        .lastChunkIndex = -1,
    };

    bootLog("stage: before DataWin_parse");
    DataWin* dataWin = DataWin_parse(
        dataWinPath,
        (DataWinParserOptions) {
            .parseGen8 = true,
            .parseOptn = true,
            .parseLang = true,
            .parseExtn = true,
            .parseSond = true,
            .parseAgrp = true,
            .parseSprt = true,
            .parseBgnd = true,
            .parsePath = true,
            .parseScpt = true,
            .parseGlob = true,
            .parseShdr = true,
            .parseFont = true,
            .parseTmln = true,
            .parseObjt = true,
            .parseRoom = true,
            .parseTpag = true,
            .parseCode = true,
            .parseVari = true,
            .parseFunc = true,
            .parseStrg = true,
            .parseTxtr = true,
            .parseAudo = true,
            .skipLoadingPreciseMasksForNonPreciseSprites = true,
            .progressCallback = dataWinProgressCallback,
            .progressCallbackUserData = &loadingState,
        }
    );
    bootLog("stage: after DataWin_parse");
    presentLoadingProgress(0.78f);

    bootLog("stage: before VM_create");
    VMContext* vm = VM_create(dataWin);
    bootLog("stage: after VM_create");
    presentLoadingProgress(0.82f);

    WiiUFileSystem* fileSystem = WiiUFileSystem_create(dataWinPath);
    bootLog("stage: after WiiUFileSystem_create");
    presentLoadingProgress(0.86f);

    Runner* runner = Runner_create(dataWin, vm, NULL, (FileSystem*) fileSystem, NULL);
    bootLog("stage: after Runner_create");
    presentLoadingProgress(0.90f);

    Renderer* renderer = WiiURenderer_create();
    bootLog("stage: after WiiURenderer_create");
    presentLoadingProgress(0.93f);
    renderer->vtable->init(renderer, dataWin);
    bootLog("stage: after renderer init");
    runner->renderer = renderer;
    presentLoadingProgress(0.96f);

    WiiUAudioSystem* audio = WiiUAudioSystem_create();
    audio->base.vtable->init((AudioSystem*) audio, dataWin, (FileSystem*) fileSystem);
    runner->audioSystem = (AudioSystem*) audio;
    bootLog("stage: after audio init");
    presentLoadingProgress(0.98f);

    bootLog("stage: before Runner_initFirstRoom");
    Runner_initFirstRoom(runner);
    bootLog("stage: after Runner_initFirstRoom");
    presentLoadingProgress(1.0f);
    shutdownLoadingGpu();

    struct timespec lastFrameTime;
    clock_gettime(CLOCK_MONOTONIC, &lastFrameTime);
    bool loggedFirstFrame = false;
    uint32_t perfFrameCount = 0;
    double perfVmMs = 0.0;
    double perfRenderMs = 0.0;

    while (WHBProcIsRunning() && !runner->shouldExit) {
        struct timespec frameStartTime;
        clock_gettime(CLOCK_MONOTONIC, &frameStartTime);
        RunnerKeyboard_beginFrame(runner->keyboard);

        VPADStatus vpadStatus;
        VPADReadError error;
        memset(&vpadStatus, 0, sizeof(vpadStatus));
        if (VPADRead(VPAD_CHAN_0, &vpadStatus, 1, &error) > 0 && error == VPAD_READ_SUCCESS) {
            syncButtonsToKeyboard(runner->keyboard, vpadStatus.hold);
            syncDirectionalInputToKeyboard(runner->keyboard, vpadStatus.hold, &vpadStatus);
        }

        struct timespec vmStart;
        struct timespec vmEnd;
        clock_gettime(CLOCK_MONOTONIC, &vmStart);
        Runner_step(runner);
        clock_gettime(CLOCK_MONOTONIC, &vmEnd);
        perfVmMs += elapsedMs(&vmStart, &vmEnd);

        float deltaTime = (float) (frameStartTime.tv_sec - lastFrameTime.tv_sec);
        deltaTime += (float) (frameStartTime.tv_nsec - lastFrameTime.tv_nsec) / 1000000000.0f;
        if (deltaTime < 0.0f) deltaTime = 0.0f;
        if (deltaTime > 0.1f) deltaTime = 0.1f;

        if (runner->audioSystem != NULL) {
            runner->audioSystem->vtable->update(runner->audioSystem, deltaTime);
        }

        if (!loggedFirstFrame) {
            bootLog("frame: before beginFrame");
        }

        Gen8* gen8 = &dataWin->gen8;
        int32_t nativeGameW = (int32_t) gen8->defaultWindowWidth;
        int32_t nativeGameH = (int32_t) gen8->defaultWindowHeight;
        int32_t gameW = clampRenderDimension(nativeGameW, 640, nativeGameW > 0 ? nativeGameW : 640);
        int32_t gameH = clampRenderDimension(nativeGameH, 480, nativeGameH > 0 ? nativeGameH : 480);
        float portScaleX = nativeGameW > 0 ? (float) gameW / (float) nativeGameW : 1.0f;
        float portScaleY = nativeGameH > 0 ? (float) gameH / (float) nativeGameH : 1.0f;

        struct timespec renderStart;
        struct timespec renderEnd;
        clock_gettime(CLOCK_MONOTONIC, &renderStart);

        WiiURenderer_setClearColor((WiiURenderer*) renderer, runner->drawBackgroundColor ? runner->backgroundColor : 0x000000);
        renderer->vtable->beginFrame(renderer, 0, 0, gameW, gameH, gameW, gameH);

        if (!loggedFirstFrame) {
            bootLog("frame: after beginFrame");
        }

        Room* activeRoom = runner->currentRoom;
        bool viewsEnabled = (activeRoom->flags & 1) != 0;
        bool anyViewRendered = false;

        if (viewsEnabled) {
            repeat(8, vi) {
                if (!activeRoom->views[vi].enabled) continue;

                runner->viewCurrent = vi;
                renderer->vtable->beginView(
                    renderer,
                    activeRoom->views[vi].viewX,
                    activeRoom->views[vi].viewY,
                    activeRoom->views[vi].viewWidth,
                    activeRoom->views[vi].viewHeight,
                    (int32_t) lroundf((float) activeRoom->views[vi].portX * portScaleX),
                    (int32_t) lroundf((float) activeRoom->views[vi].portY * portScaleY),
                    (int32_t) lroundf((float) activeRoom->views[vi].portWidth * portScaleX),
                    (int32_t) lroundf((float) activeRoom->views[vi].portHeight * portScaleY),
                    runner->viewAngles[vi],
                    (uint32_t) vi
                );
                if (!loggedFirstFrame) {
                    bootLog("frame: before Runner_draw view");
                }
                Runner_draw(runner);
                if (!loggedFirstFrame) {
                    bootLog("frame: after Runner_draw view");
                }
                renderer->vtable->endView(renderer);
                anyViewRendered = true;
            }
        }

        if (!anyViewRendered) {
            runner->viewCurrent = 0;
            renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, 0, 0, gameW, gameH, 0.0f, 0);
            if (!loggedFirstFrame) {
                bootLog("frame: before Runner_draw default");
            }
            Runner_draw(runner);
            if (!loggedFirstFrame) {
                bootLog("frame: after Runner_draw default");
            }
            renderer->vtable->endView(renderer);
        }

        runner->viewCurrent = 0;
        if (!loggedFirstFrame) {
            bootLog("frame: before endFrame");
        }
        renderer->vtable->endFrame(renderer);
        clock_gettime(CLOCK_MONOTONIC, &renderEnd);
        perfRenderMs += elapsedMs(&renderStart, &renderEnd);
        if (!loggedFirstFrame) {
            bootLog("frame: after endFrame");
        }

        if (!loggedFirstFrame) {
            bootLog("stage: first frame presented");
            loggedFirstFrame = true;
        }

        perfFrameCount++;
        if (perfFrameCount >= 60) {
            char perfBuffer[256];
            snprintf(
                perfBuffer,
                sizeof(perfBuffer),
                "perf: avg over %u frames vm=%.2fms render=%.2fms total=%.2fms",
                perfFrameCount,
                perfVmMs / (double) perfFrameCount,
                perfRenderMs / (double) perfFrameCount,
                (perfVmMs + perfRenderMs) / (double) perfFrameCount
            );
            bootLog(perfBuffer);
            perfFrameCount = 0;
            perfVmMs = 0.0;
            perfRenderMs = 0.0;
        }

        struct timespec frameEndTime;
        clock_gettime(CLOCK_MONOTONIC, &frameEndTime);
        double frameElapsedMs = elapsedMs(&frameStartTime, &frameEndTime);
        if (frameElapsedMs < 33.333) {
            useconds_t remainingUs = (useconds_t) ((33.333 - frameElapsedMs) * 1000.0);
            if (remainingUs > 0) {
                usleep(remainingUs);
            }
            clock_gettime(CLOCK_MONOTONIC, &lastFrameTime);
        } else {
            lastFrameTime = frameEndTime;
        }
    }

    if (runner->audioSystem != NULL) {
        runner->audioSystem->vtable->destroy(runner->audioSystem);
    }
    renderer->vtable->destroy(renderer);
    Runner_free(runner);
    WiiUFileSystem_destroy(fileSystem);
    VM_free(vm);
    DataWin_free(dataWin);
    free(dataWinPath);

    KPADShutdown();
    VPADShutdown();
    if (gBootLogFd >= 0) close(gBootLogFd);
    WHBUnmountSdCard();
    WHBGfxShutdown();
    WHBProcShutdown();
    return 0;
}
