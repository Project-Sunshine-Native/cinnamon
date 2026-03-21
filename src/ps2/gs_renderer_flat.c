#include "gs_renderer_flat.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "utils.h"
#include "text_utils.h"
#include "ps2_utils.h"

// ===[ Color Generation ]===
// Generates a unique color for each tpagIndex so sprites are visually distinguishable.
// Uses a simple hash to spread colors across the RGB space.
static u64 colorForTpagIndex(int32_t tpagIndex, float alpha) {
    // Golden ratio hash for good color distribution
    uint32_t hash = (uint32_t) tpagIndex * 2654435761u;
    uint8_t r = (uint8_t) ((hash >> 0) & 0xFF);
    uint8_t g = (uint8_t) ((hash >> 8) & 0xFF);
    uint8_t b = (uint8_t) ((hash >> 16) & 0xFF);

    // Ensure colors are never too dark (min brightness ~100)
    if (128 > r + g + b) {
        r = (uint8_t) (r | 0x80);
        g = (uint8_t) (g | 0x40);
    }

    uint8_t a = alphaToGS(alpha);
    return GS_SETREG_RGBAQ(r, g, b, a, 0x00);
}

// ===[ Vtable Implementations ]===

static void gsInit(Renderer* renderer, DataWin* dataWin) {
    GsRendererFlat* gs = (GsRendererFlat*) renderer;

    renderer->dataWin = dataWin;
    renderer->drawColor = 0xFFFFFF;
    renderer->drawAlpha = 1.0f;
    renderer->drawFont = -1;
    renderer->drawHalign = 0;
    renderer->drawValign = 0;

    // Enable alpha blending on all primitives (sets ABE bit in GS PRIM register)
    gs->gsGlobal->PrimAlphaEnable = GS_SETTING_ON;

    // Set alpha blend equation: (Cs - Cd) * As / 128 + Cd (standard src-over blend)
    gsKit_set_primalpha(gs->gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);

    printf("GsRendererFlat: initialized (colored quads mode, no textures)\n");
    printf("GsRendererFlat: %u sprites, %u TPAG items\n", dataWin->sprt.count, dataWin->tpag.count);
}

static void gsDestroy(Renderer* renderer) {
    GsRendererFlat* gs = (GsRendererFlat*) renderer;
    free(gs);
}

static void gsBeginFrame(Renderer* renderer, [[maybe_unused]] int32_t gameW, [[maybe_unused]] int32_t gameH, [[maybe_unused]] int32_t windowW, [[maybe_unused]] int32_t windowH) {
    GsRendererFlat* gs = (GsRendererFlat*) renderer;
    gs->zCounter = 1;
}

static void gsEndFrame([[maybe_unused]] Renderer* renderer) {
    // No-op: flip happens in main loop
}

static void gsBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, [[maybe_unused]] int32_t portX, [[maybe_unused]] int32_t portY, int32_t portW, int32_t portH, [[maybe_unused]] float viewAngle) {
    GsRendererFlat* gs = (GsRendererFlat*) renderer;
    gs->viewX = viewX;
    gs->viewY = viewY;

    // Scale game view to PS2 screen (640x448 NTSC interlaced)
    // Use uniform scale based on width (640/viewW) so pixels stay square.
    if (viewW > 0 && viewH > 0) {
        gs->scaleX = 640.0f / (float) viewW;
        gs->scaleY = gs->scaleX;
    } else {
        gs->scaleX = 2.0f;
        gs->scaleY = 2.0f;
    }

    // Center vertically: offset so the rendered image is centered on the 448px screen
    float renderedH = (float) viewH * gs->scaleY;
    gs->offsetX = 0.0f;
    gs->offsetY = (448.0f - renderedH) / 2.0f;
}

static void gsEndView([[maybe_unused]] Renderer* renderer) {
    // No-op
}

static void gsDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, [[maybe_unused]] float angleDeg, [[maybe_unused]] uint32_t color, float alpha) {
    GsRendererFlat* gs = (GsRendererFlat*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t) tpagIndex >= dw->tpag.count) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    float w = (float) tpag->boundingWidth;
    float h = (float) tpag->boundingHeight;

    // Compute screen rect in game coordinates
    float gameX1 = x - originX * xscale;
    float gameY1 = y - originY * yscale;
    float gameX2 = x + (w - originX) * xscale;
    float gameY2 = y + (h - originY) * yscale;

    // Apply view offset
    gameX1 -= (float) gs->viewX;
    gameY1 -= (float) gs->viewY;
    gameX2 -= (float) gs->viewX;
    gameY2 -= (float) gs->viewY;

    // Scale to screen coordinates
    float sx1 = gameX1 * gs->scaleX + gs->offsetX;
    float sy1 = gameY1 * gs->scaleY + gs->offsetY;
    float sx2 = gameX2 * gs->scaleX + gs->offsetX;
    float sy2 = gameY2 * gs->scaleY + gs->offsetY;

    u64 quadColor = colorForTpagIndex(tpagIndex, alpha);
    gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, quadColor);
    gs->zCounter++;
}

static void gsDrawSpritePart(Renderer* renderer, int32_t tpagIndex, [[maybe_unused]] int32_t srcOffX, [[maybe_unused]] int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, [[maybe_unused]] uint32_t color, float alpha) {
    GsRendererFlat* gs = (GsRendererFlat*) renderer;

    if (0 > tpagIndex || (uint32_t) tpagIndex >= renderer->dataWin->tpag.count) return;

    // Compute screen rect
    float gameX1 = x - (float) gs->viewX;
    float gameY1 = y - (float) gs->viewY;
    float gameX2 = gameX1 + (float) srcW * xscale;
    float gameY2 = gameY1 + (float) srcH * yscale;

    float sx1 = gameX1 * gs->scaleX + gs->offsetX;
    float sy1 = gameY1 * gs->scaleY + gs->offsetY;
    float sx2 = gameX2 * gs->scaleX + gs->offsetX;
    float sy2 = gameY2 * gs->scaleY + gs->offsetY;

    u64 quadColor = colorForTpagIndex(tpagIndex, alpha);
    gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, quadColor);
    gs->zCounter++;
}

