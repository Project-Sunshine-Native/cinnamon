#include "../data_win.h"
#include "../runner.h"
#include "../runner_keyboard.h"
#include "../utils.h"
#include "../vm.h"

#include "n3ds_audio_system.h"
#include "n3ds_file_system.h"
#include "n3ds_renderer.h"

#include <3ds.h>
#include <citro2d.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define N3DS_LOADING_TEXT_SCALE 0.42f
#define N3DS_BOOT_LOG_MAX_LINES 6
#define N3DS_BOOT_LOG_LINE_CHARS 88
#define N3DS_TOTAL_VRAM_BYTES (6u * 1024u * 1024u)
#define N3DS_DEBUG_ASRIEL_ROOM 330

void N3DS_tryTriggerAsrielLed(Runner* runner);

typedef struct {
    bool useCitro2D;
    C3D_RenderTarget* target;
    C2D_TextBuf textBuf;
    PrintConsole console;
    char statusLine[128];
    int chunkIndex;
    int totalChunks;
} N3DSLoadingScreen;

static char gN3DSBootLogLines[N3DS_BOOT_LOG_MAX_LINES][N3DS_BOOT_LOG_LINE_CHARS];
static int gN3DSBootLogLineCount = 0;

typedef struct {
    C2D_TextBuf textBuf;
    double displayedFps;
    double displayedRenderMs;
    double sampledRenderMs;
    uint32_t sampledFrames;
    u64 sampleStartMs;
} N3DSDebugMonitor;

static bool fileExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void N3DS_formatDebugSize(char* out, size_t outSize, uint32_t bytes) {
    if (out == NULL || outSize == 0) return;

    double kib = (double) bytes / 1024.0;
    if (kib < 1024.0) {
        snprintf(out, outSize, "%.0f KB", kib);
        return;
    }

    snprintf(out, outSize, "%.2f MB", kib / 1024.0);
}

static void N3DSDebugMonitor_init(N3DSDebugMonitor* monitor) {
    if (monitor == NULL) return;
    memset(monitor, 0, sizeof(*monitor));
    monitor->textBuf = C2D_TextBufNew(1024);
    monitor->sampleStartMs = osGetTime();
}

static void N3DSDebugMonitor_free(N3DSDebugMonitor* monitor) {
    if (monitor == NULL) return;
    if (monitor->textBuf != NULL) {
        C2D_TextBufDelete(monitor->textBuf);
        monitor->textBuf = NULL;
    }
}

static void N3DSDebugMonitor_tickFrame(N3DSDebugMonitor* monitor, double renderMs) {
    if (monitor == NULL) return;

    monitor->sampledFrames++;
    monitor->sampledRenderMs += renderMs;
    u64 nowMs = osGetTime();
    u64 elapsedMs = nowMs - monitor->sampleStartMs;
    if (elapsedMs < 250) return;

    monitor->displayedFps = ((double) monitor->sampledFrames * 1000.0) / (double) elapsedMs;
    monitor->displayedRenderMs = monitor->sampledFrames > 0
        ? monitor->sampledRenderMs / (double) monitor->sampledFrames
        : 0.0;
    monitor->sampledFrames = 0;
    monitor->sampledRenderMs = 0.0;
    monitor->sampleStartMs = nowMs;
}

//font used in godmode9 that's easy to read. got lazy and just embedded the pbm bytes directly here 

