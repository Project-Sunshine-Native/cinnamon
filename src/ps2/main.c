#include <kernel.h>
#include <sifrpc.h>
#include <stdio.h>
#include <malloc.h>
#include <dmaKit.h>
#include <gsKit.h>
#include <gsToolkit.h>

#include "runner.h"
#include "vm.h"
#include "../data_win.h"
#include "gs_renderer.h"
#include "utils.h"

int main(int argc, char* argv[]) {
    SifInitRpc(0);

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
            .parseAudo = false
        }
    );

    {
        struct mallinfo mi = mallinfo();
        printf("Memory after data.win parsing: used=%d bytes (%.1f KB), free=%d bytes (%.1f KB)\n", mi.uordblks, mi.uordblks / 1024.0f, mi.fordblks, mi.fordblks / 1024.0f);
    }
    // ===[ Create renderer and runner ]===
    Renderer* renderer = GsRenderer_create(gsGlobal);

    VMContext* vm = VM_create(dataWin);
    Runner* runner = Runner_create(dataWin, vm);

    {
        struct mallinfo mi = mallinfo();
        printf("Memory after VM and runner creation: used=%d bytes (%.1f KB), free=%d bytes (%.1f KB)\n", mi.uordblks, mi.uordblks / 1024.0f, mi.fordblks, mi.fordblks / 1024.0f);
    }

    runner->renderer = renderer;

    renderer->vtable->init(renderer, dataWin);

    Runner_initFirstRoom(runner);

    Gen8* gen8 = &dataWin->gen8;
    int32_t gameW = (int32_t) gen8->defaultWindowWidth;
    int32_t gameH = (int32_t) gen8->defaultWindowHeight;

    // ===[ Main Loop ]===
    while (!runner->shouldExit) {
        struct mallinfo mi = mallinfo();
        printf("Memory: used=%d bytes (%.1f KB), free=%d bytes (%.1f KB)\n", mi.uordblks, mi.uordblks / 1024.0f, mi.fordblks, mi.fordblks / 1024.0f);

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

        // Execute draw queue and flip
        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);
    }

    renderer->vtable->destroy(renderer);
    DataWin_free(dataWin);

    return 0;
}
