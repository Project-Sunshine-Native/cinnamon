#include "n3ds_renderer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include "utils.h"
#include "text_utils.h"

// Write errors to both stderr (on-screen console) and stdout (file log).
#define LOG_ERR(...) do { fprintf(stderr, __VA_ARGS__); printf(__VA_ARGS__); } while(0)

// ===[ Linear-backed lodepng allocator ]===
//
// lodepng calls malloc/realloc/free internally.  On 3DS the app heap is only
// ~4 MB, which is not enough to decode a 1024x2048 atlas (8 MB raw RGBA8).
// The linear heap has ~30 MB free, so we redirect lodepng there.
//
// linearAlloc has no native realloc, so we store the allocation size in an
// 8-byte header immediately before the data pointer lodepng receives.
//
// IMPORTANT: define LODEPNG_NO_COMPILE_ALLOCATORS before including lodepng.h so
// the header sees our declarations instead of the default malloc/free ones.
// Add   CFLAGS += -DLODEPNG_NO_COMPILE_ALLOCATORS   to your Makefile, or define
// it here before the include (whichever your build system supports).

typedef struct { size_t size; uint32_t _pad; } LodePNGAllocHeader;

void* lodepng_malloc(size_t size) {
    size_t total = size + sizeof(LodePNGAllocHeader);
    LodePNGAllocHeader* hdr = (LodePNGAllocHeader*) linearAlloc(total);
    if (!hdr) return NULL;
    hdr->size = size;
    return hdr + 1;
}

void lodepng_free(void* ptr);

void* lodepng_realloc(void* ptr, size_t new_size) {
    if (!ptr)      return lodepng_malloc(new_size);
    if (!new_size) { lodepng_free(ptr); return NULL; }
    LodePNGAllocHeader* old_hdr = (LodePNGAllocHeader*)ptr - 1;
    size_t old_size = old_hdr->size;
    void* new_ptr = lodepng_malloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    lodepng_free(ptr);
    return new_ptr;
}

void lodepng_free(void* ptr) {
    if (!ptr) return;
    linearFree((LodePNGAllocHeader*)ptr - 1);
}

#include "lodepng.h"

static void verifyLodepngAllocator(void) {
    u32 before = linearSpaceFree();
    void* p = lodepng_malloc(1024 * 1024);
    u32 after = linearSpaceFree();
    if (p && before != after)
        printf("[lodepng] custom allocator OK (linear -%ld KB)\n", (long)(before - after) / 1024);
    else {
        LOG_ERR("[lodepng] WARNING: custom allocator NOT using linear heap!\n");
        LOG_ERR("[lodepng] Add -DLODEPNG_NO_COMPILE_ALLOCATORS to the build rule for lodepng.c\n");
    }
    if (p) lodepng_free(p);
}

// ===[ Memory logging ]===

static void logMemory(const char* tag) {
    printf("[MEM] %-40s linear: %lu KB\n", tag, (unsigned long)(linearSpaceFree() / 1024));
}

// ===[ POT helpers ]===

