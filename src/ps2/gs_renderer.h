#pragma once

#include "renderer.h"
#include <gsKit.h>

// ===[ GsRenderer Struct ]===
// Simple PS2 renderer using gsKit ONE SHOT mode.
// Renders all sprites/text as colored rectangles (no textures yet).
typedef struct {
    Renderer base; // Must be first field for struct embedding

    GSGLOBAL* gsGlobal;

    // View transform state (set each view in beginView)
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
    int32_t viewX;
    int32_t viewY;

    // Z counter for depth ordering (gsKit uses Z for draw order)
    uint16_t zCounter;
} GsRenderer;

Renderer* GsRenderer_create(GSGLOBAL* gsGlobal);
