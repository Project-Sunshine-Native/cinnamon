#pragma once

#include <stdint.h>
#include <math.h>

#include "data_win.h"
#include "instance.h"

// ===[ Renderer Vtable ]===

typedef struct Renderer Renderer;

typedef struct {
    void (*init)(Renderer* renderer, DataWin* dataWin);
    void (*destroy)(Renderer* renderer);
    void (*beginFrame)(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH);
    void (*endFrame)(Renderer* renderer);
    void (*beginView)(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle);
    void (*endView)(Renderer* renderer);
    void (*drawSprite)(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha);
    void (*drawSpritePart)(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha);
    void (*drawRectangle)(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline);
    void (*drawLine)(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha);
    void (*drawLineColor)(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha);
    void (*drawText)(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg);
    void (*flush)(Renderer* renderer);
    int32_t (*createSpriteFromSurface)(Renderer* renderer, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig);
    void (*deleteSprite)(Renderer* renderer, int32_t spriteIndex);
    // Optional: platform-specific tile rendering (nullptr = use default drawSpritePart path)
    void (*drawTile)(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY);
} RendererVtable;

// ===[ Renderer Base Struct ]===

struct Renderer {
    RendererVtable* vtable;
    DataWin* dataWin;
    uint32_t drawColor;  // BGR format, default 0xFFFFFF (white)
    float drawAlpha;     // default 1.0
    int32_t drawFont;    // default -1 (no font)
    int32_t drawHalign;  // 0=left, 1=center, 2=right
    int32_t drawValign;  // 0=top, 1=middle, 2=bottom
};

// ===[ Shared Helpers (platform-agnostic) ]===

// Resolves a sprite + subimage to a TPAG index, with frame wrapping
static int32_t Renderer_resolveTPAGIndex(DataWin* dataWin, int32_t spriteIndex, int32_t subimg) {
    if (0 > spriteIndex || dataWin->sprt.count <= (uint32_t) spriteIndex) return -1;

    Sprite* sprite = &dataWin->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return -1;

    // Wrap subimage index
    int32_t frameIndex = subimg % (int32_t) sprite->textureCount;
    if (0 > frameIndex) frameIndex += (int32_t) sprite->textureCount;

    uint32_t tpagOffset = sprite->textureOffsets[frameIndex];
    return DataWin_resolveTPAG(dataWin, tpagOffset);
}

// Convenience: draw_sprite(sprite, subimg, x, y)
static void Renderer_drawSprite(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    renderer->vtable->drawSprite(renderer, tpagIndex, x, y, (float) sprite->originX, (float) sprite->originY, 1.0f, 1.0f, 0.0f, 0xFFFFFF, renderer->drawAlpha);
}

// Stretched: draw_sprite_stretched(sprite, subimg, x, y, w, h)
static void Renderer_drawSpriteStretched(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y, float w, float h, uint32_t color, float alpha) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    float xscale = w / (float) tpag->boundingWidth;
    float yscale = h / (float) tpag->boundingHeight;
    renderer->vtable->drawSprite(renderer, tpagIndex, x, y, 0.0f, 0.0f, xscale, yscale, 0.0f, color, alpha);
}

// Full version: draw_sprite_ext(sprite, subimg, x, y, xscale, yscale, rot, color, alpha)
static void Renderer_drawSpriteExt(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y, float xscale, float yscale, float rot, uint32_t color, float alpha) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    renderer->vtable->drawSprite(renderer, tpagIndex, x, y, (float) sprite->originX, (float) sprite->originY, xscale, yscale, rot, color, alpha);
}

// Partial draw: draw_sprite_part(sprite, subimg, left, top, width, height, x, y)
static void Renderer_drawSpritePart(Renderer* renderer, int32_t spriteIndex, int32_t subimg, int32_t left, int32_t top, int32_t width, int32_t height, float x, float y) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    // Clip region to TPAG bounds (matching HTML5 Graphics_DrawPart logic)
    // left/top are in original sprite space; targetX/targetY is where cropped data starts
    if (left < tpag->targetX) {
        int32_t off = tpag->targetX - left;
        x += (float) off;
        width -= off;
        left = 0;
    } else {
        left -= tpag->targetX;
    }

    if (top < tpag->targetY) {
        int32_t off = tpag->targetY - top;
        y += (float) off;
        height -= off;
        top = 0;
    } else {
        top -= tpag->targetY;
    }

    if (width > tpag->sourceWidth - left) width = tpag->sourceWidth - left;
    if (height > tpag->sourceHeight - top) height = tpag->sourceHeight - top;
    if (0 >= width || 0 >= height) return;

    renderer->vtable->drawSpritePart(renderer, tpagIndex, left, top, width, height, x, y, 1.0f, 1.0f, 0xFFFFFF, renderer->drawAlpha);
}

// Draws part of a sprite with extended parameters (scale, color, alpha)
static void Renderer_drawSpritePartExt(Renderer* renderer, int32_t spriteIndex, int32_t subimg, int32_t left, int32_t top, int32_t width, int32_t height, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    // Clip region to TPAG bounds (same as Renderer_drawSpritePart)
    if (left < tpag->targetX) {
        int32_t off = tpag->targetX - left;
        x += (float) off * xscale;
        width -= off;
        left = 0;
    } else {
        left -= tpag->targetX;
    }

    if (top < tpag->targetY) {
        int32_t off = tpag->targetY - top;
        y += (float) off * yscale;
        height -= off;
        top = 0;
    } else {
        top -= tpag->targetY;
    }

    if (width > tpag->sourceWidth - left) width = tpag->sourceWidth - left;
    if (height > tpag->sourceHeight - top) height = tpag->sourceHeight - top;
    if (0 >= width || 0 >= height) return;

    renderer->vtable->drawSpritePart(renderer, tpagIndex, left, top, width, height, x, y, xscale, yscale, color, alpha);
}