static uint8_t N3DSDebugTinyFont_getRow(char c, uint32_t row) {
    static const uint8_t font[95][10] = {
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x00, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04, 0x00, 0x00 },
        { 0x00, 0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x00, 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A, 0x00, 0x00 },
        { 0x00, 0x04, 0x0E, 0x15, 0x0C, 0x06, 0x15, 0x0E, 0x04, 0x00 },
        { 0x00, 0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13, 0x00, 0x00 },
        { 0x00, 0x04, 0x0A, 0x04, 0x09, 0x15, 0x12, 0x0D, 0x00, 0x00 },
        { 0x00, 0x04, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x00, 0x02, 0x04, 0x04, 0x04, 0x04, 0x04, 0x02, 0x00, 0x00 },
        { 0x00, 0x08, 0x04, 0x04, 0x04, 0x04, 0x04, 0x08, 0x00, 0x00 },
        { 0x00, 0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00, 0x00, 0x00 },
        { 0x00, 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x18, 0x00 },
        { 0x00, 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00, 0x00 },
        { 0x00, 0x02, 0x02, 0x04, 0x04, 0x08, 0x08, 0x10, 0x10, 0x00 },
        { 0x00, 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E, 0x00, 0x00 },
        { 0x00, 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x1F, 0x00, 0x00 },
        { 0x00, 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F, 0x00, 0x00 },
        { 0x00, 0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E, 0x00, 0x00 },
        { 0x00, 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02, 0x00, 0x00 },
        { 0x00, 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x11, 0x0E, 0x00, 0x00 },
        { 0x00, 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E, 0x00, 0x00 },
        { 0x00, 0x1F, 0x01, 0x02, 0x02, 0x04, 0x04, 0x04, 0x00, 0x00 },
        { 0x00, 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E, 0x00, 0x00 },
        { 0x00, 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x18, 0x00 },
        { 0x00, 0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x00 },
        { 0x00, 0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00 },
        { 0x00, 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00, 0x00 },
        { 0x00, 0x0E, 0x11, 0x13, 0x15, 0x17, 0x10, 0x0F, 0x00, 0x00 },
        { 0x00, 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11, 0x00, 0x00 },
        { 0x00, 0x1E, 0x09, 0x09, 0x0E, 0x09, 0x09, 0x1E, 0x00, 0x00 },
        { 0x00, 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E, 0x00, 0x00 },
        { 0x00, 0x1E, 0x09, 0x09, 0x09, 0x09, 0x09, 0x1E, 0x00, 0x00 },
        { 0x00, 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F, 0x00, 0x00 },
        { 0x00, 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10, 0x00, 0x00 },
        { 0x00, 0x0F, 0x10, 0x10, 0x13, 0x11, 0x11, 0x0F, 0x00, 0x00 },
        { 0x00, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11, 0x00, 0x00 },
        { 0x00, 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F, 0x00, 0x00 },
        { 0x00, 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x18, 0x00, 0x00 },
        { 0x00, 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11, 0x00, 0x00 },
        { 0x00, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F, 0x00, 0x00 },
        { 0x00, 0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11, 0x00, 0x00 },
        { 0x00, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11, 0x00, 0x00 },
        { 0x00, 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00, 0x00 },
        { 0x00, 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10, 0x00, 0x00 },
        { 0x00, 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D, 0x00, 0x00 },
        { 0x00, 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x11, 0x00, 0x00 },
        { 0x00, 0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E, 0x00, 0x00 },
        { 0x00, 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00 },
        { 0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00, 0x00 },
        { 0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04, 0x00, 0x00 },
        { 0x00, 0x11, 0x11, 0x11, 0x11, 0x15, 0x15, 0x0A, 0x00, 0x00 },
        { 0x00, 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11, 0x00, 0x00 },
        { 0x00, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00 },
        { 0x00, 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F, 0x00, 0x00 },
        { 0x00, 0x06, 0x04, 0x04, 0x04, 0x04, 0x04, 0x06, 0x00, 0x00 },
        { 0x00, 0x10, 0x10, 0x08, 0x08, 0x04, 0x04, 0x02, 0x02, 0x00 },
        { 0x00, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0C, 0x00, 0x00 },
        { 0x00, 0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x00 },
        { 0x00, 0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F, 0x00, 0x00 },
        { 0x00, 0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1E, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E, 0x00, 0x00 },
        { 0x00, 0x01, 0x01, 0x0F, 0x11, 0x11, 0x13, 0x0D, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0F, 0x00, 0x00 },
        { 0x00, 0x06, 0x09, 0x08, 0x1E, 0x08, 0x08, 0x08, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x0D, 0x13, 0x11, 0x11, 0x0F, 0x01, 0x1E },
        { 0x00, 0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11, 0x00, 0x00 },
        { 0x00, 0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x1F, 0x00, 0x00 },
        { 0x00, 0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x18 },
        { 0x00, 0x10, 0x10, 0x12, 0x14, 0x1C, 0x12, 0x11, 0x00, 0x00 },
        { 0x00, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x06, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x1E, 0x10, 0x10 },
        { 0x00, 0x00, 0x00, 0x0F, 0x11, 0x11, 0x13, 0x0D, 0x01, 0x01 },
        { 0x00, 0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E, 0x00, 0x00 },
        { 0x00, 0x04, 0x04, 0x0E, 0x04, 0x04, 0x04, 0x02, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x0F, 0x01, 0x1E },
        { 0x00, 0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F, 0x00, 0x00 },
        { 0x00, 0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02, 0x00, 0x00 },
        { 0x00, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
        { 0x00, 0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x08, 0x15, 0x02, 0x00, 0x00, 0x00, 0x00 },
    };

    if (row >= 10u || c < 32 || c > 126) return 0;
    return font[(uint8_t) c - 32u][row];
}

static void N3DSDebugTinyFont_drawText(const char* text, float x, float y, uint32_t color) {
    if (text == NULL) return;

    const float pixel = 1.0f;
    const float charAdvance = 6.0f;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == ' ') {
            x += charAdvance;
            continue;
        }

        repeat(10u, row) {
            uint8_t bits = N3DSDebugTinyFont_getRow(*cursor, (uint32_t) row);
            repeat(6u, col) {
                if ((bits & (uint8_t) (1u << (5u - col))) == 0u) continue;
                C2D_DrawRectSolid(x + (float) col * pixel, y + (float) row * pixel, 0.0f, pixel, pixel, color);
            }
        }
        x += charAdvance;
    }
}

