#pragma once

#include "renderer.h"
#include <citro2d.h>
#include <3ds.h>

// ===[ CRenderer3DS Struct ]===
// Simple 3DS renderer using Citro2D.
// Renders everything as flat colored primitives (no textures).
typedef struct {
    Renderer base; // must be first

    // Citro2D render target (top screen)
    C3D_RenderTarget* top;

    // View transform state
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
    int32_t viewX;
    int32_t viewY;

    // Z depth (Citro2D uses float depth)
    float zCounter;
} CRenderer3DS;

// constructor
Renderer* CRenderer3DS_create(void);