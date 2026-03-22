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
    if (p < 8) p = 8; // PICA200 hard minimum: textures must be at least 8x8
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

                    if (lx < copyW && ly < copyH) {
                        uint32_t srcOff = ((srcY0 + ly) * fullSrcW + (srcX0 + lx)) * 4;
                        uint8_t a = src[srcOff + 3];
                        dst[dstOff + 3] = a;

                        if (a) {
                            dst[dstOff + 0] = src[srcOff + 2]; // B
                            dst[dstOff + 1] = src[srcOff + 1]; // G
                            dst[dstOff + 2] = src[srcOff + 0]; // R
                        } else {
                            dst[dstOff + 0] = 0;
                            dst[dstOff + 1] = 0;
                            dst[dstOff + 2] = 0;
                        }
                    }
                    else {
                        dst[dstOff + 0] = 0; // B
                        dst[dstOff + 1] = 0; // G
                        dst[dstOff + 2] = 0; // R
                        dst[dstOff + 3] = 0; // A (transparent)
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
//
// Checks the pinned font glyph array first (never evicted), then the sprite
// LRU pool.  Keeping them separate means font glyphs never consume sprite slots
// and sprites never crowd out glyphs, regardless of how many fonts share a page.

static RegionCacheEntry* regionLookup(TexCachePage* page,
                                       uint16_t srcX, uint16_t srcY,
                                       uint16_t srcW, uint16_t srcH,
                                       uint32_t frameCounter)
{
    // Pinned font glyphs — no LRU update needed, these never expire
    for (uint32_t i = 0; i < page->pinnedRegionCount; i++) {
        RegionCacheEntry* e = &page->pinnedRegions[i];
        if (e->srcX == srcX && e->srcY == srcY &&
            e->srcW == srcW && e->srcH == srcH)
            return e;
    }

    // Sprite LRU pool
    for (uint32_t i = 0; i < page->regionCount; i++) {
        RegionCacheEntry* e = &page->regions[i];
        if (e->srcX == srcX && e->srcY == srcY &&
            e->srcW == srcW && e->srcH == srcH)
        {
            if (e->loaded) e->lastUsed = frameCounter;
            return e;
        }
    }
    return NULL;
}

// ===[ Region cache: pinned alloc (font glyphs only) ]===
//
// Grows pinnedRegions with realloc.  Called only from preloadFontGlyphs at init.

static RegionCacheEntry* regionAllocPinned(TexCachePage* page,
                                            uint16_t srcX, uint16_t srcY,
                                            uint16_t srcW, uint16_t srcH)
{
    if (page->pinnedRegionCount >= page->pinnedRegionCapacity) {
        uint32_t newCap = (page->pinnedRegionCapacity == 0) ? 64 : page->pinnedRegionCapacity * 2;
        RegionCacheEntry* newArr = realloc(page->pinnedRegions, newCap * sizeof(RegionCacheEntry));
        if (!newArr) {
            LOG_ERR("CRenderer3DS: OOM growing pinnedRegions\n");
            return NULL;
        }
        page->pinnedRegions        = newArr;
        page->pinnedRegionCapacity = newCap;
    }

    RegionCacheEntry* e = &page->pinnedRegions[page->pinnedRegionCount++];
    memset(e, 0, sizeof(*e));
    e->srcX = srcX; e->srcY = srcY;
    e->srcW = srcW; e->srcH = srcH;
    return e;
}

// ===[ Region cache: sprite alloc / LRU eviction ]===
//
// Operates only on the sprite regions[] array — pinned font glyphs are never
// touched here.  The sprite pool is always fully available regardless of how
// many font glyphs are pinned on the same page.

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
    // (no pinned entries can exist in regions[] — they live in pinnedRegions[])
    uint32_t oldestIdx = 0;
    uint32_t oldest    = page->regions[0].lastUsed;
    for (uint32_t i = 1; i < REGION_CACHE_MAX; i++) {
        if (page->regions[i].lastUsed < oldest) {
            oldest    = page->regions[i].lastUsed;
            oldestIdx = i;
        }
    }

    RegionCacheEntry* e = &page->regions[oldestIdx];
    if (e->loaded) C3D_TexDelete(&e->tex);
    memset(e, 0, sizeof(*e));
    e->srcX = srcX; e->srcY = srcY;
    e->srcW = srcW; e->srcH = srcH;
    e->lastUsed = frameCounter;
    return e;
}

