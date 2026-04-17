#pragma once

#include "data_win.h"
#include "instance.h"
#include "vm.h"

#include <math.h>

// Checks if an instance matches a collision target.
// target >= 100000: instance ID (match specific instance)
// target == INSTANCE_ALL (-3): match any instance
// target >= 0 && < 100000: object index (match via parent chain)
static inline bool Collision_matchesTarget(DataWin* dataWin, Instance* inst, int32_t target) {
    if (target >= 100000) return inst->instanceId == target;
    if (target == INSTANCE_ALL) return true;
    return VM_isObjectOrDescendant(dataWin, inst->objectIndex, target);
}

typedef struct {
    double left, right, top, bottom;
    bool valid;
} InstanceBBox;

// Returns the collision sprite for an instance (mask sprite if set, else display sprite)
static inline Sprite* Collision_getSprite(DataWin* dataWin, Instance* inst) {
    int32_t sprIdx = (inst->maskIndex >= 0) ? inst->maskIndex : inst->spriteIndex;
    if (0 > sprIdx || (uint32_t) sprIdx >= dataWin->sprt.count) return NULL;
    return &dataWin->sprt.sprites[sprIdx];
}

// Computes the axis-aligned bounding box for an instance using its collision sprite
static inline InstanceBBox Collision_computeBBox(DataWin* dataWin, Instance* inst) {
    Sprite* spr = Collision_getSprite(dataWin, inst);
    if (spr == NULL) return (InstanceBBox){0, 0, 0, 0, false};

    double marginL = (double) spr->marginLeft;
    double marginR = (double) (spr->marginRight + 1);
    double marginT = (double) spr->marginTop;
    double marginB = (double) (spr->marginBottom + 1);
    double originX = (double) spr->originX;
    double originY = (double) spr->originY;

    if (fabs(inst->imageAngle) > 0.0001) {
        // Compute rotated AABB: transform the 4 corners of the unrotated bbox
        double rad = inst->imageAngle * M_PI / 180.0;
        double cs = cos(rad);
        double sn = sin(rad);

        // Local-space corners relative to origin, scaled
        double lx0 = inst->imageXscale * (marginL - originX);
        double ly0 = inst->imageYscale * (marginT - originY);
        double lx1 = inst->imageXscale * (marginR - originX);
        double ly1 = inst->imageYscale * (marginB - originY);

        // Rotate all 4 corners (CW rotation matching renderer's negated angle for Y-down screen coords)
        double cx[4], cy[4];
        cx[0] = cs * lx0 + sn * ly0;  cy[0] = -sn * lx0 + cs * ly0;
        cx[1] = cs * lx1 + sn * ly0;  cy[1] = -sn * lx1 + cs * ly0;
        cx[2] = cs * lx0 + sn * ly1;  cy[2] = -sn * lx0 + cs * ly1;
        cx[3] = cs * lx1 + sn * ly1;  cy[3] = -sn * lx1 + cs * ly1;

        double minX = cx[0], maxX = cx[0], minY = cy[0], maxY = cy[0];
        for (int c = 1; 4 > c; c++) {
            if (minX > cx[c]) minX = cx[c];
            if (cx[c] > maxX) maxX = cx[c];
            if (minY > cy[c]) minY = cy[c];
            if (cy[c] > maxY) maxY = cy[c];
        }

        return (InstanceBBox){
            .left   = inst->x + minX,
            .right  = inst->x + maxX,
            .top    = inst->y + minY,
            .bottom = inst->y + maxY,
            .valid  = true
        };
    }

    // No rotation fast path
    double left   = inst->x + inst->imageXscale * (marginL - originX);
    double right  = inst->x + inst->imageXscale * (marginR - originX);
    double top    = inst->y + inst->imageYscale * (marginT - originY);
    double bottom = inst->y + inst->imageYscale * (marginB - originY);

    // Normalize if negative scale
    if (left > right) { double tmp = left; left = right; right = tmp; }
    if (top > bottom) { double tmp = top; top = bottom; bottom = tmp; }

    return (InstanceBBox){left, right, top, bottom, true};
}

// Tests if point (px, py) hits the instance's precise collision mask
static inline bool Collision_pointInMask(Sprite* spr, Instance* inst, double px, double py) {
    if (spr->masks == NULL || spr->maskCount == 0) return true; // no mask = all solid

    // Pick mask for current frame
    uint32_t frameIdx = ((uint32_t) inst->imageIndex) % spr->maskCount;
    uint8_t* mask = spr->masks[frameIdx];

    // Transform world coords to sprite-local coords
    double dx = px - inst->x;
    double dy = py - inst->y;

    // Inverse of CW rotation is standard CCW rotation (positive angle)
    if (fabs(inst->imageAngle) > 0.0001) {
        double rad = inst->imageAngle * M_PI / 180.0;
        double cs = cos(rad);
        double sn = sin(rad);
        double rx = cs * dx - sn * dy;
        double ry = sn * dx + cs * dy;
        dx = rx;
        dy = ry;
    }

    // Inverse scale + add origin
    double localX = dx / inst->imageXscale + (double) spr->originX;
    double localY = dy / inst->imageYscale + (double) spr->originY;

    int32_t ix = (int32_t) localX;
    int32_t iy = (int32_t) localY;

    // Bounds check
    if (0 > ix || 0 > iy || ix >= (int32_t) spr->width || iy >= (int32_t) spr->height) return false;

    uint32_t bytesPerRow = (spr->width + 7) / 8;
    return (mask[iy * bytesPerRow + (ix >> 3)] & (1 << (7 - (ix & 7)))) != 0;
}

// Tests if two instances' precise masks overlap
static inline bool Collision_instancesOverlapPrecise(DataWin* dataWin, Instance* a, Instance* b, InstanceBBox bboxA, InstanceBBox bboxB) {
    // Compute world-space intersection of the two AABBs
    double iLeft   = fmax(bboxA.left, bboxB.left);
    double iRight  = fmin(bboxA.right, bboxB.right);
    double iTop    = fmax(bboxA.top, bboxB.top);
    double iBottom = fmin(bboxA.bottom, bboxB.bottom);

    if (iLeft >= iRight || iTop >= iBottom) return false;

    Sprite* sprA = Collision_getSprite(dataWin, a);
    Sprite* sprB = Collision_getSprite(dataWin, b);

    bool preciseA = (sprA != NULL && sprA->sepMasks == 1 && sprA->masks != NULL && sprA->maskCount > 0);
    bool preciseB = (sprB != NULL && sprB->sepMasks == 1 && sprB->masks != NULL && sprB->maskCount > 0);

    // If neither needs precise, bbox overlap is sufficient
    if (!preciseA && !preciseB) return true;

    int32_t startX = (int32_t) floor(iLeft);
    int32_t endX   = (int32_t) ceil(iRight);
    int32_t startY = (int32_t) floor(iTop);
    int32_t endY   = (int32_t) ceil(iBottom);

    for (int32_t py = startY; endY > py; py++) {
        for (int32_t px = startX; endX > px; px++) {
            double wpx = (double) px + 0.5;
            double wpy = (double) py + 0.5;

            bool hitA = true;
            if (preciseA) {
                hitA = Collision_pointInMask(sprA, a, wpx, wpy);
            }

            bool hitB = true;
            if (preciseB) {
                hitB = Collision_pointInMask(sprB, b, wpx, wpy);
            }

            if (hitA && hitB) return true;
        }
    }

    return false;
}