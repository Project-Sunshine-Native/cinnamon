#pragma once

#include <stdbool.h>

#include <gx2/context.h>
#include <gx2/sampler.h>
#include <gx2/texture.h>
#include <gx2r/buffer.h>
#include <whb/gfx.h>

#include "../renderer.h"

typedef struct {
    bool ready;
    GX2Texture texture;
} WiiUTexturePage;

typedef struct {
    float x;
    float y;
    float z;
    float w;
    float u;
    float v;
    float r;
    float g;
    float b;
    float a;
} WiiUBatchVertex;

typedef struct {
    float x;
    float y;
} WiiUVec2;

typedef struct {
    int32_t texturePageId;
    bool gradient;
    WiiUVec2 p00;
    WiiUVec2 p10;
    WiiUVec2 p01;
    float u0;
    float v0;
    float u1;
    float v1;
    uint32_t color0;
    uint32_t color1;
    float alpha;
} WiiUQuadCommand;

typedef struct {
    Renderer base;

    WHBGfxShaderGroup shaderGroup;
    GX2Sampler sampler;
    uint32_t textureUnit;
    bool shaderReady;

    WiiUTexturePage* texturePages;
    uint32_t texturePageCount;
    WiiUTexturePage whiteTexture;

    GX2RBuffer batchVertexBuffer;
    WiiUBatchVertex* batchVertices;
    uint32_t batchVertexCapacity;
    uint32_t batchVertexCount;
    WiiUQuadCommand* commands;
    uint32_t commandCount;
    uint32_t commandCapacity;
    uint32_t queuedQuadCount;

    int32_t frameWidth;
    int32_t frameHeight;

    // 640x480 offscreen render target.  The game draws into this buffer every
    // frame, and it is then blitted to the TV and DRC scan buffers using the
    // POINT sampler for nearest-neighbour integer upscaling.
    GX2ColorBuffer offscreenColorBuffer;
    GX2Texture     offscreenTexture;
    bool           offscreenReady;

    // TV scan buffer dimensions, detected once at init from GX2GetSystemTVScanMode().
    uint32_t tvWidth;
    uint32_t tvHeight;

    int32_t viewX;
    int32_t viewY;
    int32_t viewW;
    int32_t viewH;
    int32_t portX;
    int32_t portY;
    int32_t portW;
    int32_t portH;
    float viewScaleX;
    float viewScaleY;

    uint8_t clearR;
    uint8_t clearG;
    uint8_t clearB;

    uint32_t perfFrameCount;
    double perfRenderTvMs;
    double perfRenderDrcMs;
    uint32_t perfFlushCount;
    uint32_t perfQuadCount;
    uint32_t debugFrameIndex;
} WiiURenderer;

Renderer* WiiURenderer_create(void);
void WiiURenderer_setClearColor(WiiURenderer* renderer, uint32_t color);
void WiiURenderer_runStartupSmokeTest(uint32_t frameCount);