// ===[ Pre-decode eviction ]===
//
// GPU region textures (C3D_Tex) live in linear RAM alongside lodepng's decode
// buffers.  Even when total linearSpaceFree() looks sufficient, interleaved
// small allocations fragment the heap so lodepng cannot get one large
// contiguous block.  Before each decode we evict loaded region entries
// (globally, LRU order) until there is enough headroom.
//
// We need: atlasW * atlasH * 4        — lodepng output buffer
//        + atlasW * atlasH * 4        — lodepng internal pre-filter scratch
//        + 2 MB                       — huffman tables, zlib window, misc
// (Total ≈ 2× raw size + 2 MB)
//
// After eviction the linear heap has one large free region that lodepng can
// grab contiguously.  Any evicted regions are re-uploaded from the fresh
// pixels during the same frame without a second decode.

// C must be passed so we can walk all pages' region caches.
// pageCache/pageCacheCount are not accessible from TexCachePage alone.
// We store a module-level pointer set during CInit for this purpose.
// (Alternatively, thread it through, but this is simpler for a single-threaded renderer.)
static CRenderer3DS* g_renderer = NULL;

static void evictRegionsForDecode(uint32_t skipPageIdx, size_t bytesNeeded) {
    if (!g_renderer) return;

    // Keep evicting the globally oldest loaded region until we have enough room.
    for (;;) {
        if ((size_t)linearSpaceFree() >= bytesNeeded) return;

        // Find the globally oldest loaded region entry across all pages.
        uint32_t victimPage  = UINT32_MAX;
        uint32_t victimSlot  = UINT32_MAX;
        uint32_t oldest      = UINT32_MAX;

        for (uint32_t pi = 0; pi < g_renderer->pageCacheCount; pi++) {
            if (pi == skipPageIdx) continue;
            TexCachePage* p = &g_renderer->pageCache[pi];
            for (uint32_t ri = 0; ri < p->regionCount; ri++) {
                RegionCacheEntry* e = &p->regions[ri];
                if (!e->loaded) continue; // sprite pool only; pinned live in pinnedRegions[]
                if (e->lastUsed < oldest) {
                    oldest     = e->lastUsed;
                    victimPage = pi;
                    victimSlot = ri;
                }
            }
        }

        if (victimPage == UINT32_MAX) {
            // Nothing left to evict.
            printf("[TEX] eviction exhausted; %lu KB free (needed %lu KB)\n",
                   (unsigned long)(linearSpaceFree() / 1024),
                   (unsigned long)(bytesNeeded / 1024));
            return;
        }

        RegionCacheEntry* e = &g_renderer->pageCache[victimPage].regions[victimSlot];
        C3D_TexDelete(&e->tex);
        e->loaded = false;
    }
}

static void regionEvictAllNonPinned(CRenderer3DS* C)
{
    for (uint32_t pi = 0; pi < C->pageCacheCount; pi++) {
        TexCachePage* page = &C->pageCache[pi];

        for (uint32_t r = 0; r < page->regionCount; r++) {
            if (page->regions[r].loaded)
                C3D_TexDelete(&page->regions[r].tex);
        }

        page->regionCount = 0;
    }
}

// Use this on OLD 3DS to give the linear heap a chance to recover between evicting and decoding
static void regionEvictAllNonPinnedOLD3DS(CRenderer3DS* C)
{
    bool isNew3DS = false;
    APT_CheckNew3DS(&isNew3DS);

    if (!isNew3DS)
        regionEvictAllNonPinned(C);
}

// ===[ Page decode ]===
//
// Decode the page PNG into linear RAM exactly once per frame, storing the
// result in page->pixels.  Subsequent region misses on the same page reuse
// the already-decoded pixels without re-decoding.  CEndFrame frees them.
//
// Before decoding, evict enough GPU region textures to guarantee a contiguous
// block for lodepng — otherwise heap fragmentation causes error 83 even when
// total free bytes appear sufficient.

