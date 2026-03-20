#include "n3ds_renderer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <3ds.h>
#include <citro2d.h>

#include "utils.h"
#include "text_utils.h"

// ===[ Color Helpers ]===

static u32 colorForTpagIndex(int32_t tpagIndex, float alpha) {
    uint32_t hash = (uint32_t) tpagIndex * 2654435761u;

    uint8_t r = (hash >> 0) & 0xFF;
    uint8_t g = (hash >> 8) & 0xFF;
    uint8_t b = (hash >> 16) & 0xFF;

    if (128 > r + g + b) {
        r |= 0x80;
        g |= 0x40;
    }

    uint8_t a = (uint8_t)(alpha * 255.0f);
    return C2D_Color32(r, g, b, a);
}

// ===[ Vtable ]===

static void CInit(Renderer* renderer, DataWin* dataWin) {
    CRenderer3DS* C = (CRenderer3DS*) renderer;

    renderer->dataWin = dataWin;
    renderer->drawColor = 0xFFFFFF;
    renderer->drawAlpha = 1.0f;
    renderer->drawFont = -1;
    renderer->drawHalign = 0;
    renderer->drawValign = 0;

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    C->top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);

    printf("CRenderer3DS: initialized (flat mode)\n");
}

static void CDestroy(Renderer* renderer) {
    CRenderer3DS* C = (CRenderer3DS*) renderer;

    C2D_Fini();
    C3D_Fini();
    gfxExit();

    free(C);
}

static void CBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    CRenderer3DS* C = (CRenderer3DS*) renderer;

    C->zCounter = 0.5f;

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(C->top, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(C->top);
}

static void CEndFrame(Renderer* renderer) {
    C3D_FrameEnd(0);
}

static void CBeginView(Renderer* renderer,
    int32_t viewX, int32_t viewY,
    int32_t viewW, int32_t viewH,
    int32_t portX, int32_t portY,
    int32_t portW, int32_t portH,
    float viewAngle)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;

    C->viewX = viewX;
    C->viewY = viewY;

    if (viewW > 0 && viewH > 0) {
        C->scaleX = 400.0f / (float) viewW;
        C->scaleY = C->scaleX;
    } else {
        C->scaleX = 2.0f;
        C->scaleY = 2.0f;
    }

    float renderedH = (float) viewH * C->scaleY;
    C->offsetX = 0.0f;
    C->offsetY = (240.0f - renderedH) / 2.0f;
}

static void CEndView(Renderer* renderer) {
    // no-op
}

// ===[ Draw ]===

static void CDrawSprite(Renderer* renderer, int32_t tpagIndex,
    float x, float y, float originX, float originY,
    float xscale, float yscale,
    float angleDeg, uint32_t color, float alpha)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t) tpagIndex >= dw->tpag.count) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    float w = (float) tpag->boundingWidth;
    float h = (float) tpag->boundingHeight;

    float gameX1 = x - originX * xscale;
    float gameY1 = y - originY * yscale;
    float gameX2 = x + (w - originX) * xscale;
    float gameY2 = y + (h - originY) * yscale;

    gameX1 -= C->viewX;
    gameY1 -= C->viewY;
    gameX2 -= C->viewX;
    gameY2 -= C->viewY;

    float sx = gameX1 * C->scaleX + C->offsetX;
    float sy = gameY1 * C->scaleY + C->offsetY;
    float sw = (gameX2 - gameX1) * C->scaleX;
    float sh = (gameY2 - gameY1) * C->scaleY;

    u32 col = colorForTpagIndex(tpagIndex, alpha);

    C2D_DrawRectSolid(sx, sy, C->zCounter, sw, sh, col);
    C->zCounter += 0.0001f;
}

static void CDrawSpritePart(Renderer* renderer, int32_t tpagIndex,
    int32_t srcOffX, int32_t srcOffY,
    int32_t srcW, int32_t srcH,
    float x, float y, float xscale, float yscale,
    uint32_t color, float alpha)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;

    if (0 > tpagIndex || (uint32_t) tpagIndex >= renderer->dataWin->tpag.count) return;

    float gameX1 = x - C->viewX;
    float gameY1 = y - C->viewY;

    float sx = gameX1 * C->scaleX + C->offsetX;
    float sy = gameY1 * C->scaleY + C->offsetY;
    float sw = (float) srcW * xscale * C->scaleX;
    float sh = (float) srcH * yscale * C->scaleY;

    u32 col = colorForTpagIndex(tpagIndex, alpha);

    C2D_DrawRectSolid(sx, sy, C->zCounter, sw, sh, col);
    C->zCounter += 0.0001f;
}

static void CDrawRectangle(Renderer* renderer,
    float x1, float y1, float x2, float y2,
    uint32_t color, float alpha, bool outline)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;

    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = (uint8_t)(alpha * 255.0f);

    float sx = (x1 - C->viewX) * C->scaleX + C->offsetX;
    float sy = (y1 - C->viewY) * C->scaleY + C->offsetY;
    float sw = (x2 - x1) * C->scaleX;
    float sh = (y2 - y1) * C->scaleY;

    C2D_DrawRectSolid(sx, sy, C->zCounter, sw, sh, C2D_Color32(r, g, b, a));
    C->zCounter += 0.0001f;
}

static void CDrawLine(Renderer* renderer,
    float x1, float y1, float x2, float y2,
    float width, uint32_t color, float alpha)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;

    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = (uint8_t)(alpha * 255.0f);

    float sx1 = (x1 - C->viewX) * C->scaleX + C->offsetX;
    float sy1 = (y1 - C->viewY) * C->scaleY + C->offsetY;
    float sx2 = (x2 - C->viewX) * C->scaleX + C->offsetX;
    float sy2 = (y2 - C->viewY) * C->scaleY + C->offsetY;

    C2D_DrawLine(sx1, sy1, C2D_Color32(r, g, b, a),
                 sx2, sy2, C2D_Color32(r, g, b, a),
                 width, C->zCounter);
    C->zCounter += 0.0001f;
}

static void CDrawText(Renderer* renderer,
    const char* text, float x, float y,
    float xscale, float yscale, float angleDeg)
{
    // stub for now (matches your "flat" goal)
    (void)renderer;
    (void)text;
    (void)x;
    (void)y;
}

static void CFlush(Renderer* renderer) {
    // no-op
}

static int32_t CCreateSpriteFromSurface(Renderer* renderer,
    int32_t x, int32_t y, int32_t w, int32_t h,
    bool removeback, bool smooth,
    int32_t xorig, int32_t yorig)
{
    fprintf(stderr, "CRenderer3DS: createSpriteFromSurface not supported\n");
    return -1;
}

static void CDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    // no-op
}

// ===[ Vtable ]===

static RendererVtable CVtable = {
    .init = CInit,
    .destroy = CDestroy,
    .beginFrame = CBeginFrame,
    .endFrame = CEndFrame,
    .beginView = CBeginView,
    .endView = CEndView,
    .drawSprite = CDrawSprite,
    .drawSpritePart = CDrawSpritePart,
    .drawRectangle = CDrawRectangle,
    .drawLine = CDrawLine,
    .drawText = CDrawText,
    .flush = CFlush,
    .createSpriteFromSurface = CCreateSpriteFromSurface,
    .deleteSprite = CDeleteSprite,
};

Renderer* CRenderer3DS_create(void) {
    CRenderer3DS* C = safeCalloc(1, sizeof(CRenderer3DS));

    C->base.vtable = &CVtable;
    C->scaleX = 2.0f;
    C->scaleY = 2.0f;
    C->zCounter = 0.5f;

    return (Renderer*) C;
}