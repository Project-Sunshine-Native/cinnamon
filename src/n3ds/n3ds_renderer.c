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
    return hdr + 1;  // pointer lodepng sees
}

void lodepng_free(void* ptr);

void* lodepng_realloc(void* ptr, size_t new_size) {
    if (!ptr)       return lodepng_malloc(new_size);
    if (!new_size)  { lodepng_free(ptr); return NULL; }
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

// Include lodepng AFTER defining the allocator functions above.
// IMPORTANT: -DLODEPNG_NO_COMPILE_ALLOCATORS must be in CFLAGS for all TUs,
// including lodepng.c itself. If lodepng.c has its own build rule, add it there too.
// regardless of per-TU compiler flags.
#include "lodepng.h"

static void verifyLodepngAllocator(void) {
    u32 before = linearSpaceFree();
    void* p = lodepng_malloc(1024 * 1024); // 1MB probe
    u32 after  = linearSpaceFree();
    if (p && before != after) {
        printf("[lodepng] custom allocator OK (linear -%ld KB)\n",
               (long)(before - after) / 1024);
    } else {
        LOG_ERR("[lodepng] WARNING: custom allocator NOT using linear heap!\n");
        LOG_ERR("[lodepng] Add -DLODEPNG_NO_COMPILE_ALLOCATORS to the build rule for lodepng.c\n");
    }
    if (p) lodepng_free(p);
}
// ===[ Memory logging ]===

static void logMemory(const char* tag) {
    u32 appBytes    = osGetMemRegionFree(MEMREGION_APPLICATION);
    u32 linearBytes = linearSpaceFree();
    printf("[MEM] %-40s app heap: %lu KB   linear: %lu KB\n",
           tag,
           (unsigned long)(appBytes    / 1024),
           (unsigned long)(linearBytes / 1024));
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
                }
            }
        }
    }
}

// ===[ Lazy tile upload ]===
//
// Decodes the page's PNG into linear RAM on first tile draw, uploads that
// tile to the GPU, then frees the decoded pixels once every tile is loaded.
// At peak only one page's pixels + one linearAlloc swizzle buffer coexist.