static void N3DSDebugMonitor_draw(N3DSDebugMonitor* monitor, Runner* runner, Renderer* renderer) {
#ifdef N3DS_DISABLE_BOTTOM_SCREEN
    (void) monitor;
    (void) runner;
    (void) renderer;
    return;
#endif
    if (monitor == NULL || runner == NULL || renderer == NULL || monitor->textBuf == NULL) return;

    const char* roomName = "(none)";
    if (runner->currentRoom != NULL && runner->currentRoom->name != NULL && runner->currentRoom->name[0] != '\0') {
        roomName = runner->currentRoom->name;
    }

    uint32_t atlasVRAMBytes = N3DSRenderer_getResidentAtlasVRAMBytes(renderer);
    uint32_t atlasVRAMLimitBytes = N3DSRenderer_getResidentAtlasVRAMLimitBytes(renderer);
    uint32_t atlasPageCount = N3DSRenderer_getResidentAtlasPageCount(renderer);
    uint32_t atlasPageLimit = N3DSRenderer_getResidentAtlasPageLimit(renderer);
    uint32_t directVRAMBytes = N3DSRenderer_getResidentDirectAssetVRAMBytes(renderer);
    uint32_t trackedVRAMBytes = atlasVRAMBytes + directVRAMBytes;
    uint32_t ramFreeBytes = osGetMemRegionFree(MEMREGION_APPLICATION);
    uint32_t ramTotalBytes = osGetMemRegionSize(MEMREGION_APPLICATION);
    uint32_t linearFreeBytes = linearSpaceFree();

    char fpsLine[96];
    char vramLine[64];
    char atlasLine[96];
    char ramLine[96];
    char audioLine[96];
    char roomLine[96];
    char vramUsed[24];
    char vramTotal[24];
    char atlasUsed[24];
    char atlasLimit[24];
    char directUsed[24];
    char ramFree[24];
    char ramTotal[24];
    char linearFree[24];
    char audioCached[24];
    char audioLimit[24];
    uint32_t cachedSounds = 0;
    uint32_t totalSounds = 0;
    uint32_t cachedSoundBytes = 0;
    uint32_t cacheLimitBytes = 0;

    N3DSAudioSystem_getCacheStats(runner->audioSystem, &cachedSounds, &totalSounds, &cachedSoundBytes, &cacheLimitBytes);

    snprintf(
        fpsLine,
        sizeof(fpsLine),
        "FPS %.1f  R %.1fms",
        monitor->displayedFps > 0.0 ? monitor->displayedFps : 0.0,
        monitor->displayedRenderMs > 0.0 ? monitor->displayedRenderMs : 0.0
    );
    N3DS_formatDebugSize(vramUsed, sizeof(vramUsed), trackedVRAMBytes);
    N3DS_formatDebugSize(vramTotal, sizeof(vramTotal), N3DS_TOTAL_VRAM_BYTES);
    N3DS_formatDebugSize(atlasUsed, sizeof(atlasUsed), atlasVRAMBytes);
    N3DS_formatDebugSize(atlasLimit, sizeof(atlasLimit), atlasVRAMLimitBytes);
    N3DS_formatDebugSize(directUsed, sizeof(directUsed), directVRAMBytes);
    N3DS_formatDebugSize(ramFree, sizeof(ramFree), ramFreeBytes);
    N3DS_formatDebugSize(ramTotal, sizeof(ramTotal), ramTotalBytes);
    N3DS_formatDebugSize(linearFree, sizeof(linearFree), linearFreeBytes);
    N3DS_formatDebugSize(audioCached, sizeof(audioCached), cachedSoundBytes);
    N3DS_formatDebugSize(audioLimit, sizeof(audioLimit), cacheLimitBytes);
    snprintf(vramLine, sizeof(vramLine), "VRAM %s/%s",
        vramUsed,
        vramTotal);
    snprintf(atlasLine, sizeof(atlasLine), "AT %lu/%lu %s  DR %s",
        (unsigned long) atlasPageCount,
        (unsigned long) atlasPageLimit,
        atlasUsed,
        directUsed);
    snprintf(ramLine, sizeof(ramLine), "RAM %s/%s  L %s",
        ramFree,
        ramTotal,
        linearFree);
    snprintf(audioLine, sizeof(audioLine), "AUD %lu/%lu %s/%s",
        (unsigned long) cachedSounds,
        (unsigned long) totalSounds,
        audioCached,
        audioLimit);
    snprintf(roomLine, sizeof(roomLine), "R %.28s", roomName);

    N3DSRenderer_beginBottomScreenGUI(renderer, 320, 240);
    const float boxX = 92.0f;
    const float boxY = 8.0f;
    const float boxW = 220.0f;
    const float boxH = 110.0f;
    const float textX = boxX + 7.0f;
    C2D_DrawRectSolid(boxX, boxY, 0.0f, boxW, boxH, C2D_Color32(8, 10, 16, 218));
    C2D_DrawRectSolid(boxX, boxY, 0.0f, boxW, 2.0f, C2D_Color32(77, 118, 255, 245));
    C2D_DrawRectSolid(boxX, boxY + boxH - 1.0f, 0.0f, boxW, 1.0f, C2D_Color32(32, 46, 78, 230));
    N3DSDebugTinyFont_drawText("DBG", textX, boxY + 7.0f, C2D_Color32(255, 255, 255, 255));
    N3DSDebugTinyFont_drawText(fpsLine, boxX + 42.0f, boxY + 7.0f, C2D_Color32(196, 230, 255, 255));
    N3DSDebugTinyFont_drawText(vramLine, textX, boxY + 25.0f, C2D_Color32(196, 230, 255, 255));
    N3DSDebugTinyFont_drawText(atlasLine, textX, boxY + 42.0f, C2D_Color32(146, 188, 236, 255));
    N3DSDebugTinyFont_drawText(ramLine, textX, boxY + 59.0f, C2D_Color32(196, 230, 255, 255));
    N3DSDebugTinyFont_drawText(audioLine, textX, boxY + 76.0f, C2D_Color32(196, 255, 196, 255));
    N3DSDebugTinyFont_drawText(roomLine, textX, boxY + 93.0f, C2D_Color32(255, 230, 163, 255));
    N3DSRenderer_endBottomScreenGUI(renderer);
}

