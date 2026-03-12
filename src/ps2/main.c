#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <stdio.h>
#include <malloc.h>
#include <dmaKit.h>
#include <gsKit.h>
#include <gsToolkit.h>
#include <gsFontM.h>
#include <libpad.h>
#include <timer.h>

#include "runner.h"
#include "runner_keyboard.h"
#include "vm.h"
#include "../data_win.h"
#include "gs_renderer.h"
#include "utils.h"

// The maximum memory of a normal PS2 console
// Developer consoles may have more memory, but because ps2sdk does not have a way to know
// how much memory the console really has, we will use this value instead
static int MAX_MEMORY_BYTES = 33554432;

// 256-byte aligned buffer for libpad
static char padBuf[256] __attribute__((aligned(64)));

// Controller button to GML key mapping
typedef struct {
    uint16_t padButton;
    int32_t gmlKey;
} PadMapping;

static const PadMapping PAD_MAPPINGS[] = {
    { PAD_UP,       VK_UP },
    { PAD_DOWN,     VK_DOWN },
    { PAD_LEFT,     VK_LEFT },
    { PAD_RIGHT,    VK_RIGHT },
    { PAD_CROSS,    'Z' },
    { PAD_CIRCLE,   'X' },
    { PAD_START,    'C' },
    { PAD_TRIANGLE, VK_ESCAPE },
    { PAD_L1, VK_PAGEDOWN },
    { PAD_R1, VK_PAGEUP },
};

static const int PAD_MAPPING_COUNT = sizeof(PAD_MAPPINGS) / sizeof(PAD_MAPPINGS[0]);

// Previous frame's button state for detecting press/release edges
static uint16_t prevButtons = 0xFFFF; // All buttons released (buttons are active-low)