static uint32_t nextPow2(uint32_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

static uint32_t gpuTexDim(uint32_t pixels) {
    uint32_t p = nextPow2(pixels);
    return (p > RENDERER_MAX_TEX_DIM) ? RENDERER_MAX_TEX_DIM : p;
}

// ===[ Morton (Z-order) swizzle ]===
//
// Converts a rectangle from a linear RGBA8 source image into the Morton-order
// tiled layout the 3DS GPU expects.  The source image is stored top-row-first
// (standard PNG order); the GPU tile format is Y-flipped, so we read
// flippedLy = (texH-1)-ly from the source when filling each Morton slot.

static void linearToTile(uint8_t*       dst,
                          const uint8_t* src,
                          uint32_t srcX0, uint32_t srcY0,
                          uint32_t copyW, uint32_t copyH,
                          uint32_t fullSrcW,
                          uint32_t texW,  uint32_t texH)
{
    for (uint32_t ty = 0; ty < texH; ty += 8) {
        for (uint32_t tx = 0; tx < texW; tx += 8) {
            for (uint32_t py = 0; py < 8; py++) {
                for (uint32_t px = 0; px < 8; px++) {
                    uint32_t lx = tx + px;
                    uint32_t ly = ty + py;
                    uint32_t flippedLy = (texH - 1) - ly;

                    uint32_t m = 0;
                    for (uint32_t bit = 0; bit < 3; bit++) {
                        m |= ((px >> bit) & 1u) << (bit * 2);
                        m |= ((py >> bit) & 1u) << (bit * 2 + 1);
                    }
                    uint32_t tileIdx = (ty / 8) * (texW / 8) + (tx / 8);
                    uint32_t dstOff  = (tileIdx * 64 + m) * 4;

                    if (lx < copyW && flippedLy < copyH) {
                        uint32_t srcOff = ((srcY0 + flippedLy) * fullSrcW + (srcX0 + lx)) * 4;
                        dst[dstOff + 0] = src[srcOff + 2]; // B
                        dst[dstOff + 1] = src[srcOff + 1]; // G
                        dst[dstOff + 2] = src[srcOff + 0]; // R
                        dst[dstOff + 3] = src[srcOff + 3]; // A
                    }
                    // else: out-of-bounds texels stay zero (transparent)
                }
            }
        }
    }
}

// ===[ Region cache ]===
//
// Instead of uploading entire page slabs (up to 2048x2048 = 16 MB of linear
// RAM), we upload only the exact (srcX, srcY, srcW, srcH) rectangle needed by
// each draw call.  A 64x64 sprite uses 16 KB instead of a 4-16 MB slab, so
// many more regions coexist in linear RAM at once.
//
// Each TexCachePage holds a flat array of REGION_CACHE_MAX RegionCacheEntry
// slots; the LRU entry is evicted (C3D_TexDelete) when the array is full.
//
// page->pixels holds the decoded PNG pixels for the duration of one frame.
// Multiple region misses on the same page share one decode.  CEndFrame frees
// all page->pixels so only the small per-region GPU textures persist.
//
// ===[ Required header changes (n3ds_renderer.h) ]===
//
// Replace the TexCacheTile / tile-grid fields with:
//
//   #define REGION_CACHE_MAX 256
//
//   typedef struct {
//       uint16_t srcX, srcY, srcW, srcH; // cache key (source atlas texels)
//       uint32_t texW, texH;             // actual POT GPU texture dimensions
//       C3D_Tex  tex;
//       bool     loaded;
//       uint32_t lastUsed;               // frameCounter when last drawn (LRU)
//   } RegionCacheEntry;
//
//   typedef struct {
//       const uint8_t*   blobData;       // points into DataWin; never owned
//       size_t           blobSize;
//       uint32_t         atlasW, atlasH;
//       bool             loadFailed;     // permanent decode failure; never retry
//       uint8_t*         pixels;         // non-null during frames with cache misses
//       uint32_t         lastUsedFrame;
//       RegionCacheEntry regions[REGION_CACHE_MAX];
//       uint32_t         regionCount;
//   } TexCachePage;
//
//   typedef struct {
//       Renderer          base;
//       TexCachePage*     pageCache;
//       uint32_t          pageCacheCount;
//       C3D_RenderTarget* top;
//       float  scaleX, scaleY, offsetX, offsetY;
//       int32_t viewX, viewY;
//       float    zCounter;
//       uint32_t frameCounter;
//   } CRenderer3DS;

// ===[ Region cache: lookup ]===

static RegionCacheEntry* regionLookup(TexCachePage* page,
                                       uint16_t srcX, uint16_t srcY,
                                       uint16_t srcW, uint16_t srcH,
                                       uint32_t frameCounter)
{
    for (uint32_t i = 0; i < page->regionCount; i++) {
        RegionCacheEntry* e = &page->regions[i];
        if (e->loaded &&
            e->srcX == srcX && e->srcY == srcY &&
            e->srcW == srcW && e->srcH == srcH)
        {
            e->lastUsed = frameCounter;
            return e;
        }
    }
    return NULL;
}

// ===[ Region cache: alloc / LRU eviction ]===

static RegionCacheEntry* regionAlloc(TexCachePage* page,
                                      uint16_t srcX, uint16_t srcY,
                                      uint16_t srcW, uint16_t srcH,
                                      uint32_t frameCounter)
{
    // Prefer an empty slot
    if (page->regionCount < REGION_CACHE_MAX) {
        RegionCacheEntry* e = &page->regions[page->regionCount++];
        memset(e, 0, sizeof(*e));
        e->srcX = srcX; e->srcY = srcY;
        e->srcW = srcW; e->srcH = srcH;
        e->lastUsed = frameCounter;
        return e;
    }

    // All slots occupied: evict the least-recently-used entry
    uint32_t oldestIdx = 0;
    uint32_t oldest    = page->regions[0].lastUsed;
    for (uint32_t i = 1; i < REGION_CACHE_MAX; i++) {
        if (page->regions[i].lastUsed < oldest) {
            oldest    = page->regions[i].lastUsed;
            oldestIdx = i;
        }
    }

    RegionCacheEntry* e = &page->regions[oldestIdx];
    if (e->loaded) {
        C3D_TexDelete(&e->tex);
    }
    memset(e, 0, sizeof(*e));
    e->srcX = srcX; e->srcY = srcY;
    e->srcW = srcW; e->srcH = srcH;
    e->lastUsed = frameCounter;
    return e;
}

// ===[ Page decode ]===
//
// Decode the page PNG into linear RAM exactly once per frame, storing the
// result in page->pixels.  Subsequent region misses on the same page reuse
// the already-decoded pixels without re-decoding.  CEndFrame frees them.

static bool ensurePageDecoded(TexCachePage* page, uint32_t pageIdx) {
    if (page->pixels)     return true;  // already decoded this frame
    if (page->loadFailed) return false; // permanent failure; never retry

    logMemory("before PNG decode");
    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode32(&page->pixels, &w, &h,
                                    page->blobData, page->blobSize);
    if (err) {
        LOG_ERR("CRenderer3DS: lodepng error %u on page %lu: %s (marked failed)\n",
                err, (unsigned long)pageIdx, lodepng_error_text(err));
        page->pixels     = NULL;
        page->loadFailed = true;
        logMemory("after failed PNG decode");
        return false;
    }

    logMemory("after PNG decode");
    return true;
}

// ===[ Region upload ]===
//
// Extract entry's source rectangle from page->pixels, Morton-swizzle it into
// a temporary linear buffer, DMA-copy to the GPU texture, then free the
// swizzle buffer.  page->pixels stays alive until CEndFrame.

static bool uploadRegion(TexCachePage* page, RegionCacheEntry* entry, uint32_t pageIdx) {
    entry->texW = gpuTexDim(entry->srcW);
    entry->texH = gpuTexDim(entry->srcH);

    if (!C3D_TexInit(&entry->tex,
                     (uint16_t)entry->texW, (uint16_t)entry->texH,
                     GPU_RGBA8))
    {
        LOG_ERR("CRenderer3DS: C3D_TexInit failed page %lu region %ux%u @ (%u,%u)\n",
                (unsigned long)pageIdx,
                (unsigned)entry->srcW, (unsigned)entry->srcH,
                (unsigned)entry->srcX, (unsigned)entry->srcY);
        return false;
    }

    size_t bufSize = (size_t)entry->texW * entry->texH * 4;
    uint8_t* swizzle = (uint8_t*) linearAlloc(bufSize);
    if (!swizzle) {
        LOG_ERR("CRenderer3DS: linearAlloc(%lu KB) failed for swizzle buffer\n",
                (unsigned long)(bufSize / 1024));
        C3D_TexDelete(&entry->tex);
        return false;
    }
    memset(swizzle, 0, bufSize);

    linearToTile(swizzle,
                 page->pixels,
                 entry->srcX, entry->srcY,
                 entry->srcW, entry->srcH,
                 page->atlasW,
                 entry->texW, entry->texH);

    memcpy(entry->tex.data, swizzle, bufSize);
    GSPGPU_FlushDataCache(entry->tex.data, bufSize);
    linearFree(swizzle);

    C3D_TexSetFilter(&entry->tex, GPU_LINEAR, GPU_NEAREST);
    C3D_TexSetWrap(&entry->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    entry->loaded = true;
    return true;
}

// ===[ Page registration ]===
//
// Read only the PNG IHDR (~33 bytes) to record dimensions.  No decode, no GPU
// work — everything is demand-driven from drawRegion on first use.

static bool registerPage(TexCachePage* page, Texture* tx, uint32_t pageIdx) {
    if (!tx->blobData || tx->blobSize == 0) {
        LOG_ERR("CRenderer3DS: txtr[%lu] has no blob\n", (unsigned long)pageIdx);
        return false;
    }

    page->blobData      = tx->blobData;
    page->blobSize      = tx->blobSize;
    page->pixels        = NULL;
    page->loadFailed    = false;
    page->lastUsedFrame = 0;
    page->regionCount   = 0;

    unsigned w = 0, h = 0;
    if (lodepng_inspect(&w, &h, NULL, tx->blobData, tx->blobSize) != 0) {
        LOG_ERR("CRenderer3DS: lodepng_inspect failed on txtr[%lu]\n",
                (unsigned long)pageIdx);
        return false;
    }

    page->atlasW = (uint32_t)w;
    page->atlasH = (uint32_t)h;

    printf("CRenderer3DS: page %lu registered %ux%u (decode peak ~%lu KB)\n",
           (unsigned long)pageIdx, w, h,
           (unsigned long)((size_t)w * h * 4 / 1024));
    return true;
}

// ===[ drawRegion ]===
//
// Core draw primitive.  Looks up the exact source rectangle as a cached GPU
// texture, uploading it on first use.  Draws a single C2D_DrawImage call.
//
// If the requested region is larger than RENDERER_MAX_TEX_DIM in either
// dimension it is split along RENDERER_MAX_TEX_DIM boundaries and each chunk
// is cached and drawn independently.  In practice rotated draws come from
// sprite frames that are well under RENDERER_MAX_TEX_DIM, so the chunk loop
// executes exactly once for them.

static void drawRegion(CRenderer3DS* C,
                        uint32_t pageIdx,
                        float srcX,  float srcY,
                        float srcW,  float srcH,
                        float dstX,  float dstY,
                        float dstW,  float dstH,
                        float angle, float alpha)
{
    if (pageIdx >= C->pageCacheCount) return;
    TexCachePage* page = &C->pageCache[pageIdx];
    if (page->loadFailed) return;

    page->lastUsedFrame = C->frameCounter;

    // Clamp source rect to atlas bounds
    if (srcX < 0.0f) { float d = -srcX * dstW / srcW; dstX += d; dstW -= d; srcW += srcX; srcX = 0.0f; }
    if (srcY < 0.0f) { float d = -srcY * dstH / srcH; dstY += d; dstH -= d; srcH += srcY; srcY = 0.0f; }
    {
        float overX = (srcX + srcW) - (float)page->atlasW;
        float overY = (srcY + srcH) - (float)page->atlasH;
        if (overX > 0.0f) { dstW -= overX * dstW / srcW; srcW -= overX; }
        if (overY > 0.0f) { dstH -= overY * dstH / srcH; srcH -= overY; }
    }
    if (srcW <= 0.0f || srcH <= 0.0f) return;

    float pixScaleX = dstW / srcW;
    float pixScaleY = dstH / srcH;

    // Split along RENDERER_MAX_TEX_DIM chunk boundaries so each chunk fits in
    // a single GPU texture.
    float chunkY = srcY;
    while (chunkY < srcY + srcH) {
        float nextBY = (float)(((uint32_t)chunkY / RENDERER_MAX_TEX_DIM) + 1) * RENDERER_MAX_TEX_DIM;
        float chunkH = nextBY - chunkY;
        if (chunkY + chunkH > srcY + srcH) chunkH = (srcY + srcH) - chunkY;

        float chunkX = srcX;
        while (chunkX < srcX + srcW) {
            float nextBX = (float)(((uint32_t)chunkX / RENDERER_MAX_TEX_DIM) + 1) * RENDERER_MAX_TEX_DIM;
            float chunkW = nextBX - chunkX;
            if (chunkX + chunkW > srcX + srcW) chunkW = (srcX + srcW) - chunkX;

            uint16_t iSrcX = (uint16_t)chunkX;
            uint16_t iSrcY = (uint16_t)chunkY;
            uint16_t iSrcW = (uint16_t)chunkW;
            uint16_t iSrcH = (uint16_t)chunkH;

            // Cache lookup
            RegionCacheEntry* entry = regionLookup(page, iSrcX, iSrcY, iSrcW, iSrcH,
                                                   C->frameCounter);
            if (!entry) {
                // Cache miss: decode the page once this frame, then upload region
                if (!ensurePageDecoded(page, pageIdx)) goto next_chunk;

                entry = regionAlloc(page, iSrcX, iSrcY, iSrcW, iSrcH, C->frameCounter);
                if (!uploadRegion(page, entry, pageIdx)) goto next_chunk;
            }

            {
                // The region fills [0..iSrcW/texW] x [0..iSrcH/texH] of the GPU texture.
                // linearToTile Y-flips, so citro2d top > bottom (top = 1.0, bottom < 1.0).
                float u1 = (float)iSrcW / (float)entry->texW;
                float v1 = (float)iSrcH / (float)entry->texH;

                Tex3DS_SubTexture subtex = {
                    .width  = iSrcW,
                    .height = iSrcH,
                    .left   = 0.0f, .right  = u1,
                    .top    = 1.0f, .bottom = 1.0f - v1,
                };
                C2D_Image image = { .tex = &entry->tex, .subtex = &subtex };

                C2D_ImageTint tint;
                C2D_AlphaImageTint(&tint, alpha);

                C2D_DrawParams params = {
                    .pos    = { dstX + (chunkX - srcX) * pixScaleX,
                                dstY + (chunkY - srcY) * pixScaleY,
                                chunkW * pixScaleX,
                                chunkH * pixScaleY },
                    .center = { 0.0f, 0.0f },
                    .depth  = C->zCounter,
                    .angle  = angle,
                };
                C2D_DrawImage(image, &params, &tint);
                C->zCounter += 0.0001f;
            }

next_chunk:
            chunkX = nextBX;
        }
        chunkY = nextBY;
    }
}

// ===[ Vtable ]===

static void CInit(Renderer* renderer, DataWin* dataWin) {
    CRenderer3DS* C = (CRenderer3DS*) renderer;

    renderer->dataWin    = dataWin;
    renderer->drawColor  = 0xFFFFFF;
    renderer->drawAlpha  = 1.0f;
    renderer->drawFont   = -1;
    renderer->drawHalign = 0;
    renderer->drawValign = 0;

    uint32_t pageCount = dataWin->txtr.count;
    C->pageCache       = (TexCachePage*) safeCalloc(pageCount, sizeof(TexCachePage));
    C->pageCacheCount  = pageCount;
    C->frameCounter    = 1;

    verifyLodepngAllocator();
    logMemory("before page registration");
    for (uint32_t i = 0; i < pageCount; i++)
        registerPage(&C->pageCache[i], &dataWin->txtr.textures[i], i);
    logMemory("after page registration");

    C->top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    printf("CRenderer3DS: initialized (%lu pages, region-cache mode)\n",
           (unsigned long)pageCount);
    logMemory("renderer ready");
}

static void CDestroy(Renderer* renderer) {
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    for (uint32_t i = 0; i < C->pageCacheCount; i++) {
        TexCachePage* page = &C->pageCache[i];
        if (page->pixels) {
            lodepng_free(page->pixels);
            page->pixels = NULL;
        }
        for (uint32_t r = 0; r < page->regionCount; r++) {
            if (page->regions[r].loaded)
                C3D_TexDelete(&page->regions[r].tex);
        }
    }
    free(C->pageCache);
    free(C);
}

static void CBeginView(Renderer* renderer,
    int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
    int32_t portX, int32_t portY, int32_t portW, int32_t portH,
    float viewAngle)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    C->viewX = viewX;
    C->viewY = viewY;

    const float screenW = 400.0f, screenH = 240.0f;
    if (viewW > 0 && viewH > 0) {
        float scale = fminf(screenW / (float)viewW, screenH / (float)viewH);
        C->scaleX  = scale;
        C->scaleY  = scale;
        C->offsetX = (screenW - (float)viewW * scale) * 0.5f;
        C->offsetY = (screenH - (float)viewH * scale) * 0.5f;
    } else {
        C->scaleX = C->scaleY = 1.0f;
        C->offsetX = C->offsetY = 0.0f;
    }
}

static void CBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH,
                         int32_t windowW, int32_t windowH)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    C->zCounter = 0.5f;
    CBeginView(renderer, 0, 0, gameW, gameH, 0, 0, 400, 240, 0.0f);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(C->top, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(C->top);
}