static void gsDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, [[maybe_unused]] bool outline) {
    GsRendererFlat* gs = (GsRendererFlat*) renderer;

    // BGR to RGB
    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = alphaToGS(alpha);

    float sx1 = (x1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (y1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx2 = (x2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy2 = (y2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    u64 rectColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);
    gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, rectColor);
    gs->zCounter++;
}

static void gsDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, [[maybe_unused]] float width, uint32_t color, float alpha) {
    GsRendererFlat* gs = (GsRendererFlat*) renderer;

    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = alphaToGS(alpha);

    float sx1 = (x1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (y1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx2 = (x2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy2 = (y2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    u64 lineColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);
    gsKit_prim_line(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, lineColor);
    gs->zCounter++;
}

// PS2 gsKit doesn't support per-vertex colors on lines, so we just use color1
static void gsDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, [[maybe_unused]] uint32_t color2, float alpha) {
    renderer->vtable->drawLine(renderer, x1, y1, x2, y2, width, color1, alpha);
}

static void gsDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, [[maybe_unused]] float angleDeg) {
    GsRendererFlat* gs = (GsRendererFlat*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > renderer->drawFont || (uint32_t) renderer->drawFont >= dw->font.count) return;

    Font* font = &dw->font.fonts[renderer->drawFont];

    // BGR to RGB for text color
    uint32_t color = renderer->drawColor;
    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = alphaToGS(renderer->drawAlpha);
    u64 textColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    // Preprocess GML text (# -> \n, \# -> #)
    char* processed = TextUtils_preprocessGmlText(text);
    int32_t textLen = (int32_t) strlen(processed);

    // Compute vertical alignment offset
    int32_t lineCount = TextUtils_countLines(processed, textLen);
    float totalHeight = (float) lineCount * (float) font->emSize;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float cursorY = valignOffset;
    int32_t lineStart = 0;

    while (textLen >= lineStart) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) {
            lineEnd++;
        }

        int32_t lineLen = lineEnd - lineStart;
        const char* line = processed + lineStart;

        // Measure line width for horizontal alignment
        float lineWidth = TextUtils_measureLineWidth(font, line, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;

        // Draw each glyph as a colored rectangle
        int32_t pos = 0;
        while (lineLen > pos) {
            uint16_t ch = TextUtils_decodeUtf8(line, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == nullptr) continue;

            if (glyph->sourceWidth > 0 && glyph->sourceHeight > 0) {
                float glyphX = x + (cursorX + (float) glyph->offset) * xscale * font->scaleX;
                float glyphY = y + cursorY * yscale * font->scaleY;
                float glyphW = (float) glyph->sourceWidth * xscale * font->scaleX;
                float glyphH = (float) glyph->sourceHeight * yscale * font->scaleY;

                // Apply view offset and scale to screen coordinates
                float sx1 = (glyphX - (float) gs->viewX) * gs->scaleX + gs->offsetX;
                float sy1 = (glyphY - (float) gs->viewY) * gs->scaleY + gs->offsetY;
                float sx2 = (glyphX + glyphW - (float) gs->viewX) * gs->scaleX + gs->offsetX;
                float sy2 = (glyphY + glyphH - (float) gs->viewY) * gs->scaleY + gs->offsetY;

                gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, textColor);
            }

            cursorX += (float) glyph->shift;

            // Apply kerning
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(line, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        gs->zCounter++;

        // Advance to next line
        cursorY += (float) font->emSize;
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        } else {
            break;
        }
    }

    free(processed);
}

static void gsFlush([[maybe_unused]] Renderer* renderer) {
    // No-op: gsKit queues commands, executed in main loop via gsKit_queue_exec
}

static int32_t gsCreateSpriteFromSurface([[maybe_unused]] Renderer* renderer, [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y, [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h, [[maybe_unused]] bool removeback, [[maybe_unused]] bool smooth, [[maybe_unused]] int32_t xorig, [[maybe_unused]] int32_t yorig) {
    fprintf(stderr, "GsRendererFlat: createSpriteFromSurface not supported on PS2\n");
    return -1;
}

static void gsDeleteSprite([[maybe_unused]] Renderer* renderer, [[maybe_unused]] int32_t spriteIndex) {
    // No-op
}

// ===[ Constructor ]===

static RendererVtable gsVtable = {
    .init = gsInit,
    .destroy = gsDestroy,
    .beginFrame = gsBeginFrame,
    .endFrame = gsEndFrame,
    .beginView = gsBeginView,
    .endView = gsEndView,
    .drawSprite = gsDrawSprite,
    .drawSpritePart = gsDrawSpritePart,
    .drawRectangle = gsDrawRectangle,
    .drawLine = gsDrawLine,
    .drawLineColor = gsDrawLineColor,
    .drawText = gsDrawText,
    .flush = gsFlush,
    .createSpriteFromSurface = gsCreateSpriteFromSurface,
    .deleteSprite = gsDeleteSprite,
};

Renderer* GsRendererFlat_create(GSGLOBAL* gsGlobal) {
    GsRendererFlat* gs = safeCalloc(1, sizeof(GsRendererFlat));
    gs->base.vtable = &gsVtable;
    gs->gsGlobal = gsGlobal;
    gs->scaleX = 2.0f;
    gs->scaleY = 2.0f;
    gs->zCounter = 1;
    return (Renderer*) gs;
}