int main(int argc, char* argv[]) {
    SifInitRpc(0);

    // ===[ Initialize Controller ]===
    int ret;
    ret = SifLoadModule("rom0:SIO2MAN", 0, nullptr);
    if (0 > ret) {
        printf("Failed to load SIO2MAN: %d\n", ret);
        return 1;
    }
    ret = SifLoadModule("rom0:PADMAN", 0, nullptr);
    if (0 > ret) {
        printf("Failed to load PADMAN: %d\n", ret);
        return 1;
    }

    padInit(0);
    padPortOpen(0, 0, padBuf);

    // Wait for pad to be ready
    int padState;
    do {
        padState = padGetState(0, 0);
    } while (PAD_STATE_STABLE != padState && PAD_STATE_FINDCTP1 != padState);

    printf("Controller initialized\n");

    const char* dataWinPath = "host:data.win";
    if (argc > 1) {
        dataWinPath = argv[1];
    }

    printf("Butterscotch PS2 - Loading %s\n", dataWinPath);

    // ===[ Initialize gsKit ]===
    GSGLOBAL* gsGlobal = gsKit_init_global();
    gsGlobal->Mode = GS_MODE_NTSC;
    gsGlobal->Interlace = GS_INTERLACED;
    gsGlobal->Field = GS_FIELD;
    gsGlobal->Width = 640;
    gsGlobal->Height = 448;
    gsGlobal->PSM = GS_PSM_CT16;
    gsGlobal->PSMZ = GS_PSMZ_16;
    gsGlobal->DoubleBuffering = GS_SETTING_ON;
    gsGlobal->ZBuffering = GS_SETTING_OFF;

    gsGlobal->PrimAAEnable = GS_SETTING_OFF;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsKit_init_screen(gsGlobal);
    // Use ONE SHOT mode
    gsKit_mode_switch(gsGlobal, GS_ONESHOT);

    // ===[ Initialize FONTM (ROM font) for debug overlay ]===
    GSFONTM* gsFontM = gsKit_init_fontm();
    gsKit_fontm_upload(gsGlobal, gsFontM);
    gsFontM->Spacing = 0.95f;

    // ===[ Parse data.win ]===
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
            .parseTxtr = false,
            .parseAudo = false,
            .skipLoadingPreciseMasksForNonPreciseSprites = true
        }
    );

    {
        struct mallinfo mi = mallinfo();
        printf("Memory after data.win parsing: used=%d bytes (%.1f KB), total=%d bytes (%.1f KB), free=%d bytes (%.1f KB)\n", mi.uordblks, mi.uordblks / 1024.0f, MAX_MEMORY_BYTES, MAX_MEMORY_BYTES / 1024.0f, MAX_MEMORY_BYTES - mi.uordblks, (MAX_MEMORY_BYTES - mi.uordblks) / 1024.0f);
    }
    // ===[ Create renderer and runner ]===
    Renderer* renderer = GsRenderer_create(gsGlobal);

    VMContext* vm = VM_create(dataWin);
    Runner* runner = Runner_create(dataWin, vm);

    {
        struct mallinfo mi = mallinfo();
        printf("Memory after VM and runner creation: used=%d bytes (%.1f KB), total=%d bytes (%.1f KB), free=%d bytes (%.1f KB)\n", mi.uordblks, mi.uordblks / 1024.0f, MAX_MEMORY_BYTES, MAX_MEMORY_BYTES / 1024.0f, MAX_MEMORY_BYTES - mi.uordblks, (MAX_MEMORY_BYTES - mi.uordblks) / 1024.0f);
    }

    runner->renderer = renderer;

    renderer->vtable->init(renderer, dataWin);

    Runner_initFirstRoom(runner);

    Gen8* gen8 = &dataWin->gen8;
    int32_t gameW = (int32_t) gen8->defaultWindowWidth;
    int32_t gameH = (int32_t) gen8->defaultWindowHeight;

    // ===[ Main Loop ]===
    float lastFrameTimeMs = 0.0f;

    while (!runner->shouldExit) {
        u32 frameStartTicks = cpu_ticks();

        struct mallinfo mi = mallinfo();
        printf("Memory: used=%d bytes (%.1f KB), total=%d bytes (%.1f KB), free=%d bytes (%.1f KB)\n", mi.uordblks, mi.uordblks / 1024.0f, MAX_MEMORY_BYTES, MAX_MEMORY_BYTES / 1024.0f, MAX_MEMORY_BYTES - mi.uordblks, (MAX_MEMORY_BYTES - mi.uordblks) / 1024.0f);

        // ===[ Poll Controller ]===
        RunnerKeyboard_beginFrame(runner->keyboard);

        struct padButtonStatus padStatus;
        unsigned char padResult = padRead(0, 0, &padStatus);
        if (padResult != 0) {
            uint16_t buttons = padStatus.btns;

            for (int i = 0; PAD_MAPPING_COUNT > i; i++) {
                uint16_t mask = PAD_MAPPINGS[i].padButton;
                int32_t gmlKey = PAD_MAPPINGS[i].gmlKey;

                // PS2 buttons are active-low: 0 = pressed, 1 = released
                bool wasPressed = (prevButtons & mask) == 0;
                bool isPressed = (buttons & mask) == 0;

                if (isPressed && !wasPressed) {
                    RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
                } else if (!isPressed && wasPressed) {
                    RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
                }
            }

            prevButtons = buttons;
        }

        // Go to next room
        if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEUP)) {
            DataWin* dw = runner->dataWin;
            if ((int32_t) dw->gen8.roomOrderCount > runner->currentRoomOrderPosition + 1) {
                int32_t nextIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition + 1];
                runner->pendingRoom = nextIdx;
                fprintf(stderr, "Debug: Going to next room -> %s\n", dw->room.rooms[nextIdx].name);
            }
        }

        // Go to previous room
        if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEDOWN)) {
            DataWin* dw = runner->dataWin;
            if (runner->currentRoomOrderPosition > 0) {
                int32_t prevIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition - 1];
                runner->pendingRoom = prevIdx;
                fprintf(stderr, "Debug: Going to previous room -> %s\n", dw->room.rooms[prevIdx].name);
            }
        }

        // Run one game step
        Runner_step(runner);

        // ===[ Render ]===
        gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));

        renderer->vtable->beginFrame(renderer, gameW, gameH, 640, 448);

        // Clear with room background color
        if (runner->drawBackgroundColor) {
            uint8_t bgR = BGR_R(runner->backgroundColor);
            uint8_t bgG = BGR_G(runner->backgroundColor);
            uint8_t bgB = BGR_B(runner->backgroundColor);
            u64 bgColor = GS_SETREG_RGBAQ(bgR, bgG, bgB, 0x80, 0x00);
            gsKit_prim_sprite(gsGlobal, 0, 0, 640, 448, 0, bgColor);
        }

        // Render views
        Room* activeRoom = runner->currentRoom;
        bool anyViewRendered = false;

        bool viewsEnabled = (activeRoom->flags & 1) != 0;

        if (viewsEnabled) {
            repeat(8, vi) {
                if (!activeRoom->views[vi].enabled) continue;

                int32_t viewX = activeRoom->views[vi].viewX;
                int32_t viewY = activeRoom->views[vi].viewY;
                int32_t viewW = activeRoom->views[vi].viewWidth;
                int32_t viewH = activeRoom->views[vi].viewHeight;
                int32_t portX = activeRoom->views[vi].portX;
                int32_t portY = activeRoom->views[vi].portY;
                int32_t portW = activeRoom->views[vi].portWidth;
                int32_t portH = activeRoom->views[vi].portHeight;
                float viewAngle = runner->viewAngles[vi];

                runner->viewCurrent = (int32_t) vi;
                renderer->vtable->beginView(renderer, viewX, viewY, viewW, viewH, portX, portY, portW, portH, viewAngle);

                Runner_draw(runner);

                renderer->vtable->endView(renderer);
                anyViewRendered = true;
            }
        }

        if (!anyViewRendered) {
            // No views enabled: render with default full-screen view
            runner->viewCurrent = 0;
            renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, 0, 0, gameW, gameH, 0.0f);
            Runner_draw(runner);
            renderer->vtable->endView(renderer);
        }

        runner->viewCurrent = 0;

        renderer->vtable->endFrame(renderer);

        // Measure frame time BEFORE flipping
        u32 frameEndTicks = cpu_ticks();
        u32 elapsedTicks = frameEndTicks - frameStartTicks;
        // cpu_ticks() runs at kBUSCLK (147.456 MHz), so ticks / (kBUSCLK / 1000) = milliseconds
        lastFrameTimeMs = (float) elapsedTicks / (float) (kBUSCLK / 1000);

        // ===[ Debug Overlay ]===
        {
            u64 debugColor = GS_SETREG_RGBAQ(0xFF, 0xFF, 0xFF, 0x80, 0x00);
            int32_t freeBytes = MAX_MEMORY_BYTES - mi.uordblks;

            char debugText[128];
            snprintf(debugText, sizeof(debugText), "Tick: %.2fms\nFree: %d bytes", lastFrameTimeMs, freeBytes);
            gsKit_fontm_print_scaled(gsGlobal, gsFontM, 10.0f, 10.0f, 10, 0.6f, debugColor, debugText);
        }

        // Execute draw queue and flip
        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);
    }

    renderer->vtable->destroy(renderer);
    DataWin_free(dataWin);

    return 0;
}