static void CEndFrame(Renderer* renderer) {
    CRenderer3DS* C = (CRenderer3DS*) renderer;

    // Free decoded page pixels.  The per-region GPU textures (C3D_Tex) stay
    // resident in linear RAM across frames; they are evicted lazily by LRU
    // when regionAlloc needs to reclaim a slot.
    for (uint32_t i = 0; i < C->pageCacheCount; i++) {
        TexCachePage* page = &C->pageCache[i];
        if (page->pixels) {
            lodepng_free(page->pixels);
            page->pixels = NULL;
        }
    }

    C->frameCounter++;
    C3D_FrameEnd(0);
}

static void CEndView(Renderer* renderer) { /* no-op */ }

static void CDrawSprite(Renderer* renderer, int32_t tpagIndex,
    float x, float y, float originX, float originY,
    float xscale, float yscale, float angleDeg, uint32_t color, float alpha)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    DataWin* dw = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    uint32_t pageIdx = (uint32_t)tpag->texturePageId;

    float dstX = (x + ((float)tpag->targetX - originX) * xscale - (float)C->viewX) * C->scaleX + C->offsetX;
    float dstY = (y + ((float)tpag->targetY - originY) * yscale - (float)C->viewY) * C->scaleY + C->offsetY;
    float dstW = (float)tpag->sourceWidth  * xscale * C->scaleX;
    float dstH = (float)tpag->sourceHeight * yscale * C->scaleY;

    if (pageIdx < C->pageCacheCount) {
        drawRegion(C, pageIdx,
                   (float)tpag->sourceX,     (float)tpag->sourceY,
                   (float)tpag->sourceWidth,  (float)tpag->sourceHeight,
                   dstX, dstY, dstW, dstH,
                   angleDeg * (float)(M_PI / 180.0), alpha);
    } else {
        LOG_ERR("CRenderer3DS: page %lu out of range\n", (unsigned long)pageIdx);
        uint8_t a = (uint8_t)(alpha * 255.0f);
        C2D_DrawRectSolid(dstX, dstY, C->zCounter, dstW, dstH,
                          C2D_Color32(BGR_R(color), BGR_G(color), BGR_B(color), a));
        C->zCounter += 0.001f;
    }
}