static bool N3DS_stringStartsWithIgnoreCase(const char* value, const char* prefix) {
    if (value == NULL || prefix == NULL) return false;
    while (*prefix != '\0') {
        if (*value == '\0') return false;
        if (tolower((unsigned char) *value) != tolower((unsigned char) *prefix)) return false;
        ++value;
        ++prefix;
    }
    return true;
}

static const char* N3DS_getRoomBorderAssetName(const Room* room) {
    const char* roomName = room != NULL ? room->name : NULL;
    if (roomName == NULL || roomName[0] == '\0') return "border_none";

    if (N3DS_stringStartsWithIgnoreCase(roomName, "room_gaster") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_mysteryman")) return "room_gaster";

    if (N3DS_stringStartsWithIgnoreCase(roomName, "room_truelab")) return "room_truelab";

    if (N3DS_stringStartsWithIgnoreCase(roomName, "room_castle") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_asghouse") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_asgoreroom") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_lastruins_corridor") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_sanscorridor") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_undertale_end")) return "room_castle";

    if (N3DS_stringStartsWithIgnoreCase(roomName, "room_fire")) return "room_fire";

    if (N3DS_stringStartsWithIgnoreCase(roomName, "room_water")) return "room_water";

    if (N3DS_stringStartsWithIgnoreCase(roomName, "room_tundra") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_ice") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_fogroom")) return "room_tundra";

    if (N3DS_stringStartsWithIgnoreCase(roomName, "room_ruins") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_area1") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_torhouse") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_torielroom") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_asrielroom") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_kitchen") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_basement") ||
        N3DS_stringStartsWithIgnoreCase(roomName, "room_ruinsexit")) return "room_ruins";

    return "border_none";
}

static bool N3DS_tryLoadRoomBorderSprite(const char* assetName, C2D_Sprite* outSprite, C2D_SpriteSheet* outSheet) {
    if (assetName == NULL || outSprite == NULL || outSheet == NULL) return false;

    char candidatePaths[2][256];
    snprintf(candidatePaths[0], sizeof(candidatePaths[0]), "sdmc:/3ds/cinnamon/gfx/borders/%s.t3x", assetName);
    snprintf(candidatePaths[1], sizeof(candidatePaths[1]), "romfs:/gfx/borders/%s.t3x", assetName);

    repeat(2, i) {
        const char* path = candidatePaths[i];
        C2D_SpriteSheet sheet = C2D_SpriteSheetLoad(path);
        if (sheet == NULL) continue;
        if (C2D_SpriteSheetCount(sheet) <= 0) {
            C2D_SpriteSheetFree(sheet);
            continue;
        }

        *outSheet = sheet;
        C2D_SpriteFromSheet(outSprite, sheet, 0);
        C2D_SpriteSetPos(outSprite, 0.0f, 0.0f);
        C2D_SpriteSetDepth(outSprite, 1.0f);
        return true;
    }

    return false;
}