static bool ensurePageDecoded(TexCachePage* page, uint32_t pageIdx) {
    // If pixels are already loaded (this frame or recent frame), good to go
    if (page->pixels) return true;
    
    if (page->loadFailed) return false;

    CRenderer3DS* C = g_renderer;
    uint32_t currentFrame = C->frameCounter;

    // Optimize: if we recently decoded and the memory pressure is low, reuse decoded data
    bool shouldReuse = (currentFrame - page->lastDecodeFrame < page->decodeTimeout) && 
                       page->pixels;  // Has pixels from recent frame

    if (shouldReuse) {
        printf("Reusing decoded page %lu (decoded %u frames ago)\n", 
               (unsigned long)pageIdx, currentFrame - page->lastDecodeFrame);
        return true; // Already decoded recently
    }

    page->decodeTimeout = 120; // Keep decoded for 120 frames
    page->lastDecodeFrame = 0; // Never decoded yet

    // Need to decode now - but try to evict efficiently
    uint8_t* blobData = DataWin_loadTexture(C->base.dataWin, pageIdx);
    if (!blobData) {
        page->loadFailed = true;
        return false;
    }

    size_t blobSize = C->base.dataWin->txtr.textures[pageIdx].blobSize;
    size_t estimatedPeak = (size_t)page->atlasW * page->atlasH * 4 * 2 + 1024 * 1024;
    evictRegionsForDecode(pageIdx, estimatedPeak);

    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode32(&page->pixels, &w, &h, blobData, blobSize);
    if (err) {
        LOG_ERR("CRenderer3DS: lodepng error %u on page %lu\n", err, (unsigned long)pageIdx);
        page->pixels = NULL;
        page->loadFailed = true;
        return false;
    }

    page->lastDecodeFrame = currentFrame;
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
        LOG_ERR("CRenderer3DS: C3D_TexInit failed page %lu region %ux%u @ (%u,%u) "
                "(GPU tex %ux%u) — marked failed\n",
                (unsigned long)pageIdx,
                (unsigned)entry->srcW, (unsigned)entry->srcH,
                (unsigned)entry->srcX, (unsigned)entry->srcY,
                (unsigned)entry->texW, (unsigned)entry->texH);
        entry->loadFailed = true;
        return false;
    }

    size_t bufSize = (size_t)entry->texW * entry->texH * 4;
    uint8_t* swizzle = (uint8_t*) linearAlloc(bufSize);
    if (!swizzle) {
        LOG_ERR("CRenderer3DS: linearAlloc(%lu KB) failed for swizzle buffer "
                "page %lu region %ux%u @ (%u,%u) — marked failed\n",
                (unsigned long)(bufSize / 1024), (unsigned long)pageIdx,
                (unsigned)entry->srcW, (unsigned)entry->srcH,
                (unsigned)entry->srcX, (unsigned)entry->srcY);
        C3D_TexDelete(&entry->tex);
        entry->loadFailed = true;
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
    // Trust the fact that the data.win has valid OFFSET for this texture
    if (tx->blobOffset == 0) {
        printf("CRenderer3DS: page %lu has invalid offset (0), marking as invalid\n", 
               (unsigned long)pageIdx);
        page->loadFailed = true;
        return false;
    }

    page->blobData      = NULL; // Set during ensurePageDecoded
    page->blobSize      = 0;    // Set during ensurePageDecoded
    page->pixels        = NULL;
    page->loadFailed    = false;
    page->lastUsedFrame = 0;
    page->regionCount   = 0;

    // Get actual dimensions by loading just for inspection
    uint8_t* inspectBlob = DataWin_loadTexture(g_renderer->base.dataWin, pageIdx);
    if (!inspectBlob || tx->blobSize == 0) {
        printf("CRenderer3DS: Cannot inspect texture at offset %llu for page %lu\n", 
               (unsigned long long)tx->blobOffset, (unsigned long)pageIdx);
        page->loadFailed = true;
        return false;
    }
    
    unsigned w = 0, h = 0;
    if (lodepng_inspect(&w, &h, NULL, inspectBlob, tx->blobSize) != 0) {
        LOG_ERR("CRenderer3DS: lodepng_inspect failed on txtr[%lu] even after loading\n",
                (unsigned long)pageIdx);
        page->loadFailed = true;
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
                        float angle, u32 color)
{
    printf("drawRegion: Called for page %lu: src=(%.1f,%.1f,%.1f,%.1f) dst=(%.1f,%.1f,%.1f,%.1f)\n",
           (unsigned long)pageIdx, srcX, srcY, srcW, srcH, dstX, dstY, dstW, dstH);

    if (pageIdx >= C->pageCacheCount) {
        printf("drawRegion: ERROR - page index %lu >= cache count %lu\n", 
               (unsigned long)pageIdx, (unsigned long)C->pageCacheCount);
        return;
    }
    
    TexCachePage* page = &C->pageCache[pageIdx];
    if (page->loadFailed) {
        printf("drawRegion: ERROR - page %lu marked as loadFailed\n", (unsigned long)pageIdx);
        return;
    }

    page->lastUsedFrame = C->frameCounter;

    // Clamp source rect to atlas bounds
    float origSrcX = srcX, origSrcY = srcY, origSrcW = srcW, origSrcH = srcH;
    if (srcX < 0.0f) { 
        float d = -srcX * dstW / srcW; 
        dstX += d; dstW -= d; 
        srcW += srcX; 
        srcX = 0.0f; 
        printf("Clamped src X: (%.1f,%.1f) -> (%.1f,%.1f)\n", origSrcX, origSrcW, srcX, srcW);
    }
    if (srcY < 0.0f) { 
        float d = -srcY * dstH / srcH; 
        dstY += d; dstH -= d; 
        srcH += srcY; 
        srcY = 0.0f;
        printf("Clamped src Y: (%.1f,%.1f) -> (%.1f,%.1f)\n", origSrcY, origSrcH, srcY, srcH);
    }
    {
        float overX = (srcX + srcW) - (float)page->atlasW;
        float overY = (srcY + srcH) - (float)page->atlasH;
        if (overX > 0.0f) { 
            dstW -= overX * dstW / srcW; 
            srcW -= overX; 
            printf("Clamped srcWidth: cropped %f from %f to %f\n", overX, origSrcW, srcW);
        }
        if (overY > 0.0f) { 
            dstH -= overY * dstH / srcH; 
            srcH -= overY; 
            printf("Clamped srcHeight: cropped %f from %f to %f\n", overY, origSrcH, srcH);
        }
    }
    if (srcW <= 0.0f || srcH <= 0.0f) {
        printf("drawRegion: INFO - Source rect empty after clamping (%.1f,%.1f), skipping\n", srcW, srcH);
        return;
    }

    float pixScaleX = dstW / srcW;
    float pixScaleY = dstH / srcH;

    printf("drawRegion: Drawing chunks for page %u: src=(%.1f,%.1f,%.1f,%.1f) dst=(%.1f,%.1f,%.1f,%.1f)\n",
           pageIdx, srcX, srcY, srcW, srcH, dstX, dstY, dstW, dstH);

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

            // Cache lookup — returns any entry with a matching key, loaded or not
            printf("drawRegion: Looking up chunk src=(%u,%u,%u,%u) on page %lu\n", 
                   iSrcX, iSrcY, iSrcW, iSrcH, (unsigned long)pageIdx);
            
            RegionCacheEntry* entry = regionLookup(page, iSrcX, iSrcY, iSrcW, iSrcH,
                                                   C->frameCounter);
            if (!entry) {
                printf("drawRegion: CACHE MISS for chunk (%u,%u,%u,%u) - need to load page %lu\n", 
                       iSrcX, iSrcY, iSrcW, iSrcH, (unsigned long)pageIdx);
                       
                // True cache miss: decode the page once this frame, then upload
                if (!ensurePageDecoded(page, pageIdx)) {
                    printf("drawRegion: FAILED to decode page %lu, skipping chunk\n", (unsigned long)pageIdx);
                    goto next_chunk;
                }

                entry = regionAlloc(page, iSrcX, iSrcY, iSrcW, iSrcH, C->frameCounter);
                if (!entry) {
                    printf("drawRegion: FAILED to allocate cache entry for (%u,%u,%u,%u), skipping\n",
                           iSrcX, iSrcY, iSrcW, iSrcH);
                    goto next_chunk;
                }
                
                printf("drawRegion: Allocated cache entry for (%u,%u,%u,%u), about to upload\n", 
                       iSrcX, iSrcY, iSrcW, iSrcH);
                
                uploadRegion(page, entry, pageIdx);
                printf("drawRegion: Uploaded chunk (%u,%u,%u,%u) - loaded=%s\n",
                       iSrcX, iSrcY, iSrcW, iSrcH, entry->loaded ? "YES" : "NO");
                
                if (!entry->loaded) {
                    printf("drawRegion: FAILED to upload chunk (%u,%u,%u,%u), marked failed\n",
                           iSrcX, iSrcY, iSrcW, iSrcH);
                    goto next_chunk;
                }
            } else {
                printf("drawRegion: CACHE HIT for chunk (%u,%u,%u,%u) - cache state: loaded=%s\n",
                       iSrcX, iSrcY, iSrcW, iSrcH, entry->loaded ? "YES" : "NO");
            }

            // Skip draw if this region permanently failed to upload
            if (!entry->loaded) {
                printf("drawRegion: Chunk (%u,%u,%u,%u) marked failed, skipping draw\n",
                       iSrcX, iSrcY, iSrcW, iSrcH);
                goto next_chunk;
            }

            printf("drawRegion: Drawing chunk (%u,%u,%u,%u) with GPU tex (%ux%u)\n",
                   iSrcX, iSrcY, iSrcW, iSrcH, (unsigned)entry->texW, (unsigned)entry->texH);

            {
                // The region fills [0..iSrcW/texW] x [0..iSrcH/texH] of the GPU texture.
                // linearToTile Y-flips, so citro2d top > bottom (top = 1.0, bottom < 1.0).
                float scaleW = (float)iSrcW / (float)entry->texW;
                float scaleH = (float)iSrcH / (float)entry->texH;

                Tex3DS_SubTexture subtex = {
                    .width  = iSrcW,
                    .height = iSrcH,
                    .left   = 0.0f, .right  = scaleW,
                    .top    = 1.0f, .bottom = 1.0f - scaleH,
                };
                
                // Verify texture is valid
                if (entry->tex.data == NULL) {
                    printf("drawRegion: ERROR - GPU texture has NULL data!\n");
                    goto next_chunk;
                }

                C2D_Image image = { .tex = &entry->tex, .subtex = &subtex };

                C2D_ImageTint tint;
                C2D_PlainImageTint(&tint, color, 1.0f);

                float chunkDestX = dstX + (chunkX - srcX) * pixScaleX;
                float chunkDestY = dstY + (chunkY - srcY) * pixScaleY;

                C2D_DrawParams params = {
                    .pos    = { chunkDestX, chunkDestY, 
                                chunkW * pixScaleX, chunkH * pixScaleY },
                    .center = { 0.0f, 0.0f },
                    .depth  = C->zCounter,
                    .angle  = angle,
                };
                
                printf("drawRegion: Calling C2D_DrawImage with pos:(%.1f,%.1f,%.1f,%.1f)\n",
                       params.pos.x, params.pos.y, params.pos.w, params.pos.h);
                       
                C2D_DrawImage(image, &params, &tint);
                C->zCounter += 0.0001f;
            }

next_chunk:
            chunkX = nextBX;
        }
        chunkY = nextBY;
    }
}