static void CDrawSpritePart(Renderer* renderer, int32_t tpagIndex,
    int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH,
    float x, float y, float xscale, float yscale, uint32_t color, float alpha)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    DataWin* dw = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    uint32_t pageIdx = (uint32_t)tpag->texturePageId;

    float dstX = (x - (float)C->viewX) * C->scaleX + C->offsetX;
    float dstY = (y - (float)C->viewY) * C->scaleY + C->offsetY;
    float dstW = (float)srcW * xscale * C->scaleX;
    float dstH = (float)srcH * yscale * C->scaleY;

    if (pageIdx < C->pageCacheCount) {
        drawRegion(C, pageIdx,
                   (float)(tpag->sourceX + srcOffX),
                   (float)(tpag->sourceY + srcOffY),
                   (float)srcW, (float)srcH,
                   dstX, dstY, dstW, dstH, 0.0f, alpha);
    } else {
        LOG_ERR("CRenderer3DS: page %lu out of range\n", (unsigned long)pageIdx);
        uint8_t a = (uint8_t)(alpha * 255.0f);
        C2D_DrawRectSolid(dstX, dstY, C->zCounter, dstW, dstH,
                          C2D_Color32(BGR_R(color), BGR_G(color), BGR_B(color), a));
        C->zCounter += 0.0001f;
    }
}