// Resolves a BGND index to a TPAG index via Background.textureOffset -> DataWin_resolveTPAG()
static int32_t Renderer_resolveBackgroundTPAGIndex(DataWin* dataWin, int32_t bgndIndex) {
    if (0 > bgndIndex || (uint32_t) bgndIndex >= dataWin->bgnd.count) return -1;
    Background* bg = &dataWin->bgnd.backgrounds[bgndIndex];
    return DataWin_resolveTPAG(dataWin, bg->textureOffset);
}

// Draws a tiled background
static void Renderer_drawBackgroundTiled(Renderer* renderer, int32_t tpagIndex, float bgX, float bgY, bool tileX, bool tileY, float roomW, float roomH, float alpha) {
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || (uint32_t) tpagIndex >= dw->tpag.count) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    float bgW = (float) tpag->boundingWidth;
    float bgH = (float) tpag->boundingHeight;
    if (0 >= bgW || 0 >= bgH) return;

    // Compute start/end for each axis
    float startX, endX, startY, endY;

    if (tileX) {
        startX = fmodf(bgX, bgW);
        if (startX > 0) startX -= bgW;
        endX = roomW;
    } else {
        startX = bgX;
        endX = bgX + bgW;
    }

    if (tileY) {
        startY = fmodf(bgY, bgH);
        if (startY > 0) startY -= bgH;
        endY = roomH;
    } else {
        startY = bgY;
        endY = bgY + bgH;
    }

    for (float dy = startY; endY > dy; dy += bgH) {
        for (float dx = startX; endX > dx; dx += bgW) {
            renderer->vtable->drawSprite(renderer, tpagIndex, dx, dy, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFF, alpha);
            if (!tileX) break;
        }
        if (!tileY) break;
    }
}

// Default draw: draws instance's sprite using its image_* properties
static void Renderer_drawSelf(Renderer* renderer, Instance* instance) {
    if (0 > instance->spriteIndex) return;

    int32_t subimg = (int32_t) instance->imageIndex;
    Renderer_drawSpriteExt(
        renderer,
        instance->spriteIndex,
        subimg,
        (float) instance->x,
        (float) instance->y,
        (float) instance->imageXscale,
        (float) instance->imageYscale,
        (float) instance->imageAngle,
        instance->imageBlend,
        (float) instance->imageAlpha
    );
}

// Draws a room tile with layer shift offset applied
static void Renderer_drawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY) {
    // If the platform has a dedicated tile renderer, use it (PS2 has separate tile atlas entries)
    if (renderer->vtable->drawTile != nullptr) {
        renderer->vtable->drawTile(renderer, tile, offsetX, offsetY);
        return;
    }

    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(renderer->dataWin, tile->backgroundDefinition);
    if (0 > tpagIndex) return;

    TexturePageItem* tpag = &renderer->dataWin->tpag.items[tpagIndex];

    // The tile's sourceX/Y are in the background image's coordinate space (bounding rect).
    // The TPAG may have been trimmed: actual content starts at (targetX, targetY) within the
    // bounding rect and has size sourceWidth x sourceHeight. We must clamp the tile's source
    // rect to the TPAG's content area to avoid sampling adjacent atlas textures.
    int32_t srcX = tile->sourceX;
    int32_t srcY = tile->sourceY;
    int32_t srcW = (int32_t) tile->width;
    int32_t srcH = (int32_t) tile->height;
    float drawX = (float) tile->x + offsetX;
    float drawY = (float) tile->y + offsetY;

    // Clip left/top: if tile starts before the content region
    int32_t contentLeft = tpag->targetX;
    int32_t contentTop = tpag->targetY;
    if (contentLeft > srcX) {
        int32_t clip = contentLeft - srcX;
        drawX += (float) clip * tile->scaleX;
        srcW -= clip;
        srcX = contentLeft;
    }
    if (contentTop > srcY) {
        int32_t clip = contentTop - srcY;
        drawY += (float) clip * tile->scaleY;
        srcH -= clip;
        srcY = contentTop;
    }

    // Clip right/bottom: if tile extends past the content region
    int32_t contentRight = tpag->targetX + tpag->sourceWidth;
    int32_t contentBottom = tpag->targetY + tpag->sourceHeight;
    if (srcX + srcW > contentRight) {
        srcW = contentRight - srcX;
    }
    if (srcY + srcH > contentBottom) {
        srcH = contentBottom - srcY;
    }

    if (0 >= srcW || 0 >= srcH) return;

    // Convert from bounding-rect coords to atlas-relative coords (subtract targetX/Y)
    int32_t atlasOffX = srcX - tpag->targetX;
    int32_t atlasOffY = srcY - tpag->targetY;

    // Extract alpha from high byte, default to 1.0 if alpha byte is 0
    uint8_t alphaByte = (tile->color >> 24) & 0xFF;
    float alpha = (alphaByte == 0) ? 1.0f : (float) alphaByte / 255.0f;
    uint32_t bgr = tile->color & 0x00FFFFFF;

    renderer->vtable->drawSpritePart(renderer, tpagIndex, atlasOffX, atlasOffY, srcW, srcH, drawX, drawY, tile->scaleX, tile->scaleY, bgr, alpha);
}
