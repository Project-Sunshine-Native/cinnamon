#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include "renderer.h"

// ===[ Tiled Texture Cache ]===
//
// The PICA200 GPU hard-caps textures at 1024x1024 and requires POT dimensions.
// Each atlas page is split into a grid of <=1024 tiles.
//
// Memory strategy — three levels, fully lazy:
//   1. registerPage()  : reads PNG IHDR only (cheap), stores blob pointer.
//                        No heap alloc beyond the tile metadata array.
//   2. uploadTileNow() : called on first draw of a tile.
//                        Decodes PNG → RGBA8 (if not already in ram),
//                        swizzles + uploads one tile to GPU,
//                        frees pixels once all tiles of the page are loaded.
//   3. CDestroy()      : frees any remaining pixels + all GPU tex handles.
//
// This means at peak only ONE page's decoded pixels + ONE tile's linearAlloc
// buffer are in RAM simultaneously during upload.

#define RENDERER_MAX_TEX_DIM 1024u

typedef struct {
    C3D_Tex  tex;
    uint32_t regionX, regionY;  // top-left in atlas pixel space
    uint32_t regionW, regionH;  // atlas pixels this tile covers
    uint32_t texW,    texH;     // POT GPU dimensions (set at upload time)
    bool     loaded;
} TexCacheTile;

typedef struct {
    TexCacheTile* tiles;        // [tilesX * tilesY], row-major
    uint32_t      tilesX;
    uint32_t      tilesY;
    uint32_t      atlasW;       // from PNG IHDR
    uint32_t      atlasH;

    // Raw PNG blob — pointer into DataWin-owned memory, never freed here.
    const uint8_t* blobData;
    size_t         blobSize;

    // Decoded RGBA8 pixels — allocated by lodepng, freed after all tiles
    // of this page are uploaded (or in CDestroy if upload never completed).
    uint8_t* pixels;
} TexCachePage;

// ===[ CRenderer3DS ]===

typedef struct {
    Renderer base;              // must remain first member

    C3D_RenderTarget* top;

    int32_t viewX, viewY;
    float   scaleX, scaleY;
    float   offsetX, offsetY;
    float   zCounter;

    TexCachePage* pageCache;
    uint32_t      pageCacheCount;
} CRenderer3DS;

Renderer* CRenderer3DS_create(void);