static void CDrawRectangle(Renderer* renderer,
    float x1, float y1, float x2, float y2,
    uint32_t color, float alpha, bool outline)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = (uint8_t)(alpha * 255.0f);
    u32 col = C2D_Color32(r, g, b, a);

    float sx1 = (x1 - (float)C->viewX) * C->scaleX + C->offsetX;
    float sy1 = (y1 - (float)C->viewY) * C->scaleY + C->offsetY;
    float sx2 = (x2 - (float)C->viewX) * C->scaleX + C->offsetX;
    float sy2 = (y2 - (float)C->viewY) * C->scaleY + C->offsetY;
    float w   = sx2 - sx1;
    float h   = sy2 - sy1;

    if (outline) {
        float pw = C->scaleX;
        float ph = C->scaleY;
        C2D_DrawRectSolid(sx1,      sy1,      C->zCounter, w + pw, ph,     col); // top
        C->zCounter += 0.0001f;
        C2D_DrawRectSolid(sx1,      sy2,      C->zCounter, w + pw, ph,     col); // bottom
        C->zCounter += 0.0001f;
        C2D_DrawRectSolid(sx1,      sy1 + ph, C->zCounter, pw,     h - ph, col); // left
        C->zCounter += 0.0001f;
        C2D_DrawRectSolid(sx2,      sy1 + ph, C->zCounter, pw,     h - ph, col); // right
        C->zCounter += 0.0001f;
    } else {
        C2D_DrawRectSolid(sx1, sy1, C->zCounter, w, h, col);
        C->zCounter += 0.0001f;
    }
}

