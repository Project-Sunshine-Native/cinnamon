#include "debug_overlay.h"

#include "collision.h"
#include "real_type.h"
#include "utils.h"

#include "stb_ds.h"

// Colors are in BGR format (0xBBGGRR)
#define MASK_COLOR_AABB_ONLY  0x0000FF // red: bbox only (no precise mask)
#define MASK_COLOR_AABB       0x00FF00 // green: bbox of an instance with a precise mask
#define MASK_COLOR_PIXEL      0xFFFF00 // cyan: a set pixel inside the precise mask

void DebugOverlay_drawCollisionMasks(Runner* runner) {
    Renderer* renderer = runner->renderer;
    if (renderer == nullptr) return;

    DataWin* dataWin = runner->dataWin;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);

    repeat(instanceCount, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active || !inst->visible) continue;

        Sprite* spr = Collision_getSprite(dataWin, inst);
        if (spr == nullptr) continue;

        bool hasPreciseMask = Collision_hasFrameMasks(spr);

        // Draw the precise mask pixels (only when not rotated, to keep this simple)
        if (hasPreciseMask && 0.0001 > GMLReal_fabs(inst->imageAngle)) {
            uint32_t frameIdx = ((uint32_t) inst->imageIndex) % spr->maskCount;
            uint8_t* mask = spr->masks[frameIdx];
            uint32_t bytesPerRow = (spr->width + 7) / 8;

            GMLReal originX = (GMLReal) spr->originX;
            GMLReal originY = (GMLReal) spr->originY;

            // Iterate the bbox-relevant pixels rather than the full sprite area
            int32_t startX = spr->marginLeft;
            int32_t endX = spr->marginRight;
            int32_t startY = spr->marginTop;
            int32_t endY = spr->marginBottom;

            if (0 > startX) startX = 0;
            if (0 > startY) startY = 0;
            if (startX >= (int32_t) spr->width) startX = (int32_t) spr->width - 1;
            if (startY >= (int32_t) spr->height) startY = (int32_t) spr->height - 1;
            if (endX >= (int32_t) spr->width) endX = (int32_t) spr->width - 1;
            if (endY >= (int32_t) spr->height) endY = (int32_t) spr->height - 1;

            for (int32_t py = startY; endY >= py; py++) {
                for (int32_t px = startX; endX >= px; px++) {
                    bool set = (mask[py * bytesPerRow + (px >> 3)] & (1 << (7 - (px & 7)))) != 0;
                    if (!set) continue;

                    // Convert mask pixel to world space (origin + scale, no rotation)
                    GMLReal wx1 = inst->x + inst->imageXscale * ((GMLReal) px - originX);
                    GMLReal wy1 = inst->y + inst->imageYscale * ((GMLReal) py - originY);
                    GMLReal wx2 = wx1 + inst->imageXscale;
                    GMLReal wy2 = wy1 + inst->imageYscale;

                    // Normalize on negative scale
                    if (wx1 > wx2) { GMLReal tmp = wx1; wx1 = wx2; wx2 = tmp; }
                    if (wy1 > wy2) { GMLReal tmp = wy1; wy1 = wy2; wy2 = tmp; }

                    // The fill path adds +1 to (x2, y2), so subtract one pixel here
                    renderer->vtable->drawRectangle(renderer, (float) wx1, (float) wy1, (float) wx2 - 1.0f, (float) wy2 - 1.0f, MASK_COLOR_PIXEL, 0.4f, false);
                }
            }
        }

        // Draw the AABB outline (works for both AABB-only and precise instances)
        InstanceBBox bbox = Collision_computeBBox(dataWin, inst);
        if (!bbox.valid) continue;

        uint32_t outlineColor = hasPreciseMask ? MASK_COLOR_AABB : MASK_COLOR_AABB_ONLY;

        // bbox.right / bbox.bottom are exclusive, but the outline path adds +1 to x2/y2 when
        // drawing the top/bottom edges, so we pass -1 to keep the outline aligned to the bbox
        renderer->vtable->drawRectangle(renderer, (float) bbox.left, (float) bbox.top, (float) bbox.right - 1.0f, (float) bbox.bottom - 1.0f, outlineColor, 1.0f, true);
    }

    // Flush so the overlay quads land on top of whatever Runner_draw queued
    if (renderer->vtable->flush != nullptr) {
        renderer->vtable->flush(renderer);
    }
}