// ===[ Font glyph preload ]===
//
// At init time, decode every font's atlas page once and upload all glyphs as
// pinned region cache entries.  Pinned entries survive LRU eviction and the
// pre-decode eviction pass, so CDrawText never touches the PNG again.
//
// Pages shared by multiple fonts (or by fonts and sprites) are decoded once
// and all their fonts' glyphs are uploaded before pixels are freed.

static void preloadFontGlyphs(CRenderer3DS* C, DataWin* dw) {
    printf("CRenderer3DS: preloading font glyphs...\n");
    logMemory("before font preload");

    for (uint32_t fi = 0; fi < dw->font.count; fi++) {
        Font* font = &dw->font.fonts[fi];

        int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
        if (fontTpagIndex < 0 || (uint32_t)fontTpagIndex >= dw->tpag.count) continue;

        TexturePageItem* tpag = &dw->tpag.items[fontTpagIndex];
        uint32_t pageIdx = (uint32_t)tpag->texturePageId;
        if (pageIdx >= C->pageCacheCount) continue;

        TexCachePage* page = &C->pageCache[pageIdx];
        page->blobData = DataWin_loadTexture(dw, pageIdx);
        page->blobSize = dw->txtr.textures[pageIdx].blobSize;
        if (page->loadFailed) continue;

        // Allow ram to recover, this lets OLD 3DS work
        logMemory("regionEvictAllNonPinned before font page preload");
        regionEvictAllNonPinnedOLD3DS(C);
        logMemory("regionEvictAllNonPinned finished before font page preload");

        // Decode the page if not already in RAM from a previous font on the same page
        if (!page->pixels) {
            size_t peakBytes = (size_t)page->atlasW * page->atlasH * 4 * 2 + 2 * 1024 * 1024;
            evictRegionsForDecode(pageIdx, peakBytes);

            logMemory("before font page PNG decode");
            unsigned w = 0, h = 0;
            unsigned err = lodepng_decode32(&page->pixels, &w, &h,
                                            page->blobData, page->blobSize);
            if (err) {
                LOG_ERR("CRenderer3DS: font page %lu decode error %u: %s\n",
                        (unsigned long)pageIdx, err, lodepng_error_text(err));
                page->loadFailed = true;
                continue;
            }
        }

        logMemory("regionEvictAllNonPinned after font page preload");
        regionEvictAllNonPinnedOLD3DS(C);
        logMemory("regionEvictAllNonPinned finished after font page preload");

        // Upload every glyph that has visible pixels as a pinned cache entry
        uint32_t uploaded = 0;
        uint32_t skipped  = 0;
        for (uint32_t gi = 0; gi < font->glyphCount; gi++) {
            FontGlyph* glyph = &font->glyphs[gi];
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) continue;

            uint16_t srcX = (uint16_t)(tpag->sourceX + glyph->sourceX);
            uint16_t srcY = (uint16_t)(tpag->sourceY + glyph->sourceY);
            uint16_t srcW = (uint16_t)glyph->sourceWidth;
            uint16_t srcH = (uint16_t)glyph->sourceHeight;

            // Skip if already uploaded by a previous font sharing this page
            RegionCacheEntry* entry = regionLookup(page, srcX, srcY, srcW, srcH, 0);
            if (entry) { skipped++; continue; }

            entry = regionAlloc(page, srcX, srcY, srcW, srcH, C->frameCounter);
            if (!entry) {
                LOG_ERR("CRenderer3DS: OOM allocating pinned slot for font %lu glyph %lu\n",
                        (unsigned long)fi, (unsigned long)gi);
                continue;
            }
            if (uploadRegion(page, entry, pageIdx))
                uploaded++;
        }

        printf("CRenderer3DS: font %lu (%lu glyphs) on page %lu — %u uploaded, %u already cached\n",
               (unsigned long)fi, (unsigned long)font->glyphCount,
               (unsigned long)pageIdx, uploaded, skipped);
    }

    // Free all decoded page pixels — pinned GPU textures remain resident
    for (uint32_t i = 0; i < C->pageCacheCount; i++) {
        if (C->pageCache[i].pixels) {
            lodepng_free(C->pageCache[i].pixels);
            C->pageCache[i].pixels = NULL;
        }
    }

    logMemory("after font preload");
    printf("CRenderer3DS: font preload complete\n");
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
    g_renderer         = C; // used by evictRegionsForDecode

    verifyLodepngAllocator();
    logMemory("before page registration");
    for (uint32_t i = 0; i < pageCount; i++)
        registerPage(&C->pageCache[i], &dataWin->txtr.textures[i], i);
    logMemory("after page registration");

    bool isNew3DS = false;
    if (APT_CheckNew3DS(&isNew3DS) == 0 && isNew3DS) {
        printf("CRenderer3DS: running on New 3DS, good linear RAM availability expected\n");
        preloadFontGlyphs(C, dataWin);
    } else {
        printf("CRenderer3DS: running on Old 3DS or APT_CheckNew3DS failed, expect tight linear RAM, not preloading font glyphs\n");
    }

    C->top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    printf("CRenderer3DS: initialized (%lu pages, region-cache mode)\n",
           (unsigned long)pageCount);
    logMemory("renderer ready");
}