static void CDrawLine(Renderer* renderer,
    float x1, float y1, float x2, float y2,
    float width, uint32_t color, float alpha)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = (uint8_t)(alpha * 255.0f);
    C2D_DrawLine((x1 - C->viewX) * C->scaleX + C->offsetX,
                 (y1 - C->viewY) * C->scaleY + C->offsetY, C2D_Color32(r, g, b, a),
                 (x2 - C->viewX) * C->scaleX + C->offsetX,
                 (y2 - C->viewY) * C->scaleY + C->offsetY, C2D_Color32(r, g, b, a),
                 width, C->zCounter);
    C->zCounter += 0.0001f;
}

static void CDrawLineColor(Renderer* renderer,
    float x1, float y1, float x2, float y2,
    float width, uint32_t color1, uint32_t color2, float alpha)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    uint8_t a = (uint8_t)(alpha * 255.0f);
    u32 c1 = C2D_Color32(BGR_R(color1), BGR_G(color1), BGR_B(color1), a);
    u32 c2 = C2D_Color32(BGR_R(color2), BGR_G(color2), BGR_B(color2), a);
    C2D_DrawLine((x1 - C->viewX) * C->scaleX + C->offsetX,
                 (y1 - C->viewY) * C->scaleY + C->offsetY, c1,
                 (x2 - C->viewX) * C->scaleX + C->offsetX,
                 (y2 - C->viewY) * C->scaleY + C->offsetY, c2,
                 width, C->zCounter);
    C->zCounter += 0.0001f;
}