static bool N3DS_refreshRoomBorderSprite(
    const Room* room,
    C2D_Sprite* borderSprite,
    C2D_SpriteSheet* borderSheet,
    bool* haveRoomBorder,
    char* loadedBorderAssetName,
    size_t loadedBorderAssetNameSize
) {
    if (borderSprite == NULL || borderSheet == NULL || haveRoomBorder == NULL ||
        loadedBorderAssetName == NULL || loadedBorderAssetNameSize == 0) {
        return false;
    }

    const char* desiredAssetName = N3DS_getRoomBorderAssetName(room);
    if (strcmp(loadedBorderAssetName, desiredAssetName) == 0) return *haveRoomBorder;

    if (*borderSheet != NULL) {
        C2D_SpriteSheetFree(*borderSheet);
        *borderSheet = NULL;
    }

    memset(borderSprite, 0, sizeof(*borderSprite));
    *haveRoomBorder = N3DS_tryLoadRoomBorderSprite(desiredAssetName, borderSprite, borderSheet);
    if (!*haveRoomBorder && strcmp(desiredAssetName, "border_none") != 0) {
        *haveRoomBorder = N3DS_tryLoadRoomBorderSprite("border_none", borderSprite, borderSheet);
        snprintf(loadedBorderAssetName, loadedBorderAssetNameSize, "%s", *haveRoomBorder ? "border_none" : desiredAssetName);
        return *haveRoomBorder;
    }

    snprintf(loadedBorderAssetName, loadedBorderAssetNameSize, "%s", desiredAssetName);
    return *haveRoomBorder;
}

static void N3DS_drawFallbackRoomBorder(void) {
    const float screenW = 400.0f;
    const float screenH = 240.0f;
    const float outer = 6.0f;
    const float inner = 3.0f;
    const uint32_t outerColor = C2D_Color32(24, 18, 8, 255);
    const uint32_t trimColor = C2D_Color32(208, 170, 84, 255);
    const uint32_t highlightColor = C2D_Color32(255, 232, 170, 255);

    const float overlayDepth = 1.0f;

    C2D_DrawRectSolid(0.0f, 0.0f, overlayDepth, screenW, outer, outerColor);
    C2D_DrawRectSolid(0.0f, screenH - outer, overlayDepth, screenW, outer, outerColor);
    C2D_DrawRectSolid(0.0f, outer, overlayDepth, outer, screenH - outer * 2.0f, outerColor);
    C2D_DrawRectSolid(screenW - outer, outer, overlayDepth, outer, screenH - outer * 2.0f, outerColor);

    C2D_DrawRectSolid(outer, outer, overlayDepth, screenW - outer * 2.0f, inner, trimColor);
    C2D_DrawRectSolid(outer, screenH - outer - inner, overlayDepth, screenW - outer * 2.0f, inner, trimColor);
    C2D_DrawRectSolid(outer, outer + inner, overlayDepth, inner, screenH - (outer + inner) * 2.0f, trimColor);
    C2D_DrawRectSolid(screenW - outer - inner, outer + inner, overlayDepth, inner, screenH - (outer + inner) * 2.0f, trimColor);

    C2D_DrawRectSolid(outer + inner, outer + inner, overlayDepth, screenW - (outer + inner) * 2.0f, 1.0f, highlightColor);
    C2D_DrawRectSolid(outer + inner, screenH - outer - inner - 1.0f, overlayDepth, screenW - (outer + inner) * 2.0f, 1.0f, highlightColor);
}

static char* chooseDataWinPath(void) {
    if (fileExists("romfs:/data.win")) return safeStrdup("romfs:/data.win");
    if (fileExists("romfs:/game.unx")) return safeStrdup("romfs:/game.unx");
    if (fileExists("sdmc:/3ds/cinnamon/game.unx")) return safeStrdup("sdmc:/3ds/cinnamon/game.unx");
    return safeStrdup("sdmc:/3ds/cinnamon/data.win");
}

static void syncKey(RunnerKeyboardState* keyboard, bool* state, int32_t key, bool held) {
    if (held && !*state) RunnerKeyboard_onKeyDown(keyboard, key);
    if (!held && *state) RunnerKeyboard_onKeyUp(keyboard, key);
    *state = held;
}

static void N3DSLoadingScreen_free(N3DSLoadingScreen* screen) {
    if (screen == NULL) return;
    if (screen->textBuf != NULL) {
        C2D_TextBufDelete(screen->textBuf);
        screen->textBuf = NULL;
    }
    if (screen->target != NULL) {
        C3D_RenderTargetDelete(screen->target);
        screen->target = NULL;
    }
}

static void N3DS_appendBootLog(const char* message) {
    if (message == NULL || message[0] == '\0') return;

    if (gN3DSBootLogLineCount < N3DS_BOOT_LOG_MAX_LINES) {
        snprintf(
            gN3DSBootLogLines[gN3DSBootLogLineCount],
            sizeof(gN3DSBootLogLines[gN3DSBootLogLineCount]),
            "%s",
            message
        );
        gN3DSBootLogLineCount++;
        return;
    }

    repeat(N3DS_BOOT_LOG_MAX_LINES - 1, i) {
        snprintf(
            gN3DSBootLogLines[i],
            sizeof(gN3DSBootLogLines[i]),
            "%s",
            gN3DSBootLogLines[i + 1]
        );
    }
    snprintf(
        gN3DSBootLogLines[N3DS_BOOT_LOG_MAX_LINES - 1],
        sizeof(gN3DSBootLogLines[N3DS_BOOT_LOG_MAX_LINES - 1]),
        "%s",
        message
    );
}

void Runner_platformBootLog(const char* message) {
    N3DS_appendBootLog(message);
}