static void CDestroy(Renderer* renderer) {
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    g_renderer = NULL;
    for (uint32_t i = 0; i < C->pageCacheCount; i++) {
        TexCachePage* page = &C->pageCache[i];
        if (page->pixels) {
            lodepng_free(page->pixels);
            page->pixels = NULL;
        }
        // Free pinned font glyph textures
        for (uint32_t r = 0; r < page->pinnedRegionCount; r++) {
            if (page->pinnedRegions[r].loaded)
                C3D_TexDelete(&page->pinnedRegions[r].tex);
        }
        free(page->pinnedRegions);
        // Free sprite region textures
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

    if (viewW > 0 && viewH > 0 && portW > 0 && portH > 0) {
        // Scale the view to fit the port, preserving aspect ratio (letterbox/pillarbox)
        float scale = fminf((float)portW / (float)viewW, (float)portH / (float)viewH);
        C->scaleX  = scale;
        C->scaleY  = scale;
        // Center within the port, then offset by the port's own origin
        C->offsetX = (float)portX + ((float)portW - (float)viewW * scale) * 0.5f;
        C->offsetY = (float)portY + ((float)portH - (float)viewH * scale) * 0.5f;
    } else {
        C->scaleX  = 1.0f;
        C->scaleY  = 1.0f;
        C->offsetX = (float)portX;
        C->offsetY = (float)portY;
    }
}

static void CBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH,
                         int32_t windowW, int32_t windowH)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    C->zCounter = 0.5f;
    CBeginView(renderer, 0, 0, gameW, gameH, 50, 0, windowW, windowH, 0.0f);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(C->top, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(C->top);
}