// Upload one already-decoded tile to the GPU.  pixels must be valid.
// Does NOT free pixels or check allLoaded — caller handles that.
static bool uploadOneTile(TexCachePage* page, TexCacheTile* tile, uint32_t pageIdx) {
    tile->texW = gpuTexDim(tile->regionW);
    tile->texH = gpuTexDim(tile->regionH);

    if (!C3D_TexInit(&tile->tex, (uint16_t)tile->texW, (uint16_t)tile->texH,
                     GPU_RGBA8)) {
        LOG_ERR("CRenderer3DS: C3D_TexInit failed for page %lu tile (%lux%lu)\n",
                (unsigned long)pageIdx,
                (unsigned long)tile->texW, (unsigned long)tile->texH);
        logMemory("after C3D_TexInit failure");
        return false;
    }

    size_t bufSize = tile->texW * tile->texH * 4;
    uint8_t* tileBuf = (uint8_t*) linearAlloc(bufSize);
    if (!tileBuf) {
        LOG_ERR("CRenderer3DS: linearAlloc(%lu KB) failed for swizzle buffer\n",
                (unsigned long)(bufSize / 1024));
        C3D_TexDelete(&tile->tex);
        logMemory("after swizzle linearAlloc failure");
        return false;
    }
    memset(tileBuf, 0, bufSize);

    linearToTile(tileBuf,
                 page->pixels,
                 tile->regionX, tile->regionY,
                 tile->regionW, tile->regionH,
                 page->atlasW,
                 tile->texW, tile->texH);

    memcpy(tile->tex.data, tileBuf, bufSize);
    GSPGPU_FlushDataCache(tile->tex.data, bufSize);
    linearFree(tileBuf);

    C3D_TexSetFilter(&tile->tex, GPU_LINEAR, GPU_NEAREST);
    C3D_TexSetWrap(&tile->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    tile->loaded = true;
    return true;
}

// Decode a page's PNG and upload ALL its tiles in one shot, then free pixels.
// This ensures decoded pixels are never held across draw calls, keeping peak
// memory usage to: (one page decoded) + (one tile swizzle buffer) at a time.
static bool uploadPageAllTiles(TexCachePage* page, uint32_t pageIdx) {
    if (!page->blobData || page->blobSize == 0) {
        LOG_ERR("CRenderer3DS: page %lu has no blob\n", (unsigned long)pageIdx);
        return false;
    }

    logMemory("before PNG decode");
    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode32(&page->pixels, &w, &h,
                                    page->blobData, page->blobSize);
    if (err) {
        LOG_ERR("CRenderer3DS: lodepng error %u on page %lu: %s\n",
                err, (unsigned long)pageIdx, lodepng_error_text(err));
        page->pixels = NULL;
        logMemory("after failed PNG decode");
        return false;
    }
    logMemory("after PNG decode");

    // Upload every tile while pixels are in RAM, then free immediately.
    uint32_t total = page->tilesX * page->tilesY;
    bool anyFailed = false;
    for (uint32_t t = 0; t < total; t++) {
        TexCacheTile* tile = &page->tiles[t];
        if (tile->loaded) continue;
        logMemory("before tile upload");
        if (!uploadOneTile(page, tile, pageIdx)) {
            LOG_ERR("CRenderer3DS: tile %lu of page %lu failed\n",
                    (unsigned long)t, (unsigned long)pageIdx);
            anyFailed = true;
            // Continue uploading other tiles even if one fails.
        }
        logMemory("after tile upload");
    }

    lodepng_free(page->pixels);
    page->pixels = NULL;
    logMemory("freed pixels after all tiles attempted");
    return !anyFailed;
}


// ===[ Page registration ]===
//
// Reads only the PNG IHDR (cheap: ~33 bytes) to get dimensions for the tile
// grid.  No decoding, no GPU work.

static bool registerPage(TexCachePage* page, Texture* tx, uint32_t pageIdx)
{
    if (!tx->blobData || tx->blobSize == 0) {
        LOG_ERR("CRenderer3DS: txtr[%lu] has no blob\n", (unsigned long)pageIdx);
        return false;
    }

    page->blobData = tx->blobData;
    page->blobSize = tx->blobSize;
    page->pixels   = NULL;

    unsigned w = 0, h = 0;
    if (lodepng_inspect(&w, &h, NULL, tx->blobData, tx->blobSize) != 0) {
        LOG_ERR("CRenderer3DS: lodepng_inspect failed on txtr[%lu]\n",
                (unsigned long)pageIdx);
        return false;
    }

    page->atlasW = (uint32_t)w;
    page->atlasH = (uint32_t)h;
    page->tilesX = (w + RENDERER_MAX_TEX_DIM - 1) / RENDERER_MAX_TEX_DIM;
    page->tilesY = (h + RENDERER_MAX_TEX_DIM - 1) / RENDERER_MAX_TEX_DIM;

    uint32_t totalTiles = page->tilesX * page->tilesY;
    page->tiles = (TexCacheTile*) safeCalloc(totalTiles, sizeof(TexCacheTile));

    for (uint32_t row = 0; row < page->tilesY; row++) {
        for (uint32_t col = 0; col < page->tilesX; col++) {
            TexCacheTile* tile = &page->tiles[row * page->tilesX + col];
            tile->regionX = col * RENDERER_MAX_TEX_DIM;
            tile->regionY = row * RENDERER_MAX_TEX_DIM;
            uint32_t re   = tile->regionX + RENDERER_MAX_TEX_DIM;
            uint32_t be   = tile->regionY + RENDERER_MAX_TEX_DIM;
            tile->regionW = (re <= w) ? RENDERER_MAX_TEX_DIM : (w - tile->regionX);
            tile->regionH = (be <= h) ? RENDERER_MAX_TEX_DIM : (h - tile->regionY);
        }
    }

    printf("CRenderer3DS: page %lu registered %ux%u -> %lux%lu tiles\n",
           (unsigned long)pageIdx, w, h,
           (unsigned long)page->tilesX, (unsigned long)page->tilesY);
    return true;
}

// ===[ Tiled draw ]===

static void drawTiledRegion(CRenderer3DS* C,
                             uint32_t pageIdx,
                             float srcX, float srcY,
                             float srcW, float srcH,
                             float dstX, float dstY,
                             float dstW, float dstH,
                             float angle, float alpha)
{
    if (pageIdx >= C->pageCacheCount) return;
    TexCachePage* page = &C->pageCache[pageIdx];
    if (!page->tiles) return;

    float pixScaleX = (srcW > 0.0f) ? (dstW / srcW) : 1.0f;
    float pixScaleY = (srcH > 0.0f) ? (dstH / srcH) : 1.0f;

    uint32_t colMin = (uint32_t)srcX / RENDERER_MAX_TEX_DIM;
    uint32_t rowMin = (uint32_t)srcY / RENDERER_MAX_TEX_DIM;
    uint32_t colMax = (srcW > 0.0f) ? ((uint32_t)(srcX + srcW - 1.0f) / RENDERER_MAX_TEX_DIM) : colMin;
    uint32_t rowMax = (srcH > 0.0f) ? ((uint32_t)(srcY + srcH - 1.0f) / RENDERER_MAX_TEX_DIM) : rowMin;

    if (colMax >= page->tilesX) colMax = page->tilesX - 1;
    if (rowMax >= page->tilesY) rowMax = page->tilesY - 1;

    // FIX: check if any tile in the needed region is unloaded, and if so,
    // decode the PNG once and upload ALL tiles for this page in one shot.
    // The old code decoded the full PNG individually for each unloaded tile,
    // causing O(n) redundant decodes for a page with n unloaded tiles.
    bool needsUpload = false;
    for (uint32_t row = rowMin; row <= rowMax && !needsUpload; row++) {
        for (uint32_t col = colMin; col <= colMax && !needsUpload; col++) {
            if (!page->tiles[row * page->tilesX + col].loaded)
                needsUpload = true;
        }
    }
    if (needsUpload) {
        uploadPageAllTiles(page, pageIdx);
    }

    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, alpha);

    for (uint32_t row = rowMin; row <= rowMax; row++) {
        for (uint32_t col = colMin; col <= colMax; col++) {
            TexCacheTile* tile = &page->tiles[row * page->tilesX + col];
            if (!tile->loaded) continue; // upload failed for this tile; skip

            float tileL = (float)tile->regionX;
            float tileT = (float)tile->regionY;
            float tileR = tileL + (float)tile->regionW;
            float tileB = tileT + (float)tile->regionH;

            float clipL = (srcX       > tileL) ? srcX       : tileL;
            float clipT = (srcY       > tileT) ? srcY       : tileT;
            float clipR = (srcX + srcW < tileR) ? srcX + srcW : tileR;
            float clipB = (srcY + srcH < tileB) ? srcY + srcH : tileB;

            if (clipR <= clipL || clipB <= clipT) continue;

            float u0 = (clipL - tileL) / (float)tile->texW;
            float u1 = (clipR - tileL) / (float)tile->texW;
            float v0 = (clipT - tileT) / (float)tile->texH;
            float v1 = (clipB - tileT) / (float)tile->texH;

            Tex3DS_SubTexture subtex = {
                .width  = (uint16_t)(clipR - clipL),
                .height = (uint16_t)(clipB - clipT),
                .left   = u0,  .right  = u1,
                .top    = 1.0f - v0, .bottom = 1.0f - v1,
            };
            C2D_Image image = { .tex = &tile->tex, .subtex = &subtex };

            C2D_DrawParams params = {
                .pos    = { dstX + (clipL - srcX) * pixScaleX,
                            dstY + (clipT - srcY) * pixScaleY,
                            (clipR - clipL) * pixScaleX,
                            (clipB - clipT) * pixScaleY },
                .center = { 0.0f, 0.0f },
                .depth  = C->zCounter,
                .angle  = angle,
            };
            C2D_DrawImage(image, &params, &tint);
            C->zCounter += 0.0001f;
        }
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
    C->pageCache      = (TexCachePage*) safeCalloc(pageCount, sizeof(TexCachePage));
    C->pageCacheCount = pageCount;

    verifyLodepngAllocator();
    logMemory("before page registration");
    for (uint32_t i = 0; i < pageCount; i++)
        registerPage(&C->pageCache[i], &dataWin->txtr.textures[i], i);
    logMemory("after page registration");

    C->top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    printf("CRenderer3DS: initialized (%lu pages, fully lazy)\n",
           (unsigned long)pageCount);
    logMemory("renderer ready");
}

static void CDestroy(Renderer* renderer) {
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    for (uint32_t i = 0; i < C->pageCacheCount; i++) {
        TexCachePage* page = &C->pageCache[i];
        if (page->pixels) { lodepng_free(page->pixels); page->pixels = NULL; }
        if (!page->tiles) continue;
        uint32_t total = page->tilesX * page->tilesY;
        for (uint32_t t = 0; t < total; t++)
            if (page->tiles[t].loaded) C3D_TexDelete(&page->tiles[t].tex);
        free(page->tiles);
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

static void CEndFrame(Renderer* renderer) { C3D_FrameEnd(0); }
static void CEndView(Renderer* renderer)  { /* no-op */ }

static void CDrawSprite(Renderer* renderer, int32_t tpagIndex,
    float x, float y, float originX, float originY,
    float xscale, float yscale, float angleDeg, uint32_t color, float alpha)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    DataWin* dw = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    uint32_t pageIdx = (uint32_t)tpag->texturePageId;

    // FIX: removed unused gameX/gameY variables that were computed but never read.
    float dstX = (x + ((float)tpag->targetX - originX) * xscale - (float)C->viewX) * C->scaleX + C->offsetX;
    float dstY = (y + ((float)tpag->targetY - originY) * yscale - (float)C->viewY) * C->scaleY + C->offsetY;
    float dstW = (float)tpag->sourceWidth  * xscale * C->scaleX;
    float dstH = (float)tpag->sourceHeight * yscale * C->scaleY;

    if (pageIdx < C->pageCacheCount && C->pageCache[pageIdx].tiles) {
        drawTiledRegion(C, pageIdx,
                        (float)tpag->sourceX, (float)tpag->sourceY,
                        (float)tpag->sourceWidth, (float)tpag->sourceHeight,
                        dstX, dstY, dstW, dstH,
                        angleDeg * (float)(M_PI / 180.0), alpha);
        return;
    }
    else
    {
        LOG_ERR("CRenderer3DS: texture page %lu not loaded, drawing placeholder\n",
                 (unsigned long)pageIdx);
        uint8_t a = (uint8_t)(alpha * 255.0f);
        C2D_DrawRectSolid(dstX, dstY, C->zCounter, dstW, dstH,
                C2D_Color32(BGR_R(color), BGR_G(color), BGR_B(color), a));
    }
    C->zCounter += 0.001f;
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

    if (pageIdx < C->pageCacheCount && C->pageCache[pageIdx].tiles) {
        drawTiledRegion(C, pageIdx,
                        (float)(tpag->sourceX + srcOffX),
                        (float)(tpag->sourceY + srcOffY),
                        (float)srcW, (float)srcH,
                        dstX, dstY, dstW, dstH, 0.0f, alpha);
        return;
    }
    else
    {
        LOG_ERR("CRenderer3DS: texture page %lu not loaded, drawing placeholder\n",
                 (unsigned long)pageIdx);
        uint8_t a = (uint8_t)(alpha * 255.0f);
        C2D_DrawRectSolid(dstX, dstY, C->zCounter, dstW, dstH,
                C2D_Color32(BGR_R(color), BGR_G(color), BGR_B(color), a));
    }
    C->zCounter += 0.0001f;
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

    // FIX: outline was previously ignored; always filled regardless of the flag.
    if (outline) {
        float pw = C->scaleX; // one game-pixel wide in screen space
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
                 (y1 - C->viewY) * C->scaleY + C->offsetY, C2D_Color32(r,g,b,a),
                 (x2 - C->viewX) * C->scaleX + C->offsetX,
                 (y2 - C->viewY) * C->scaleY + C->offsetY, C2D_Color32(r,g,b,a),
                 width, C->zCounter);
    C->zCounter += 0.0001f;
}

// FIX: added missing drawLineColor — PS2 renderer has it in the vtable;
// omitting it leaves a NULL slot that crashes on any drawLineColor call.
// citro2d C2D_DrawLine supports per-endpoint colors natively, so wire both up.
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
    .drawLineColor           = CDrawLineColor, // FIX: was missing, leaving a NULL vtable slot
    .drawText                = CDrawText,
    .flush                   = CFlush,
    .createSpriteFromSurface = CCreateSpriteFromSurface,
    .deleteSprite            = CDeleteSprite,
};

Renderer* CRenderer3DS_create(void) {
    CRenderer3DS* C = safeCalloc(1, sizeof(CRenderer3DS));
    C->base.vtable = &CVtable;
    C->scaleX   = 2.0f;
    C->scaleY   = 2.0f;
    C->zCounter = 0.5f;
    return (Renderer*) C;
}