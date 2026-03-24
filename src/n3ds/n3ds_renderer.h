#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include "renderer.h"

// ===[ Region Texture Cache ]===
//
// Instead of splitting each page into a fixed grid of <=1024 tiles and
// uploading entire slabs, we cache only the exact source rectangles that are
// actually drawn.  A 48x48 sprite from a 1024x2048 page costs 64x64x4 = 16 KB
// of linear RAM instead of a 4 MB slab, so many more regions coexist at once.
//
// Memory strategy — fully lazy, three levels:
//   1. registerPage()      : reads PNG IHDR only (~33 bytes).  No decode, no GPU work.
//   2. drawRegion()        : on a cache miss, decodes the page PNG once (stored in
//                            page->pixels for the rest of the frame), swizzles + uploads
//                            only the needed rectangle, caches it in a RegionCacheEntry.
//                            Subsequent hits on the same region skip the decode entirely.
//   3. CEndFrame()         : frees all page->pixels.  Per-region GPU textures persist
//                            across frames and are evicted lazily (LRU) when the slot
//                            array is full.
//   4. CDestroy()          : deletes all resident GPU textures and frees page memory.

#define RENDERER_MAX_TEX_DIM 1024u

// Maximum number of cached source rectangles per page.
// Each loaded slot holds one C3D_Tex (GPU linear RAM = texW * texH * 4 bytes).
// 256 slots * worst-case 1024x1024x4 = 1 GB theoretical max, but in practice
// slots are small sprite frames so the total stays well within linear RAM.
#define REGION_CACHE_MAX 256u

typedef struct {
    uint16_t srcX, srcY;   // top-left of the source rect in atlas pixel space (cache key)
    uint16_t srcW, srcH;   // dimensions of the source rect (cache key)
    uint32_t texW, texH;   // actual POT GPU texture dimensions (>= srcW/srcH)
    C3D_Tex  tex;
    bool     loaded;       // true once the GPU texture is ready to draw
    bool     loadFailed;   // true if C3D_TexInit or swizzle alloc failed; never retry
    // NOTE: no 'pinned' field — whether an entry is pinned is determined by which
    // array it lives in: pinnedRegions[] = permanent font glyphs, regions[] = LRU sprites
    uint32_t lastUsed;     // frameCounter when last drawn (LRU; unused for pinned entries)
} RegionCacheEntry;

typedef struct {
    // Raw PNG blob — pointer into DataWin-owned memory, never freed here.
    const uint8_t* blobData;
    size_t         blobSize;

    uint32_t atlasW;        // from PNG IHDR
    uint32_t atlasH;

    // Decoded RGBA8 pixels — allocated by lodepng on first cache miss each frame,
    // freed in CEndFrame.  NULL between frames.
    uint8_t* pixels;

    bool     loadFailed;    // permanent decode failure (OOM etc.); never retry
    uint32_t lastSuccessfullDecodedFrame;
    uint32_t lastUsedFrame; // frameCounter of the last frame that drew from this page

    uint32_t decodeTimeout;
    uint32_t lastDecodeFrame;

    // Font glyph cache — allocated once at init, never evicted, separate from sprites.
    // Searched first in regionLookup so font draws never touch the sprite LRU pool.
    RegionCacheEntry* pinnedRegions;
    uint32_t          pinnedRegionCount;
    uint32_t          pinnedRegionCapacity; // current allocation size

    // Sprite region cache — LRU evictable, completely separate from font glyphs.
    // All 256 slots are always available for sprites regardless of how many fonts
    // share this page.
    RegionCacheEntry regions[REGION_CACHE_MAX];
    uint32_t         regionCount;
} TexCachePage;

// ===[ CRenderer3DS ]===

typedef struct {
    Renderer base;          // must remain first member

    C3D_RenderTarget* top;

    int32_t viewX, viewY;
    float   scaleX, scaleY;
    float   offsetX, offsetY;
    float   zCounter;
    uint32_t frameCounter;  // incremented at end of each frame; used for LRU timestamps

    // Stored from beginFrame so beginView can compose the two-level transform:
    // game-space port  ->  screen-space rect  ->  world-to-screen.
    int32_t gameW, gameH;     // game resolution passed to beginFrame
    int32_t screenW, screenH; // physical screen dimensions (e.g. 400x240 top screen)

    TexCachePage* pageCache;
    uint32_t      pageCacheCount;
} CRenderer3DS;

Renderer* CRenderer3DS_create(void);