static void N3DSLoadingScreen_draw(N3DSLoadingScreen* screen) {
    if (screen == NULL) return;

    float progress = 0.0f;
    if (screen->totalChunks > 0) {
        progress = (float) screen->chunkIndex / (float) screen->totalChunks;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
    }

    if (screen->useCitro2D && screen->target != NULL && screen->textBuf != NULL) {
        const float barX = 36.0f;
        const float barY = 146.0f;
        const float barW = 328.0f;
        const float barH = 18.0f;
        const float fillW = (barW - 4.0f) * progress;

        C2D_Text title;
        C2D_Text status;
        C2D_Text detail;
        char detailLine[64];
        snprintf(detailLine, sizeof(detailLine), "%d / %d chunks", screen->chunkIndex, screen->totalChunks);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(screen->target, C2D_Color32(8, 10, 14, 255));
        C2D_SceneBegin(screen->target);

        C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 400.0f, 240.0f, C2D_Color32(8, 10, 14, 255));
        C2D_DrawRectSolid(barX, barY, 0.5f, barW, barH, C2D_Color32(42, 48, 60, 255));
        C2D_DrawRectSolid(barX + 2.0f, barY + 2.0f, 0.5f, fillW, barH - 4.0f, C2D_Color32(110, 224, 160, 255));

        C2D_TextBufClear(screen->textBuf);
        C2D_TextParse(&title, screen->textBuf, "Loading Game Data");
        C2D_TextParse(&status, screen->textBuf, screen->statusLine);
        C2D_TextParse(&detail, screen->textBuf, detailLine);
        C2D_TextOptimize(&title);
        C2D_TextOptimize(&status);
        C2D_TextOptimize(&detail);

        C2D_DrawText(&title, C2D_WithColor, 36.0f, 74.0f, 0.5f, 0.60f, 0.60f, C2D_Color32(255, 255, 255, 255));
        C2D_DrawText(&status, C2D_WithColor, 36.0f, 104.0f, 0.5f, N3DS_LOADING_TEXT_SCALE, N3DS_LOADING_TEXT_SCALE, C2D_Color32(210, 214, 224, 255));
        C2D_DrawText(&detail, C2D_WithColor, 36.0f, 172.0f, 0.5f, 0.34f, 0.34f, C2D_Color32(146, 153, 168, 255));

        int firstLogLine = gN3DSBootLogLineCount > 3 ? gN3DSBootLogLineCount - 3 : 0;
        float logY = 188.0f;
        for (int i = firstLogLine; i < gN3DSBootLogLineCount; ++i) {
            C2D_Text logLine;
            C2D_TextParse(&logLine, screen->textBuf, gN3DSBootLogLines[i]);
            C2D_TextOptimize(&logLine);
            C2D_DrawText(&logLine, C2D_WithColor, 36.0f, logY, 0.5f, 0.25f, 0.25f, C2D_Color32(164, 176, 192, 255));
            logY += 12.0f;
        }

        C3D_FrameEnd(0);
        gspWaitForVBlank();
        return;
    }

    int filled = (int) lroundf(progress * 24.0f);
    if (filled < 0) filled = 0;
    if (filled > 24) filled = 24;

    char bar[25];
    repeat(24, i) {
        bar[i] = (int) i < filled ? '#' : '-';
    }
    bar[24] = '\0';

    consoleSelect(&screen->console);
    printf("\x1b[2J");
    printf("\x1b[4;8HLoading Game Data");
    printf("\x1b[7;8H%s", screen->statusLine);
    printf("\x1b[10;8H[%s]", bar);
    printf("\x1b[12;8H%d / %d chunks", screen->chunkIndex, screen->totalChunks);
    printf("\x1b[15;8HPlease wait...");
    int firstLogLine = gN3DSBootLogLineCount > 5 ? gN3DSBootLogLineCount - 5 : 0;
    for (int i = firstLogLine; i < gN3DSBootLogLineCount; ++i) {
        printf("\x1b[%d;4H%s", 17 + (i - firstLogLine), gN3DSBootLogLines[i]);
    }

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static void N3DSLoadingScreen_set(N3DSLoadingScreen* screen, const char* statusLine, int chunkIndex, int totalChunks) {
    if (screen == NULL) return;
    snprintf(screen->statusLine, sizeof(screen->statusLine), "%s", statusLine != NULL ? statusLine : "");
    screen->chunkIndex = chunkIndex;
    screen->totalChunks = totalChunks;
    N3DSLoadingScreen_draw(screen);
}

static void N3DS_waitForStartExitScreen(N3DSLoadingScreen* screen, const char* statusLine) {
    N3DSLoadingScreen_set(screen, statusLine, 1, 1);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}

static s64 N3DS_ticksToNs(u64 ticks) {
    return (s64) ((ticks * 1000000000ULL) / SYSCLOCK_ARM11);
}