static void CDrawText(Renderer* renderer, const char* text,
    float x, float y, float xscale, float yscale, float angleDeg)
{
    (void)renderer; (void)text; (void)x; (void)y; // stub
}

static void CFlush(Renderer* renderer) { /* no-op */ }

static int32_t CCreateSpriteFromSurface(Renderer* renderer,
    int32_t x, int32_t y, int32_t w, int32_t h,
    bool removeback, bool smooth, int32_t xorig, int32_t yorig)
{
    LOG_ERR("CRenderer3DS: createSpriteFromSurface not supported\n");
    return -1;
}

static void CDeleteSprite(Renderer* renderer, int32_t spriteIndex) { /* no-op */ }

static RendererVtable CVtable = {
    .init                    = CInit,
    .destroy                 = CDestroy,
    .beginFrame              = CBeginFrame,
    .endFrame                = CEndFrame,
    .beginView               = CBeginView,
    .endView                 = CEndView,
    .drawSprite              = CDrawSprite,
    .drawSpritePart          = CDrawSpritePart,
    .drawRectangle           = CDrawRectangle,
    .drawLine                = CDrawLine,
    .drawLineColor           = CDrawLineColor,
    .drawText                = CDrawText,
    .flush                   = CFlush,
    .createSpriteFromSurface = CCreateSpriteFromSurface,
    .deleteSprite            = CDeleteSprite,
};

Renderer* CRenderer3DS_create(void) {
    CRenderer3DS* C = safeCalloc(1, sizeof(CRenderer3DS));
    C->base.vtable  = &CVtable;
    C->scaleX       = 2.0f;
    C->scaleY       = 2.0f;
    C->zCounter     = 0.5f;
    C->frameCounter = 1;
    return (Renderer*) C;
}