static void CEndFrame(Renderer* renderer) {
    CRenderer3DS* C = (CRenderer3DS*) renderer;

    // Free decoded pixels that AREN'T on the hot list (keep some for next frame)
    uint32_t currentFrame = C->frameCounter;
    for (uint32_t i = 0; i < C->pageCacheCount; i++) {
        TexCachePage* page = &C->pageCache[i];
        
        // Only free if it's older than our desired timeout
        if (page->pixels && 
            (currentFrame - page->lastDecodeFrame > page->decodeTimeout)) {
            lodepng_free(page->pixels);
            page->pixels = NULL;
            printf("CRenderer3DS: Freed decoded pixels for page %lu (age: %u frames)\n", 
                   (unsigned long)i, currentFrame - page->lastDecodeFrame);
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
    printf("CDrawSprite: Requested tpagIndex=%d at (%.1f,%.1f) scale=(%.1f,%.1f)\n", 
           tpagIndex, x, y, xscale, yscale);
           
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    DataWin* dw = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) {
        printf("CDrawSprite: ERROR - Invalid tpag index %d (max count %d)\n", 
               tpagIndex, dw->tpag.count);
        C2D_DrawRectSolid(x * C->scaleX, y * C->scaleY, C->zCounter, 10, 10, C2D_Color32(255,0,0,255));
        C->zCounter += 0.001f;
        return;
    }
    
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    uint32_t pageIdx = (uint32_t)tpag->texturePageId;

    printf("CDrawSprite: Tpag %d -> page %lu, atlas (%lux%lu) -> target (%lu,%lu,%lux%lu)\n",
           tpagIndex, (unsigned long)pageIdx,
           (unsigned long)tpag->sourceWidth, (unsigned long)tpag->sourceHeight,
           (unsigned long)tpag->targetX, (unsigned long)tpag->targetY,
           (unsigned long)tpag->sourceWidth, (unsigned long)tpag->sourceHeight);

    float dstX = (x + ((float)tpag->targetX - originX) * xscale - (float)C->viewX) * C->scaleX + C->offsetX;
    float dstY = (y + ((float)tpag->targetY - originY) * yscale - (float)C->viewY) * C->scaleY + C->offsetY;
    float dstW = (float)tpag->sourceWidth  * xscale * C->scaleX;
    float dstH = (float)tpag->sourceHeight * yscale * C->scaleY;

    printf("CDrawSprite: Final draw: rect (%.1f,%.1f,%.1f,%.1f) on page %lu\n",
           dstX, dstY, dstW, dstH, (unsigned long)pageIdx);

    if (pageIdx < C->pageCacheCount && pageIdx <= 25) { // Check if valid page
        printf("CDrawSprite: About to draw region from page %lu\n", (unsigned long)pageIdx);
        drawRegion(C, pageIdx,
                   (float)tpag->sourceX,     (float)tpag->sourceY,
                   (float)tpag->sourceWidth,  (float)tpag->sourceHeight,
                   dstX, dstY, dstW, dstH,
                   angleDeg * (float)(M_PI / 180.0), color);
    } else {
        printf("CDrawSprite: ERROR - Invalid page %lu or page out of range\n", (unsigned long)pageIdx);
        // Draw a red square so you can tell something should have been drawn
        C2D_DrawRectSolid(dstX, dstY, C->zCounter, dstW, dstH, C2D_Color32(255,0,0,255)); // Red
        C->zCounter += 0.001f;
    }
}

static void CDrawSpritePart(Renderer* renderer, int32_t tpagIndex,
    int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH,
    float x, float y, float xscale, float yscale, uint32_t color, float alpha)
{
    printf("CDrawSpritePart: Requested tpagIndex=%d src=(%d,%d,%d,%d) at (%.1f,%.1f)\n", 
           tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y);
           
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    DataWin* dw = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) {
        printf("CDrawSpritePart: ERROR - Invalid tpag index %d\n", tpagIndex);
        C2D_DrawRectSolid(x, y, C->zCounter, 10, 10, C2D_Color32(255,0,0,255)); // Red square
        C->zCounter += 0.001f;
        return;
    }
    
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    uint32_t pageIdx = (uint32_t)tpag->texturePageId;

    printf("CDrawSpritePart: Tpag %d -> page %lu, request src=(%d,%d,%d,%d)\n",
           tpagIndex, (unsigned long)pageIdx, srcOffX, srcOffY, srcW, srcH);
           
    float dstX = (x - (float)C->viewX) * C->scaleX + C->offsetX;
    float dstY = (y - (float)C->viewY) * C->scaleY + C->offsetY;
    float dstW = (float)srcW * xscale * C->scaleX;
    float dstH = (float)srcH * yscale * C->scaleY;

    printf("CDrawSpritePart: Final draw: rect (%.1f,%.1f,%.1f,%.1f) on page %lu\n",
           dstX, dstY, dstW, dstH, (unsigned long)pageIdx);

    if (pageIdx < C->pageCacheCount) {
        drawRegion(C, pageIdx,
                   (float)(tpag->sourceX + srcOffX),
                   (float)(tpag->sourceY + srcOffY),
                   (float)srcW, (float)srcH,
                   dstX, dstY, dstW, dstH, 0.0f, C2D_Color32(BGR_R(color), BGR_G(color), BGR_B(color), (uint8_t)(alpha * 255.0f)));
    } else {
        printf("CDrawSpritePart: ERROR - Page %lu out of range\n", (unsigned long)pageIdx);
        C2D_DrawRectSolid(dstX, dstY, C->zCounter, dstW, dstH, C2D_Color32(255,0,0,255)); // Red
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
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    DataWin* dw = renderer->dataWin;

    if (renderer->drawFont < 0 || (uint32_t)renderer->drawFont >= dw->font.count) return;

    Font* font = &dw->font.fonts[renderer->drawFont];

    // Resolve which texture page the font lives on
    int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (fontTpagIndex < 0 || (uint32_t)fontTpagIndex >= dw->tpag.count) return;

    TexturePageItem* tpag = &dw->tpag.items[fontTpagIndex];
    uint32_t pageIdx = (uint32_t)tpag->texturePageId;

    // Preprocess GML text (# -> newline, \# -> #)
    char* processed = TextUtils_preprocessGmlText(text);
    int32_t textLen = (int32_t)strlen(processed);

    // Vertical alignment
    int32_t lineCount   = TextUtils_countLines(processed, textLen);
    float   totalHeight = (float)lineCount * (float)font->emSize;
    float   valignOffset = 0.0f;
    if      (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float cursorY  = valignOffset;
    int32_t lineStart = 0;

    while (lineStart <= textLen) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(processed[lineEnd]))
            lineEnd++;

        int32_t    lineLen = lineEnd - lineStart;
        const char* line   = processed + lineStart;

        // Horizontal alignment
        float lineWidth   = TextUtils_measureLineWidth(font, line, lineLen);
        float halignOffset = 0.0f;
        if      (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        int32_t pos   = 0;

        while (pos < lineLen) {
            uint16_t ch    = TextUtils_decodeUtf8(line, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (!glyph) continue;

            if (glyph->sourceWidth > 0 && glyph->sourceHeight > 0) {
                // Screen-space destination rect for this glyph
                float glyphX = x + (cursorX + (float)glyph->offset) * xscale * font->scaleX;
                float glyphY = y + cursorY * yscale * font->scaleY;
                float glyphW = (float)glyph->sourceWidth  * xscale * font->scaleX;
                float glyphH = (float)glyph->sourceHeight * yscale * font->scaleY;

                float dstX = (glyphX - (float)C->viewX) * C->scaleX + C->offsetX;
                float dstY = (glyphY - (float)C->viewY) * C->scaleY + C->offsetY;
                float dstW = glyphW * C->scaleX;
                float dstH = glyphH * C->scaleY;

                // Source rect within the atlas page: tpag origin + glyph offset
                float srcX = (float)(tpag->sourceX + glyph->sourceX);
                float srcY = (float)(tpag->sourceY + glyph->sourceY);
                float srcW = (float)glyph->sourceWidth;
                float srcH = (float)glyph->sourceHeight;

                drawRegion(C, pageIdx, srcX, srcY, srcW, srcH,
                        dstX, dstY, dstW, dstH, 0.0f, C2D_Color32(BGR_R(renderer->drawColor), BGR_G(renderer->drawColor), BGR_B(renderer->drawColor), (uint8_t)(renderer->drawAlpha * 255.0f)));
            }

            cursorX += (float)glyph->shift;

            // Kerning lookahead
            if (pos < lineLen) {
                int32_t savedPos = pos;
                uint16_t nextCh  = TextUtils_decodeUtf8(line, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        // Advance to next line
        cursorY += (float)font->emSize;
        if (lineEnd < textLen)
            lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        else
            break;
    }

    free(processed);
}

// ===[ Room transition ]===
//
// Evict all non-pinned region cache entries across every page.  Called when
// the game switches rooms so stale sprite textures from the previous room are
// freed, making linear RAM available for the new room's assets.
//
// Pinned entries (font glyphs preloaded at init) are kept — they compact to
// the front of each page's region array so the slot space they vacate is
// immediately reusable.
static void COnRoomEnd(Renderer* renderer) {
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    uint32_t totalEvicted = 0;

    for (uint32_t pi = 0; pi < C->pageCacheCount; pi++) {
        TexCachePage* page = &C->pageCache[pi];
        if (page->regionCount == 0) continue;

        // Evict and discard all sprite regions — pinnedRegions (font glyphs) are untouched
        for (uint32_t r = 0; r < page->regionCount; r++) {
            if (page->regions[r].loaded) {
                C3D_TexDelete(&page->regions[r].tex);
                totalEvicted++;
            }
        }
        memset(page->regions, 0, page->regionCount * sizeof(RegionCacheEntry));
        page->regionCount = 0;
    }

    printf("CRenderer3DS: room end — evicted %u sprite textures, font glyphs retained\n",
           totalEvicted);
    logMemory("after room eviction");
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
    .onRoomEnd               = COnRoomEnd,
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