static void N3DS_sleepUntilTick(u64 targetTick) {
    const u64 coarseGuardTicks = SYSCLOCK_ARM11 / 2000u; // ~0.5 ms

    while (true) {
        u64 now = svcGetSystemTick();
        if (now >= targetTick) return;

        u64 remaining = targetTick - now;
        if (remaining <= coarseGuardTicks) break;

        svcSleepThread(N3DS_ticksToNs(remaining - coarseGuardTicks));
    }

    while (svcGetSystemTick() < targetTick) {
    }
}

static void N3DS_beginPacedFrame(u64* nextFrameTick, u64 frameTicks) {
    if (nextFrameTick == NULL || frameTicks == 0) return;

    u64 now = svcGetSystemTick();
    if (*nextFrameTick == 0) {
        *nextFrameTick = now;
    } else if (now > *nextFrameTick + frameTicks * 4u) {
        *nextFrameTick = now;
    } else {
        while (now > *nextFrameTick) {
            *nextFrameTick += frameTicks;
        }
    }

    N3DS_sleepUntilTick(*nextFrameTick);
    *nextFrameTick += frameTicks;
}

static void N3DSDataWinProgressCallback(const char* chunkName, int chunkIndex, int totalChunks, MAYBE_UNUSED DataWin* dataWin, void* userData) {
    N3DSLoadingScreen* screen = (N3DSLoadingScreen*) userData;
    if (screen == NULL) return;

    char status[128];
    snprintf(status, sizeof(status), "Parsing chunk %.4s", chunkName != NULL ? chunkName : "----");
    N3DSLoadingScreen_set(screen, status, chunkIndex + 1, totalChunks);
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    gfxInitDefault();
    romfsInit();
    APT_SetAppCpuTimeLimit(30);
    osSetSpeedupEnable(true);

    bool citroReady = false;

    if (C3D_Init(0x80000) && C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
        C2D_Prepare();
        citroReady = true;
    } else {
        C2D_Fini();
        C3D_Fini();
    }

    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/cinnamon", 0777);

    char* dataWinPath = chooseDataWinPath();

    N3DSLoadingScreen loadingScreen = {0};
    if (citroReady) {
        loadingScreen.useCitro2D = true;
        loadingScreen.target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
        loadingScreen.textBuf = C2D_TextBufNew(512);
        if (loadingScreen.target == NULL || loadingScreen.textBuf == NULL) {
            N3DSLoadingScreen_free(&loadingScreen);
            loadingScreen.useCitro2D = false;
        }
    } else {
        consoleInit(GFX_TOP, &loadingScreen.console);
    }

    N3DSLoadingScreen_set(&loadingScreen, "Scanning data.win", 0, 1);

    DataWin* dataWin = DataWin_parse(
        dataWinPath,
        (DataWinParserOptions) {
            .parseGen8 = true,
            .parseOptn = true,
            .parseLang = true,
            .parseExtn = false,
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
            .parseTxtr = false,
            .parseAudo = false,
            .skipLoadingPreciseMasksForNonPreciseSprites = true,
            .progressCallback = N3DSDataWinProgressCallback,
            .progressCallbackUserData = &loadingScreen,
        }
    );

    free(dataWinPath);

    if (dataWin == NULL) {
        N3DS_waitForStartExitScreen(&loadingScreen, "Failed to load data.win. Press START.");
        N3DSLoadingScreen_free(&loadingScreen);
        return 1;
    }

    FileSystem* fileSystem = (FileSystem*) N3DSFileSystem_create("romfs:/", "sdmc:/3ds/cinnamon/");
    AudioSystem* audioSystem = (AudioSystem*) N3DSAudioSystem_create();
    N3DSLoadingScreen_set(&loadingScreen, "Loading sound bank", 1, 2);
    audioSystem->vtable->init(audioSystem, dataWin, fileSystem);

    N3DSLoadingScreen_set(&loadingScreen, "Initializing renderer", 2, 2);
    VMContext* vm = VM_create(dataWin);
    Renderer* renderer = N3DSRenderer_create();
    Runner* runner = Runner_create(dataWin, vm, renderer, fileSystem, audioSystem);

    if (!N3DSRenderer_isReady(renderer)) {
        const char* error = N3DSRenderer_getStartupError(renderer);
        N3DS_waitForStartExitScreen(&loadingScreen, error != NULL ? error : "Renderer init failed. Press START.");
        audioSystem->vtable->destroy(audioSystem);
        renderer->vtable->destroy(renderer);
        Runner_free(runner);
        N3DSFileSystem_destroy((N3DSFileSystem*) fileSystem);
        VM_free(vm);
        DataWin_free(dataWin);
        N3DSLoadingScreen_free(&loadingScreen);
        if (citroReady) {
            C2D_Fini();
            C3D_Fini();
        }
        romfsExit();
        gfxExit();
        return 1;
    }

    N3DSLoadingScreen_free(&loadingScreen);
    N3DSDebugMonitor debugMonitor;
    N3DSDebugMonitor_init(&debugMonitor);

    runner->osType = OS_3DS;
    Runner_initFirstRoom(runner);

    bool haveRoomBorder = false;
    C2D_Sprite roomBorder;
    memset(&roomBorder, 0, sizeof(roomBorder));
    C2D_SpriteSheet roomBorderSheet = NULL;
    char loadedRoomBorderAsset[32] = "";
    bool debugMonitorVisible = false;
    N3DS_refreshRoomBorderSprite(runner->currentRoom, &roomBorder, &roomBorderSheet, &haveRoomBorder, loadedRoomBorderAsset, sizeof(loadedRoomBorderAsset));

    bool leftHeld = false, rightHeld = false, upHeld = false, downHeld = false;
    bool aHeld = false, bHeld = false, yHeld = false;
    bool lHeld = false, rHeld = false;

    Gen8* gen8 = &dataWin->gen8;
    int32_t gameW = (int32_t) gen8->defaultWindowWidth;
    int32_t gameH = (int32_t) gen8->defaultWindowHeight;

    const u64 frameTicks = (SYSCLOCK_ARM11 + 15u) / 30u;
    u64 nextFrameTick = svcGetSystemTick();
    while (aptMainLoop() && !runner->shouldExit) {
        N3DS_beginPacedFrame(&nextFrameTick, frameTicks);
        hidScanInput();
        u32 held = hidKeysHeld();
        u32 down = hidKeysDown();

        if (down & KEY_START) break;
        if (down & KEY_SELECT) debugMonitorVisible = !debugMonitorVisible;
        bool debugWarpToAsriel = debugMonitorVisible && (down & KEY_L) &&
            (uint32_t) N3DS_DEBUG_ASRIEL_ROOM < runner->dataWin->room.count;
        if (debugWarpToAsriel) {
            runner->pendingRoom = N3DS_DEBUG_ASRIEL_ROOM;
        }

        circlePosition circle;
        hidCircleRead(&circle);

        syncKey(runner->keyboard, &leftHeld, VK_LEFT, (held & KEY_LEFT) || circle.dx < -80);
        syncKey(runner->keyboard, &rightHeld, VK_RIGHT, (held & KEY_RIGHT) || circle.dx > 80);
        syncKey(runner->keyboard, &upHeld, VK_UP, (held & KEY_UP) || circle.dy > 80);
        syncKey(runner->keyboard, &downHeld, VK_DOWN, (held & KEY_DOWN) || circle.dy < -80);

        syncKey(runner->keyboard, &aHeld, 'Z', (held & KEY_A));
        syncKey(runner->keyboard, &bHeld, 'X', (held & KEY_B));
        syncKey(runner->keyboard, &yHeld, 'C', (held & KEY_Y) || (held & KEY_X));
        syncKey(runner->keyboard, &lHeld, VK_PAGEDOWN, (held & KEY_L) && !debugWarpToAsriel);
        syncKey(runner->keyboard, &rHeld, VK_PAGEUP, (held & KEY_R));

        Runner_step(runner);
        N3DS_tryTriggerAsrielLed(runner);
        runner->audioSystem->vtable->update(runner->audioSystem, 1.0f / 30.0f);
        N3DS_refreshRoomBorderSprite(runner->currentRoom, &roomBorder, &roomBorderSheet, &haveRoomBorder, loadedRoomBorderAsset, sizeof(loadedRoomBorderAsset));

        float displayScaleX = 1.0f;
        float displayScaleY = 1.0f;
        Runner_computeViewDisplayScale(runner, gameW, gameH, &displayScaleX, &displayScaleY);
        u64 renderStartMs = osGetTime();
        C3D_FrameBegin(0);
        renderer->vtable->beginFrame(renderer, gameW, gameH, 400, 240);
        Runner_drawViews(runner, gameW, gameH, displayScaleX, displayScaleY, false);
        N3DSRenderer_beginOverlay(renderer);
        if (haveRoomBorder) C2D_DrawSprite(&roomBorder);
        else N3DS_drawFallbackRoomBorder();
        if (debugMonitorVisible) {
            N3DSDebugMonitor_draw(&debugMonitor, runner, renderer);
        }
        renderer->vtable->endFrame(renderer);
        C3D_FrameEnd(0);
        N3DSDebugMonitor_tickFrame(&debugMonitor, (double) (osGetTime() - renderStartMs));

        RunnerKeyboard_beginFrame(runner->keyboard);
    }

    audioSystem->vtable->destroy(audioSystem);
    renderer->vtable->destroy(renderer);
    Runner_free(runner);
    N3DSFileSystem_destroy((N3DSFileSystem*) fileSystem);
    VM_free(vm);
    DataWin_free(dataWin);
    N3DSDebugMonitor_free(&debugMonitor);

    if (roomBorderSheet != NULL) {
        C2D_SpriteSheetFree(roomBorderSheet);
    }

    if (citroReady) {
        C2D_Fini();
        C3D_Fini();
    }

    romfsExit();
    gfxExit();
    return 0;
}
