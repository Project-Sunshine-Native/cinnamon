#include "n3ds_renderer.h"
#include "n3ds_platform_config.h"

#include "../runner.h"
#include "../text_utils.h"
#include "../utils.h"

#include <3ds.h>
#include <citro2d.h>
#include <tex3ds.h>
#include <stb/ds/stb_ds.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define N3DS_ENABLE_LOGGING 0
#define N3DS_ENABLE_DIRECT_ASSETS 1

#if !N3DS_ENABLE_LOGGING
#define fprintf(...) ((int) 0)
#endif

#define N3DS_ATLAS_MAGIC 0x5441334Eu /* N3AT */
#define N3DS_ATLAS_VERSION_RAW 1u
#define N3DS_ATLAS_VERSION_T3X 2u
#define N3DS_ATLAS_VERSION_FRAGMENTED 3u
#define N3DS_ATLAS_VERSION_FRAGMENTED_TILES 4u
#define N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS 5u
#define N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT 6u
#define N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT 7u
#define N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED 8u
#define N3DS_DIRECT_ASSET_MAGIC 0x3152444Eu /* NDR1 */
#define N3DS_DIRECT_ASSET_VERSION 1u
#define N3DS_ROOM_MANIFEST_MAGIC 0x4D52334Eu /* N3RM */
#define N3DS_ROOM_MANIFEST_VERSION 1u
#define N3DS_DIRECT_ASSET_ENTRY_SIZE 16u
#define N3DS_TOP_WIDTH 400
#define N3DS_TOP_HEIGHT 240
#define N3DS_BOTTOM_WIDTH 320
#define N3DS_BOTTOM_HEIGHT 240
#define N3DS_SDMC_ASSET_BASE "sdmc:/3ds/cinnamon/gfx"
#define N3DS_ROMFS_ASSET_BASE "romfs:/gfx"
#define N3DS_RENDERER_FILE_BUFFER_SIZE (32u * 1024u)
#define N3DS_TILE_LAYER_CHUNK_SIZE 256u
#define N3DS_TILE_LAYER_CHUNK_TEXTURE_BYTES (N3DS_TILE_LAYER_CHUNK_SIZE * N3DS_TILE_LAYER_CHUNK_SIZE * 2u)
#define N3DS_TILE_LAYER_CHUNK_VRAM_BUDGET (512u * 1024u)
#define N3DS_MAX_RESIDENT_ATLAS_PAGES_OLD3DS 16u
#define N3DS_MAX_RESIDENT_ATLAS_PAGES_NEW3DS 32u
#define N3DS_MAX_RESIDENT_ATLAS_PAGES_OLD3DS_ETC1A4 40u
#define N3DS_MAX_RESIDENT_ATLAS_PAGES_NEW3DS_ETC1A4 80u
#define N3DS_PREWARM_GPU_PAGE_BUDGET_OLD3DS 12u
#define N3DS_PREWARM_GPU_PAGE_BUDGET_NEW3DS 32u
#define N3DS_PREWARM_GPU_PAGE_BUDGET_OLD3DS_ETC1A4 20u
#define N3DS_PREWARM_GPU_PAGE_BUDGET_NEW3DS_ETC1A4 48u
#define N3DS_PREWARM_BLOB_BYTES_OLD3DS (8u * 1024u * 1024u)
#define N3DS_PREWARM_BLOB_BYTES_NEW3DS (12u * 1024u * 1024u)
#define N3DS_RESIDENT_ATLAS_VRAM_BUDGET_OLD3DS (2560u * 1024u)
#define N3DS_RESIDENT_ATLAS_VRAM_BUDGET_NEW3DS (5120u * 1024u)
#define N3DS_MAX_CACHED_T3X_BYTES_OLD3DS (16u * 1024u * 1024u)
#define N3DS_MAX_CACHED_T3X_BYTES_NEW3DS (24u * 1024u * 1024u)
#define N3DS_MAX_CACHED_DIRECT_T3X_BYTES_OLD3DS (4u * 1024u * 1024u)
#define N3DS_MAX_CACHED_DIRECT_T3X_BYTES_NEW3DS (8u * 1024u * 1024u)
#define N3DS_DIRECT_ASSET_VRAM_BUDGET_OLD3DS (1024u * 1024u)
#define N3DS_DIRECT_ASSET_VRAM_BUDGET_NEW3DS (3072u * 1024u)
// battle screen offset
#define N3DS_TOP_BATTLE_SCENE_Y_OFFSET 200.0f
#define N3DS_TOP_BATTLE_ENEMY_Y_OFFSET 200.0f
#define N3DS_C2D_FLUSH_DRAW_BUDGET 192u
#define N3DS_PERF_LOG_INTERVAL_FRAMES 30u
#define N3DS_ATLAS_TRACE_LOG_PATH "sdmc:/3ds/cinnamon/atlas_trace.log"

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t textureFormat;
    uint32_t dataOffset;
    uint32_t dataSize;
} N3DSAtlasPageInfo;

typedef enum {
    N3DS_TEXFMT_RGBA5551 = 0,
    N3DS_TEXFMT_ETC1A4 = 1,
    N3DS_TEXFMT_INDEXED8 = 2,
    N3DS_TEXFMT_HYBRID = 3,
    N3DS_TEXFMT_L4 = 4,
    N3DS_TEXFMT_LA4 = 5,
} N3DSTextureFormat;

typedef struct {
    uint16_t atlasId;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} N3DSAtlasItem;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t fragmentStart;
    uint16_t fragmentCount;
} N3DSAtlasItemV3;

typedef struct {
    uint16_t atlasId;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint16_t sourceX;
    uint16_t sourceY;
} N3DSAtlasFragment;

typedef struct {
    int16_t bgDef;
    uint16_t srcX;
    uint16_t srcY;
    uint16_t srcW;
    uint16_t srcH;
    uint16_t atlasId;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t fragmentStart;
    uint16_t fragmentCount;
} N3DSTileAtlasEntry;

typedef struct {
    int16_t bgDef;
    uint16_t srcX;
    uint16_t srcY;
    uint16_t srcW;
    uint16_t srcH;
} N3DSTileLookupKey;

typedef struct {
    N3DSTileLookupKey key;
    uint32_t value;
} N3DSTileEntryMap;

typedef struct {
    C2D_SpriteSheet sheet;
    C2D_Image image;
    uint8_t* blobData;
    uint32_t blobSize;
    uint32_t blobLastUsedStamp;
    bool ready;
    bool failed;
    bool pinned;
    uint32_t vramBytes;
    uint32_t lastUsedStamp;
    uint32_t lastUsedFrame;
} N3DSDirectTextureAsset;

typedef struct {
    uint32_t frameCount;
    uint32_t sheetFrameCount;
    bool sheetLoadAttempted;
    bool useFrameFallback;
    C2D_Image* sheetFrameImages;
    N3DSDirectTextureAsset sheetAsset;
    N3DSDirectTextureAsset* frameAssets;
} N3DSDirectSpriteAsset;

typedef struct {
    bool allocated;
    int32_t x;
    int32_t y;
    uint16_t width;
    uint16_t height;
    C3D_Tex texture;
    C3D_RenderTarget* target;
    Tex3DS_SubTexture subtex;
    C2D_Image image;
} N3DSTileLayerChunk;

typedef struct {
    bool used;
    uint32_t chunkCount;
    uint32_t vramBytes;
    N3DSTileLayerChunk* chunks;
} N3DSTileLayerChunkCache;

typedef struct {
    C3D_Tex texture;
    Tex3DS_Texture t3x;
    uint8_t* t3xData;
    uint32_t t3xSize;
    uint32_t blobLastUsedStamp;
    bool ready;
    bool pinned;
    uint16_t width;
    uint16_t height;
    uint32_t textureFormat;
    uint32_t dataOffset;
    uint32_t dataSize;
    uint32_t lastUsedStamp;
    uint32_t lastUsedFrame;
} N3DSLoadedAtlasPage;

typedef struct {
    bool used;
    int32_t ownerSpriteIndex;
    uint16_t logicalWidth;
    uint16_t logicalHeight;
    C3D_Tex texture;
    Tex3DS_SubTexture subtex;
    C2D_Image image;
} N3DSDynamicCaptureTPAG;

typedef struct {
    float localX;
    float localY;
    uint16_t sourceX;
    uint16_t sourceY;
    uint16_t sourceWidth;
    uint16_t sourceHeight;
} N3DSCachedTextGlyph;

typedef struct {
    char* text;
    int32_t textCapacity;
    int32_t fontIndex;
    int32_t drawHalign;
    int32_t drawValign;
    int32_t glyphCount;
    int32_t glyphCapacity;
    N3DSCachedTextGlyph* glyphs;
    float appendCursorX;
    float appendCursorY;
    uint16_t appendPrevCodepoint;
    bool appendAtLineStart;
} N3DSCachedTextLayout;

typedef struct {
    char* key;
    char* value;
} N3DSResolvedAssetPathEntry;

typedef struct {
    uint32_t dataOffset;
    uint32_t dataSize;
} N3DSPackedDirectAssetEntry;

typedef struct {
    char* key;
    N3DSPackedDirectAssetEntry value;
} N3DSPackedDirectAssetMapEntry;

typedef struct {
    uint32_t roomIndex;
    uint32_t pageStart;
    uint32_t pageCount;
    uint32_t directSpriteStart;
    uint32_t directSpriteCount;
    uint32_t directBackgroundStart;
    uint32_t directBackgroundCount;
} N3DSRoomManifestEntry;

typedef struct {
    uint16_t pageIndex;
    uint8_t flags;
    uint8_t reserved;
} N3DSRoomManifestPageRef;

typedef struct {
    Renderer base;
    C3D_RenderTarget* topTarget;
    C3D_RenderTarget* bottomTarget;
    N3DSLoadedAtlasPage* atlasPages;
    N3DSAtlasItem* atlasItems;
    N3DSAtlasItemV3* atlasItemsV3;
    N3DSAtlasFragment* atlasFragments;
    N3DSTileAtlasEntry* tileEntries;
    N3DSTileEntryMap* tileEntryMap;
    N3DSTileLayerChunkCache* tileLayerChunkCaches;
    N3DSDirectSpriteAsset* directSpriteAssets;
    N3DSDirectTextureAsset* directBackgroundAssets;
    N3DSDirectTextureAsset* directFontAssets;
    int32_t* tpagToSpriteIndex;
    int32_t* tpagToSpriteFrameIndex;
    int32_t* tpagToBackgroundIndex;
    uint32_t atlasPageCount;
    uint32_t atlasItemCount;
    uint32_t atlasFragmentCount;
    uint32_t tileEntryCount;
    uint32_t directSpriteAssetCount;
    uint32_t directBackgroundAssetCount;
    uint32_t directFontAssetCount;
    uint16_t atlasVersion;
    uint32_t atlasTextureFormat;
    float frameScaleX;
    float frameScaleY;
    float frameOffsetX;
    float frameOffsetY;
    float portOffsetX;
    float portOffsetY;
    int32_t viewX;
    int32_t viewY;
    float viewScaleX;
    float viewScaleY;
    bool blendEnabled;
    int32_t blendEquation;
    int32_t blendSrcFactor;
    int32_t blendDstFactor;
    bool alphaTestEnabled;
    uint8_t alphaTestRef;
    uint32_t clearColor;
    float clearAlpha;
    uint32_t atlasUseCounter;
    uint32_t blobUseCounter;
    uint32_t residentAtlasPageCount;
    uint32_t residentAtlasVRAMBytes;
    uint32_t residentAtlasPageLimit;
    uint32_t residentAtlasVRAMLimitBytes;
    uint32_t directAssetUseCounter;
    uint32_t directBlobUseCounter;
    uint32_t residentDirectAssetVRAMBytes;
    uint32_t residentDirectAssetVRAMLimitBytes;
    uint32_t prewarmGpuPageBudget;
    uint32_t cachedT3xByteLimit;
    uint32_t cachedT3xBytes;
    uint32_t cachedDirectT3xByteLimit;
    uint32_t cachedDirectT3xBytes;
    uint32_t tileLayerChunkVRAMBytes;
    uint32_t frameBlobReads;
    uint32_t frameDirectBlobReads;
    uint32_t framePageImports;
    uint32_t frameImportFailures;
    uint32_t frameImportEvictions;
    uint32_t frameFragmentDraws;
    uint32_t frameSpriteDrawCalls;
    uint32_t frameSpritePartDrawCalls;
    uint32_t frameDirectSpriteHits;
    uint32_t frameDirectAssetLoads;
    uint32_t frameTextureSwitches;
    uint32_t frameTextGlyphDraws;
    uint32_t frameTextTenthsMs;
    uint32_t pendingC2DDraws;
    bool atlasLoaded;
    char startupError[160];
    int32_t lastDirectTPAGIndex;
    C2D_Image* lastDirectTPAGImage;
    uint32_t perfWindowFrames;
    uint32_t perfWindowBlobReads;
    uint32_t perfWindowPageImports;
    uint32_t perfWindowImportFailures;
    uint32_t perfWindowImportEvictions;
    uint32_t perfWindowFragmentDraws;
    uint32_t perfWindowSpriteDrawCalls;
    uint32_t perfWindowSpritePartDrawCalls;
    uint32_t frameSequence;
    const Room* lastPrewarmedRoom;
    N3DSCachedTextLayout cachedTextLayout;
    N3DSResolvedAssetPathEntry* resolvedAssetPathCache;
    N3DSPackedDirectAssetMapEntry* packedDirectAssetMap;
    N3DSRoomManifestEntry* roomManifestEntries;
    N3DSRoomManifestPageRef* roomManifestPageRefs;
    N3DSDynamicCaptureTPAG* dynamicCaptureTPAGs;
    uint8_t* atlasTraceMask;
    FILE* atlasTraceFile;
    FILE* packedAtlasFile;
    FILE* packedDirectAssetFile;
    uint32_t roomManifestEntryCount;
    uint32_t roomManifestPageRefCount;
    uint32_t roomManifestRoomCount;
    uint32_t baseTPAGCount;
    uint32_t dynamicCaptureTPAGCount;
    bool bottomScreenGuiActive;
    bool topScreenGuiActive;
    bool topScreenGui2xActive;
    bool topScreenBattleViewActive;
    bool textLinearFilterActive;
    uint8_t activeSceneTarget;
    const C3D_Tex* lastDrawTexture;
    float savedFrameScaleX;
    float savedFrameScaleY;
    float savedFrameOffsetX;
    float savedFrameOffsetY;
    float savedPortOffsetX;
    float savedPortOffsetY;
    int32_t savedViewX;
    int32_t savedViewY;
    float savedViewScaleX;
    float savedViewScaleY;
    bool isNew3DS;
    bool pendingOld3DSAtlasFlush;
} N3DSRenderer;

enum {
    N3DS_SCENE_TARGET_NONE = 0,
    N3DS_SCENE_TARGET_TOP = 1,
    N3DS_SCENE_TARGET_BOTTOM = 2,
};

static void N3DSRenderer_drawSprite(Renderer* base, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha);
static void N3DSRenderer_drawSpritePart(Renderer* base, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha);
static void N3DSRenderer_drawTile(Renderer* base, RoomTile* tile, float offsetX, float offsetY);
static void N3DSRenderer_drawTiled(Renderer* base, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha);
static void N3DSRenderer_drawTiledPart(Renderer* base, int32_t tpagIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, float dstX, float dstY, float dstW, float dstH, uint32_t color, float alpha);
static bool N3DSRenderer_getPageIndexForTPAG(N3DSRenderer* renderer, int32_t tpagIndex, uint32_t* outPageIndex);
static N3DSTileAtlasEntry* N3DSRenderer_findTileEntryByKey(N3DSRenderer* renderer, int32_t backgroundIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, uint32_t* outEntryIndex);
static N3DSTileAtlasEntry* N3DSRenderer_findTileEntry(N3DSRenderer* renderer, RoomTile* tile);
static void N3DSRenderer_prewarmPage(N3DSRenderer* renderer, bool* seenPages, uint32_t pageIndex);
static bool N3DSRenderer_prewarmPageBlobOnly(N3DSRenderer* renderer, bool* seenPages, uint32_t pageIndex, uint32_t* remainingBlobBytes);
static void N3DSRenderer_prewarmTPAGBlobOnly(N3DSRenderer* renderer, bool* seenPages, int32_t tpagIndex, uint32_t* remainingBlobBytes);
static void N3DSRenderer_prewarmTileEntryBlobOnly(N3DSRenderer* renderer, bool* seenPages, const N3DSTileAtlasEntry* entry, uint32_t* remainingBlobBytes);
static void N3DSRenderer_prewarmRoomBlobCache(N3DSRenderer* renderer, Runner* runner);
static void N3DSRenderer_preloadFontPages(N3DSRenderer* renderer);
static bool N3DSRenderer_ensurePageBlobLoaded(N3DSRenderer* renderer, uint32_t pageIndex);
static void N3DSRenderer_prewarmRoom(Renderer* base, Runner* runner);
static void N3DSRenderer_freeCachedTextLayout(N3DSCachedTextLayout* layout);
static void N3DSRenderer_appendCachedTextLayoutSuffix(N3DSCachedTextLayout* layout, Font* font, const char* suffix, int32_t suffixLen);
static const N3DSCachedTextLayout* N3DSRenderer_getCachedTextLayout(N3DSRenderer* renderer, Font* font, int32_t fontIndex, const char* text);
static bool N3DSRenderer_tryDrawSingleGlyphTextFast(
    Renderer* base,
    N3DSRenderer* renderer,
    Font* font,
    const char* text,
    int32_t len,
    float x,
    float y,
    float effectiveXScale,
    float effectiveYScale,
    float angleDeg,
    bool gradient,
    int32_t c1,
    float alpha,
    bool useDirectFontAsset,
    const C2D_Image* directFontImage,
    const Tex3DS_SubTexture* directFontBaseSubtex,
    const N3DSAtlasFragment* fontFragment,
    N3DSLoadedAtlasPage* fontPage
);
static bool N3DSRenderer_drawPackedTileEntry(Renderer* base, N3DSRenderer* renderer, const N3DSTileAtlasEntry* tileEntry, float drawX, float drawY, float xscale, float yscale, uint32_t color, float alpha);
static bool N3DSRenderer_isFragmentedAtlasVersion(uint16_t atlasVersion);
static bool N3DSRenderer_isPackedAtlasVersion(uint16_t atlasVersion);
static const char* N3DSRenderer_getTextureFormatName(uint32_t textureFormat);
static void N3DSRenderer_sceneBeginTarget(N3DSRenderer* renderer, uint8_t targetKind, bool force);
static void N3DSRenderer_freeTileLayerChunkCache(N3DSRenderer* renderer, N3DSTileLayerChunkCache* cache);
static void N3DSRenderer_freeDirectTextureAsset(N3DSDirectTextureAsset* asset, N3DSRenderer* renderer);
static void N3DSRenderer_unloadDirectTextureBlob(N3DSDirectTextureAsset* asset, N3DSRenderer* renderer);
static bool N3DSRenderer_evictLRUDirectAsset(N3DSRenderer* renderer, const N3DSDirectTextureAsset* excludeAsset);
static bool N3DSRenderer_evictLRUDirectAssetBlob(N3DSRenderer* renderer, const N3DSDirectTextureAsset* excludeAsset);
static void N3DSRenderer_buildDirectAssetMaps(N3DSRenderer* renderer);
static bool N3DSRenderer_tryDrawDirectMappedSprite(Renderer* base, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha);
static bool N3DSRenderer_tryResolveDirectFontImage(N3DSRenderer* renderer, int32_t fontIndex, C2D_Image* outImage, Tex3DS_SubTexture* outSubtex);
static void N3DSRenderer_drawImage(Renderer* base, C2D_Image* image, float localX, float localY, float width, float height, float pivotX, float pivotY, float angleDeg, uint32_t color, float alpha);
static void N3DSRenderer_getActiveTargetSize(const N3DSRenderer* renderer, float* outW, float* outH);
static bool N3DSRenderer_isScreenRectOffscreen(const N3DSRenderer* renderer, float x, float y, float w, float h);
static bool N3DSRenderer_isScreenRotatedRectOffscreen(const N3DSRenderer* renderer, float x, float y, float w, float h, float angleDeg);
static bool N3DSRenderer_loadPackedDirectAssets(N3DSRenderer* renderer);
static bool N3DSRenderer_loadRoomManifest(N3DSRenderer* renderer);
static const N3DSRoomManifestEntry* N3DSRenderer_findRoomManifestEntry(const N3DSRenderer* renderer, uint32_t roomIndex);
static void N3DSRenderer_prewarmRoomManifestBlobCache(N3DSRenderer* renderer, uint32_t roomIndex, bool* seenPages, uint32_t* remainingBlobBytes);
static bool N3DSRenderer_tryLoadPackedDirectTextureBlob(N3DSRenderer* renderer, N3DSDirectTextureAsset* asset, const char* relativePath);
static N3DSDynamicCaptureTPAG* N3DSRenderer_getDynamicCaptureTPAG(N3DSRenderer* renderer, int32_t tpagIndex);
static void N3DSRenderer_freeDynamicCaptureTPAG(N3DSRenderer* renderer, N3DSDynamicCaptureTPAG* capture);
static int32_t N3DSRenderer_allocDynamicCaptureTPAG(N3DSRenderer* renderer);

enum {
    N3DS_TRACE_KIND_SPRITE = 1u << 0,
    N3DS_TRACE_KIND_SPRITE_PART = 1u << 1,
    N3DS_TRACE_KIND_FONT = 1u << 2,
};

static const char* N3DSRenderer_getTraceKindName(uint8_t traceKind) {
    switch (traceKind) {
        case N3DS_TRACE_KIND_SPRITE: return "sprite";
        case N3DS_TRACE_KIND_SPRITE_PART: return "sprite_part";
        case N3DS_TRACE_KIND_FONT: return "font";
        default: return "unknown";
    }
}

static void N3DSRenderer_traceTPAGUsage(N3DSRenderer* renderer, uint8_t traceKind, int32_t tpagIndex) {
#if !N3DS_ENABLE_LOGGING
    (void) renderer;
    (void) traceKind;
    (void) tpagIndex;
    return;
#else
    if (renderer == NULL || tpagIndex < 0 || (uint32_t) tpagIndex >= renderer->atlasItemCount) return;
    if (renderer->atlasTraceMask == NULL) return;

    uint8_t* traceMask = &renderer->atlasTraceMask[tpagIndex];
    if ((*traceMask & traceKind) != 0u) return;
    *traceMask |= traceKind;

    const char* kindName = N3DSRenderer_getTraceKindName(traceKind);
    if (!N3DSRenderer_isFragmentedAtlasVersion(renderer->atlasVersion)) {
        N3DSAtlasItem* item = &renderer->atlasItems[tpagIndex];
        fprintf(
            stderr,
            "N3DS TRACE: kind=%s tpag=%d page=%u format=%s\n",
            kindName,
            (int) tpagIndex,
            (unsigned int) item->atlasId,
            (item->atlasId < renderer->atlasPageCount) ? N3DSRenderer_getTextureFormatName(renderer->atlasPages[item->atlasId].textureFormat) : "unknown"
        );
        if (renderer->atlasTraceFile != NULL) {
            fprintf(
                renderer->atlasTraceFile,
                "kind=%s tpag=%d page=%u format=%s\n",
                kindName,
                (int) tpagIndex,
                (unsigned int) item->atlasId,
                (item->atlasId < renderer->atlasPageCount) ? N3DSRenderer_getTextureFormatName(renderer->atlasPages[item->atlasId].textureFormat) : "unknown"
            );
            fflush(renderer->atlasTraceFile);
        }
        return;
    }

    N3DSAtlasItemV3* item = &renderer->atlasItemsV3[tpagIndex];
    char pagesBuf[256];
    size_t cursor = 0;
    pagesBuf[0] = '\0';
    repeat(item->fragmentCount, i) {
        uint32_t fragmentIndex = item->fragmentStart + (uint32_t) i;
        if (fragmentIndex >= renderer->atlasFragmentCount) break;
        uint16_t pageIndex = renderer->atlasFragments[fragmentIndex].atlasId;
        bool alreadyListed = false;
        repeat(i, prevIndex) {
            uint32_t prevFragmentIndex = item->fragmentStart + (uint32_t) prevIndex;
            if (prevFragmentIndex >= renderer->atlasFragmentCount) break;
            if (renderer->atlasFragments[prevFragmentIndex].atlasId == pageIndex) {
                alreadyListed = true;
                break;
            }
        }
        if (alreadyListed) continue;
        int written = snprintf(
            pagesBuf + cursor,
            sizeof(pagesBuf) - cursor,
            "%s%u",
            cursor == 0 ? "" : ",",
            (unsigned int) pageIndex
        );
        if (written <= 0 || (size_t) written >= sizeof(pagesBuf) - cursor) break;
        cursor += (size_t) written;
    }

    fprintf(
        stderr,
        "N3DS TRACE: kind=%s tpag=%d fragments=%u pages=%s\n",
        kindName,
        (int) tpagIndex,
        (unsigned int) item->fragmentCount,
        pagesBuf[0] != '\0' ? pagesBuf : "none"
    );
    if (renderer->atlasTraceFile != NULL) {
        fprintf(
            renderer->atlasTraceFile,
            "kind=%s tpag=%d fragments=%u pages=%s\n",
            kindName,
            (int) tpagIndex,
            (unsigned int) item->fragmentCount,
            pagesBuf[0] != '\0' ? pagesBuf : "none"
        );
        fflush(renderer->atlasTraceFile);
    }
#endif
}

static void N3DSRenderer_computeFrameLayoutForTarget(N3DSRenderer* renderer, int32_t gameW, int32_t gameH, int32_t targetW, int32_t targetH) {
    if (gameW <= 0) gameW = targetW;
    if (gameH <= 0) gameH = targetH;

    float sx = (float) targetW / (float) gameW;
    float sy = (float) targetH / (float) gameH;
    float scale = sx < sy ? sx : sy;
    renderer->frameScaleX = scale;
    renderer->frameScaleY = scale;
    renderer->frameOffsetX = ((float) targetW - ((float) gameW * scale)) * 0.5f;
    renderer->frameOffsetY = ((float) targetH - ((float) gameH * scale)) * 0.5f;
}

static void N3DSRenderer_setTopBattle320x240Layout(N3DSRenderer* renderer, int32_t guiW, int32_t guiH, float yOffset) {
    if (renderer == NULL) return;
    if (guiW <= 0) guiW = 640;
    if (guiH <= 0) guiH = 480;

    renderer->viewX = 0;
    renderer->viewY = 0;
    renderer->frameScaleX = (float) N3DS_BOTTOM_WIDTH / (float) guiW;
    renderer->frameScaleY = (float) N3DS_BOTTOM_HEIGHT / (float) guiH;
    renderer->viewScaleX = renderer->frameScaleX;
    renderer->viewScaleY = renderer->frameScaleY;
    renderer->portOffsetX = 0.0f;
    renderer->portOffsetY = 0.0f;
    renderer->frameOffsetX = floorf((((float) N3DS_TOP_WIDTH - (float) N3DS_BOTTOM_WIDTH) * 0.5f) + 0.5f);
    renderer->frameOffsetY = floorf(yOffset + 0.5f);
}

static bool N3DSRenderer_isFragmentedAtlasVersion(uint16_t atlasVersion) {
    return atlasVersion == N3DS_ATLAS_VERSION_FRAGMENTED ||
        atlasVersion == N3DS_ATLAS_VERSION_FRAGMENTED_TILES ||
        atlasVersion == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS ||
        atlasVersion == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT ||
        atlasVersion == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT ||
        atlasVersion == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED;
}

static bool N3DSRenderer_isPackedAtlasVersion(uint16_t atlasVersion) {
    return atlasVersion == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED;
}

static double N3DSRenderer_ticksToMs(u64 ticks) {
    return (double) ticks * 1000.0 / (double) SYSCLOCK_ARM11;
}

static void N3DSRenderer_resetFramePerfCounters(N3DSRenderer* renderer) {
    renderer->frameBlobReads = 0;
    renderer->framePageImports = 0;
    renderer->frameImportFailures = 0;
    renderer->frameImportEvictions = 0;
    renderer->frameFragmentDraws = 0;
    renderer->frameSpriteDrawCalls = 0;
    renderer->frameSpritePartDrawCalls = 0;
    renderer->frameDirectSpriteHits = 0;
    renderer->frameDirectAssetLoads = 0;
    renderer->frameTextureSwitches = 0;
    renderer->frameTextGlyphDraws = 0;
    renderer->frameTextTenthsMs = 0;
    renderer->pendingC2DDraws = 0;
    renderer->lastDrawTexture = NULL;
    renderer->lastDirectTPAGIndex = -1;
    renderer->lastDirectTPAGImage = NULL;
}

static void N3DSRenderer_logPerfWindowIfNeeded(N3DSRenderer* renderer) {
#ifndef N3DS_ENABLE_PERF_LOGS
    (void) renderer;
    return;
#else
    renderer->perfWindowFrames++;
    renderer->perfWindowBlobReads += renderer->frameBlobReads;
    renderer->perfWindowPageImports += renderer->framePageImports;
    renderer->perfWindowImportFailures += renderer->frameImportFailures;
    renderer->perfWindowImportEvictions += renderer->frameImportEvictions;
    renderer->perfWindowFragmentDraws += renderer->frameFragmentDraws;
    renderer->perfWindowSpriteDrawCalls += renderer->frameSpriteDrawCalls;
    renderer->perfWindowSpritePartDrawCalls += renderer->frameSpritePartDrawCalls;

    if (renderer->perfWindowFrames < N3DS_PERF_LOG_INTERVAL_FRAMES) return;

    fprintf(
        stderr,
        "N3DS perf/%lu: blobReads=%lu pageImports=%lu importFail=%lu evictions=%lu fragDraws=%lu spriteCalls=%lu partCalls=%lu resident=%lu blobKB=%lu\n",
        (unsigned long) renderer->perfWindowFrames,
        (unsigned long) renderer->perfWindowBlobReads,
        (unsigned long) renderer->perfWindowPageImports,
        (unsigned long) renderer->perfWindowImportFailures,
        (unsigned long) renderer->perfWindowImportEvictions,
        (unsigned long) renderer->perfWindowFragmentDraws,
        (unsigned long) renderer->perfWindowSpriteDrawCalls,
        (unsigned long) renderer->perfWindowSpritePartDrawCalls,
        (unsigned long) renderer->residentAtlasPageCount,
        (unsigned long) (renderer->cachedT3xBytes / 1024u)
    );

    renderer->perfWindowFrames = 0;
    renderer->perfWindowBlobReads = 0;
    renderer->perfWindowPageImports = 0;
    renderer->perfWindowImportFailures = 0;
    renderer->perfWindowImportEvictions = 0;
    renderer->perfWindowFragmentDraws = 0;
    renderer->perfWindowSpriteDrawCalls = 0;
    renderer->perfWindowSpritePartDrawCalls = 0;
#endif
}

static uint16_t N3DS_readU16(const uint8_t* ptr) {
    return (uint16_t) (ptr[0] | (ptr[1] << 8));
}

static uint32_t N3DS_readU32(const uint8_t* ptr) {
    return (uint32_t) ptr[0] |
        ((uint32_t) ptr[1] << 8) |
        ((uint32_t) ptr[2] << 16) |
        ((uint32_t) ptr[3] << 24);
}

static uint32_t N3DSRenderer_getTextureFormatBytesPerPixel(uint32_t textureFormat) {
    switch (textureFormat) {
        case N3DS_TEXFMT_ETC1A4: return 1u;
        case N3DS_TEXFMT_INDEXED8: return 2u;
        case N3DS_TEXFMT_HYBRID: return 1u;
        case N3DS_TEXFMT_LA4: return 1u;
        case N3DS_TEXFMT_L4: return 0u;
        case N3DS_TEXFMT_RGBA5551:
        default: return 2u;
    }
}

static GPU_TEXCOLOR N3DSRenderer_getGPUTextureFormat(uint32_t textureFormat) {
    switch (textureFormat) {
        case N3DS_TEXFMT_ETC1A4: return GPU_ETC1A4;
        case N3DS_TEXFMT_INDEXED8: return GPU_RGBA5551;
        case N3DS_TEXFMT_HYBRID: return GPU_ETC1A4;
        case N3DS_TEXFMT_L4: return GPU_L4;
        case N3DS_TEXFMT_LA4: return GPU_LA4;
        case N3DS_TEXFMT_RGBA5551:
        default: return GPU_RGBA5551;
    }
}

static const char* N3DSRenderer_getTextureFormatName(uint32_t textureFormat) {
    switch (textureFormat) {
        case N3DS_TEXFMT_ETC1A4: return "etc1a4";
        case N3DS_TEXFMT_INDEXED8: return "indexed8";
        case N3DS_TEXFMT_HYBRID: return "hybrid";
        case N3DS_TEXFMT_L4: return "l4";
        case N3DS_TEXFMT_LA4: return "la4";
        case N3DS_TEXFMT_RGBA5551:
        default: return "rgba5551";
    }
}

static uint32_t N3DSRenderer_getPageVRAMBytes(MAYBE_UNUSED const N3DSRenderer* renderer, const N3DSLoadedAtlasPage* page) {
    if (page == NULL) return 0;
    uint32_t texelCount = (uint32_t) page->width * (uint32_t) page->height;
    switch (page->textureFormat) {
        case N3DS_TEXFMT_L4:
            return texelCount / 2u;
        case N3DS_TEXFMT_LA4:
        case N3DS_TEXFMT_ETC1A4:
        case N3DS_TEXFMT_HYBRID:
            return texelCount;
        case N3DS_TEXFMT_INDEXED8:
        case N3DS_TEXFMT_RGBA5551:
        default:
            return texelCount * 2u;
    }
}

static void N3DSRenderer_getActiveTargetSize(const N3DSRenderer* renderer, float* outW, float* outH) {
    if (outW == NULL || outH == NULL) return;

    bool useBottomTarget =
        renderer != NULL &&
        (renderer->activeSceneTarget == N3DS_SCENE_TARGET_BOTTOM || renderer->bottomScreenGuiActive);
    *outW = useBottomTarget ? (float) N3DS_BOTTOM_WIDTH : (float) N3DS_TOP_WIDTH;
    *outH = useBottomTarget ? (float) N3DS_BOTTOM_HEIGHT : (float) N3DS_TOP_HEIGHT;
}

static bool N3DSRenderer_isScreenRectOffscreen(const N3DSRenderer* renderer, float x, float y, float w, float h) {
    if (w < 0.0f) {
        x += w;
        w = -w;
    }
    if (h < 0.0f) {
        y += h;
        h = -h;
    }

    float targetW = 0.0f;
    float targetH = 0.0f;
    N3DSRenderer_getActiveTargetSize(renderer, &targetW, &targetH);
    return x >= targetW || y >= targetH || (x + w) <= 0.0f || (y + h) <= 0.0f;
}

static bool N3DSRenderer_isScreenRotatedRectOffscreen(const N3DSRenderer* renderer, float x, float y, float w, float h, float angleDeg) {
    if (fabsf(angleDeg) < 0.001f) {
        return N3DSRenderer_isScreenRectOffscreen(renderer, x, y, w, h);
    }

    if (w < 0.0f) {
        x += w;
        w = -w;
    }
    if (h < 0.0f) {
        y += h;
        h = -h;
    }

    float halfW = w * 0.5f;
    float halfH = h * 0.5f;
    float centerX = x + halfW;
    float centerY = y + halfH;
    float radius = sqrtf(halfW * halfW + halfH * halfH);

    float targetW = 0.0f;
    float targetH = 0.0f;
    N3DSRenderer_getActiveTargetSize(renderer, &targetW, &targetH);
    return (centerX - radius) >= targetW || (centerY - radius) >= targetH ||
        (centerX + radius) <= 0.0f || (centerY + radius) <= 0.0f;
}

static void N3DSRenderer_freeDirectTextureAsset(N3DSDirectTextureAsset* asset, N3DSRenderer* renderer) {
    if (asset == NULL || !asset->ready) {
        if (asset != NULL) {
            asset->sheet = NULL;
            asset->ready = false;
            asset->pinned = false;
        }
        return;
    }

    if (asset->sheet != NULL) {
        C2D_SpriteSheetFree(asset->sheet);
        asset->sheet = NULL;
    }
    asset->image.tex = NULL;
    asset->image.subtex = NULL;
    if (renderer != NULL) {
        if (renderer->residentDirectAssetVRAMBytes >= asset->vramBytes) renderer->residentDirectAssetVRAMBytes -= asset->vramBytes;
        else renderer->residentDirectAssetVRAMBytes = 0;
    }
    asset->vramBytes = 0;
    asset->ready = false;
    asset->pinned = false;
    asset->lastUsedStamp = 0;
    asset->lastUsedFrame = 0;
}

static void N3DSRenderer_unloadDirectTextureBlob(N3DSDirectTextureAsset* asset, N3DSRenderer* renderer) {
    if (asset == NULL || asset->blobData == NULL) return;

    free(asset->blobData);
    asset->blobData = NULL;
    if (renderer != NULL) {
        if (renderer->cachedDirectT3xBytes >= asset->blobSize) renderer->cachedDirectT3xBytes -= asset->blobSize;
        else renderer->cachedDirectT3xBytes = 0;
    }
    asset->blobSize = 0;
    asset->blobLastUsedStamp = 0;
}

static void N3DSRenderer_buildSheetFrameImages(N3DSDirectSpriteAsset* spriteAsset) {
    if (spriteAsset == NULL) return;
    if (spriteAsset->sheetAsset.sheet == NULL || spriteAsset->sheetFrameCount == 0) return;
    if (spriteAsset->sheetFrameImages != NULL) return;

    spriteAsset->sheetFrameImages = safeCalloc(spriteAsset->sheetFrameCount, sizeof(C2D_Image));
    repeat(spriteAsset->sheetFrameCount, i) {
        spriteAsset->sheetFrameImages[i] = C2D_SpriteSheetGetImage(spriteAsset->sheetAsset.sheet, i);
    }
}

static void N3DSRenderer_clearDirectAssetPins(N3DSRenderer* renderer) {
    if (renderer == NULL) return;

    repeat(renderer->directSpriteAssetCount, spriteIndex) {
        N3DSDirectSpriteAsset* spriteAsset = &renderer->directSpriteAssets[spriteIndex];
        spriteAsset->sheetAsset.pinned = false;
        repeat(spriteAsset->frameCount, frameIndex) {
            spriteAsset->frameAssets[frameIndex].pinned = false;
        }
    }

    repeat(renderer->directBackgroundAssetCount, bgIndex) {
        renderer->directBackgroundAssets[bgIndex].pinned = false;
    }

    repeat(renderer->directFontAssetCount, fontIndex) {
        renderer->directFontAssets[fontIndex].pinned = false;
    }
}

static void N3DSRenderer_flushRoomDirectAssets(N3DSRenderer* renderer) {
    if (renderer == NULL) return;

    repeat(renderer->directSpriteAssetCount, spriteIndex) {
        N3DSDirectSpriteAsset* spriteAsset = &renderer->directSpriteAssets[spriteIndex];
        if (!spriteAsset->sheetAsset.pinned) {
            N3DSRenderer_freeDirectTextureAsset(&spriteAsset->sheetAsset, renderer);
            N3DSRenderer_unloadDirectTextureBlob(&spriteAsset->sheetAsset, renderer);
        }

        repeat(spriteAsset->frameCount, frameIndex) {
            N3DSDirectTextureAsset* frameAsset = &spriteAsset->frameAssets[frameIndex];
            if (frameAsset->pinned) continue;
            N3DSRenderer_freeDirectTextureAsset(frameAsset, renderer);
            N3DSRenderer_unloadDirectTextureBlob(frameAsset, renderer);
        }
    }

    repeat(renderer->directBackgroundAssetCount, bgIndex) {
        N3DSRenderer_freeDirectTextureAsset(&renderer->directBackgroundAssets[bgIndex], renderer);
        N3DSRenderer_unloadDirectTextureBlob(&renderer->directBackgroundAssets[bgIndex], renderer);
    }
}

static void N3DSRenderer_buildDirectAssetMaps(N3DSRenderer* renderer) {
#if !N3DS_ENABLE_DIRECT_ASSETS
    if (renderer == NULL) return;
    renderer->directSpriteAssetCount = 0;
    renderer->directBackgroundAssetCount = 0;
    renderer->directFontAssetCount = 0;
    renderer->directSpriteAssets = NULL;
    renderer->directBackgroundAssets = NULL;
    renderer->directFontAssets = NULL;
    renderer->tpagToSpriteIndex = NULL;
    renderer->tpagToSpriteFrameIndex = NULL;
    renderer->tpagToBackgroundIndex = NULL;
    return;
#endif
    if (renderer == NULL || renderer->base.dataWin == NULL) return;

    DataWin* dw = renderer->base.dataWin;
    uint32_t tpagCount = dw->tpag.count > 0 ? dw->tpag.count : 1u;
    renderer->tpagToSpriteIndex = safeMalloc((size_t) tpagCount * sizeof(int32_t));
    renderer->tpagToSpriteFrameIndex = safeMalloc((size_t) tpagCount * sizeof(int32_t));
    renderer->tpagToBackgroundIndex = safeMalloc((size_t) tpagCount * sizeof(int32_t));
    repeat(tpagCount, i) {
        renderer->tpagToSpriteIndex[i] = -1;
        renderer->tpagToSpriteFrameIndex[i] = -1;
        renderer->tpagToBackgroundIndex[i] = -1;
    }

    renderer->directSpriteAssetCount = dw->sprt.count;
    renderer->directBackgroundAssetCount = dw->bgnd.count;
    renderer->directFontAssetCount = dw->font.count;
    renderer->directSpriteAssets = safeCalloc(renderer->directSpriteAssetCount > 0 ? renderer->directSpriteAssetCount : 1u, sizeof(N3DSDirectSpriteAsset));
    renderer->directBackgroundAssets = safeCalloc(renderer->directBackgroundAssetCount > 0 ? renderer->directBackgroundAssetCount : 1u, sizeof(N3DSDirectTextureAsset));
    renderer->directFontAssets = safeCalloc(renderer->directFontAssetCount > 0 ? renderer->directFontAssetCount : 1u, sizeof(N3DSDirectTextureAsset));

    repeat(dw->sprt.count, spriteIndex) {
        Sprite* sprite = &dw->sprt.sprites[spriteIndex];
        N3DSDirectSpriteAsset* spriteAsset = &renderer->directSpriteAssets[spriteIndex];
        spriteAsset->frameCount = sprite->textureCount;
        if (sprite->textureCount > 0) {
            spriteAsset->frameAssets = safeCalloc((size_t) sprite->textureCount, sizeof(N3DSDirectTextureAsset));
        }

        repeat(sprite->textureCount, frameIndex) {
            int32_t tpagIndex = sprite->tpagIndices[frameIndex];
            if (tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count) continue;
            if (renderer->tpagToSpriteIndex[tpagIndex] < 0) {
                renderer->tpagToSpriteIndex[tpagIndex] = (int32_t) spriteIndex;
                renderer->tpagToSpriteFrameIndex[tpagIndex] = (int32_t) frameIndex;
            }
        }
    }

    repeat(dw->bgnd.count, bgIndex) {
        int32_t tpagIndex = dw->bgnd.backgrounds[bgIndex].tpagIndex;
        if (tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count) continue;
        if (renderer->tpagToBackgroundIndex[tpagIndex] < 0) {
            renderer->tpagToBackgroundIndex[tpagIndex] = (int32_t) bgIndex;
        }
    }
}

static bool N3DS_pathExists(const char* path) {
    struct stat st;
    return path != NULL && stat(path, &st) == 0;
}

static void N3DSRenderer_configureFileBuffer(FILE* file) {
    if (file == NULL) return;
    setvbuf(file, NULL, _IOFBF, N3DS_RENDERER_FILE_BUFFER_SIZE);
}

static char* N3DSRenderer_getCachedAssetPath(N3DSRenderer* renderer, const char* relativePath) {
    if (renderer == NULL || relativePath == NULL) return NULL;
    ptrdiff_t idx = shgeti(renderer->resolvedAssetPathCache, relativePath);
    if (idx < 0) return NULL;
    return safeStrdup(renderer->resolvedAssetPathCache[idx].value);
}

static void N3DSRenderer_setCachedAssetPath(N3DSRenderer* renderer, const char* relativePath, const char* resolvedPath) {
    if (renderer == NULL || relativePath == NULL || resolvedPath == NULL) return;
    ptrdiff_t idx = shgeti(renderer->resolvedAssetPathCache, relativePath);
    if (idx >= 0) {
        free(renderer->resolvedAssetPathCache[idx].value);
        renderer->resolvedAssetPathCache[idx].value = safeStrdup(resolvedPath);
        return;
    }

    shput(renderer->resolvedAssetPathCache, relativePath, safeStrdup(resolvedPath));
}

static void N3DSRenderer_setStartupError(N3DSRenderer* renderer, const char* message) {
    if (renderer == NULL) return;
    snprintf(
        renderer->startupError,
        sizeof(renderer->startupError),
        "%s",
        message != NULL ? message : "Unknown 3DS renderer startup error"
    );
}

static bool N3DSRenderer_resolveAssetPath(N3DSRenderer* renderer, const char* relativePath, char* resolvedPath, size_t resolvedPathSize) {
    if (relativePath == NULL || resolvedPath == NULL || resolvedPathSize == 0) return false;

    char* cachedPath = N3DSRenderer_getCachedAssetPath(renderer, relativePath);
    if (cachedPath != NULL) {
        snprintf(resolvedPath, resolvedPathSize, "%s", cachedPath);
        free(cachedPath);
        return true;
    }

    snprintf(resolvedPath, resolvedPathSize, "%s/%s", N3DS_ROMFS_ASSET_BASE, relativePath);
    if (N3DS_pathExists(resolvedPath)) {
        N3DSRenderer_setCachedAssetPath(renderer, relativePath, resolvedPath);
        return true;
    }

    snprintf(resolvedPath, resolvedPathSize, "%s/%s", N3DS_SDMC_ASSET_BASE, relativePath);
    if (N3DS_pathExists(resolvedPath)) {
        N3DSRenderer_setCachedAssetPath(renderer, relativePath, resolvedPath);
        return true;
    }

    resolvedPath[0] = '\0';
    return false;
}

static FILE* N3DSRenderer_openAssetFile(N3DSRenderer* renderer, const char* relativePath, char* resolvedPath, size_t resolvedPathSize) {
    if (!N3DSRenderer_resolveAssetPath(renderer, relativePath, resolvedPath, resolvedPathSize)) return NULL;
    FILE* file = fopen(resolvedPath, "rb");
    N3DSRenderer_configureFileBuffer(file);
    return file;
}

static bool N3DSRenderer_loadPackedDirectAssets(N3DSRenderer* renderer) {
    if (renderer == NULL) return false;
    if (renderer->packedDirectAssetFile != NULL) return true;

    char resolvedPath[512];
    FILE* file = N3DSRenderer_openAssetFile(renderer, "direct_assets.bin", resolvedPath, sizeof(resolvedPath));
    if (file == NULL) return false;

    uint8_t header[16];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return false;
    }

    uint32_t magic = N3DS_readU32(header + 0);
    uint32_t version = N3DS_readU32(header + 4);
    uint32_t entryCount = N3DS_readU32(header + 8);
    uint32_t stringTableSize = N3DS_readU32(header + 12);
    if (magic != N3DS_DIRECT_ASSET_MAGIC || version != N3DS_DIRECT_ASSET_VERSION) {
        fclose(file);
        return false;
    }
    if (entryCount > (UINT32_MAX / N3DS_DIRECT_ASSET_ENTRY_SIZE)) {
        fclose(file);
        return false;
    }

    uint32_t entryTableSize = entryCount * N3DS_DIRECT_ASSET_ENTRY_SIZE;
    uint64_t metadataSize64 = 16u + (uint64_t) entryTableSize + (uint64_t) stringTableSize;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long fileSizeLong = ftell(file);
    if (fileSizeLong <= 0 || metadataSize64 > (uint64_t) fileSizeLong) {
        fclose(file);
        return false;
    }
    uint32_t fileSize = (uint32_t) fileSizeLong;
    if (fseek(file, 16, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    uint8_t* entryTable = safeMalloc(entryTableSize > 0 ? (size_t) entryTableSize : 1u);
    uint8_t* stringTable = safeMalloc(stringTableSize > 0 ? (size_t) stringTableSize : 1u);
    bool ok = true;
    if (entryTableSize > 0) {
        ok = fread(entryTable, 1, (size_t) entryTableSize, file) == (size_t) entryTableSize;
    }
    if (ok && stringTableSize > 0) {
        ok = fread(stringTable, 1, (size_t) stringTableSize, file) == (size_t) stringTableSize;
    }
    if (!ok) {
        free(entryTable);
        free(stringTable);
        fclose(file);
        return false;
    }

    if (renderer->packedDirectAssetMap != NULL) {
        shfree(renderer->packedDirectAssetMap);
        renderer->packedDirectAssetMap = NULL;
    }
    sh_new_strdup(renderer->packedDirectAssetMap);

    repeat(entryCount, i) {
        const uint8_t* row = entryTable + ((size_t) i * N3DS_DIRECT_ASSET_ENTRY_SIZE);
        uint32_t pathOffset = N3DS_readU32(row + 0);
        uint32_t dataOffset = N3DS_readU32(row + 4);
        uint32_t dataSize = N3DS_readU32(row + 8);
        if (pathOffset >= stringTableSize || dataOffset > fileSize || dataSize > fileSize || dataOffset + dataSize > fileSize) {
            ok = false;
            break;
        }

        const char* path = (const char*) (stringTable + pathOffset);
        size_t remaining = (size_t) (stringTableSize - pathOffset);
        if (memchr(path, '\0', remaining) == NULL) {
            ok = false;
            break;
        }

        N3DSPackedDirectAssetEntry entry = {
            .dataOffset = dataOffset,
            .dataSize = dataSize,
        };
        shput(renderer->packedDirectAssetMap, path, entry);
    }

    free(entryTable);
    free(stringTable);
    if (!ok) {
        shfree(renderer->packedDirectAssetMap);
        renderer->packedDirectAssetMap = NULL;
        fclose(file);
        return false;
    }

    renderer->packedDirectAssetFile = file;
    fprintf(stderr, "N3DS: loaded packed direct texture blob (%u entries)\n", (unsigned int) entryCount);
    return true;
}

static bool N3DSRenderer_loadRoomManifest(N3DSRenderer* renderer) {
    if (renderer == NULL) return false;
    if (renderer->roomManifestEntries != NULL) return true;

    char resolvedPath[512];
    FILE* file = N3DSRenderer_openAssetFile(renderer, "room_manifest.bin", resolvedPath, sizeof(resolvedPath));
    if (file == NULL) return false;

    uint8_t header[20];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return false;
    }

    uint32_t magic = N3DS_readU32(header + 0);
    uint32_t version = N3DS_readU32(header + 4);
    uint32_t entryCount = N3DS_readU32(header + 8);
    uint32_t pageRefCount = N3DS_readU32(header + 12);
    uint32_t roomCount = N3DS_readU32(header + 16);
    if (magic != N3DS_ROOM_MANIFEST_MAGIC || version != N3DS_ROOM_MANIFEST_VERSION) {
        fclose(file);
        return false;
    }

    uint64_t requiredBytes =
        sizeof(header) +
        (uint64_t) entryCount * sizeof(N3DSRoomManifestEntry) +
        (uint64_t) pageRefCount * sizeof(N3DSRoomManifestPageRef);
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long fileSize = ftell(file);
    if (fileSize < 0 || requiredBytes > (uint64_t) fileSize) {
        fclose(file);
        return false;
    }
    if (fseek(file, (long) sizeof(header), SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    renderer->roomManifestEntries = safeCalloc(entryCount > 0 ? entryCount : 1u, sizeof(N3DSRoomManifestEntry));
    renderer->roomManifestPageRefs = safeCalloc(pageRefCount > 0 ? pageRefCount : 1u, sizeof(N3DSRoomManifestPageRef));
    renderer->roomManifestEntryCount = entryCount;
    renderer->roomManifestPageRefCount = pageRefCount;
    renderer->roomManifestRoomCount = roomCount;

    bool ok = true;
    if (entryCount > 0) {
        ok = fread(renderer->roomManifestEntries, sizeof(N3DSRoomManifestEntry), entryCount, file) == entryCount;
    }
    if (ok && pageRefCount > 0) {
        ok = fread(renderer->roomManifestPageRefs, sizeof(N3DSRoomManifestPageRef), pageRefCount, file) == pageRefCount;
    }
    fclose(file);

    if (!ok) {
        free(renderer->roomManifestEntries);
        free(renderer->roomManifestPageRefs);
        renderer->roomManifestEntries = NULL;
        renderer->roomManifestPageRefs = NULL;
        renderer->roomManifestEntryCount = 0;
        renderer->roomManifestPageRefCount = 0;
        renderer->roomManifestRoomCount = 0;
        return false;
    }

    fprintf(
        stderr,
        "N3DS: loaded room manifest (%u rooms, %u entries, %u page refs)\n",
        roomCount,
        entryCount,
        pageRefCount
    );
    return true;
}

static const N3DSRoomManifestEntry* N3DSRenderer_findRoomManifestEntry(const N3DSRenderer* renderer, uint32_t roomIndex) {
    if (renderer == NULL || renderer->roomManifestEntries == NULL) return NULL;

    repeat(renderer->roomManifestEntryCount, i) {
        const N3DSRoomManifestEntry* entry = &renderer->roomManifestEntries[i];
        if (entry->roomIndex == roomIndex) return entry;
    }
    return NULL;
}

static void N3DSRenderer_prewarmRoomManifestBlobCache(N3DSRenderer* renderer, uint32_t roomIndex, bool* seenPages, uint32_t* remainingBlobBytes) {
    if (renderer == NULL || seenPages == NULL || remainingBlobBytes == NULL || *remainingBlobBytes == 0) return;

    const N3DSRoomManifestEntry* entry = N3DSRenderer_findRoomManifestEntry(renderer, roomIndex);
    if (entry == NULL || entry->pageCount == 0) return;
    if (entry->pageStart > renderer->roomManifestPageRefCount) return;
    if (entry->pageCount > renderer->roomManifestPageRefCount - entry->pageStart) return;

    repeat(entry->pageCount, i) {
        const N3DSRoomManifestPageRef* pageRef = &renderer->roomManifestPageRefs[entry->pageStart + (uint32_t) i];
        (void) N3DSRenderer_prewarmPageBlobOnly(renderer, seenPages, pageRef->pageIndex, remainingBlobBytes);
        if (*remainingBlobBytes == 0) break;
    }
}

static bool N3DSRenderer_tryLoadPackedDirectTextureBlob(N3DSRenderer* renderer, N3DSDirectTextureAsset* asset, const char* relativePath) {
    if (renderer == NULL || asset == NULL || relativePath == NULL) return false;
    if (renderer->packedDirectAssetFile == NULL || renderer->packedDirectAssetMap == NULL) return false;

    ptrdiff_t mapIndex = shgeti(renderer->packedDirectAssetMap, relativePath);
    if (mapIndex < 0) return false;

    N3DSPackedDirectAssetEntry entry = renderer->packedDirectAssetMap[mapIndex].value;
    if (entry.dataSize == 0) return false;

    while (renderer->cachedDirectT3xBytes + entry.dataSize > renderer->cachedDirectT3xByteLimit) {
        if (!N3DSRenderer_evictLRUDirectAssetBlob(renderer, asset)) break;
    }

    uint8_t* blobData = safeMalloc((size_t) entry.dataSize);
    if (blobData == NULL) return false;

    if (fseek(renderer->packedDirectAssetFile, (long) entry.dataOffset, SEEK_SET) != 0) {
        free(blobData);
        return false;
    }
    if (fread(blobData, 1, (size_t) entry.dataSize, renderer->packedDirectAssetFile) != (size_t) entry.dataSize) {
        free(blobData);
        return false;
    }

    asset->blobData = blobData;
    asset->blobSize = entry.dataSize;
    asset->blobLastUsedStamp = ++renderer->directBlobUseCounter;
    renderer->cachedDirectT3xBytes += entry.dataSize;
    renderer->frameDirectBlobReads++;
    return true;
}

static void N3DSRenderer_unloadPage(N3DSRenderer* renderer, uint32_t pageIndex) {
    if (pageIndex >= renderer->atlasPageCount) return;
    N3DSLoadedAtlasPage* page = &renderer->atlasPages[pageIndex];
    if (!page->ready) return;
    C2D_Flush();
    renderer->pendingC2DDraws = 0;
    renderer->lastDrawTexture = NULL;

    uint32_t pageBytes = N3DSRenderer_getPageVRAMBytes(renderer, page);

    if (page->t3x != NULL) {
        Tex3DS_TextureFree(page->t3x);
        page->t3x = NULL;
    }
    C3D_TexDelete(&page->texture);
    memset(&page->texture, 0, sizeof(page->texture));
    page->ready = false;
    page->lastUsedStamp = 0;
    if (renderer->residentAtlasVRAMBytes >= pageBytes) renderer->residentAtlasVRAMBytes -= pageBytes;
    else renderer->residentAtlasVRAMBytes = 0;
    if (renderer->residentAtlasPageCount > 0) {
        renderer->residentAtlasPageCount--;
    }
}

static void N3DSRenderer_unloadPageBlob(N3DSRenderer* renderer, uint32_t pageIndex) {
    if (pageIndex >= renderer->atlasPageCount) return;
    N3DSLoadedAtlasPage* page = &renderer->atlasPages[pageIndex];
    if (page->t3xData == NULL) return;

    free(page->t3xData);
    page->t3xData = NULL;
    if (renderer->cachedT3xBytes >= page->t3xSize) renderer->cachedT3xBytes -= page->t3xSize;
    else renderer->cachedT3xBytes = 0;
    page->t3xSize = 0;
    page->blobLastUsedStamp = 0;
}

static void N3DSRenderer_clearAtlasPagePins(N3DSRenderer* renderer) {
    if (renderer == NULL) return;
    repeat(renderer->atlasPageCount, i) {
        renderer->atlasPages[i].pinned = false;
    }
}

static void N3DSRenderer_flushRoomAtlasResidencyOld3DS(N3DSRenderer* renderer) {
    if (renderer == NULL || renderer->isNew3DS) return;

    repeat(renderer->atlasPageCount, i) {
        N3DSLoadedAtlasPage* page = &renderer->atlasPages[i];
        if (!page->ready) continue;
        if (page->pinned) continue;
        N3DSRenderer_unloadPage(renderer, (uint32_t) i);
    }
}

static bool N3DSRenderer_evictLRUPageBlob(N3DSRenderer* renderer, uint32_t excludePageIndex) {
    uint32_t bestIndex = UINT32_MAX;
    uint32_t bestStamp = UINT32_MAX;

    repeat(renderer->atlasPageCount, i) {
        if ((uint32_t) i == excludePageIndex) continue;
        N3DSLoadedAtlasPage* page = &renderer->atlasPages[i];
        if (page->t3xData == NULL || page->t3xSize == 0) continue;
        if (page->blobLastUsedStamp < bestStamp) {
            bestStamp = page->blobLastUsedStamp;
            bestIndex = (uint32_t) i;
        }
    }

    if (bestIndex == UINT32_MAX) return false;
    N3DSRenderer_unloadPageBlob(renderer, bestIndex);
    return true;
}

static bool N3DSRenderer_ensurePageBlobLoaded(N3DSRenderer* renderer, uint32_t pageIndex) {
    if (pageIndex >= renderer->atlasPageCount) return false;
    N3DSLoadedAtlasPage* page = &renderer->atlasPages[pageIndex];
    if (page->t3xData != NULL && page->t3xSize > 0) {
        page->blobLastUsedStamp = ++renderer->blobUseCounter;
        return true;
    }

    FILE* pageFile = NULL;
    uint32_t size = 0;
    uint32_t dataOffset = 0;
    bool usingPackedAtlas = false;
    char assetPath[256];
    char pageName[32];

    if (N3DSRenderer_isPackedAtlasVersion(renderer->atlasVersion) &&
        renderer->packedAtlasFile != NULL &&
        page->dataSize > 0) {
        pageFile = renderer->packedAtlasFile;
        size = page->dataSize;
        dataOffset = page->dataOffset;
        usingPackedAtlas = true;
    } else {
        const char* extension = page->textureFormat == N3DS_TEXFMT_INDEXED8 ? "i8" : "t3x";
        snprintf(pageName, sizeof(pageName), "page_%03lu.%s", (unsigned long) pageIndex, extension);
        pageFile = N3DSRenderer_openAssetFile(renderer, pageName, assetPath, sizeof(assetPath));
        if (pageFile == NULL) {
            fprintf(stderr, "N3DS: missing %s\n", pageName);
            return false;
        }

        fseek(pageFile, 0, SEEK_END);
        long t3xSize = ftell(pageFile);
        fseek(pageFile, 0, SEEK_SET);
        if (t3xSize <= 0) {
            fclose(pageFile);
            fprintf(stderr, "N3DS: empty %s\n", assetPath);
            return false;
        }

        size = (uint32_t) t3xSize;
    }
    while (renderer->cachedT3xBytes + size > renderer->cachedT3xByteLimit) {
        if (!N3DSRenderer_evictLRUPageBlob(renderer, pageIndex)) break;
    }

    page->t3xData = safeMalloc((size_t) size);
    page->t3xSize = size;
    if (usingPackedAtlas) {
        fseek(pageFile, (long) dataOffset, SEEK_SET);
    }
    if (fread(page->t3xData, 1, (size_t) size, pageFile) != (size_t) size) {
        free(page->t3xData);
        page->t3xData = NULL;
        page->t3xSize = 0;
        if (!usingPackedAtlas) fclose(pageFile);
        fprintf(stderr, "N3DS: failed to read atlas page blob %lu\n", (unsigned long) pageIndex);
        return false;
    }
    if (!usingPackedAtlas) fclose(pageFile);

    renderer->cachedT3xBytes += size;
    page->blobLastUsedStamp = ++renderer->blobUseCounter;
    renderer->frameBlobReads++;
    return true;
}

static bool N3DSRenderer_loadIndexed8Page(N3DSRenderer* renderer, N3DSLoadedAtlasPage* page) {
    if (renderer == NULL || page == NULL || page->t3xData == NULL || page->t3xSize == 0) return false;

    size_t pixelCount = (size_t) page->width * (size_t) page->height;
    size_t requiredSize = (256u * sizeof(uint16_t)) + pixelCount;
    if ((size_t) page->t3xSize < requiredSize) {
        fprintf(stderr, "N3DS: indexed8 page blob too small (%lu < %lu)\n",
            (unsigned long) page->t3xSize,
            (unsigned long) requiredSize);
        return false;
    }

    if (!C3D_TexInit(&page->texture, page->width, page->height, GPU_RGBA5551)) {
        return false;
    }
    C3D_TexSetFilter(&page->texture, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&page->texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    uint16_t* expanded = (uint16_t*) linearAlloc(pixelCount * sizeof(uint16_t));
    if (expanded == NULL) {
        C3D_TexDelete(&page->texture);
        memset(&page->texture, 0, sizeof(page->texture));
        return false;
    }

    const uint8_t* paletteBlob = page->t3xData;
    const uint8_t* indexBlob = page->t3xData + (256u * sizeof(uint16_t));
    uint16_t palette[256];
    repeat(256u, i) {
        palette[i] = (uint16_t) (paletteBlob[i * 2u + 0u] | ((uint16_t) paletteBlob[i * 2u + 1u] << 8));
    }
    repeat(pixelCount, i) {
        expanded[i] = palette[indexBlob[i]];
    }

    C3D_TexUpload(&page->texture, expanded);
    C3D_TexFlush(&page->texture);
    linearFree(expanded);
    page->ready = true;
    renderer->framePageImports++;
    return true;
}

static bool N3DSRenderer_loadPage(N3DSRenderer* renderer, uint32_t pageIndex) {
    if (pageIndex >= renderer->atlasPageCount) return false;
    N3DSLoadedAtlasPage* page = &renderer->atlasPages[pageIndex];
    if (page->ready) return true;

    if (renderer->atlasVersion == N3DS_ATLAS_VERSION_RAW) {
        char assetPath[256];
        FILE* textureFile = N3DSRenderer_openAssetFile(renderer, "textures.bin", assetPath, sizeof(assetPath));
        if (textureFile == NULL) {
            fprintf(stderr, "N3DS: missing textures.bin for raw atlas version\n");
            return false;
        }

        if (!C3D_TexInit(&page->texture, page->width, page->height, N3DSRenderer_getGPUTextureFormat(page->textureFormat))) {
            fclose(textureFile);
            return false;
        }
        C3D_TexSetFilter(&page->texture, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&page->texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

        uint8_t* textureData = linearAlloc(page->dataSize);
        if (textureData == NULL) {
            fclose(textureFile);
            C3D_TexDelete(&page->texture);
            memset(&page->texture, 0, sizeof(page->texture));
            return false;
        }

        fseek(textureFile, (long) page->dataOffset, SEEK_SET);
        fread(textureData, 1, page->dataSize, textureFile);
        fclose(textureFile);

        C3D_TexUpload(&page->texture, textureData);
        C3D_TexFlush(&page->texture);
        linearFree(textureData);
        page->ready = true;
        return true;
    }

    if (!N3DSRenderer_ensurePageBlobLoaded(renderer, pageIndex)) return false;

    if (page->textureFormat == N3DS_TEXFMT_INDEXED8) {
        return N3DSRenderer_loadIndexed8Page(renderer, page);
    }

    page->t3x = Tex3DS_TextureImport(page->t3xData, page->t3xSize, &page->texture, NULL, false);
    if (page->t3x == NULL) {
        return false;
    }

    C3D_TexSetFilter(&page->texture, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&page->texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    page->ready = true;
    renderer->framePageImports++;
    return true;
}

static bool N3DSRenderer_evictLRUPage(N3DSRenderer* renderer, uint32_t excludePageIndex) {
    uint32_t bestIndex = UINT32_MAX;
    uint32_t bestStamp = UINT32_MAX;

    repeat(renderer->atlasPageCount, i) {
        if ((uint32_t) i == excludePageIndex) continue;
        N3DSLoadedAtlasPage* page = &renderer->atlasPages[i];
        if (!page->ready) continue;
        if (page->pinned) continue;
        if (page->lastUsedFrame == renderer->frameSequence) continue;
        if (page->lastUsedStamp < bestStamp) {
            bestStamp = page->lastUsedStamp;
            bestIndex = (uint32_t) i;
        }
    }

    if (bestIndex == UINT32_MAX) return false;
    N3DSRenderer_unloadPage(renderer, bestIndex);
    return true;
}

static bool N3DSRenderer_ensurePageLoaded(N3DSRenderer* renderer, uint32_t pageIndex) {
    if (pageIndex >= renderer->atlasPageCount) return false;
    N3DSLoadedAtlasPage* page = &renderer->atlasPages[pageIndex];
    uint32_t pageBytes = N3DSRenderer_getPageVRAMBytes(renderer, page);
    if (page->ready) {
        page->lastUsedStamp = ++renderer->atlasUseCounter;
        page->lastUsedFrame = renderer->frameSequence;
        return true;
    }

    while (renderer->residentAtlasPageCount >= renderer->residentAtlasPageLimit ||
        renderer->residentAtlasVRAMBytes + pageBytes > renderer->residentAtlasVRAMLimitBytes) {
        if (!N3DSRenderer_evictLRUPage(renderer, pageIndex)) break;
    }

    uint32_t evictionsForLoad = 0;
    while (!N3DSRenderer_loadPage(renderer, pageIndex)) {
        if (!N3DSRenderer_evictLRUPage(renderer, pageIndex)) {
            renderer->frameImportFailures++;
            renderer->frameImportEvictions += evictionsForLoad;
            fprintf(
                stderr,
                "N3DS: failed to import page %lu from RAM cache after evicting %lu pages\n",
                (unsigned long) pageIndex,
                (unsigned long) evictionsForLoad
            );
            return false;
        }
        evictionsForLoad++;
    }

    page->lastUsedStamp = ++renderer->atlasUseCounter;
    page->lastUsedFrame = renderer->frameSequence;
    renderer->residentAtlasPageCount++;
    renderer->residentAtlasVRAMBytes += pageBytes;
    renderer->frameImportEvictions += evictionsForLoad;
    return true;
}

static bool N3DSRenderer_evictLRUDirectAsset(N3DSRenderer* renderer, const N3DSDirectTextureAsset* excludeAsset) {
    if (renderer == NULL) return false;

    N3DSDirectTextureAsset* bestAsset = NULL;
    uint32_t bestStamp = UINT32_MAX;

    repeat(renderer->directSpriteAssetCount, spriteIndex) {
        N3DSDirectSpriteAsset* spriteAsset = &renderer->directSpriteAssets[spriteIndex];
        N3DSDirectTextureAsset* asset = &spriteAsset->sheetAsset;
        if (asset->ready && asset != excludeAsset && asset->lastUsedFrame != renderer->frameSequence && asset->lastUsedStamp < bestStamp) {
            if (asset->pinned) continue;
            bestStamp = asset->lastUsedStamp;
            bestAsset = asset;
        }

        repeat(spriteAsset->frameCount, frameIndex) {
            asset = &spriteAsset->frameAssets[frameIndex];
            if (!asset->ready || asset == excludeAsset) continue;
            if (asset->lastUsedFrame == renderer->frameSequence) continue;
            if (asset->pinned) continue;
            if (asset->lastUsedStamp < bestStamp) {
                bestStamp = asset->lastUsedStamp;
                bestAsset = asset;
            }
        }
    }

    repeat(renderer->directBackgroundAssetCount, bgIndex) {
        N3DSDirectTextureAsset* asset = &renderer->directBackgroundAssets[bgIndex];
        if (!asset->ready || asset == excludeAsset) continue;
        if (asset->lastUsedFrame == renderer->frameSequence) continue;
        if (asset->pinned) continue;
        if (asset->lastUsedStamp < bestStamp) {
            bestStamp = asset->lastUsedStamp;
            bestAsset = asset;
        }
    }

    repeat(renderer->directFontAssetCount, fontIndex) {
        N3DSDirectTextureAsset* asset = &renderer->directFontAssets[fontIndex];
        if (!asset->ready || asset == excludeAsset) continue;
        if (asset->lastUsedFrame == renderer->frameSequence) continue;
        if (asset->pinned) continue;
        if (asset->lastUsedStamp < bestStamp) {
            bestStamp = asset->lastUsedStamp;
            bestAsset = asset;
        }
    }

    if (bestAsset == NULL) return false;
    N3DSRenderer_freeDirectTextureAsset(bestAsset, renderer);
    return true;
}

static bool N3DSRenderer_evictLRUDirectAssetBlob(N3DSRenderer* renderer, const N3DSDirectTextureAsset* excludeAsset) {
    if (renderer == NULL) return false;

    N3DSDirectTextureAsset* bestAsset = NULL;
    uint32_t bestStamp = UINT32_MAX;

    repeat(renderer->directSpriteAssetCount, spriteIndex) {
        N3DSDirectSpriteAsset* spriteAsset = &renderer->directSpriteAssets[spriteIndex];
        N3DSDirectTextureAsset* asset = &spriteAsset->sheetAsset;
        if (asset->blobData != NULL && !asset->ready && asset != excludeAsset && asset->blobLastUsedStamp < bestStamp) {
            bestStamp = asset->blobLastUsedStamp;
            bestAsset = asset;
        }

        repeat(spriteAsset->frameCount, frameIndex) {
            asset = &spriteAsset->frameAssets[frameIndex];
            if (asset->blobData == NULL || asset->ready || asset == excludeAsset) continue;
            if (asset->blobLastUsedStamp < bestStamp) {
                bestStamp = asset->blobLastUsedStamp;
                bestAsset = asset;
            }
        }
    }

    repeat(renderer->directBackgroundAssetCount, bgIndex) {
        N3DSDirectTextureAsset* asset = &renderer->directBackgroundAssets[bgIndex];
        if (asset->blobData == NULL || asset->ready || asset == excludeAsset) continue;
        if (asset->blobLastUsedStamp < bestStamp) {
            bestStamp = asset->blobLastUsedStamp;
            bestAsset = asset;
        }
    }

    repeat(renderer->directFontAssetCount, fontIndex) {
        N3DSDirectTextureAsset* asset = &renderer->directFontAssets[fontIndex];
        if (asset->blobData == NULL || asset->ready || asset == excludeAsset) continue;
        if (asset->blobLastUsedStamp < bestStamp) {
            bestStamp = asset->blobLastUsedStamp;
            bestAsset = asset;
        }
    }

    if (bestAsset == NULL) return false;
    N3DSRenderer_unloadDirectTextureBlob(bestAsset, renderer);
    return true;
}

static bool N3DSRenderer_ensureDirectTextureBlobLoaded(N3DSRenderer* renderer, N3DSDirectTextureAsset* asset, const char* relativePath) {
    if (renderer == NULL || asset == NULL || relativePath == NULL) return false;
    if (asset->blobData != NULL && asset->blobSize > 0) {
        asset->blobLastUsedStamp = ++renderer->directBlobUseCounter;
        return true;
    }

    if (N3DSRenderer_tryLoadPackedDirectTextureBlob(renderer, asset, relativePath)) {
        return true;
    }

    char resolvedPath[512];
    if (!N3DSRenderer_resolveAssetPath(renderer, relativePath, resolvedPath, sizeof(resolvedPath))) return false;

    FILE* file = fopen(resolvedPath, "rb");
    if (file == NULL) return false;
    N3DSRenderer_configureFileBuffer(file);

    fseek(file, 0, SEEK_END);
    long sizeLong = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (sizeLong <= 0) {
        fclose(file);
        return false;
    }

    uint32_t size = (uint32_t) sizeLong;
    while (renderer->cachedDirectT3xBytes + size > renderer->cachedDirectT3xByteLimit) {
        if (!N3DSRenderer_evictLRUDirectAssetBlob(renderer, asset)) break;
    }

    uint8_t* blobData = safeMalloc((size_t) size);
    if (blobData == NULL) {
        fclose(file);
        return false;
    }

    bool ok = fread(blobData, 1, (size_t) size, file) == (size_t) size;
    fclose(file);
    if (!ok) {
        free(blobData);
        return false;
    }

    asset->blobData = blobData;
    asset->blobSize = size;
    asset->blobLastUsedStamp = ++renderer->directBlobUseCounter;
    renderer->cachedDirectT3xBytes += size;
    renderer->frameDirectBlobReads++;
    return true;
}

static bool N3DSRenderer_loadDirectTextureAsset(N3DSRenderer* renderer, N3DSDirectTextureAsset* asset, const char* relativePath) {
    if (renderer == NULL || asset == NULL || relativePath == NULL) return false;
    if (asset->ready) {
        asset->lastUsedStamp = ++renderer->directAssetUseCounter;
        asset->lastUsedFrame = renderer->frameSequence;
        if (asset->blobData != NULL) asset->blobLastUsedStamp = ++renderer->directBlobUseCounter;
        return true;
    }
    if (asset->failed) return false;

    if (!N3DSRenderer_ensureDirectTextureBlobLoaded(renderer, asset, relativePath)) {
        asset->failed = true;
        return false;
    }

    C2D_SpriteSheet sheet = C2D_SpriteSheetLoadFromMem(asset->blobData, asset->blobSize);
    if (sheet == NULL) {
        asset->failed = true;
        return false;
    }

    C2D_Image image = C2D_SpriteSheetGetImage(sheet, 0);
    if (image.tex == NULL || image.subtex == NULL) {
        C2D_SpriteSheetFree(sheet);
        asset->failed = true;
        return false;
    }

    uint32_t vramBytes = (uint32_t) image.tex->width * (uint32_t) image.tex->height * 2u;
    while (renderer->residentDirectAssetVRAMBytes + vramBytes > renderer->residentDirectAssetVRAMLimitBytes) {
        if (!N3DSRenderer_evictLRUDirectAsset(renderer, asset)) break;
    }

    asset->sheet = sheet;
    asset->image = image;
    if (asset->image.tex != NULL) {
        C3D_TexSetFilter(asset->image.tex, GPU_NEAREST, GPU_NEAREST);
    }
    asset->ready = true;
    asset->failed = false;
    asset->vramBytes = vramBytes;
    asset->lastUsedStamp = ++renderer->directAssetUseCounter;
    asset->lastUsedFrame = renderer->frameSequence;
    renderer->residentDirectAssetVRAMBytes += vramBytes;
    renderer->frameDirectAssetLoads++;
    return true;
}

static void N3DSRenderer_applyBlendState(N3DSRenderer* renderer) {
    if (!renderer->blendEnabled) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        return;
    }

    GPU_BLENDEQUATION equation = GPU_BLEND_ADD;
    switch (renderer->blendEquation) {
        case bm_subtract: equation = GPU_BLEND_SUBTRACT; break;
        case bm_reverse_subtract: equation = GPU_BLEND_REVERSE_SUBTRACT; break;
        case bm_min: equation = GPU_BLEND_MIN; break;
        case bm_max: equation = GPU_BLEND_MAX; break;
        default: break;
    }

    GPU_BLENDFACTOR src = GPU_SRC_ALPHA;
    GPU_BLENDFACTOR dst = GPU_ONE_MINUS_SRC_ALPHA;
    switch (renderer->blendSrcFactor) {
        case bm_zero: src = GPU_ZERO; break;
        case bm_one: src = GPU_ONE; break;
        case bm_src_color: src = GPU_SRC_COLOR; break;
        case bm_inv_src_color: src = GPU_ONE_MINUS_SRC_COLOR; break;
        case bm_dest_alpha: src = GPU_DST_ALPHA; break;
        case bm_inv_dest_alpha: src = GPU_ONE_MINUS_DST_ALPHA; break;
        case bm_dest_color: src = GPU_DST_COLOR; break;
        case bm_inv_dest_color: src = GPU_ONE_MINUS_DST_COLOR; break;
        case bm_src_alpha_sat: src = GPU_SRC_ALPHA_SATURATE; break;
        default: src = GPU_SRC_ALPHA; break;
    }
    switch (renderer->blendDstFactor) {
        case bm_zero: dst = GPU_ZERO; break;
        case bm_one: dst = GPU_ONE; break;
        case bm_src_color: dst = GPU_SRC_COLOR; break;
        case bm_inv_src_color: dst = GPU_ONE_MINUS_SRC_COLOR; break;
        case bm_src_alpha: dst = GPU_SRC_ALPHA; break;
        case bm_dest_alpha: dst = GPU_DST_ALPHA; break;
        case bm_inv_dest_alpha: dst = GPU_ONE_MINUS_DST_ALPHA; break;
        case bm_dest_color: dst = GPU_DST_COLOR; break;
        case bm_inv_dest_color: dst = GPU_ONE_MINUS_DST_COLOR; break;
        case bm_src_alpha_sat: dst = GPU_SRC_ALPHA_SATURATE; break;
        default: dst = GPU_ONE_MINUS_SRC_ALPHA; break;
    }

    C3D_AlphaBlend(equation, equation, src, dst, src, dst);
}

static void N3DSRenderer_applyAlphaState(N3DSRenderer* renderer) {
    if (renderer->alphaTestEnabled) {
        C3D_AlphaTest(true, GPU_GREATER, renderer->alphaTestRef);
    } else {
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
    }
}

static void N3DSRenderer_setDefaultGPUState(N3DSRenderer* renderer) {
    if (renderer == NULL) return;
    renderer->blendEnabled = true;
    renderer->blendEquation = bm_normal;
    renderer->blendSrcFactor = bm_src_alpha;
    renderer->blendDstFactor = bm_inv_src_alpha;
    renderer->alphaTestEnabled = false;
    renderer->alphaTestRef = 0;
    N3DSRenderer_applyBlendState(renderer);
    N3DSRenderer_applyAlphaState(renderer);
}

static void N3DSRenderer_flushC2DQueue(N3DSRenderer* renderer) {
    if (renderer == NULL || renderer->pendingC2DDraws == 0) return;
    C2D_Flush();
    renderer->pendingC2DDraws = 0;
    renderer->lastDrawTexture = NULL;
}

static void N3DSRenderer_noteC2DDraws(N3DSRenderer* renderer, uint32_t drawCount) {
    if (renderer == NULL || drawCount == 0) return;
    renderer->pendingC2DDraws += drawCount;
    if (renderer->pendingC2DDraws >= N3DS_C2D_FLUSH_DRAW_BUDGET) {
        N3DSRenderer_flushC2DQueue(renderer);
    }
}

static void N3DSRenderer_sceneBeginTarget(N3DSRenderer* renderer, uint8_t targetKind, bool force) {
    if (renderer == NULL) return;
    if (!force && renderer->activeSceneTarget == targetKind) return;

    N3DSRenderer_flushC2DQueue(renderer);

    if (targetKind == N3DS_SCENE_TARGET_TOP) {
        if (renderer->topTarget == NULL) return;
        C2D_SceneBegin(renderer->topTarget);
        renderer->activeSceneTarget = N3DS_SCENE_TARGET_TOP;
        return;
    }

    if (targetKind == N3DS_SCENE_TARGET_BOTTOM) {
        if (renderer->bottomTarget == NULL) return;
        C2D_SceneBegin(renderer->bottomTarget);
        renderer->activeSceneTarget = N3DS_SCENE_TARGET_BOTTOM;
        return;
    }

    renderer->activeSceneTarget = N3DS_SCENE_TARGET_NONE;
}

static void N3DSRenderer_freeTileLayerChunkCache(N3DSRenderer* renderer, N3DSTileLayerChunkCache* cache) {
    if (cache == NULL || cache->chunks == NULL) return;

    repeat(cache->chunkCount, i) {
        N3DSTileLayerChunk* chunk = &cache->chunks[i];
        if (chunk->target != NULL) {
            C3D_RenderTargetDelete(chunk->target);
            chunk->target = NULL;
        }
        if (chunk->allocated) {
            C3D_TexDelete(&chunk->texture);
            memset(&chunk->texture, 0, sizeof(chunk->texture));
            chunk->allocated = false;
        }
    }

    free(cache->chunks);
    cache->chunks = NULL;
    cache->chunkCount = 0;
    if (renderer != NULL) {
        if (renderer->tileLayerChunkVRAMBytes >= cache->vramBytes) renderer->tileLayerChunkVRAMBytes -= cache->vramBytes;
        else renderer->tileLayerChunkVRAMBytes = 0;
    }
    cache->vramBytes = 0;
    cache->used = false;
}

static u32 N3DSRenderer_makeColor(uint32_t bgr, float alpha) {
    return C2D_Color32(
        (u8) BGR_R(bgr),
        (u8) BGR_G(bgr),
        (u8) BGR_B(bgr),
        (u8) (C2D_Clamp(alpha, 0.0f, 1.0f) * 255.0f)
    );
}

static bool N3DSRenderer_isIdentityTint(uint32_t color, float alpha) {
    return color == 0x00FFFFFFu && alpha >= 0.999f;
}

static void N3DSRenderer_trackTextureUse(N3DSRenderer* renderer, const C2D_Image* image) {
    if (renderer == NULL || image == NULL || image->tex == NULL) return;
    if (renderer->lastDrawTexture != image->tex) {
        renderer->frameTextureSwitches++;
        renderer->lastDrawTexture = image->tex;
    }
}

static void N3DSRenderer_applyTextureFilterForScale(N3DSRenderer* renderer, C3D_Tex* texture, float scaleX, float scaleY) {
    if (texture == NULL || !Renderer_isFiniteFloat(scaleX) || !Renderer_isFiniteFloat(scaleY)) return;

    if (renderer != NULL && renderer->textLinearFilterActive) {
        C3D_TexSetFilter(texture, GPU_LINEAR, GPU_LINEAR);
        return;
    }

    if (renderer != NULL && (renderer->bottomScreenGuiActive || renderer->topScreenGuiActive || renderer->topScreenBattleViewActive)) {
        C3D_TexSetFilter(texture, GPU_NEAREST, GPU_NEAREST);
        return;
    }

    GPU_TEXTURE_FILTER_PARAM minFilter = (fabsf(scaleX) < 0.999f || fabsf(scaleY) < 0.999f)
        ? GPU_LINEAR
        : GPU_NEAREST;
    C3D_TexSetFilter(texture, minFilter, GPU_NEAREST);
}

static float N3DSRenderer_snapToPixel(float value) {
    if (!Renderer_isFiniteFloat(value)) return value;
    return floorf(value + 0.5f);
}

static float N3DSRenderer_transformScreenX(const N3DSRenderer* renderer, float localX) {
    float screenX = renderer->frameOffsetX + renderer->portOffsetX + (localX - (float) renderer->viewX) * renderer->viewScaleX;
    return N3DSRenderer_snapToPixel(screenX);
}

static float N3DSRenderer_transformScreenY(const N3DSRenderer* renderer, float localY) {
    float screenY = renderer->frameOffsetY + renderer->portOffsetY + (localY - (float) renderer->viewY) * renderer->viewScaleY;
    return N3DSRenderer_snapToPixel(screenY);
}

static void N3DSRenderer_transformScreenRect(
    const N3DSRenderer* renderer,
    float localX,
    float localY,
    float localW,
    float localH,
    float* outX,
    float* outY,
    float* outW,
    float* outH
) {
    float screenX0 = N3DSRenderer_transformScreenX(renderer, localX);
    float screenY0 = N3DSRenderer_transformScreenY(renderer, localY);
    float screenX1 = N3DSRenderer_transformScreenX(renderer, localX + localW);
    float screenY1 = N3DSRenderer_transformScreenY(renderer, localY + localH);

    *outX = screenX0 < screenX1 ? screenX0 : screenX1;
    *outY = screenY0 < screenY1 ? screenY0 : screenY1;
    *outW = fabsf(screenX1 - screenX0);
    *outH = fabsf(screenY1 - screenY0);
}

static void N3DSRenderer_drawImageFast(Renderer* base, C2D_Image* image, float localX, float localY, float xscale, float yscale, uint32_t color, float alpha) {
    if (image == NULL || image->tex == NULL || image->subtex == NULL) return;
    if (!Renderer_isFiniteFloat(localX) || !Renderer_isFiniteFloat(localY) ||
        !Renderer_isFiniteFloat(xscale) || !Renderer_isFiniteFloat(yscale) ||
        !Renderer_isFiniteFloat(alpha) || xscale == 0.0f || yscale == 0.0f) {
        return;
    }

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    float localW = (float) image->subtex->width * xscale;
    float localH = (float) image->subtex->height * yscale;
    float screenX = 0.0f;
    float screenY = 0.0f;
    float screenW = 0.0f;
    float screenH = 0.0f;
    N3DSRenderer_transformScreenRect(renderer, localX, localY, localW, localH, &screenX, &screenY, &screenW, &screenH);
    float screenScaleX = screenW / (float) image->subtex->width;
    float screenScaleY = screenH / (float) image->subtex->height;
    if (screenW <= 0.0f || screenH <= 0.0f) return;
    if (N3DSRenderer_isScreenRectOffscreen(renderer, screenX, screenY, screenW, screenH)) return;
    N3DSRenderer_trackTextureUse(renderer, image);
    N3DSRenderer_applyTextureFilterForScale(renderer, image->tex, screenScaleX, screenScaleY);

    if (N3DSRenderer_isIdentityTint(color, alpha)) {
        C2D_DrawImageAt(*image, screenX, screenY, 0.5f, NULL, screenScaleX, screenScaleY);
    } else {
        C2D_ImageTint tint;
        C2D_PlainImageTint(&tint, N3DSRenderer_makeColor(color, alpha), 1.0f);
        C2D_DrawImageAt(*image, screenX, screenY, 0.5f, &tint, screenScaleX, screenScaleY);
    }
    N3DSRenderer_noteC2DDraws(renderer, 1u);
}

static bool N3DSRenderer_getSourceCropRect(
    N3DSRenderer* renderer,
    int32_t tpagIndex,
    int32_t* outX,
    int32_t* outY,
    int32_t* outW,
    int32_t* outH
) {
    if (renderer == NULL || outX == NULL || outY == NULL || outW == NULL || outH == NULL) return false;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= renderer->base.dataWin->tpag.count) return false;

    TexturePageItem* tpag = &renderer->base.dataWin->tpag.items[tpagIndex];
    if (!N3DSRenderer_isFragmentedAtlasVersion(renderer->atlasVersion)) {
        *outX = 0;
        *outY = 0;
        *outW = (int32_t) tpag->sourceWidth;
        *outH = (int32_t) tpag->sourceHeight;
        return *outW > 0 && *outH > 0;
    }

    if ((uint32_t) tpagIndex >= renderer->atlasItemCount) return false;
    N3DSAtlasItemV3* item = &renderer->atlasItemsV3[tpagIndex];
    if (item->fragmentCount == 0) return false;

    int32_t minX = INT32_MAX;
    int32_t minY = INT32_MAX;
    int32_t maxX = INT32_MIN;
    int32_t maxY = INT32_MIN;

    repeat(item->fragmentCount, fragIndex) {
        uint32_t fragmentIndex = item->fragmentStart + (uint32_t) fragIndex;
        if (fragmentIndex >= renderer->atlasFragmentCount) break;
        N3DSAtlasFragment* fragment = &renderer->atlasFragments[fragmentIndex];
        int32_t fragLeft = fragment->sourceX;
        int32_t fragTop = fragment->sourceY;
        int32_t fragRight = fragLeft + fragment->width;
        int32_t fragBottom = fragTop + fragment->height;
        if (minX > fragLeft) minX = fragLeft;
        if (minY > fragTop) minY = fragTop;
        if (maxX < fragRight) maxX = fragRight;
        if (maxY < fragBottom) maxY = fragBottom;
    }

    if (minX >= maxX || minY >= maxY) return false;
    *outX = minX;
    *outY = minY;
    *outW = maxX - minX;
    *outH = maxY - minY;
    return true;
}

static void N3DSRenderer_fillSubTexture(Tex3DS_SubTexture* subtex, const N3DSLoadedAtlasPage* page, uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    subtex->width = width;
    subtex->height = height;
    subtex->left = (float) x / (float) page->width;
    subtex->top = (float) (page->height - y) / (float) page->height;
    subtex->right = (float) (x + width) / (float) page->width;
    subtex->bottom = (float) (page->height - (y + height)) / (float) page->height;
}

static bool N3DSRenderer_resolveFragmentImage(N3DSRenderer* renderer, const N3DSAtlasFragment* fragment, C2D_Image* outImage, Tex3DS_SubTexture* subtex) {
    if ((uint32_t) fragment->atlasId >= renderer->atlasPageCount) return false;
    if (!N3DSRenderer_ensurePageLoaded(renderer, fragment->atlasId)) return false;
    N3DSLoadedAtlasPage* page = &renderer->atlasPages[fragment->atlasId];
    if (!page->ready || fragment->width == 0 || fragment->height == 0) return false;

    N3DSRenderer_fillSubTexture(subtex, page, fragment->x, fragment->y, fragment->width, fragment->height);
    outImage->tex = &page->texture;
    outImage->subtex = subtex;
    return true;
}

static N3DSDynamicCaptureTPAG* N3DSRenderer_getDynamicCaptureTPAG(N3DSRenderer* renderer, int32_t tpagIndex) {
    if (renderer == NULL) return NULL;
    if (tpagIndex < 0 || (uint32_t) tpagIndex < renderer->baseTPAGCount) return NULL;

    uint32_t dynamicIndex = (uint32_t) tpagIndex - renderer->baseTPAGCount;
    if (dynamicIndex >= renderer->dynamicCaptureTPAGCount) return NULL;
    N3DSDynamicCaptureTPAG* capture = &renderer->dynamicCaptureTPAGs[dynamicIndex];
    if (!capture->used || capture->image.tex == NULL || capture->image.subtex == NULL) return NULL;
    return capture;
}

static void N3DSRenderer_freeDynamicCaptureTPAG(N3DSRenderer* renderer, N3DSDynamicCaptureTPAG* capture) {
    if (renderer == NULL || capture == NULL || !capture->used) return;

    C2D_Flush();
    renderer->pendingC2DDraws = 0;
    renderer->lastDrawTexture = NULL;
    C3D_TexDelete(&capture->texture);
    memset(capture, 0, sizeof(*capture));
    capture->ownerSpriteIndex = -1;
}

static int32_t N3DSRenderer_allocDynamicCaptureTPAG(N3DSRenderer* renderer) {
    if (renderer == NULL || renderer->base.dataWin == NULL) return -1;

    repeat(renderer->dynamicCaptureTPAGCount, i) {
        if (!renderer->dynamicCaptureTPAGs[i].used) {
            renderer->dynamicCaptureTPAGs[i].ownerSpriteIndex = -1;
            return (int32_t) (renderer->baseTPAGCount + (uint32_t) i);
        }
    }

    uint32_t dynamicIndex = renderer->dynamicCaptureTPAGCount;
    uint32_t newDynamicCount = dynamicIndex + 1u;
    renderer->dynamicCaptureTPAGs = safeRealloc(
        renderer->dynamicCaptureTPAGs,
        (size_t) newDynamicCount * sizeof(N3DSDynamicCaptureTPAG)
    );
    memset(&renderer->dynamicCaptureTPAGs[dynamicIndex], 0, sizeof(renderer->dynamicCaptureTPAGs[dynamicIndex]));
    renderer->dynamicCaptureTPAGs[dynamicIndex].ownerSpriteIndex = -1;
    renderer->dynamicCaptureTPAGCount = newDynamicCount;

    DataWin* dw = renderer->base.dataWin;
    uint32_t newTPAGCount = renderer->baseTPAGCount + renderer->dynamicCaptureTPAGCount;
    dw->tpag.items = safeRealloc(dw->tpag.items, (size_t) newTPAGCount * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newTPAGCount - 1u], 0, sizeof(TexturePageItem));
    dw->tpag.count = newTPAGCount;
    return (int32_t) (newTPAGCount - 1u);
}

static bool N3DSRenderer_tryLoadDirectSpriteImage(N3DSRenderer* renderer, int32_t spriteIndex, int32_t frameIndex, C2D_Image** outImage) {
    if (renderer == NULL || outImage == NULL) return false;
    if (spriteIndex < 0 || (uint32_t) spriteIndex >= renderer->directSpriteAssetCount) return false;

    N3DSDirectSpriteAsset* spriteAsset = &renderer->directSpriteAssets[spriteIndex];
    if (frameIndex < 0 || (uint32_t) frameIndex >= spriteAsset->frameCount) return false;

    N3DSDirectTextureAsset* sheetAsset = &spriteAsset->sheetAsset;
    bool preferFrameFallback = (spriteIndex == 24);
    const char* spriteName = NULL;
    bool preferNamedButtonOverride = false;

    if (renderer->base.dataWin != NULL &&
        (uint32_t) spriteIndex < renderer->base.dataWin->sprt.count) {
        spriteName = renderer->base.dataWin->sprt.sprites[spriteIndex].name;
        preferNamedButtonOverride = Renderer_isBattleButtonSpriteName(spriteName);
        if (spriteName != NULL &&
            (strstr(spriteName, "spr_mainchara") != NULL ||
             strstr(spriteName, "spr_f_mainchara") != NULL ||
             strstr(spriteName, "spr_chara") != NULL ||
             strstr(spriteName, "spr_heart") != NULL ||
             strstr(spriteName, "heart_") != NULL ||
             strstr(spriteName, "spr_soul") != NULL ||
             strstr(spriteName, "soul") != NULL)) {
            preferFrameFallback = true;
            spriteAsset->useFrameFallback = true;
        }
    }

    if (preferFrameFallback) spriteAsset->useFrameFallback = true;

    bool hasLoadedDirectSheet = sheetAsset->ready || (sheetAsset->blobData != NULL && sheetAsset->blobSize > 0);
    bool hasLoadedDirectFrame = false;
    if (spriteAsset->frameAssets != NULL) {
        N3DSDirectTextureAsset* existingFrameAsset = &spriteAsset->frameAssets[frameIndex];
        hasLoadedDirectFrame = existingFrameAsset->ready || (existingFrameAsset->blobData != NULL && existingFrameAsset->blobSize > 0);
    }
    bool allowColdDirectLoad = preferNamedButtonOverride;
    if (!allowColdDirectLoad && !hasLoadedDirectSheet && !hasLoadedDirectFrame) {
        return false;
    }

    if (preferNamedButtonOverride && spriteAsset->frameAssets != NULL) {
        N3DSDirectTextureAsset* frameAsset = &spriteAsset->frameAssets[frameIndex];
        if (frameAsset->ready) {
            frameAsset->lastUsedStamp = ++renderer->directAssetUseCounter;
            frameAsset->lastUsedFrame = renderer->frameSequence;
            *outImage = &frameAsset->image;
            return true;
        }
        if (!frameAsset->failed && spriteName != NULL) {
            char overridePath[256];
            const char* overrideBaseName = spriteName;
            if (strcmp(spriteName, "spr_talkbt") == 0) {
                overrideBaseName = "spr_actbt_center";
            } else if (strcmp(spriteName, "spr_talkbt_hollow") == 0) {
                overrideBaseName = "spr_actbt_center_hole";
            }
            snprintf(overridePath, sizeof(overridePath), "button_overrides/%s_%d.t3x", overrideBaseName, (int) frameIndex);
            if (N3DSRenderer_loadDirectTextureAsset(renderer, frameAsset, overridePath)) {
                *outImage = &frameAsset->image;
                return true;
            }
            snprintf(overridePath, sizeof(overridePath), "button_overrides/%s.t3x", overrideBaseName);
            if (frameIndex == 0 && N3DSRenderer_loadDirectTextureAsset(renderer, frameAsset, overridePath)) {
                *outImage = &frameAsset->image;
                return true;
            }
        }
    }

    if (preferFrameFallback && spriteAsset->frameAssets != NULL) {
        N3DSDirectTextureAsset* frameAsset = &spriteAsset->frameAssets[frameIndex];
        if (frameAsset->ready) {
            frameAsset->lastUsedStamp = ++renderer->directAssetUseCounter;
            frameAsset->lastUsedFrame = renderer->frameSequence;
            *outImage = &frameAsset->image;
            return true;
        }
        if (!frameAsset->failed) {
            char fallbackPath[256];
            snprintf(fallbackPath, sizeof(fallbackPath), "sprites/spr_%05d_frame_%05d.t3x", (int) spriteIndex, (int) frameIndex);
            if (N3DSRenderer_loadDirectTextureAsset(renderer, frameAsset, fallbackPath)) {
                *outImage = &frameAsset->image;
                return true;
            }
        }
    }

    if (sheetAsset->ready && (uint32_t) frameIndex < spriteAsset->sheetFrameCount) {
        sheetAsset->lastUsedStamp = ++renderer->directAssetUseCounter;
        sheetAsset->lastUsedFrame = renderer->frameSequence;
        N3DSRenderer_buildSheetFrameImages(spriteAsset);
        if (spriteAsset->sheetFrameImages != NULL) {
            *outImage = &spriteAsset->sheetFrameImages[frameIndex];
            return true;
        }
        *outImage = &sheetAsset->image;
        return true;
    }

    if (!spriteAsset->sheetLoadAttempted) {
        char path[256];
        spriteAsset->sheetLoadAttempted = true;
        snprintf(path, sizeof(path), "sprites/spr_%05d.t3x", (int) spriteIndex);
        if (N3DSRenderer_loadDirectTextureAsset(renderer, sheetAsset, path) && sheetAsset->sheet != NULL) {
            spriteAsset->sheetFrameCount = (uint32_t) C2D_SpriteSheetCount(sheetAsset->sheet);
            if ((uint32_t) frameIndex < spriteAsset->sheetFrameCount) {
                sheetAsset->lastUsedStamp = ++renderer->directAssetUseCounter;
                sheetAsset->lastUsedFrame = renderer->frameSequence;
                N3DSRenderer_buildSheetFrameImages(spriteAsset);
                if (spriteAsset->sheetFrameImages != NULL) {
                    *outImage = &spriteAsset->sheetFrameImages[frameIndex];
                    return true;
                }
                *outImage = &sheetAsset->image;
                return true;
            }
        }
        spriteAsset->useFrameFallback = true;
    }

    if (!spriteAsset->useFrameFallback || spriteAsset->frameAssets == NULL) return false;

    N3DSDirectTextureAsset* frameAsset = &spriteAsset->frameAssets[frameIndex];
    if (frameAsset->ready) {
        frameAsset->lastUsedStamp = ++renderer->directAssetUseCounter;
        frameAsset->lastUsedFrame = renderer->frameSequence;
        *outImage = &frameAsset->image;
        return true;
    }
    if (frameAsset->failed) return false;

    char fallbackPath[256];
    snprintf(fallbackPath, sizeof(fallbackPath), "sprites/spr_%05d_frame_%05d.t3x", (int) spriteIndex, (int) frameIndex);
    if (!N3DSRenderer_loadDirectTextureAsset(renderer, frameAsset, fallbackPath)) return false;
    *outImage = &frameAsset->image;
    return true;
}

static bool N3DSRenderer_tryLoadDirectBackgroundImage(N3DSRenderer* renderer, int32_t backgroundIndex, C2D_Image** outImage) {
    if (renderer == NULL || outImage == NULL) return false;
    if (backgroundIndex < 0 || (uint32_t) backgroundIndex >= renderer->directBackgroundAssetCount) return false;

    N3DSDirectTextureAsset* asset = &renderer->directBackgroundAssets[backgroundIndex];
    if (!asset->ready && (asset->blobData == NULL || asset->blobSize == 0)) {
        return false;
    }
    char path[256];
    snprintf(path, sizeof(path), "backgrounds/bg_%05d.t3x", (int) backgroundIndex);
    if (!N3DSRenderer_loadDirectTextureAsset(renderer, asset, path)) return false;
    *outImage = &asset->image;
    return true;
}

static bool N3DSRenderer_tryResolveDirectFontImage(N3DSRenderer* renderer, int32_t fontIndex, C2D_Image* outImage, Tex3DS_SubTexture* outSubtex) {
#if !N3DS_ENABLE_DIRECT_ASSETS
    (void) renderer;
    (void) fontIndex;
    (void) outImage;
    (void) outSubtex;
    return false;
#endif
    if (renderer == NULL || outImage == NULL || outSubtex == NULL) return false;
    if (fontIndex < 0 || (uint32_t) fontIndex >= renderer->directFontAssetCount) return false;

    N3DSDirectTextureAsset* asset = &renderer->directFontAssets[fontIndex];
    char path[256];
    snprintf(path, sizeof(path), "fonts/font_%05d.t3x", (int) fontIndex);
    if (!N3DSRenderer_loadDirectTextureAsset(renderer, asset, path)) return false;
    if (asset->image.tex == NULL || asset->image.subtex == NULL) return false;

    *outImage = asset->image;
    *outSubtex = *asset->image.subtex;
    outImage->subtex = outSubtex;
    return true;
}

static bool N3DSRenderer_cropDirectSpriteImage(
    const C2D_Image* directImage,
    const TexturePageItem* tpag,
    C2D_Image* outImage,
    Tex3DS_SubTexture* outSubtex,
    float* outDrawX,
    float* outDrawY,
    float x,
    float y,
    float originX,
    float originY,
    float xscale,
    float yscale
) {
    if (directImage == NULL || directImage->tex == NULL || directImage->subtex == NULL ||
        tpag == NULL || outImage == NULL || outSubtex == NULL || outDrawX == NULL || outDrawY == NULL) {
        return false;
    }

    int32_t frameW = (int32_t) directImage->subtex->width;
    int32_t frameH = (int32_t) directImage->subtex->height;
    int32_t cropX = (int32_t) tpag->targetX;
    int32_t cropY = (int32_t) tpag->targetY;
    int32_t cropW = (int32_t) tpag->sourceWidth;
    int32_t cropH = (int32_t) tpag->sourceHeight;
    if (frameW <= 0 || frameH <= 0 || cropW <= 0 || cropH <= 0) return false;

    int32_t drawTargetX = cropX;
    int32_t drawTargetY = cropY;
    if (cropX < 0) {
        cropW += cropX;
        cropX = 0;
        drawTargetX = 0;
    }
    if (cropY < 0) {
        cropH += cropY;
        cropY = 0;
        drawTargetY = 0;
    }
    if (cropX + cropW > frameW) cropW = frameW - cropX;
    if (cropY + cropH > frameH) cropH = frameH - cropY;
    if (cropW <= 0 || cropH <= 0) return false;

    const Tex3DS_SubTexture* baseSubtex = directImage->subtex;
    float baseW = (float) baseSubtex->width;
    float baseH = (float) baseSubtex->height;
    if (baseW <= 0.0f || baseH <= 0.0f) return false;

    *outSubtex = *baseSubtex;
    outSubtex->width = (uint16_t) cropW;
    outSubtex->height = (uint16_t) cropH;
    outSubtex->left = baseSubtex->left + (baseSubtex->right - baseSubtex->left) * ((float) cropX / baseW);
    outSubtex->right = baseSubtex->left + (baseSubtex->right - baseSubtex->left) * ((float) (cropX + cropW) / baseW);
    outSubtex->top = baseSubtex->top + (baseSubtex->bottom - baseSubtex->top) * ((float) cropY / baseH);
    outSubtex->bottom = baseSubtex->top + (baseSubtex->bottom - baseSubtex->top) * ((float) (cropY + cropH) / baseH);

    *outImage = *directImage;
    outImage->subtex = outSubtex;
    *outDrawX = x + ((float) drawTargetX - originX) * xscale;
    *outDrawY = y + ((float) drawTargetY - originY) * yscale;
    return true;
}

static bool N3DSRenderer_tryDrawDirectMappedSprite(Renderer* base, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
#if !N3DS_ENABLE_DIRECT_ASSETS
    (void) base;
    (void) tpagIndex;
    (void) x;
    (void) y;
    (void) originX;
    (void) originY;
    (void) xscale;
    (void) yscale;
    (void) angleDeg;
    (void) color;
    (void) alpha;
    return false;
#endif
    if (base == NULL) return false;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    C2D_Image* directImage = NULL;
    bool isBackgroundImage = false;

    if (renderer->lastDirectTPAGIndex == tpagIndex &&
        renderer->lastDirectTPAGImage != NULL &&
        renderer->lastDirectTPAGImage->tex != NULL &&
        renderer->lastDirectTPAGImage->subtex != NULL) {
        directImage = renderer->lastDirectTPAGImage;
    }

    if (directImage == NULL &&
        renderer->tpagToSpriteIndex != NULL && renderer->tpagToSpriteFrameIndex != NULL &&
        tpagIndex >= 0 && (uint32_t) tpagIndex < renderer->base.dataWin->tpag.count) {
        int32_t spriteIndex = renderer->tpagToSpriteIndex[tpagIndex];
        int32_t frameIndex = renderer->tpagToSpriteFrameIndex[tpagIndex];
        if (spriteIndex >= 0 && frameIndex >= 0 && N3DSRenderer_tryLoadDirectSpriteImage(renderer, spriteIndex, frameIndex, &directImage)) {
            renderer->lastDirectTPAGIndex = tpagIndex;
            renderer->lastDirectTPAGImage = directImage;
        }
    }

    if (directImage == NULL &&
        renderer->tpagToBackgroundIndex != NULL &&
        tpagIndex >= 0 && (uint32_t) tpagIndex < renderer->base.dataWin->tpag.count) {
        int32_t backgroundIndex = renderer->tpagToBackgroundIndex[tpagIndex];
        if (backgroundIndex >= 0 && N3DSRenderer_tryLoadDirectBackgroundImage(renderer, backgroundIndex, &directImage)) {
            isBackgroundImage = true;
            renderer->lastDirectTPAGIndex = tpagIndex;
            renderer->lastDirectTPAGImage = directImage;
        }
    }

    if (directImage == NULL) return false;

    if (!isBackgroundImage &&
        renderer->tpagToBackgroundIndex != NULL &&
        tpagIndex >= 0 &&
        (uint32_t) tpagIndex < renderer->base.dataWin->tpag.count &&
        renderer->tpagToBackgroundIndex[tpagIndex] >= 0 &&
        renderer->tpagToSpriteIndex[tpagIndex] < 0) {
        isBackgroundImage = true;
    }

    C2D_Image croppedImage;
    Tex3DS_SubTexture croppedSubtex;
    C2D_Image* drawImage = directImage;
    float drawX = x - originX * xscale;
    float drawY = y - originY * yscale;
    if (!isBackgroundImage && tpagIndex >= 0 && (uint32_t) tpagIndex < renderer->base.dataWin->tpag.count) {
        TexturePageItem* tpag = &renderer->base.dataWin->tpag.items[tpagIndex];
        if (N3DSRenderer_cropDirectSpriteImage(
                directImage,
                tpag,
                &croppedImage,
                &croppedSubtex,
                &drawX,
                &drawY,
                x,
                y,
                originX,
                originY,
                xscale,
                yscale)) {
            drawImage = &croppedImage;
        }
    }

    renderer->frameDirectSpriteHits++;
    if (fabsf(angleDeg) < 0.001f && xscale > 0.0f && yscale > 0.0f) {
        N3DSRenderer_drawImageFast(base, drawImage, drawX, drawY, xscale, yscale, color, alpha);
    } else {
        float pivotX = x - drawX;
        float pivotY = y - drawY;
        N3DSRenderer_drawImage(
            base,
            drawImage,
            drawX,
            drawY,
            (float) drawImage->subtex->width * xscale,
            (float) drawImage->subtex->height * yscale,
            pivotX,
            pivotY,
            angleDeg,
            color,
            alpha
        );
    }
    return true;
}

static bool N3DSRenderer_loadAtlas(N3DSRenderer* renderer) {
    char assetPath[256];
    FILE* atlasFile = N3DSRenderer_openAssetFile(renderer, "atlas.bin", assetPath, sizeof(assetPath));
    if (atlasFile == NULL) {
        N3DSRenderer_setStartupError(renderer, "Missing gfx/atlas.bin on SD or ROMFS");
        fprintf(stderr, "N3DS: missing atlas.bin in sdmc:/3ds/cinnamon/gfx or romfs:/gfx\n");
        return false;
    }
    fprintf(stderr, "N3DS: loading atlas from %s\n", assetPath);

    fseek(atlasFile, 0, SEEK_END);
    long atlasSize = ftell(atlasFile);
    fseek(atlasFile, 0, SEEK_SET);
    if (atlasSize < 24) {
        fclose(atlasFile);
        N3DSRenderer_setStartupError(renderer, "gfx/atlas.bin is too small or corrupt");
        return false;
    }

    uint8_t header[24];
    if (fread(header, 1, sizeof(header), atlasFile) != sizeof(header)) {
        fclose(atlasFile);
        N3DSRenderer_setStartupError(renderer, "Failed to read gfx/atlas.bin header");
        return false;
    }

    uint16_t version = N3DS_readU16(header + 4);
    if (N3DS_readU32(header) != N3DS_ATLAS_MAGIC ||
        (version != N3DS_ATLAS_VERSION_RAW &&
         version != N3DS_ATLAS_VERSION_T3X &&
         version != N3DS_ATLAS_VERSION_FRAGMENTED &&
         version != N3DS_ATLAS_VERSION_FRAGMENTED_TILES &&
         version != N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS &&
         version != N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT &&
         version != N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT &&
         version != N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED)) {
        fclose(atlasFile);
        N3DSRenderer_setStartupError(renderer, "gfx/atlas.bin has an invalid header");
        fprintf(stderr, "N3DS: invalid atlas header\n");
        return false;
    }

    renderer->atlasVersion = version;
    renderer->atlasTextureFormat = N3DS_TEXFMT_RGBA5551;
    renderer->atlasPageCount = N3DS_readU16(header + 6);
    renderer->atlasItemCount = N3DS_readU32(header + 8);
    if (version == N3DS_ATLAS_VERSION_FRAGMENTED || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILES || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED) {
        renderer->atlasFragmentCount = N3DS_readU32(header + 12);
        if (version == N3DS_ATLAS_VERSION_FRAGMENTED_TILES || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED) {
            renderer->tileEntryCount = N3DS_readU32(header + 16);
            if (version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED) {
                renderer->atlasTextureFormat = N3DS_readU32(header + 20);
            }
        }
    }
    fprintf(stderr, "N3DS: atlas version=%u format=%s pages=%u items=%lu fragments=%lu tiles=%lu\n",
        (unsigned int) renderer->atlasVersion,
        N3DSRenderer_getTextureFormatName(renderer->atlasTextureFormat),
        (unsigned int) renderer->atlasPageCount,
        (unsigned long) renderer->atlasItemCount,
        (unsigned long) renderer->atlasFragmentCount,
        (unsigned long) renderer->tileEntryCount);

    renderer->atlasPages = safeCalloc(renderer->atlasPageCount, sizeof(N3DSLoadedAtlasPage));
    if (version == N3DS_ATLAS_VERSION_FRAGMENTED ||
        version == N3DS_ATLAS_VERSION_FRAGMENTED_TILES ||
        version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS ||
        version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT ||
        version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT ||
        version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED) {
        renderer->atlasItemsV3 = safeCalloc(renderer->atlasItemCount, sizeof(N3DSAtlasItemV3));
        renderer->atlasFragments = safeCalloc(renderer->atlasFragmentCount, sizeof(N3DSAtlasFragment));
        if (renderer->tileEntryCount > 0) {
            renderer->tileEntries = safeCalloc(renderer->tileEntryCount, sizeof(N3DSTileAtlasEntry));
        }
    } else {
        renderer->atlasItems = safeCalloc(renderer->atlasItemCount, sizeof(N3DSAtlasItem));
    }

    size_t cursor = 12;
    if (version == N3DS_ATLAS_VERSION_FRAGMENTED) cursor = 16;
    if (version == N3DS_ATLAS_VERSION_FRAGMENTED_TILES || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS) cursor = 20;
    if (version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED) cursor = 24;

    size_t metadataSize = cursor;
    if (version == N3DS_ATLAS_VERSION_RAW) metadataSize += (size_t) renderer->atlasPageCount * 12u;
    else if (version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT) metadataSize += (size_t) renderer->atlasPageCount * 8u;
    else if (version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED) metadataSize += (size_t) renderer->atlasPageCount * 16u;
    else metadataSize += (size_t) renderer->atlasPageCount * 4u;

    if (version == N3DS_ATLAS_VERSION_FRAGMENTED ||
        version == N3DS_ATLAS_VERSION_FRAGMENTED_TILES ||
        version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS ||
        version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT ||
        version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT ||
        version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED) {
        metadataSize += (size_t) renderer->atlasItemCount * 10u;
        metadataSize += (size_t) renderer->atlasFragmentCount * 14u;
        if (version == N3DS_ATLAS_VERSION_FRAGMENTED_TILES) metadataSize += (size_t) renderer->tileEntryCount * 20u;
        else if (version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS ||
                 version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT ||
                 version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT ||
                 version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED) metadataSize += (size_t) renderer->tileEntryCount * 26u;
    } else {
        metadataSize += (size_t) renderer->atlasItemCount * 10u;
    }

    if ((long) metadataSize > atlasSize) {
        fclose(atlasFile);
        N3DSRenderer_setStartupError(renderer, "gfx/atlas.bin metadata is truncated");
        return false;
    }

    fseek(atlasFile, 0, SEEK_SET);
    uint8_t* blob = safeMalloc(metadataSize);
    if (fread(blob, 1, metadataSize, atlasFile) != metadataSize) {
        free(blob);
        fclose(atlasFile);
        N3DSRenderer_setStartupError(renderer, "Failed to read gfx/atlas.bin metadata");
        return false;
    }

    N3DSAtlasPageInfo* pageInfos = safeCalloc(renderer->atlasPageCount, sizeof(N3DSAtlasPageInfo));
    repeat(renderer->atlasPageCount, i) {
        pageInfos[i].width = N3DS_readU16(blob + cursor + 0);
        pageInfos[i].height = N3DS_readU16(blob + cursor + 2);
        pageInfos[i].textureFormat = renderer->atlasTextureFormat;
        if (version == N3DS_ATLAS_VERSION_RAW) {
            pageInfos[i].dataOffset = N3DS_readU32(blob + cursor + 4);
            pageInfos[i].dataSize = N3DS_readU32(blob + cursor + 8);
            cursor += 12;
        } else if (version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED) {
            pageInfos[i].textureFormat = N3DS_readU32(blob + cursor + 4);
            pageInfos[i].dataOffset = N3DS_readU32(blob + cursor + 8);
            pageInfos[i].dataSize = N3DS_readU32(blob + cursor + 12);
            cursor += 16;
        } else if (version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT) {
            pageInfos[i].textureFormat = N3DS_readU32(blob + cursor + 4);
            cursor += 8;
        } else {
            cursor += 4;
        }
    }
    if (version == N3DS_ATLAS_VERSION_FRAGMENTED || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILES || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED) {
        repeat(renderer->atlasItemCount, i) {
            renderer->atlasItemsV3[i].width = N3DS_readU16(blob + cursor + 0);
            renderer->atlasItemsV3[i].height = N3DS_readU16(blob + cursor + 2);
            renderer->atlasItemsV3[i].fragmentStart = N3DS_readU32(blob + cursor + 4);
            renderer->atlasItemsV3[i].fragmentCount = N3DS_readU16(blob + cursor + 8);
            cursor += 10;
        }
        repeat(renderer->atlasFragmentCount, i) {
            renderer->atlasFragments[i].atlasId = N3DS_readU16(blob + cursor + 0);
            renderer->atlasFragments[i].x = N3DS_readU16(blob + cursor + 2);
            renderer->atlasFragments[i].y = N3DS_readU16(blob + cursor + 4);
            renderer->atlasFragments[i].width = N3DS_readU16(blob + cursor + 6);
            renderer->atlasFragments[i].height = N3DS_readU16(blob + cursor + 8);
            renderer->atlasFragments[i].sourceX = N3DS_readU16(blob + cursor + 10);
            renderer->atlasFragments[i].sourceY = N3DS_readU16(blob + cursor + 12);
            cursor += 14;
        }
        if (version == N3DS_ATLAS_VERSION_FRAGMENTED_TILES || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED) {
            repeat(renderer->tileEntryCount, i) {
                renderer->tileEntries[i].bgDef = (int16_t) N3DS_readU16(blob + cursor + 0);
                renderer->tileEntries[i].srcX = N3DS_readU16(blob + cursor + 2);
                renderer->tileEntries[i].srcY = N3DS_readU16(blob + cursor + 4);
                renderer->tileEntries[i].srcW = N3DS_readU16(blob + cursor + 6);
                renderer->tileEntries[i].srcH = N3DS_readU16(blob + cursor + 8);
                renderer->tileEntries[i].atlasId = N3DS_readU16(blob + cursor + 10);
                renderer->tileEntries[i].x = N3DS_readU16(blob + cursor + 12);
                renderer->tileEntries[i].y = N3DS_readU16(blob + cursor + 14);
                renderer->tileEntries[i].width = N3DS_readU16(blob + cursor + 16);
                renderer->tileEntries[i].height = N3DS_readU16(blob + cursor + 18);
                if (version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_FMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED) {
                    renderer->tileEntries[i].fragmentStart = N3DS_readU32(blob + cursor + 20);
                    renderer->tileEntries[i].fragmentCount = N3DS_readU16(blob + cursor + 24);
                    cursor += 26;
                } else {
                    renderer->tileEntries[i].fragmentStart = UINT32_MAX;
                    renderer->tileEntries[i].fragmentCount = 0;
                    cursor += 20;
                }
            }
        }
    } else {
        repeat(renderer->atlasItemCount, i) {
            renderer->atlasItems[i].atlasId = N3DS_readU16(blob + cursor + 0);
            renderer->atlasItems[i].x = N3DS_readU16(blob + cursor + 2);
            renderer->atlasItems[i].y = N3DS_readU16(blob + cursor + 4);
            renderer->atlasItems[i].width = N3DS_readU16(blob + cursor + 6);
            renderer->atlasItems[i].height = N3DS_readU16(blob + cursor + 8);
            cursor += 10;
        }
    }
    free(blob);
    if (N3DSRenderer_isPackedAtlasVersion(version)) renderer->packedAtlasFile = atlasFile;
    else fclose(atlasFile);

    repeat(renderer->atlasPageCount, i) {
        N3DSLoadedAtlasPage* page = &renderer->atlasPages[i];
        page->width = pageInfos[i].width;
        page->height = pageInfos[i].height;
        page->textureFormat = pageInfos[i].textureFormat;
        page->dataOffset = pageInfos[i].dataOffset;
        page->dataSize = pageInfos[i].dataSize;
    }
    free(pageInfos);
    if (renderer->tileEntryCount > 0 && renderer->tileEntries != NULL) {
        repeat(renderer->tileEntryCount, i) {
            N3DSTileLookupKey key = {
                .bgDef = renderer->tileEntries[i].bgDef,
                .srcX = renderer->tileEntries[i].srcX,
                .srcY = renderer->tileEntries[i].srcY,
                .srcW = renderer->tileEntries[i].srcW,
                .srcH = renderer->tileEntries[i].srcH,
            };
            hmput(renderer->tileEntryMap, key, (uint32_t) i);
        }
    }
    fprintf(stderr, "N3DS: atlas metadata ready for %u pages; using lazy page loading (%u resident max)\n",
        (unsigned int) renderer->atlasPageCount,
        (unsigned int) renderer->residentAtlasPageLimit);

    if (renderer->atlasTextureFormat == N3DS_TEXFMT_ETC1A4) {
        renderer->residentAtlasPageLimit = renderer->isNew3DS ? N3DS_MAX_RESIDENT_ATLAS_PAGES_NEW3DS_ETC1A4 : N3DS_MAX_RESIDENT_ATLAS_PAGES_OLD3DS_ETC1A4;
        renderer->prewarmGpuPageBudget = renderer->isNew3DS ? N3DS_PREWARM_GPU_PAGE_BUDGET_NEW3DS_ETC1A4 : N3DS_PREWARM_GPU_PAGE_BUDGET_OLD3DS_ETC1A4;
        fprintf(stderr, "N3DS: ETC1A4 atlas detected; residentLimit=%u prewarm=%u\n",
            (unsigned int) renderer->residentAtlasPageLimit,
            (unsigned int) renderer->prewarmGpuPageBudget);
    } else if (renderer->atlasTextureFormat == N3DS_TEXFMT_INDEXED8) {
        fprintf(stderr, "N3DS: indexed8 atlas detected; using temporary CPU expansion to rgba5551 until shader-backed palette sampling lands\n");
    } else if (renderer->atlasTextureFormat == N3DS_TEXFMT_HYBRID || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PAGEFMT || version == N3DS_ATLAS_VERSION_FRAGMENTED_TILE_FRAGMENTS_PACKED) {
        renderer->residentAtlasPageLimit = renderer->isNew3DS ? N3DS_MAX_RESIDENT_ATLAS_PAGES_NEW3DS_ETC1A4 : N3DS_MAX_RESIDENT_ATLAS_PAGES_OLD3DS_ETC1A4;
        renderer->prewarmGpuPageBudget = renderer->isNew3DS ? N3DS_PREWARM_GPU_PAGE_BUDGET_NEW3DS_ETC1A4 : N3DS_PREWARM_GPU_PAGE_BUDGET_OLD3DS_ETC1A4;
        fprintf(stderr, "N3DS: hybrid atlas detected; mixed page formats enabled\n");
    }
    return true;
}

static bool N3DSRenderer_resolveImage(N3DSRenderer* renderer, int32_t tpagIndex, C2D_Image* outImage, Tex3DS_SubTexture* subtex) {
    if (N3DSRenderer_isFragmentedAtlasVersion(renderer->atlasVersion)) return false;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= renderer->atlasItemCount) return false;
    N3DSAtlasItem* item = &renderer->atlasItems[tpagIndex];
    if ((uint32_t) item->atlasId >= renderer->atlasPageCount) return false;
    if (!N3DSRenderer_ensurePageLoaded(renderer, item->atlasId)) return false;
    N3DSLoadedAtlasPage* page = &renderer->atlasPages[item->atlasId];
    if (!page->ready || item->width == 0 || item->height == 0) return false;

    N3DSRenderer_fillSubTexture(subtex, page, item->x, item->y, item->width, item->height);
    outImage->tex = &page->texture;
    outImage->subtex = subtex;
    return true;
}

static bool N3DSRenderer_getPageIndexForTPAG(N3DSRenderer* renderer, int32_t tpagIndex, uint32_t* outPageIndex) {
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= renderer->atlasItemCount) return false;

    if (N3DSRenderer_isFragmentedAtlasVersion(renderer->atlasVersion)) {
        N3DSAtlasItemV3* item = &renderer->atlasItemsV3[tpagIndex];
        if (item->fragmentCount == 0) return false;
        N3DSAtlasFragment* fragment = &renderer->atlasFragments[item->fragmentStart];
        if ((uint32_t) fragment->atlasId >= renderer->atlasPageCount) return false;
        *outPageIndex = fragment->atlasId;
        return true;
    }

    N3DSAtlasItem* item = &renderer->atlasItems[tpagIndex];
    if ((uint32_t) item->atlasId >= renderer->atlasPageCount) return false;
    *outPageIndex = item->atlasId;
    return true;
}

static N3DSTileAtlasEntry* N3DSRenderer_findTileEntryByKey(N3DSRenderer* renderer, int32_t backgroundIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, uint32_t* outEntryIndex) {
    if (renderer == NULL || renderer->tileEntryMap == NULL || renderer->tileEntries == NULL) return NULL;
    if (backgroundIndex < 0 || srcW <= 0 || srcH <= 0) return NULL;

    N3DSTileLookupKey key = {
        .bgDef = (int16_t) backgroundIndex,
        .srcX = (uint16_t) srcX,
        .srcY = (uint16_t) srcY,
        .srcW = (uint16_t) srcW,
        .srcH = (uint16_t) srcH,
    };
    ptrdiff_t mapIndex = hmgeti(renderer->tileEntryMap, key);
    if (mapIndex < 0) return NULL;
    uint32_t entryIndex = renderer->tileEntryMap[mapIndex].value;
    if (entryIndex >= renderer->tileEntryCount) return NULL;
    if (outEntryIndex != NULL) *outEntryIndex = entryIndex;
    return &renderer->tileEntries[entryIndex];
}

static N3DSTileAtlasEntry* N3DSRenderer_findTileEntry(N3DSRenderer* renderer, RoomTile* tile) {
    if (renderer == NULL || tile == NULL) return NULL;
    return N3DSRenderer_findTileEntryByKey(
        renderer,
        tile->backgroundDefinition,
        tile->sourceX,
        tile->sourceY,
        (int32_t) tile->width,
        (int32_t) tile->height,
        NULL
    );
}

static void N3DSRenderer_prewarmTileEntry(N3DSRenderer* renderer, bool* seenPages, RoomTile* tile) {
    N3DSTileAtlasEntry* entry = N3DSRenderer_findTileEntry(renderer, tile);
    if (entry == NULL) return;
    if (entry->fragmentCount > 0) {
        repeat(entry->fragmentCount, i) {
            uint32_t fragmentIndex = entry->fragmentStart + (uint32_t) i;
            if (fragmentIndex >= renderer->atlasFragmentCount) break;
            N3DSRenderer_prewarmPage(renderer, seenPages, renderer->atlasFragments[fragmentIndex].atlasId);
        }
    } else if (entry->atlasId < renderer->atlasPageCount) {
        N3DSRenderer_prewarmPage(renderer, seenPages, entry->atlasId);
    }
}

static bool N3DSRenderer_prewarmPageBlobOnly(N3DSRenderer* renderer, bool* seenPages, uint32_t pageIndex, uint32_t* remainingBlobBytes) {
    if (renderer == NULL || seenPages == NULL) return false;
    if (pageIndex >= renderer->atlasPageCount || seenPages[pageIndex]) return false;

    N3DSLoadedAtlasPage* page = &renderer->atlasPages[pageIndex];
    seenPages[pageIndex] = true;

    if (page->t3xData != NULL && page->t3xSize > 0) {
        page->blobLastUsedStamp = ++renderer->blobUseCounter;
        return true;
    }

    uint32_t blobSize = page->dataSize;
    if (blobSize == 0 && page->t3xSize > 0) blobSize = page->t3xSize;
    if (remainingBlobBytes != NULL && blobSize > 0 && *remainingBlobBytes < blobSize) {
        return false;
    }

    if (!N3DSRenderer_ensurePageBlobLoaded(renderer, pageIndex)) return false;

    if (remainingBlobBytes != NULL && blobSize > 0) {
        if (*remainingBlobBytes > blobSize) *remainingBlobBytes -= blobSize;
        else *remainingBlobBytes = 0;
    }
    return true;
}

static void N3DSRenderer_prewarmTileEntryBlobOnly(N3DSRenderer* renderer, bool* seenPages, const N3DSTileAtlasEntry* entry, uint32_t* remainingBlobBytes) {
    if (renderer == NULL || seenPages == NULL || entry == NULL || remainingBlobBytes == NULL || *remainingBlobBytes == 0) return;

    if (entry->fragmentCount > 0) {
        repeat(entry->fragmentCount, i) {
            uint32_t fragmentIndex = entry->fragmentStart + (uint32_t) i;
            if (fragmentIndex >= renderer->atlasFragmentCount) break;
            (void) N3DSRenderer_prewarmPageBlobOnly(renderer, seenPages, renderer->atlasFragments[fragmentIndex].atlasId, remainingBlobBytes);
            if (*remainingBlobBytes == 0) break;
        }
    } else if (entry->atlasId < renderer->atlasPageCount) {
        (void) N3DSRenderer_prewarmPageBlobOnly(renderer, seenPages, entry->atlasId, remainingBlobBytes);
    }
}

static void N3DSRenderer_prewarmTPAGBlobOnly(N3DSRenderer* renderer, bool* seenPages, int32_t tpagIndex, uint32_t* remainingBlobBytes) {
    if (renderer == NULL || seenPages == NULL || remainingBlobBytes == NULL || *remainingBlobBytes == 0) return;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= renderer->atlasItemCount) return;

    if (N3DSRenderer_isFragmentedAtlasVersion(renderer->atlasVersion)) {
        N3DSAtlasItemV3* item = &renderer->atlasItemsV3[tpagIndex];
        repeat(item->fragmentCount, i) {
            uint32_t fragmentIndex = item->fragmentStart + (uint32_t) i;
            if (fragmentIndex >= renderer->atlasFragmentCount) break;
            (void) N3DSRenderer_prewarmPageBlobOnly(renderer, seenPages, renderer->atlasFragments[fragmentIndex].atlasId, remainingBlobBytes);
            if (*remainingBlobBytes == 0) break;
        }
        return;
    }

    (void) N3DSRenderer_prewarmPageBlobOnly(renderer, seenPages, renderer->atlasItems[tpagIndex].atlasId, remainingBlobBytes);
}

static void N3DSRenderer_prewarmPage(N3DSRenderer* renderer, bool* seenPages, uint32_t pageIndex) {
    if (pageIndex >= renderer->atlasPageCount || seenPages[pageIndex]) return;
    seenPages[pageIndex] = true;
    if (!N3DSRenderer_ensurePageBlobLoaded(renderer, pageIndex)) return;
    if (renderer->residentAtlasPageCount < renderer->prewarmGpuPageBudget) {
        (void) N3DSRenderer_ensurePageLoaded(renderer, pageIndex);
    }
}

static void N3DSRenderer_prewarmTPAG(N3DSRenderer* renderer, bool* seenPages, int32_t tpagIndex) {
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= renderer->atlasItemCount) return;

    if (N3DSRenderer_isFragmentedAtlasVersion(renderer->atlasVersion)) {
        N3DSAtlasItemV3* item = &renderer->atlasItemsV3[tpagIndex];
        repeat(item->fragmentCount, i) {
            uint32_t fragmentIndex = item->fragmentStart + (uint32_t) i;
            if (fragmentIndex >= renderer->atlasFragmentCount) break;
            N3DSRenderer_prewarmPage(renderer, seenPages, renderer->atlasFragments[fragmentIndex].atlasId);
        }
        return;
    }

    N3DSRenderer_prewarmPage(renderer, seenPages, renderer->atlasItems[tpagIndex].atlasId);
}

static void N3DSRenderer_freeCachedTextLayout(N3DSCachedTextLayout* layout) {
    free(layout->text);
    free(layout->glyphs);
    memset(layout, 0, sizeof(*layout));
    layout->fontIndex = -1;
}

static void N3DSRenderer_appendCachedTextLayoutSuffix(N3DSCachedTextLayout* layout, Font* font, const char* suffix, int32_t suffixLen) {
    if (suffixLen <= 0) return;

    int32_t pos = 0;
    float cursorX = layout->appendCursorX;
    float cursorY = layout->appendCursorY;
    uint16_t prevCodepoint = layout->appendPrevCodepoint;
    bool atLineStart = layout->appendAtLineStart;

    while (pos < suffixLen) {
        if (TextUtils_isNewlineChar(suffix[pos])) {
            pos = TextUtils_skipNewline(suffix, pos, suffixLen);
            cursorX = 0.0f;
            cursorY += TextUtils_lineStride(font);
            prevCodepoint = 0;
            atLineStart = true;
            continue;
        }

        uint16_t ch = TextUtils_decodeUtf8(suffix, suffixLen, &pos);
        FontGlyph* glyph = TextUtils_findGlyph(font, ch);
        if (glyph == NULL) {
            prevCodepoint = 0;
            atLineStart = false;
            continue;
        }

        if (!atLineStart && prevCodepoint != 0) {
            FontGlyph* prevGlyph = TextUtils_findGlyph(font, prevCodepoint);
            if (prevGlyph != NULL) {
                cursorX += TextUtils_getKerningOffset(prevGlyph, ch);
            }
        }

        if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
            if (layout->glyphCount >= layout->glyphCapacity) {
                layout->glyphCapacity = layout->glyphCapacity > 0 ? layout->glyphCapacity * 2 : 16;
                layout->glyphs = safeRealloc(layout->glyphs, (size_t) layout->glyphCapacity * sizeof(N3DSCachedTextGlyph));
            }

            N3DSCachedTextGlyph* cachedGlyph = &layout->glyphs[layout->glyphCount++];
            cachedGlyph->localX = cursorX + (float) glyph->offset;
            cachedGlyph->localY = cursorY;
            cachedGlyph->sourceX = glyph->sourceX;
            cachedGlyph->sourceY = glyph->sourceY;
            cachedGlyph->sourceWidth = glyph->sourceWidth;
            cachedGlyph->sourceHeight = glyph->sourceHeight;
        }

        cursorX += (float) glyph->shift;
        prevCodepoint = ch;
        atLineStart = false;
    }

    layout->appendCursorX = cursorX;
    layout->appendCursorY = cursorY;
    layout->appendPrevCodepoint = prevCodepoint;
    layout->appendAtLineStart = atLineStart;
}

static const N3DSCachedTextLayout* N3DSRenderer_getCachedTextLayout(N3DSRenderer* renderer, Font* font, int32_t fontIndex, const char* text) {
    N3DSCachedTextLayout* layout = &renderer->cachedTextLayout;
    if (layout->text != NULL &&
        layout->fontIndex == fontIndex &&
        layout->drawHalign == renderer->base.drawHalign &&
        layout->drawValign == renderer->base.drawValign &&
        strcmp(layout->text, text) == 0) {
        return layout;
    }

    int32_t len = (int32_t) strlen(text);
    bool canAppendSuffix =
        layout->text != NULL &&
        layout->fontIndex == fontIndex &&
        layout->drawHalign == 0 &&
        layout->drawValign == 0 &&
        renderer->base.drawHalign == 0 &&
        renderer->base.drawValign == 0 &&
        len >= 0;

    if (canAppendSuffix) {
        int32_t cachedLen = (int32_t) strlen(layout->text);
        canAppendSuffix =
            len >= cachedLen &&
            memcmp(text, layout->text, (size_t) cachedLen) == 0;

        if (canAppendSuffix) {
            if (len + 1 > layout->textCapacity) {
                int32_t newCapacity = layout->textCapacity > 0 ? layout->textCapacity : 16;
                while (newCapacity < len + 1) newCapacity *= 2;
                layout->text = safeRealloc(layout->text, (size_t) newCapacity);
                layout->textCapacity = newCapacity;
            }

            memcpy(layout->text + cachedLen, text + cachedLen, (size_t) (len - cachedLen + 1));
            N3DSRenderer_appendCachedTextLayoutSuffix(layout, font, text + cachedLen, len - cachedLen);
            return layout;
        }
    }

    N3DSRenderer_freeCachedTextLayout(layout);
    layout->fontIndex = fontIndex;
    layout->drawHalign = renderer->base.drawHalign;
    layout->drawValign = renderer->base.drawValign;
    layout->textCapacity = len + 1;
    layout->text = safeMalloc((size_t) layout->textCapacity);
    memcpy(layout->text, text, (size_t) (len + 1));
    layout->glyphCapacity = len > 0 ? len : 1;
    layout->glyphs = safeCalloc((size_t) layout->glyphCapacity, sizeof(N3DSCachedTextGlyph));

    int32_t lineCount = TextUtils_countLines(text, len);
    float totalHeight = (float) lineCount * TextUtils_lineStride(font);
    float valignOffset = 0.0f;
    if (renderer->base.drawValign == 1) valignOffset = -totalHeight * 0.5f;
    else if (renderer->base.drawValign == 2) valignOffset = -totalHeight;

    layout->appendCursorX = 0.0f;
    layout->appendCursorY = valignOffset - (float) font->ascenderOffset;
    layout->appendPrevCodepoint = 0;
    layout->appendAtLineStart = true;

    if (renderer->base.drawHalign == 0 && renderer->base.drawValign == 0) {
        layout->appendCursorY = -(float) font->ascenderOffset;
        N3DSRenderer_appendCachedTextLayoutSuffix(layout, font, text, len);
        return layout;
    }

    int32_t lineStart = 0;
    while (lineStart <= len) {
        int32_t lineEnd = lineStart;
        while (lineEnd < len && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineEnd - lineStart);
        float cursorX = 0.0f;
        if (renderer->base.drawHalign == 1) cursorX -= lineWidth * 0.5f;
        else if (renderer->base.drawHalign == 2) cursorX -= lineWidth;

        int32_t pos = lineStart;
        while (pos < lineEnd) {
            uint16_t ch = TextUtils_decodeUtf8(text, lineEnd, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == NULL) continue;

            if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                N3DSCachedTextGlyph* cachedGlyph = &layout->glyphs[layout->glyphCount++];
                cachedGlyph->localX = cursorX + (float) glyph->offset;
                cachedGlyph->localY = layout->appendCursorY;
                cachedGlyph->sourceX = glyph->sourceX;
                cachedGlyph->sourceY = glyph->sourceY;
                cachedGlyph->sourceWidth = glyph->sourceWidth;
                cachedGlyph->sourceHeight = glyph->sourceHeight;
            }

            if (pos < lineEnd) {
                int32_t previewPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(text, lineEnd, &previewPos);
                cursorX += (float) glyph->shift + TextUtils_getKerningOffset(glyph, nextCh);
            } else {
                cursorX += (float) glyph->shift;
            }
        }

        if (lineEnd >= len) break;
        lineStart = TextUtils_skipNewline(text, lineEnd, len);
        layout->appendCursorY += TextUtils_lineStride(font);
    }

    return layout;
}

static bool N3DSRenderer_tryDrawSingleGlyphTextFast(
    Renderer* base,
    N3DSRenderer* renderer,
    Font* font,
    const char* text,
    int32_t len,
    float x,
    float y,
    float effectiveXScale,
    float effectiveYScale,
    float angleDeg,
    bool gradient,
    int32_t c1,
    float alpha,
    bool useDirectFontAsset,
    const C2D_Image* directFontImage,
    const Tex3DS_SubTexture* directFontBaseSubtex,
    const N3DSAtlasFragment* fontFragment,
    N3DSLoadedAtlasPage* fontPage
) {
    if (text == NULL || len <= 0 || len > 4) return false;
    if (TextUtils_isNewlineChar(text[0])) return false;

    int32_t pos = 0;
    uint16_t ch = TextUtils_decodeUtf8(text, len, &pos);
    if (pos != len) return false;

    float cursorX = 0.0f;
    float lineWidth = TextUtils_measureLineWidth(font, text, len);
    if (base->drawHalign == 1) cursorX -= lineWidth * 0.5f;
    else if (base->drawHalign == 2) cursorX -= lineWidth;

    float cursorY = 0.0f;
    float totalHeight = TextUtils_lineStride(font);
    if (base->drawValign == 1) cursorY -= totalHeight * 0.5f;
    else if (base->drawValign == 2) cursorY -= totalHeight;
    cursorY -= (float) font->ascenderOffset;

    FontGlyph* glyph = TextUtils_findGlyph(font, ch);
    if (glyph == NULL || glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
        return true;
    }

    Tex3DS_SubTexture subtex;
    C2D_Image image;
    if (useDirectFontAsset) {
        if (directFontImage == NULL || directFontBaseSubtex == NULL) return false;
        subtex.width = glyph->sourceWidth;
        subtex.height = glyph->sourceHeight;
        float baseLeft = directFontBaseSubtex->left;
        float baseRight = directFontBaseSubtex->right;
        float baseTop = directFontBaseSubtex->top;
        float baseBottom = directFontBaseSubtex->bottom;
        float baseWidth = (float) directFontBaseSubtex->width;
        float baseHeight = (float) directFontBaseSubtex->height;
        subtex.left = baseLeft + (baseRight - baseLeft) * ((float) glyph->sourceX / baseWidth);
        subtex.right = baseLeft + (baseRight - baseLeft) * (((float) glyph->sourceX + (float) glyph->sourceWidth) / baseWidth);
        subtex.top = baseTop + (baseBottom - baseTop) * ((float) glyph->sourceY / baseHeight);
        subtex.bottom = baseTop + (baseBottom - baseTop) * (((float) glyph->sourceY + (float) glyph->sourceHeight) / baseHeight);
        image.tex = directFontImage->tex;
        image.subtex = &subtex;
    } else {
        if (fontFragment == NULL || fontPage == NULL) return false;
        image.tex = &fontPage->texture;
        image.subtex = &subtex;
        uint16_t px = (uint16_t) (fontFragment->x + glyph->sourceX);
        uint16_t py = (uint16_t) (fontFragment->y + glyph->sourceY);
        N3DSRenderer_fillSubTexture(&subtex, fontPage, px, py, glyph->sourceWidth, glyph->sourceHeight);
    }

    float drawX = x + (cursorX + (float) glyph->offset) * effectiveXScale;
    float drawY = y + cursorY * effectiveYScale;
    renderer->frameTextGlyphDraws++;
    if (fabsf(angleDeg) < 0.001f) {
        N3DSRenderer_drawImageFast(base, &image, drawX, drawY, effectiveXScale, effectiveYScale, gradient ? (uint32_t) c1 : base->drawColor, alpha);
    } else {
        N3DSRenderer_drawImage(
            base,
            &image,
            drawX,
            drawY,
            (float) glyph->sourceWidth * effectiveXScale,
            (float) glyph->sourceHeight * effectiveYScale,
            0.0f,
            0.0f,
            angleDeg,
            gradient ? (uint32_t) c1 : base->drawColor,
            alpha
        );
    }

    return true;
}

static void N3DSRenderer_preloadFontPages(N3DSRenderer* renderer) {
    DataWin* dw = renderer->base.dataWin;
    if (dw == NULL) return;

    repeat(dw->font.count, i) {
        Font* font = &dw->font.fonts[i];
        if (font->isSpriteFont || font->tpagIndex < 0) continue;

        uint32_t pageIndex = 0;
        if (!N3DSRenderer_getPageIndexForTPAG(renderer, font->tpagIndex, &pageIndex)) continue;
        if (!N3DSRenderer_ensurePageLoaded(renderer, pageIndex)) continue;
        renderer->atlasPages[pageIndex].pinned = true;
    }
}

static void N3DSRenderer_prewarmRoomBlobCache(N3DSRenderer* renderer, Runner* runner) {
    if (renderer == NULL || runner == NULL || runner->currentRoom == NULL || renderer->atlasPageCount == 0) return;

    DataWin* dw = renderer->base.dataWin;
    Room* room = runner->currentRoom;
    uint32_t remainingBlobBytes = renderer->isNew3DS ? N3DS_PREWARM_BLOB_BYTES_NEW3DS : N3DS_PREWARM_BLOB_BYTES_OLD3DS;
    bool* seenPages = safeCalloc(renderer->atlasPageCount, sizeof(bool));
    uint32_t roomIndex = UINT32_MAX;

    if (dw != NULL && dw->room.rooms != NULL && room >= dw->room.rooms && room < (dw->room.rooms + dw->room.count)) {
        roomIndex = (uint32_t) (room - dw->room.rooms);
    }

    if (roomIndex != UINT32_MAX) {
        N3DSRenderer_prewarmRoomManifestBlobCache(renderer, roomIndex, seenPages, &remainingBlobBytes);
        if (remainingBlobBytes == 0) goto done;
    }

    repeat(8, i) {
        RuntimeBackground* bg = &runner->backgrounds[i];
        if (!bg->visible || bg->backgroundIndex < 0) continue;
        N3DSRenderer_prewarmTPAGBlobOnly(renderer, seenPages, Renderer_resolveBackgroundTPAGIndex(dw, bg->backgroundIndex), &remainingBlobBytes);
        if (remainingBlobBytes == 0) goto done;
    }

    repeat(room->layerCount, layerIndex) {
        RoomLayer* layer = &room->layers[layerIndex];

        if (layer->backgroundData != NULL && layer->backgroundData->visible && layer->backgroundData->spriteIndex >= 0) {
            N3DSRenderer_prewarmTPAGBlobOnly(renderer, seenPages, Renderer_resolveSpriteTPAGIndex(dw, layer->backgroundData->spriteIndex), &remainingBlobBytes);
            if (remainingBlobBytes == 0) goto done;
        }

        if (layer->assetsData != NULL) {
            repeat(layer->assetsData->legacyTileCount, tileIndex) {
                N3DSTileAtlasEntry* entry = N3DSRenderer_findTileEntry(renderer, &layer->assetsData->legacyTiles[tileIndex]);
                N3DSRenderer_prewarmTileEntryBlobOnly(renderer, seenPages, entry, &remainingBlobBytes);
                if (remainingBlobBytes == 0) goto done;
            }

            repeat(layer->assetsData->spriteCount, spriteIndex) {
                SpriteInstance* sprite = &layer->assetsData->sprites[spriteIndex];
                int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, sprite->spriteIndex, (int32_t) sprite->frameIndex);
                N3DSRenderer_prewarmTPAGBlobOnly(renderer, seenPages, tpagIndex, &remainingBlobBytes);
                if (remainingBlobBytes == 0) goto done;
            }
        }
    }

    repeat(room->tileCount, tileIndex) {
        N3DSTileAtlasEntry* entry = N3DSRenderer_findTileEntry(renderer, &room->tiles[tileIndex]);
        N3DSRenderer_prewarmTileEntryBlobOnly(renderer, seenPages, entry, &remainingBlobBytes);
        if (remainingBlobBytes == 0) goto done;
    }

    if (runner->tileLayerCaches != NULL) {
        repeat(runner->tileLayerCacheCount, cacheIndex) {
            TileLayerRenderCache* cache = &runner->tileLayerCaches[cacheIndex];
            if (!cache->built || cache->cells == NULL) continue;

            repeat(arrlen(cache->cells), cellIndex) {
                const TileLayerCacheCell* cell = &cache->cells[cellIndex];
                if (cell->n3dsTileEntryIndex >= 0 && (uint32_t) cell->n3dsTileEntryIndex < renderer->tileEntryCount) {
                    N3DSRenderer_prewarmTileEntryBlobOnly(renderer, seenPages, &renderer->tileEntries[cell->n3dsTileEntryIndex], &remainingBlobBytes);
                } else {
                    N3DSRenderer_prewarmTPAGBlobOnly(renderer, seenPages, cell->tpagIndex, &remainingBlobBytes);
                }
                if (remainingBlobBytes == 0) goto done;
            }
        }
    }

    repeat(arrlen(runner->instances), instanceIndex) {
        Instance* inst = runner->instances[instanceIndex];
        if (inst == NULL || inst->destroyed || !inst->visible || inst->spriteIndex < 0) continue;
        int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, inst->spriteIndex, (int32_t) inst->imageIndex);
        N3DSRenderer_prewarmTPAGBlobOnly(renderer, seenPages, tpagIndex, &remainingBlobBytes);
        if (remainingBlobBytes == 0) goto done;
    }

done:
    free(seenPages);
}

static void N3DSRenderer_prewarmRoom(Renderer* base, Runner* runner) {
    if (base == NULL || runner == NULL || runner->currentRoom == NULL) return;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    Room* room = runner->currentRoom;
    bool roomChanged = renderer->lastPrewarmedRoom != room;

    N3DSRenderer_clearDirectAssetPins(renderer);
    if (roomChanged) {
        N3DSRenderer_flushRoomDirectAssets(renderer);
    }
    if (roomChanged && !renderer->isNew3DS) {
        renderer->pendingOld3DSAtlasFlush = true;
    }
    renderer->lastPrewarmedRoom = room;
    N3DSRenderer_prewarmRoomBlobCache(renderer, runner);
}

static void N3DSRenderer_computeFrameLayout(N3DSRenderer* renderer, int32_t gameW, int32_t gameH) {
    N3DSRenderer_computeFrameLayoutForTarget(renderer, gameW, gameH, N3DS_TOP_WIDTH, N3DS_TOP_HEIGHT);
}

static void N3DSRenderer_init(Renderer* base, DataWin* dataWin) {
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    bool isNew3DS = false;
    base->dataWin = dataWin;
    renderer->baseTPAGCount = dataWin != NULL ? dataWin->tpag.count : 0u;
    renderer->topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
#ifndef N3DS_DISABLE_BOTTOM_SCREEN
    renderer->bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
#endif
    renderer->blendEnabled = true;
    renderer->blendEquation = bm_normal;
    renderer->blendSrcFactor = bm_src_alpha;
    renderer->blendDstFactor = bm_inv_src_alpha;
    renderer->clearColor = 0x000000;
    renderer->clearAlpha = 1.0f;
    if (R_SUCCEEDED(APT_CheckNew3DS(&isNew3DS))) {
        renderer->isNew3DS = isNew3DS;
    }
#if N3DS_FORCE_OLD3DS_MODE
    renderer->isNew3DS = false;
#endif
    renderer->residentAtlasPageLimit = renderer->isNew3DS ? N3DS_MAX_RESIDENT_ATLAS_PAGES_NEW3DS : N3DS_MAX_RESIDENT_ATLAS_PAGES_OLD3DS;
    renderer->residentAtlasVRAMLimitBytes = renderer->isNew3DS ? N3DS_RESIDENT_ATLAS_VRAM_BUDGET_NEW3DS : N3DS_RESIDENT_ATLAS_VRAM_BUDGET_OLD3DS;
    renderer->residentDirectAssetVRAMLimitBytes = renderer->isNew3DS ? N3DS_DIRECT_ASSET_VRAM_BUDGET_NEW3DS : N3DS_DIRECT_ASSET_VRAM_BUDGET_OLD3DS;
    renderer->prewarmGpuPageBudget = renderer->isNew3DS ? N3DS_PREWARM_GPU_PAGE_BUDGET_NEW3DS : N3DS_PREWARM_GPU_PAGE_BUDGET_OLD3DS;
    renderer->cachedT3xByteLimit = renderer->isNew3DS ? N3DS_MAX_CACHED_T3X_BYTES_NEW3DS : N3DS_MAX_CACHED_T3X_BYTES_OLD3DS;
    renderer->cachedDirectT3xByteLimit = renderer->isNew3DS ? N3DS_MAX_CACHED_DIRECT_T3X_BYTES_NEW3DS : N3DS_MAX_CACHED_DIRECT_T3X_BYTES_OLD3DS;
    renderer->atlasLoaded = N3DSRenderer_loadAtlas(renderer);
    if (!renderer->atlasLoaded && renderer->startupError[0] == '\0') {
        N3DSRenderer_setStartupError(renderer, "Failed to load 3DS graphics atlas");
    }
    N3DSRenderer_buildDirectAssetMaps(renderer);
    (void) N3DSRenderer_loadPackedDirectAssets(renderer);
    (void) N3DSRenderer_loadRoomManifest(renderer);
    renderer->atlasTraceMask = safeCalloc(renderer->atlasItemCount > 0 ? renderer->atlasItemCount : 1u, sizeof(uint8_t));
    renderer->atlasTraceFile = fopen(N3DS_ATLAS_TRACE_LOG_PATH, "wb");
    if (renderer->atlasTraceFile != NULL) {
        fprintf(renderer->atlasTraceFile, "N3DS atlas trace start\n");
        fflush(renderer->atlasTraceFile);
    }
    N3DSRenderer_preloadFontPages(renderer);
    C2D_SetTintMode(C2D_TintMult);
    fprintf(
        stderr,
        "N3DSRenderer: model=%s residentLimit=%lu pages residentVRAM=%luKB directVRAM=%luKB prewarm=%lu cacheLimit=%luKB\n",
        renderer->isNew3DS ? "new3ds" : "old3ds",
        (unsigned long) renderer->residentAtlasPageLimit,
        (unsigned long) (renderer->residentAtlasVRAMLimitBytes / 1024u),
        (unsigned long) (renderer->residentDirectAssetVRAMLimitBytes / 1024u),
        (unsigned long) renderer->prewarmGpuPageBudget,
        (unsigned long) (renderer->cachedT3xByteLimit / 1024u)
    );
}

static void N3DSRenderer_destroy(Renderer* base) {
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    repeat(renderer->atlasPageCount, i) {
        N3DSRenderer_unloadPage(renderer, (uint32_t) i);
        N3DSRenderer_unloadPageBlob(renderer, (uint32_t) i);
    }
    N3DSRenderer_freeCachedTextLayout(&renderer->cachedTextLayout);
    repeat(shlen(renderer->resolvedAssetPathCache), i) {
        free(renderer->resolvedAssetPathCache[i].value);
    }
    shfree(renderer->resolvedAssetPathCache);
    shfree(renderer->packedDirectAssetMap);
    free(renderer->roomManifestEntries);
    free(renderer->roomManifestPageRefs);
    free(renderer->atlasTraceMask);
    if (renderer->atlasTraceFile != NULL) fclose(renderer->atlasTraceFile);
    if (renderer->packedAtlasFile != NULL) fclose(renderer->packedAtlasFile);
    if (renderer->packedDirectAssetFile != NULL) fclose(renderer->packedDirectAssetFile);
    repeat(renderer->directSpriteAssetCount, spriteIndex) {
        N3DSDirectSpriteAsset* spriteAsset = &renderer->directSpriteAssets[spriteIndex];
        N3DSRenderer_freeDirectTextureAsset(&spriteAsset->sheetAsset, renderer);
        N3DSRenderer_unloadDirectTextureBlob(&spriteAsset->sheetAsset, renderer);
        free(spriteAsset->sheetFrameImages);
        spriteAsset->sheetFrameImages = NULL;
        repeat(spriteAsset->frameCount, frameIndex) {
            N3DSRenderer_freeDirectTextureAsset(&spriteAsset->frameAssets[frameIndex], renderer);
            N3DSRenderer_unloadDirectTextureBlob(&spriteAsset->frameAssets[frameIndex], renderer);
        }
        free(spriteAsset->frameAssets);
    }
    repeat(renderer->directBackgroundAssetCount, bgIndex) {
        N3DSRenderer_freeDirectTextureAsset(&renderer->directBackgroundAssets[bgIndex], renderer);
        N3DSRenderer_unloadDirectTextureBlob(&renderer->directBackgroundAssets[bgIndex], renderer);
    }
    repeat(renderer->directFontAssetCount, fontIndex) {
        N3DSRenderer_freeDirectTextureAsset(&renderer->directFontAssets[fontIndex], renderer);
        N3DSRenderer_unloadDirectTextureBlob(&renderer->directFontAssets[fontIndex], renderer);
    }
    free(renderer->directSpriteAssets);
    free(renderer->directBackgroundAssets);
    free(renderer->directFontAssets);
    if (renderer->dynamicCaptureTPAGs != NULL) {
        repeat(renderer->dynamicCaptureTPAGCount, i) {
            N3DSRenderer_freeDynamicCaptureTPAG(renderer, &renderer->dynamicCaptureTPAGs[i]);
        }
        free(renderer->dynamicCaptureTPAGs);
    }
    free(renderer->tpagToSpriteIndex);
    free(renderer->tpagToSpriteFrameIndex);
    free(renderer->tpagToBackgroundIndex);
    free(renderer->atlasPages);
    free(renderer->atlasItems);
    free(renderer->atlasItemsV3);
    free(renderer->atlasFragments);
    free(renderer->tileEntries);
    hmfree(renderer->tileEntryMap);
    if (renderer->tileLayerChunkCaches != NULL) {
        size_t chunkCacheCount = arrlenu(renderer->tileLayerChunkCaches);
        repeat(chunkCacheCount, i) {
            N3DSRenderer_freeTileLayerChunkCache(renderer, &renderer->tileLayerChunkCaches[i]);
        }
        arrfree(renderer->tileLayerChunkCaches);
        renderer->tileLayerChunkCaches = NULL;
    }
    if (renderer->bottomTarget != NULL) {
        C3D_RenderTargetDelete(renderer->bottomTarget);
    }
    if (renderer->topTarget != NULL) {
        C3D_RenderTargetDelete(renderer->topTarget);
    }
    free(renderer);
}

static void N3DSRenderer_beginFrame(Renderer* base, int32_t gameW, int32_t gameH, MAYBE_UNUSED int32_t windowW, MAYBE_UNUSED int32_t windowH) {
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    renderer->frameSequence++;
    renderer->activeSceneTarget = N3DS_SCENE_TARGET_NONE;
    N3DSRenderer_resetFramePerfCounters(renderer);
    N3DSRenderer_computeFrameLayout(renderer, gameW, gameH);

    if (renderer->pendingOld3DSAtlasFlush && !renderer->isNew3DS) {
        renderer->pendingOld3DSAtlasFlush = false;
        N3DSRenderer_clearAtlasPagePins(renderer);
        N3DSRenderer_preloadFontPages(renderer);
        N3DSRenderer_flushRoomAtlasResidencyOld3DS(renderer);
    }

    N3DSRenderer_setDefaultGPUState(renderer);
    if (renderer->bottomTarget != NULL) {
        N3DSRenderer_sceneBeginTarget(renderer, N3DS_SCENE_TARGET_BOTTOM, true);
        C2D_TargetClear(renderer->bottomTarget, C2D_Color32(0, 0, 0, 255));
    }
    N3DSRenderer_sceneBeginTarget(renderer, N3DS_SCENE_TARGET_TOP, true);
    C2D_TargetClear(renderer->topTarget, N3DSRenderer_makeColor(renderer->clearColor, renderer->clearAlpha));
}

static void N3DSRenderer_endFrame(MAYBE_UNUSED Renderer* base) {
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    N3DSRenderer_logPerfWindowIfNeeded(renderer);
    N3DSRenderer_flushC2DQueue(renderer);
    C2D_Flush();
    renderer->pendingC2DDraws = 0;
    renderer->lastDrawTexture = NULL;
}

void N3DSRenderer_beginOverlay(Renderer* base) {
    if (base == NULL) return;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    N3DSRenderer_flushC2DQueue(renderer);
    N3DSRenderer_setDefaultGPUState(renderer);
    N3DSRenderer_sceneBeginTarget(renderer, N3DS_SCENE_TARGET_TOP, false);
}

void N3DSRenderer_beginBottomScreenGUIEx(Renderer* base, int32_t guiW, int32_t guiH, float scaleX, float scaleY, float offsetX, float offsetY) {
#ifdef N3DS_DISABLE_BOTTOM_SCREEN
    (void) base;
    (void) guiW;
    (void) guiH;
    (void) scaleX;
    (void) scaleY;
    (void) offsetX;
    (void) offsetY;
    return;
#endif
    if (base == NULL) return;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    if (renderer->bottomTarget == NULL || renderer->bottomScreenGuiActive) return;

    renderer->savedFrameScaleX = renderer->frameScaleX;
    renderer->savedFrameScaleY = renderer->frameScaleY;
    renderer->savedFrameOffsetX = renderer->frameOffsetX;
    renderer->savedFrameOffsetY = renderer->frameOffsetY;
    renderer->savedPortOffsetX = renderer->portOffsetX;
    renderer->savedPortOffsetY = renderer->portOffsetY;
    renderer->savedViewX = renderer->viewX;
    renderer->savedViewY = renderer->viewY;
    renderer->savedViewScaleX = renderer->viewScaleX;
    renderer->savedViewScaleY = renderer->viewScaleY;

    N3DSRenderer_computeFrameLayoutForTarget(renderer, guiW, guiH, N3DS_BOTTOM_WIDTH, N3DS_BOTTOM_HEIGHT);
    renderer->viewX = 0;
    renderer->viewY = 0;
    renderer->viewScaleX = renderer->frameScaleX * scaleX;
    renderer->viewScaleY = renderer->frameScaleY * scaleY;
    renderer->portOffsetX = ((float) guiW * (renderer->frameScaleX - renderer->viewScaleX) * 0.5f) + offsetX;
    renderer->portOffsetY = ((float) guiH * (renderer->frameScaleY - renderer->viewScaleY) * 0.5f) + offsetY;
    renderer->bottomScreenGuiActive = true;

    N3DSRenderer_flushC2DQueue(renderer);
    N3DSRenderer_setDefaultGPUState(renderer);
    N3DSRenderer_sceneBeginTarget(renderer, N3DS_SCENE_TARGET_BOTTOM, false);
}

void N3DSRenderer_beginBottomScreenGUI(Renderer* base, int32_t guiW, int32_t guiH) {
    N3DSRenderer_beginBottomScreenGUIEx(base, guiW, guiH, 1.0f, 1.0f, 0.0f, 0.0f);
}

void N3DSRenderer_beginBottomScreenGUIView(Renderer* base, int32_t guiW, int32_t guiH, int32_t viewX, int32_t viewY) {
    N3DSRenderer_beginBottomScreenGUIEx(base, guiW, guiH, 1.0f, 1.0f, 0.0f, 0.0f);
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    renderer->viewX = viewX;
    renderer->viewY = viewY;
}

void N3DSRenderer_endBottomScreenGUI(Renderer* base) {
    if (base == NULL) return;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    if (!renderer->bottomScreenGuiActive) return;

    renderer->frameScaleX = renderer->savedFrameScaleX;
    renderer->frameScaleY = renderer->savedFrameScaleY;
    renderer->frameOffsetX = renderer->savedFrameOffsetX;
    renderer->frameOffsetY = renderer->savedFrameOffsetY;
    renderer->portOffsetX = renderer->savedPortOffsetX;
    renderer->portOffsetY = renderer->savedPortOffsetY;
    renderer->viewX = renderer->savedViewX;
    renderer->viewY = renderer->savedViewY;
    renderer->viewScaleX = renderer->savedViewScaleX;
    renderer->viewScaleY = renderer->savedViewScaleY;
    renderer->bottomScreenGuiActive = false;

    N3DSRenderer_flushC2DQueue(renderer);
    N3DSRenderer_applyBlendState(renderer);
    N3DSRenderer_applyAlphaState(renderer);
    N3DSRenderer_sceneBeginTarget(renderer, N3DS_SCENE_TARGET_TOP, false);
}

void N3DSRenderer_beginBottomScreenGUI2x(Renderer* base, int32_t guiW, int32_t guiH) {
    N3DSRenderer_beginBottomScreenGUIEx(base, guiW, guiH, 2.0f, 2.0f, 0.0f, 0.0f);
}

void N3DSRenderer_endBottomScreenGUI2x(Renderer* base) {
    N3DSRenderer_endBottomScreenGUI(base);
}

void N3DSRenderer_beginTopScreenGUI(Renderer* base, int32_t guiW, int32_t guiH) {
    if (base == NULL) return;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    if (renderer->topScreenGuiActive || renderer->bottomScreenGuiActive) return;

    renderer->savedFrameScaleX = renderer->frameScaleX;
    renderer->savedFrameScaleY = renderer->frameScaleY;
    renderer->savedFrameOffsetX = renderer->frameOffsetX;
    renderer->savedFrameOffsetY = renderer->frameOffsetY;
    renderer->savedPortOffsetX = renderer->portOffsetX;
    renderer->savedPortOffsetY = renderer->portOffsetY;
    renderer->savedViewX = renderer->viewX;
    renderer->savedViewY = renderer->viewY;
    renderer->savedViewScaleX = renderer->viewScaleX;
    renderer->savedViewScaleY = renderer->viewScaleY;

    N3DSRenderer_setTopBattle320x240Layout(renderer, guiW, guiH, N3DS_TOP_BATTLE_SCENE_Y_OFFSET);
    renderer->topScreenGuiActive = true;
    renderer->topScreenGui2xActive = false;

    N3DSRenderer_flushC2DQueue(renderer);
    N3DSRenderer_setDefaultGPUState(renderer);
    N3DSRenderer_sceneBeginTarget(renderer, N3DS_SCENE_TARGET_TOP, false);
}

void N3DSRenderer_endTopScreenGUI(Renderer* base) {
    if (base == NULL) return;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    if (!renderer->topScreenGuiActive) return;

    renderer->frameScaleX = renderer->savedFrameScaleX;
    renderer->frameScaleY = renderer->savedFrameScaleY;
    renderer->frameOffsetX = renderer->savedFrameOffsetX;
    renderer->frameOffsetY = renderer->savedFrameOffsetY;
    renderer->portOffsetX = renderer->savedPortOffsetX;
    renderer->portOffsetY = renderer->savedPortOffsetY;
    renderer->viewX = renderer->savedViewX;
    renderer->viewY = renderer->savedViewY;
    renderer->viewScaleX = renderer->savedViewScaleX;
    renderer->viewScaleY = renderer->savedViewScaleY;
    renderer->topScreenGuiActive = false;
    renderer->topScreenGui2xActive = false;

    N3DSRenderer_flushC2DQueue(renderer);
    N3DSRenderer_applyBlendState(renderer);
    N3DSRenderer_applyAlphaState(renderer);
    N3DSRenderer_sceneBeginTarget(renderer, N3DS_SCENE_TARGET_TOP, false);
}

void N3DSRenderer_beginTopScreenGUI2x(Renderer* base, int32_t guiW, int32_t guiH) {
    if (base == NULL) return;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    if (renderer->topScreenGuiActive || renderer->bottomScreenGuiActive) return;

    renderer->savedFrameScaleX = renderer->frameScaleX;
    renderer->savedFrameScaleY = renderer->frameScaleY;
    renderer->savedFrameOffsetX = renderer->frameOffsetX;
    renderer->savedFrameOffsetY = renderer->frameOffsetY;
    renderer->savedPortOffsetX = renderer->portOffsetX;
    renderer->savedPortOffsetY = renderer->portOffsetY;
    renderer->savedViewX = renderer->viewX;
    renderer->savedViewY = renderer->viewY;
    renderer->savedViewScaleX = renderer->viewScaleX;
    renderer->savedViewScaleY = renderer->viewScaleY;

    N3DSRenderer_setTopBattle320x240Layout(renderer, guiW, guiH, N3DS_TOP_BATTLE_ENEMY_Y_OFFSET);
    renderer->viewScaleX *= 2.0f;
    renderer->viewScaleY *= 2.0f;
    renderer->topScreenGuiActive = true;
    renderer->topScreenGui2xActive = true;

    N3DSRenderer_flushC2DQueue(renderer);
    N3DSRenderer_setDefaultGPUState(renderer);
    N3DSRenderer_sceneBeginTarget(renderer, N3DS_SCENE_TARGET_TOP, false);
}

void N3DSRenderer_endTopScreenGUI2x(Renderer* base) {
    if (base == NULL) return;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    if (!renderer->topScreenGuiActive) return;

    renderer->frameScaleX = renderer->savedFrameScaleX;
    renderer->frameScaleY = renderer->savedFrameScaleY;
    renderer->frameOffsetX = renderer->savedFrameOffsetX;
    renderer->frameOffsetY = renderer->savedFrameOffsetY;
    renderer->portOffsetX = renderer->savedPortOffsetX;
    renderer->portOffsetY = renderer->savedPortOffsetY;
    renderer->viewX = renderer->savedViewX;
    renderer->viewY = renderer->savedViewY;
    renderer->viewScaleX = renderer->savedViewScaleX;
    renderer->viewScaleY = renderer->savedViewScaleY;
    renderer->topScreenGuiActive = false;
    renderer->topScreenGui2xActive = false;

    N3DSRenderer_flushC2DQueue(renderer);
    N3DSRenderer_applyBlendState(renderer);
    N3DSRenderer_applyAlphaState(renderer);
    N3DSRenderer_sceneBeginTarget(renderer, N3DS_SCENE_TARGET_TOP, false);
}

bool N3DSRenderer_isTopScreenGUIActive(Renderer* base) {
    if (base == NULL) return false;
    return ((N3DSRenderer*) base)->topScreenGuiActive;
}

bool N3DSRenderer_isTopScreenBattleViewActive(Renderer* base) {
    if (base == NULL) return false;
    return ((N3DSRenderer*) base)->topScreenBattleViewActive;
}

void N3DSRenderer_setTopScreenBattleViewActive(Renderer* base, bool active) {
    if (base == NULL) return;
    ((N3DSRenderer*) base)->topScreenBattleViewActive = active;
}

static void N3DSRenderer_beginView(Renderer* base, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, MAYBE_UNUSED float viewAngle) {
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    renderer->viewX = viewX;
    renderer->viewY = viewY;
    renderer->portOffsetX = (float) portX * renderer->frameScaleX;
    renderer->portOffsetY = (float) portY * renderer->frameScaleY;
    renderer->viewScaleX = viewW > 0 ? ((float) portW / (float) viewW) * renderer->frameScaleX : renderer->frameScaleX;
    renderer->viewScaleY = viewH > 0 ? ((float) portH / (float) viewH) * renderer->frameScaleY : renderer->frameScaleY;
    N3DSRenderer_sceneBeginTarget(renderer, N3DS_SCENE_TARGET_TOP, false);
}

static void N3DSRenderer_endView(MAYBE_UNUSED Renderer* base) {}

static void N3DSRenderer_beginGUI(Renderer* base, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH) {
    N3DSRenderer_beginView(base, 0, 0, guiW, guiH, portX, portY, portW, portH, 0.0f);
}

static void N3DSRenderer_endGUI(Renderer* base) {
    N3DSRenderer_endView(base);
}

static void N3DSRenderer_drawImage(Renderer* base, C2D_Image* image, float localX, float localY, float width, float height, float pivotX, float pivotY, float angleDeg, uint32_t color, float alpha) {
    if (image == NULL || image->tex == NULL || image->subtex == NULL) return;
    if (!Renderer_isFiniteFloat(localX) || !Renderer_isFiniteFloat(localY) ||
        !Renderer_isFiniteFloat(width) || !Renderer_isFiniteFloat(height) ||
        !Renderer_isFiniteFloat(pivotX) || !Renderer_isFiniteFloat(pivotY) ||
        !Renderer_isFiniteFloat(angleDeg) || !Renderer_isFiniteFloat(alpha) ||
        width == 0.0f || height == 0.0f ||
        image->subtex->width == 0.0f || image->subtex->height == 0.0f) {
        return;
    }

    bool flipX = false;
    bool flipY = false;
    if (width < 0.0f) {
        localX += width;
        pivotX -= width;
        width = -width;
        flipX = true;
    }
    if (height < 0.0f) {
        localY += height;
        pivotY -= height;
        height = -height;
        flipY = true;
    }

    Tex3DS_SubTexture flippedSubtex;
    C2D_Image drawImage = *image;
    if (flipX || flipY) {
        flippedSubtex = *image->subtex;
        if (flipX) {
            float tmp = flippedSubtex.left;
            flippedSubtex.left = flippedSubtex.right;
            flippedSubtex.right = tmp;
        }
        if (flipY) {
            float tmp = flippedSubtex.top;
            flippedSubtex.top = flippedSubtex.bottom;
            flippedSubtex.bottom = tmp;
        }
        drawImage.subtex = &flippedSubtex;
    }

    if (fabsf(angleDeg) < 0.001f && fabsf(pivotX) < 0.001f && fabsf(pivotY) < 0.001f) {
        float xscale = width / (float) drawImage.subtex->width;
        float yscale = height / (float) drawImage.subtex->height;
        N3DSRenderer_drawImageFast(base, &drawImage, localX, localY, xscale, yscale, color, alpha);
        return;
    }

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    float screenRectX = 0.0f;
    float screenRectY = 0.0f;
    float screenRectW = 0.0f;
    float screenRectH = 0.0f;
    N3DSRenderer_transformScreenRect(renderer, localX, localY, width, height, &screenRectX, &screenRectY, &screenRectW, &screenRectH);
    if (screenRectW <= 0.0f || screenRectH <= 0.0f) return;
    if (N3DSRenderer_isScreenRotatedRectOffscreen(renderer, screenRectX, screenRectY, screenRectW, screenRectH, angleDeg)) {
        return;
    }

    float pivotLocalX = localX + pivotX;
    float pivotLocalY = localY + pivotY;
    float screenX = N3DSRenderer_transformScreenX(renderer, pivotLocalX);
    float screenY = N3DSRenderer_transformScreenY(renderer, pivotLocalY);
    float screenScaleX = screenRectW / (float) drawImage.subtex->width;
    float screenScaleY = screenRectH / (float) drawImage.subtex->height;
    float screenPivotX = (width != 0.0f) ? (pivotX / width) * screenRectW : 0.0f;
    float screenPivotY = (height != 0.0f) ? (pivotY / height) * screenRectH : 0.0f;

    C2D_DrawParams params = {
        .pos = { screenX, screenY, screenRectW, screenRectH },
        .center = { screenPivotX, screenPivotY },
        .depth = 0.5f,
        .angle = C3D_AngleFromDegrees(-angleDeg),
    };
    N3DSRenderer_trackTextureUse(renderer, &drawImage);
    N3DSRenderer_applyTextureFilterForScale(renderer, drawImage.tex, screenScaleX, screenScaleY);
    if (N3DSRenderer_isIdentityTint(color, alpha)) {
        C2D_DrawImage(drawImage, &params, NULL);
    } else {
        C2D_ImageTint tint;
        C2D_PlainImageTint(&tint, N3DSRenderer_makeColor(color, alpha), 1.0f);
        C2D_DrawImage(drawImage, &params, &tint);
    }
    N3DSRenderer_noteC2DDraws(renderer, 1u);
}

static bool N3DSRenderer_tryResolveSingleFragmentFontPage(N3DSRenderer* renderer, int32_t tpagIndex, const N3DSAtlasFragment** outFragment, N3DSLoadedAtlasPage** outPage) {
    if (!N3DSRenderer_isFragmentedAtlasVersion(renderer->atlasVersion)) return false;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= renderer->atlasItemCount) return false;

    N3DSAtlasItemV3* item = &renderer->atlasItemsV3[tpagIndex];
    if (item->fragmentCount != 1) return false;

    const N3DSAtlasFragment* fragment = &renderer->atlasFragments[item->fragmentStart];
    if ((uint32_t) fragment->atlasId >= renderer->atlasPageCount) return false;
    if (!N3DSRenderer_ensurePageLoaded(renderer, fragment->atlasId)) return false;

    N3DSLoadedAtlasPage* page = &renderer->atlasPages[fragment->atlasId];
    if (!page->ready) return false;

    *outFragment = fragment;
    *outPage = page;
    return true;
}

static void N3DSRenderer_drawSprite(Renderer* base, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    if (!Renderer_isFiniteFloat(x) || !Renderer_isFiniteFloat(y) ||
        !Renderer_isFiniteFloat(originX) || !Renderer_isFiniteFloat(originY) ||
        !Renderer_isFiniteFloat(xscale) || !Renderer_isFiniteFloat(yscale) ||
        !Renderer_isFiniteFloat(angleDeg) || !Renderer_isFiniteFloat(alpha) ||
        xscale == 0.0f || yscale == 0.0f) {
        return;
    }

    DataWin* dw = base->dataWin;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    N3DSDynamicCaptureTPAG* dynamicCapture = N3DSRenderer_getDynamicCaptureTPAG(renderer, tpagIndex);
    N3DSRenderer_traceTPAGUsage(renderer, N3DS_TRACE_KIND_SPRITE, tpagIndex);
    renderer->frameSpriteDrawCalls++;
    if (dynamicCapture != NULL) {
        float localX = ((float) tpag->targetX - originX) * xscale;
        float localY = ((float) tpag->targetY - originY) * yscale;
        float drawX = x + localX;
        float drawY = y + localY;
        N3DSRenderer_drawImage(
            base,
            &dynamicCapture->image,
            drawX,
            drawY,
            (float) tpag->sourceWidth * xscale,
            (float) tpag->sourceHeight * yscale,
            -localX,
            -localY,
            angleDeg,
            color,
            alpha
        );
        return;
    }
    if (N3DSRenderer_tryDrawDirectMappedSprite(base, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha)) {
        return;
    }
    if (N3DSRenderer_isFragmentedAtlasVersion(renderer->atlasVersion)) {
        if ((uint32_t) tpagIndex >= renderer->atlasItemCount) return;
        N3DSAtlasItemV3* item = &renderer->atlasItemsV3[tpagIndex];
        repeat(item->fragmentCount, fragIndex) {
            N3DSAtlasFragment* fragment = &renderer->atlasFragments[item->fragmentStart + fragIndex];
            C2D_Image image;
            Tex3DS_SubTexture subtex;
            if (!N3DSRenderer_resolveFragmentImage(renderer, fragment, &image, &subtex)) continue;
            renderer->frameFragmentDraws++;
            float localX = ((float) tpag->targetX + (float) fragment->sourceX - originX) * xscale;
            float localY = ((float) tpag->targetY + (float) fragment->sourceY - originY) * yscale;
            float drawX = x + localX;
            float drawY = y + localY;
            N3DSRenderer_drawImage(
                base,
                &image,
                drawX,
                drawY,
                (float) fragment->width * xscale,
                (float) fragment->height * yscale,
                -localX,
                -localY,
                angleDeg,
                color,
                alpha
            );
        }
        return;
    }

    C2D_Image image;
    Tex3DS_SubTexture subtex;
    if (!N3DSRenderer_resolveImage(renderer, tpagIndex, &image, &subtex)) return;

    float localX = ((float) tpag->targetX - originX) * xscale;
    float localY = ((float) tpag->targetY - originY) * yscale;
    float drawX = x + localX;
    float drawY = y + localY;
    N3DSRenderer_drawImage(
        base,
        &image,
        drawX,
        drawY,
        (float) tpag->sourceWidth * xscale,
        (float) tpag->sourceHeight * yscale,
        -localX,
        -localY,
        angleDeg,
        color,
        alpha
    );
}

static void N3DSRenderer_drawSpritePart(Renderer* base, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    if (srcW <= 0 || srcH <= 0) return;
    if (!Renderer_isFiniteFloat(x) || !Renderer_isFiniteFloat(y) ||
        !Renderer_isFiniteFloat(xscale) || !Renderer_isFiniteFloat(yscale) ||
        !Renderer_isFiniteFloat(angleDeg) || !Renderer_isFiniteFloat(pivotX) ||
        !Renderer_isFiniteFloat(pivotY) || !Renderer_isFiniteFloat(alpha) ||
        xscale == 0.0f || yscale == 0.0f) {
        return;
    }

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    N3DSDynamicCaptureTPAG* dynamicCapture = N3DSRenderer_getDynamicCaptureTPAG(renderer, tpagIndex);
    N3DSRenderer_traceTPAGUsage(renderer, N3DS_TRACE_KIND_SPRITE_PART, tpagIndex);
    renderer->frameSpritePartDrawCalls++;
    if (dynamicCapture != NULL) {
        DataWin* dw = base->dataWin;
        if (tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count) return;
        TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
        float logicalW = (float) (tpag->sourceWidth > 0 ? tpag->sourceWidth : dynamicCapture->logicalWidth);
        float logicalH = (float) (tpag->sourceHeight > 0 ? tpag->sourceHeight : dynamicCapture->logicalHeight);
        float texW = (float) dynamicCapture->subtex.width;
        float texH = (float) dynamicCapture->subtex.height;
        if (logicalW <= 0.0f || logicalH <= 0.0f || texW <= 0.0f || texH <= 0.0f) return;

        Tex3DS_SubTexture subtex = dynamicCapture->subtex;
        float u0 = (float) srcOffX / logicalW;
        float v0 = (float) srcOffY / logicalH;
        float u1 = (float) (srcOffX + srcW) / logicalW;
        float v1 = (float) (srcOffY + srcH) / logicalH;
        if (u0 < 0.0f) u0 = 0.0f;
        if (v0 < 0.0f) v0 = 0.0f;
        if (u1 > 1.0f) u1 = 1.0f;
        if (v1 > 1.0f) v1 = 1.0f;
        if (u0 >= u1 || v0 >= v1) return;

        float left = dynamicCapture->subtex.left;
        float right = dynamicCapture->subtex.right;
        float top = dynamicCapture->subtex.top;
        float bottom = dynamicCapture->subtex.bottom;
        subtex.left = left + (right - left) * u0;
        subtex.right = left + (right - left) * u1;
        subtex.top = top + (bottom - top) * v0;
        subtex.bottom = top + (bottom - top) * v1;
        subtex.width = (uint16_t) lroundf((u1 - u0) * texW);
        subtex.height = (uint16_t) lroundf((v1 - v0) * texH);
        if (subtex.width == 0 || subtex.height == 0) return;

        C2D_Image image = {
            .tex = dynamicCapture->image.tex,
            .subtex = &subtex,
        };
        N3DSRenderer_drawImage(
            base,
            &image,
            x,
            y,
            (float) srcW * xscale,
            (float) srcH * yscale,
            pivotX - x,
            pivotY - y,
            angleDeg,
            color,
            alpha
        );
        return;
    }

    C2D_Image* directImage = NULL;
    if (renderer->tpagToSpriteIndex != NULL && renderer->tpagToSpriteFrameIndex != NULL &&
        tpagIndex >= 0 && (uint32_t) tpagIndex < renderer->base.dataWin->tpag.count) {
        int32_t spriteIndex = renderer->tpagToSpriteIndex[tpagIndex];
        int32_t frameIndex = renderer->tpagToSpriteFrameIndex[tpagIndex];
        if (spriteIndex >= 0 && frameIndex >= 0) {
            N3DSRenderer_tryLoadDirectSpriteImage(renderer, spriteIndex, frameIndex, &directImage);
        }
    }
    if (directImage == NULL && renderer->tpagToBackgroundIndex != NULL &&
        tpagIndex >= 0 && (uint32_t) tpagIndex < renderer->base.dataWin->tpag.count) {
        int32_t backgroundIndex = renderer->tpagToBackgroundIndex[tpagIndex];
        if (backgroundIndex >= 0) {
            N3DSRenderer_tryLoadDirectBackgroundImage(renderer, backgroundIndex, &directImage);
        }
    }
    if (directImage != NULL && directImage->subtex != NULL) {
        Tex3DS_SubTexture subtex = *directImage->subtex;
        float baseLeft = subtex.left;
        float baseRight = subtex.right;
        float baseTop = subtex.top;
        float baseBottom = subtex.bottom;
        float baseWidth = (float) directImage->subtex->width;
        float baseHeight = (float) directImage->subtex->height;
        if (baseWidth <= 0.0f || baseHeight <= 0.0f) return;

        subtex.width = (uint16_t) srcW;
        subtex.height = (uint16_t) srcH;
        subtex.left = baseLeft + (baseRight - baseLeft) * ((float) srcOffX / baseWidth);
        subtex.right = baseLeft + (baseRight - baseLeft) * (((float) srcOffX + (float) srcW) / baseWidth);
        subtex.top = baseTop + (baseBottom - baseTop) * ((float) srcOffY / baseHeight);
        subtex.bottom = baseTop + (baseBottom - baseTop) * (((float) srcOffY + (float) srcH) / baseHeight);

        C2D_Image image = {
            .tex = directImage->tex,
            .subtex = &subtex,
        };
        N3DSRenderer_drawImage(base, &image, x, y, (float) srcW * xscale, (float) srcH * yscale, pivotX - x, pivotY - y, angleDeg, color, alpha);
        return;
    }
    if (N3DSRenderer_isFragmentedAtlasVersion(renderer->atlasVersion)) {
        if (tpagIndex < 0 || (uint32_t) tpagIndex >= renderer->atlasItemCount) return;
        N3DSAtlasItemV3* item = &renderer->atlasItemsV3[tpagIndex];
        int32_t reqLeft = srcOffX;
        int32_t reqTop = srcOffY;
        int32_t reqRight = srcOffX + srcW;
        int32_t reqBottom = srcOffY + srcH;

        repeat(item->fragmentCount, fragIndex) {
            N3DSAtlasFragment* fragment = &renderer->atlasFragments[item->fragmentStart + fragIndex];
            int32_t fragLeft = fragment->sourceX;
            int32_t fragTop = fragment->sourceY;
            int32_t fragRight = fragLeft + fragment->width;
            int32_t fragBottom = fragTop + fragment->height;
            int32_t clipLeft = reqLeft > fragLeft ? reqLeft : fragLeft;
            int32_t clipTop = reqTop > fragTop ? reqTop : fragTop;
            int32_t clipRight = reqRight < fragRight ? reqRight : fragRight;
            int32_t clipBottom = reqBottom < fragBottom ? reqBottom : fragBottom;
            if (clipLeft >= clipRight || clipTop >= clipBottom) continue;

            if ((uint32_t) fragment->atlasId >= renderer->atlasPageCount) continue;
            if (!N3DSRenderer_ensurePageLoaded(renderer, fragment->atlasId)) continue;
            N3DSLoadedAtlasPage* page = &renderer->atlasPages[fragment->atlasId];
            if (!page->ready) continue;
            renderer->frameFragmentDraws++;

            uint16_t px = (uint16_t) (fragment->x + (uint16_t) (clipLeft - fragLeft));
            uint16_t py = (uint16_t) (fragment->y + (uint16_t) (clipTop - fragTop));
            int32_t clipW = clipRight - clipLeft;
            int32_t clipH = clipBottom - clipTop;
            Tex3DS_SubTexture subtex;
            C2D_Image image = {
                .tex = &page->texture,
                .subtex = &subtex,
            };
            N3DSRenderer_fillSubTexture(&subtex, page, px, py, (uint16_t) clipW, (uint16_t) clipH);
            float drawX = x + (float) (clipLeft - reqLeft) * xscale;
            float drawY = y + (float) (clipTop - reqTop) * yscale;
            float localPivotX = pivotX - drawX;
            float localPivotY = pivotY - drawY;
            N3DSRenderer_drawImage(base, &image, drawX, drawY, (float) clipW * xscale, (float) clipH * yscale, localPivotX, localPivotY, angleDeg, color, alpha);
        }
        return;
    }

    if (tpagIndex < 0 || (uint32_t) tpagIndex >= renderer->atlasItemCount) return;
    N3DSAtlasItem* item = &renderer->atlasItems[tpagIndex];
    if ((uint32_t) item->atlasId >= renderer->atlasPageCount) return;
    if (!N3DSRenderer_ensurePageLoaded(renderer, item->atlasId)) return;
    N3DSLoadedAtlasPage* page = &renderer->atlasPages[item->atlasId];
    if (!page->ready) return;

    Tex3DS_SubTexture subtex;
    C2D_Image image = {
        .tex = &page->texture,
        .subtex = &subtex,
    };
    uint16_t px = (uint16_t) (item->x + srcOffX);
    uint16_t py = (uint16_t) (item->y + srcOffY);
    N3DSRenderer_fillSubTexture(&subtex, page, px, py, (uint16_t) srcW, (uint16_t) srcH);
    N3DSRenderer_drawImage(base, &image, x, y, (float) srcW * xscale, (float) srcH * yscale, pivotX - x, pivotY - y, angleDeg, color, alpha);
}

static bool N3DSRenderer_tryDrawTileRect(
    Renderer* base,
    int32_t tpagIndex,
    int32_t srcX,
    int32_t srcY,
    int32_t srcW,
    int32_t srcH,
    float drawX,
    float drawY,
    float xscale,
    float yscale,
    uint32_t color,
    float alpha
) {
    if (srcW <= 0 || srcH <= 0) return false;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    if (!N3DSRenderer_isFragmentedAtlasVersion(renderer->atlasVersion)) {
        N3DSRenderer_drawSpritePart(base, tpagIndex, srcX, srcY, srcW, srcH, drawX, drawY, xscale, yscale, 0.0f, 0.0f, 0.0f, color, alpha);
        return true;
    }

    if (tpagIndex < 0 || (uint32_t) tpagIndex >= renderer->atlasItemCount) return false;
    N3DSAtlasItemV3* item = &renderer->atlasItemsV3[tpagIndex];
    if (item->fragmentCount == 0) return false;

    int32_t reqLeft = srcX;
    int32_t reqTop = srcY;
    int32_t reqRight = srcX + srcW;
    int32_t reqBottom = srcY + srcH;
    bool drewAnything = false;

    if (item->fragmentCount == 1) {
        N3DSAtlasFragment* fragment = &renderer->atlasFragments[item->fragmentStart];
        if ((uint32_t) fragment->atlasId >= renderer->atlasPageCount) return false;
        if (!N3DSRenderer_ensurePageLoaded(renderer, fragment->atlasId)) return false;
        N3DSLoadedAtlasPage* page = &renderer->atlasPages[fragment->atlasId];
        if (!page->ready) return false;

        int32_t fragLeft = fragment->sourceX;
        int32_t fragTop = fragment->sourceY;
        int32_t fragRight = fragLeft + fragment->width;
        int32_t fragBottom = fragTop + fragment->height;
        int32_t clipLeft = reqLeft > fragLeft ? reqLeft : fragLeft;
        int32_t clipTop = reqTop > fragTop ? reqTop : fragTop;
        int32_t clipRight = reqRight < fragRight ? reqRight : fragRight;
        int32_t clipBottom = reqBottom < fragBottom ? reqBottom : fragBottom;
        if (clipLeft >= clipRight || clipTop >= clipBottom) return false;

        uint16_t px = (uint16_t) (fragment->x + (uint16_t) (clipLeft - fragLeft));
        uint16_t py = (uint16_t) (fragment->y + (uint16_t) (clipTop - fragTop));
        int32_t clipW = clipRight - clipLeft;
        int32_t clipH = clipBottom - clipTop;
        Tex3DS_SubTexture subtex;
        C2D_Image image = {
            .tex = &page->texture,
            .subtex = &subtex,
        };
        N3DSRenderer_fillSubTexture(&subtex, page, px, py, (uint16_t) clipW, (uint16_t) clipH);
        renderer->frameFragmentDraws++;
        N3DSRenderer_drawImageFast(
            base,
            &image,
            drawX + (float) (clipLeft - reqLeft) * xscale,
            drawY + (float) (clipTop - reqTop) * yscale,
            xscale,
            yscale,
            color,
            alpha
        );
        return true;
    }

    repeat(item->fragmentCount, fragIndex) {
        N3DSAtlasFragment* fragment = &renderer->atlasFragments[item->fragmentStart + fragIndex];
        int32_t fragLeft = fragment->sourceX;
        int32_t fragTop = fragment->sourceY;
        int32_t fragRight = fragLeft + fragment->width;
        int32_t fragBottom = fragTop + fragment->height;
        int32_t clipLeft = reqLeft > fragLeft ? reqLeft : fragLeft;
        int32_t clipTop = reqTop > fragTop ? reqTop : fragTop;
        int32_t clipRight = reqRight < fragRight ? reqRight : fragRight;
        int32_t clipBottom = reqBottom < fragBottom ? reqBottom : fragBottom;
        if (clipLeft >= clipRight || clipTop >= clipBottom) continue;

        if ((uint32_t) fragment->atlasId >= renderer->atlasPageCount) continue;
        if (!N3DSRenderer_ensurePageLoaded(renderer, fragment->atlasId)) continue;
        N3DSLoadedAtlasPage* page = &renderer->atlasPages[fragment->atlasId];
        if (!page->ready) continue;

        uint16_t px = (uint16_t) (fragment->x + (uint16_t) (clipLeft - fragLeft));
        uint16_t py = (uint16_t) (fragment->y + (uint16_t) (clipTop - fragTop));
        int32_t clipW = clipRight - clipLeft;
        int32_t clipH = clipBottom - clipTop;
        Tex3DS_SubTexture subtex;
        C2D_Image image = {
            .tex = &page->texture,
            .subtex = &subtex,
        };
        N3DSRenderer_fillSubTexture(&subtex, page, px, py, (uint16_t) clipW, (uint16_t) clipH);
        renderer->frameFragmentDraws++;
        N3DSRenderer_drawImageFast(
            base,
            &image,
            drawX + (float) (clipLeft - reqLeft) * xscale,
            drawY + (float) (clipTop - reqTop) * yscale,
            xscale,
            yscale,
            color,
            alpha
        );
        drewAnything = true;
    }

    return drewAnything;
}

static bool N3DSRenderer_drawPackedTileEntry(
    Renderer* base,
    N3DSRenderer* renderer,
    const N3DSTileAtlasEntry* tileEntry,
    float drawX,
    float drawY,
    float xscale,
    float yscale,
    uint32_t color,
    float alpha
) {
    if (tileEntry == NULL) return false;

    if (tileEntry->fragmentCount == 0) {
        if (tileEntry->width == 0 ||
            tileEntry->height == 0 ||
            tileEntry->atlasId >= renderer->atlasPageCount ||
            !N3DSRenderer_ensurePageLoaded(renderer, tileEntry->atlasId)) {
            return false;
        }

        N3DSLoadedAtlasPage* page = &renderer->atlasPages[tileEntry->atlasId];
        if (!page->ready) return false;

        Tex3DS_SubTexture subtex;
        C2D_Image image = {
            .tex = &page->texture,
            .subtex = &subtex,
        };
        N3DSRenderer_fillSubTexture(&subtex, page, tileEntry->x, tileEntry->y, tileEntry->width, tileEntry->height);
        N3DSRenderer_drawImageFast(base, &image, drawX, drawY, xscale, yscale, color, alpha);
        return true;
    }

    bool drewAnything = false;
    repeat(tileEntry->fragmentCount, fragIndex) {
        uint32_t fragmentIndex = tileEntry->fragmentStart + (uint32_t) fragIndex;
        if (fragmentIndex >= renderer->atlasFragmentCount) break;
        N3DSAtlasFragment* fragment = &renderer->atlasFragments[fragmentIndex];
        if (!N3DSRenderer_ensurePageLoaded(renderer, fragment->atlasId)) continue;
        N3DSLoadedAtlasPage* page = &renderer->atlasPages[fragment->atlasId];
        if (!page->ready) continue;

        Tex3DS_SubTexture subtex;
        C2D_Image image = {
            .tex = &page->texture,
            .subtex = &subtex,
        };
        N3DSRenderer_fillSubTexture(&subtex, page, fragment->x, fragment->y, fragment->width, fragment->height);
        renderer->frameFragmentDraws++;
        N3DSRenderer_drawImageFast(
            base,
            &image,
            drawX + (float) fragment->sourceX * xscale,
            drawY + (float) fragment->sourceY * yscale,
            xscale,
            yscale,
            color,
            alpha
        );
        drewAnything = true;
    }

    return drewAnything;
}

static void N3DSRenderer_drawTile(Renderer* base, RoomTile* tile, float offsetX, float offsetY) {
    if (base == NULL || tile == NULL) return;
    int32_t srcW = (int32_t) tile->width;
    int32_t srcH = (int32_t) tile->height;
    if (srcW <= 0 || srcH <= 0) return;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    uint32_t bgr = tile->color & 0x00FFFFFFu;
    uint8_t alphaByte = (uint8_t) ((tile->color >> 24) & 0xFFu);
    float alpha = (alphaByte == 0) ? 1.0f : (float) alphaByte / 255.0f;
    float drawX = (float) tile->x + offsetX;
    float drawY = (float) tile->y + offsetY;
    if (!Renderer_isFiniteFloat(drawX) || !Renderer_isFiniteFloat(drawY) ||
        !Renderer_isFiniteFloat(tile->scaleX) || !Renderer_isFiniteFloat(tile->scaleY) ||
        tile->scaleX == 0.0f || tile->scaleY == 0.0f) {
        return;
    }

    N3DSTileAtlasEntry* tileEntry = N3DSRenderer_findTileEntry(renderer, tile);
    if (tileEntry != NULL &&
        N3DSRenderer_drawPackedTileEntry(base, renderer, tileEntry, drawX, drawY, tile->scaleX, tile->scaleY, bgr, alpha)) {
        return;
    }

    int32_t tpagIndex = Renderer_resolveObjectTPAGIndex(base->dataWin, tile);
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= base->dataWin->tpag.count) return;
    TexturePageItem* tpag = &base->dataWin->tpag.items[tpagIndex];

    if (N3DSRenderer_tryDrawTileRect(
            base,
            tpagIndex,
            tile->sourceX,
            tile->sourceY,
            srcW,
            srcH,
            drawX,
            drawY,
            tile->scaleX,
            tile->scaleY,
            bgr,
            alpha
        )) {
        return;
    }

    int32_t fallbackSrcX = tile->sourceX - tpag->targetX;
    int32_t fallbackSrcY = tile->sourceY - tpag->targetY;
    (void) N3DSRenderer_tryDrawTileRect(
        base,
        tpagIndex,
        fallbackSrcX,
        fallbackSrcY,
        srcW,
        srcH,
        drawX,
        drawY,
        tile->scaleX,
        tile->scaleY,
        bgr,
        alpha
    );
}

static void N3DSRenderer_drawTiled(Renderer* base, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha) {
    if (!Renderer_isFiniteFloat(originX) || !Renderer_isFiniteFloat(originY) ||
        !Renderer_isFiniteFloat(x) || !Renderer_isFiniteFloat(y) ||
        !Renderer_isFiniteFloat(xscale) || !Renderer_isFiniteFloat(yscale) ||
        !Renderer_isFiniteFloat(roomW) || !Renderer_isFiniteFloat(roomH) ||
        !Renderer_isFiniteFloat(alpha) || xscale == 0.0f || yscale == 0.0f) {
        return;
    }

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= base->dataWin->tpag.count) return;
    TexturePageItem* tpag = &base->dataWin->tpag.items[tpagIndex];
    if (tpag->boundingWidth == 0 || tpag->boundingHeight == 0) return;

    int32_t cropX = 0;
    int32_t cropY = 0;
    int32_t cropW = (int32_t) tpag->sourceWidth;
    int32_t cropH = (int32_t) tpag->sourceHeight;
    if (!N3DSRenderer_getSourceCropRect(renderer, tpagIndex, &cropX, &cropY, &cropW, &cropH)) {
        return;
    }
    if (cropW <= 0 || cropH <= 0) return;

    C2D_Image* directImage = NULL;
    if (renderer->tpagToSpriteIndex != NULL && renderer->tpagToSpriteFrameIndex != NULL &&
        tpagIndex >= 0 && (uint32_t) tpagIndex < renderer->base.dataWin->tpag.count) {
        int32_t spriteIndex = renderer->tpagToSpriteIndex[tpagIndex];
        int32_t frameIndex = renderer->tpagToSpriteFrameIndex[tpagIndex];
        if (spriteIndex >= 0 && frameIndex >= 0) {
            N3DSRenderer_tryLoadDirectSpriteImage(renderer, spriteIndex, frameIndex, &directImage);
        }
    }
    if (directImage == NULL &&
        renderer->tpagToBackgroundIndex != NULL &&
        tpagIndex >= 0 && (uint32_t) tpagIndex < renderer->base.dataWin->tpag.count) {
        int32_t backgroundIndex = renderer->tpagToBackgroundIndex[tpagIndex];
        if (backgroundIndex >= 0) {
            N3DSRenderer_tryLoadDirectBackgroundImage(renderer, backgroundIndex, &directImage);
        }
    }
    if (directImage != NULL && directImage->subtex != NULL) {
        cropX = 0;
        cropY = 0;
        cropW = (int32_t) directImage->subtex->width;
        cropH = (int32_t) directImage->subtex->height;
    }

    float axScale = fabsf(xscale);
    float ayScale = fabsf(yscale);
    float tileW = (float) cropW * axScale;
    float tileH = (float) cropH * ayScale;
    if (tileW <= 0.0f || tileH <= 0.0f) return;

    float startX;
    float endX;
    float startY;
    float endY;
    if (tileX) {
        startX = fmodf(x - originX * axScale, tileW);
        if (startX > 0.0f) startX -= tileW;
        endX = roomW;
    } else {
        startX = x - originX * axScale;
        endX = startX + tileW;
    }
    if (tileY) {
        startY = fmodf(y - originY * ayScale, tileH);
        if (startY > 0.0f) startY -= tileH;
        endY = roomH;
    } else {
        startY = y - originY * ayScale;
        endY = startY + tileH;
    }

    float dxLocalX0 = (float) cropX * xscale + originX * (axScale - xscale);
    float dyLocalY0 = (float) cropY * yscale + originY * (ayScale - yscale);
    float tileGameW = (float) cropW * xscale;
    float tileGameH = (float) cropH * yscale;

    int32_t tilesX = tileX ? ((int32_t) ((endX - startX) / tileW) + 1) : 1;
    int32_t tilesY = tileY ? ((int32_t) ((endY - startY) / tileH) + 1) : 1;
    if (tilesX <= 0 || tilesY <= 0) return;

    float viewRight = (renderer->viewScaleX != 0.0f) ? ((float) renderer->viewX + ((float) N3DS_TOP_WIDTH / fabsf(renderer->viewScaleX))) : (float) renderer->viewX;
    float viewBottom = (renderer->viewScaleY != 0.0f) ? ((float) renderer->viewY + ((float) N3DS_TOP_HEIGHT / fabsf(renderer->viewScaleY))) : (float) renderer->viewY;
    int32_t startTileX = 0;
    int32_t endTileX = tilesX;
    int32_t startTileY = 0;
    int32_t endTileY = tilesY;

    if (tileX) {
        float firstVisibleLeft = startX + dxLocalX0;
        float minTileX = floorf((((float) renderer->viewX) - (firstVisibleLeft + tileGameW)) / tileW) + 1.0f;
        float maxTileX = ceilf((viewRight - firstVisibleLeft) / tileW);
        startTileX = (int32_t) minTileX;
        endTileX = (int32_t) maxTileX;
        if (startTileX < 0) startTileX = 0;
        if (endTileX > tilesX) endTileX = tilesX;
    }

    if (tileY) {
        float firstVisibleTop = startY + dyLocalY0;
        float minTileY = floorf((((float) renderer->viewY) - (firstVisibleTop + tileGameH)) / tileH) + 1.0f;
        float maxTileY = ceilf((viewBottom - firstVisibleTop) / tileH);
        startTileY = (int32_t) minTileY;
        endTileY = (int32_t) maxTileY;
        if (startTileY < 0) startTileY = 0;
        if (endTileY > tilesY) endTileY = tilesY;
    }

    if (startTileX >= endTileX || startTileY >= endTileY) return;

    for (int32_t iy = startTileY; iy < endTileY; iy++) {
        float dy = startY + (float) iy * tileH;
        if (dy >= endY) break;

        float localTop = dy + dyLocalY0;
        float localBottom = localTop + tileGameH;
        float localMinY = localTop < localBottom ? localTop : localBottom;
        float localMaxY = localTop > localBottom ? localTop : localBottom;
        float visTop = localMinY > (float) renderer->viewY ? localMinY : (float) renderer->viewY;
        float visBottom = localMaxY < viewBottom ? localMaxY : viewBottom;
        if (visTop >= visBottom) continue;

        for (int32_t ix = startTileX; ix < endTileX; ix++) {
            float dx = startX + (float) ix * tileW;
            if (dx >= endX) break;

            float localLeft = dx + dxLocalX0;
            float localRight = localLeft + tileGameW;
            float localMinX = localLeft < localRight ? localLeft : localRight;
            float localMaxX = localLeft > localRight ? localLeft : localRight;
            float visLeft = localMinX > (float) renderer->viewX ? localMinX : (float) renderer->viewX;
            float visRight = localMaxX < viewRight ? localMaxX : viewRight;
            if (visLeft >= visRight) continue;

            float drawX = dx + dxLocalX0;
            float drawY = dy + dyLocalY0;
            if (directImage != NULL) {
                renderer->frameDirectSpriteHits++;
                if (fabsf(xscale - 1.0f) < 0.001f && fabsf(yscale - 1.0f) < 0.001f) {
                    N3DSRenderer_drawImageFast(base, directImage, drawX, drawY, 1.0f, 1.0f, color, alpha);
                } else {
                    N3DSRenderer_drawImage(
                        base,
                        directImage,
                        drawX,
                        drawY,
                        (float) cropW * xscale,
                        (float) cropH * yscale,
                        0.0f,
                        0.0f,
                        0.0f,
                        color,
                        alpha
                    );
                }
            } else if (!N3DSRenderer_tryDrawTileRect(base, tpagIndex, cropX, cropY, cropW, cropH, drawX, drawY, xscale, yscale, color, alpha)) {
                N3DSRenderer_drawSpritePart(base, tpagIndex, cropX, cropY, cropW, cropH, drawX, drawY, xscale, yscale, 0.0f, 0.0f, 0.0f, color, alpha);
            }
        }
    }
}

static void N3DSRenderer_drawTiledPart(Renderer* base, int32_t tpagIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, float dstX, float dstY, float dstW, float dstH, uint32_t color, float alpha) {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0.0f || dstH <= 0.0f) return;
    if (!Renderer_isFiniteFloat(dstX) || !Renderer_isFiniteFloat(dstY) ||
        !Renderer_isFiniteFloat(dstW) || !Renderer_isFiniteFloat(dstH) ||
        !Renderer_isFiniteFloat(alpha)) {
        return;
    }

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    int32_t cropX = 0;
    int32_t cropY = 0;
    int32_t cropW = 0;
    int32_t cropH = 0;
    if (!N3DSRenderer_getSourceCropRect(renderer, tpagIndex, &cropX, &cropY, &cropW, &cropH)) return;

    int32_t tilesY = (int32_t) (dstH / (float) srcH) + 2;
    int32_t tilesX = (int32_t) (dstW / (float) srcW) + 2;

    repeat(tilesY, iy) {
        float rowDstY = dstY + (float) iy * (float) srcH;
        if (rowDstY >= dstY + dstH) break;
        int32_t rowSrcH = srcH;
        if (rowDstY + (float) rowSrcH > dstY + dstH) {
            rowSrcH = (int32_t) ((dstY + dstH) - rowDstY);
        }
        if (rowSrcH <= 0) continue;

        int32_t intY1 = srcY > cropY ? srcY : cropY;
        int32_t intY2 = (srcY + rowSrcH) < (cropY + cropH) ? (srcY + rowSrcH) : (cropY + cropH);
        if (intY1 >= intY2) continue;

        float clipOffY = (float) (intY1 - srcY);
        int32_t visH = intY2 - intY1;

        repeat(tilesX, ix) {
            float colDstX = dstX + (float) ix * (float) srcW;
            if (colDstX >= dstX + dstW) break;
            int32_t colSrcW = srcW;
            if (colDstX + (float) colSrcW > dstX + dstW) {
                colSrcW = (int32_t) ((dstX + dstW) - colDstX);
            }
            if (colSrcW <= 0) continue;

            int32_t intX1 = srcX > cropX ? srcX : cropX;
            int32_t intX2 = (srcX + colSrcW) < (cropX + cropW) ? (srcX + colSrcW) : (cropX + cropW);
            if (intX1 >= intX2) continue;

            float clipOffX = (float) (intX1 - srcX);
            int32_t visW = intX2 - intX1;
            float drawX = colDstX + clipOffX;
            float drawY = rowDstY + clipOffY;

            if (!N3DSRenderer_tryDrawTileRect(base, tpagIndex, intX1, intY1, visW, visH, drawX, drawY, 1.0f, 1.0f, color, alpha)) {
                N3DSRenderer_drawSpritePart(base, tpagIndex, intX1, intY1, visW, visH, drawX, drawY, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, color, alpha);
            }
        }
    }
}

static void N3DSRenderer_drawSpritePos(Renderer* base, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha) {
    if (!Renderer_isFiniteFloat(x1) || !Renderer_isFiniteFloat(y1) ||
        !Renderer_isFiniteFloat(x2) || !Renderer_isFiniteFloat(y2) ||
        !Renderer_isFiniteFloat(x3) || !Renderer_isFiniteFloat(y3) ||
        !Renderer_isFiniteFloat(x4) || !Renderer_isFiniteFloat(y4) ||
        !Renderer_isFiniteFloat(alpha)) {
        return;
    }

    bool axisAligned =
        fabsf(y1 - y2) < 0.01f &&
        fabsf(x2 - x3) < 0.01f &&
        fabsf(y3 - y4) < 0.01f &&
        fabsf(x4 - x1) < 0.01f;
    if (!axisAligned) return;

    N3DSRenderer_drawSpritePart(
        base,
        tpagIndex,
        0,
        0,
        (int32_t) lroundf(fabsf(x2 - x1)),
        (int32_t) lroundf(fabsf(y4 - y1)),
        x1,
        y1,
        1.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0xFFFFFF,
        alpha
    );
}

static void N3DSRenderer_drawRectangle(Renderer* base, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    if (!Renderer_isFiniteFloat(x1) || !Renderer_isFiniteFloat(y1) ||
        !Renderer_isFiniteFloat(x2) || !Renderer_isFiniteFloat(y2) ||
        !Renderer_isFiniteFloat(alpha)) {
        return;
    }

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    float sx1 = N3DSRenderer_transformScreenX(renderer, x1);
    float sy1 = N3DSRenderer_transformScreenY(renderer, y1);
    float sx2 = N3DSRenderer_transformScreenX(renderer, x2);
    float sy2 = N3DSRenderer_transformScreenY(renderer, y2);
    float sx = sx1 < sx2 ? sx1 : sx2;
    float sy = sy1 < sy2 ? sy1 : sy2;
    float sw = fabsf(sx2 - sx1);
    float sh = fabsf(sy2 - sy1);
    u32 rgba = N3DSRenderer_makeColor(color, alpha);

    if (outline) {
        C2D_DrawLine(sx, sy, rgba, sx + sw, sy, rgba, 1.0f, 0.5f);
        C2D_DrawLine(sx + sw, sy, rgba, sx + sw, sy + sh, rgba, 1.0f, 0.5f);
        C2D_DrawLine(sx + sw, sy + sh, rgba, sx, sy + sh, rgba, 1.0f, 0.5f);
        C2D_DrawLine(sx, sy + sh, rgba, sx, sy, rgba, 1.0f, 0.5f);
        N3DSRenderer_noteC2DDraws(renderer, 4u);
    } else {
        C2D_DrawRectSolid(sx, sy, 0.5f, sw, sh, rgba);
        N3DSRenderer_noteC2DDraws(renderer, 1u);
    }
}

static void N3DSRenderer_drawLine(Renderer* base, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    if (!Renderer_isFiniteFloat(x1) || !Renderer_isFiniteFloat(y1) ||
        !Renderer_isFiniteFloat(x2) || !Renderer_isFiniteFloat(y2) ||
        !Renderer_isFiniteFloat(width) || !Renderer_isFiniteFloat(alpha)) {
        return;
    }

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    float sx1 = N3DSRenderer_transformScreenX(renderer, x1);
    float sy1 = N3DSRenderer_transformScreenY(renderer, y1);
    float sx2 = N3DSRenderer_transformScreenX(renderer, x2);
    float sy2 = N3DSRenderer_transformScreenY(renderer, y2);
    if (!Renderer_isFiniteFloat(sx1) || !Renderer_isFiniteFloat(sy1) ||
        !Renderer_isFiniteFloat(sx2) || !Renderer_isFiniteFloat(sy2)) {
        return;
    }

    width = fabsf(width);
    if (width < 1.0f) width = 1.0f;

    float dx = sx2 - sx1;
    float dy = sy2 - sy1;
    if ((dx * dx + dy * dy) <= 0.0001f) {
        C2D_DrawRectSolid(sx1, sy1, 0.5f, width, width, N3DSRenderer_makeColor(color, alpha));
        N3DSRenderer_noteC2DDraws(renderer, 1u);
        return;
    }

    u32 rgba = N3DSRenderer_makeColor(color, alpha);
    C2D_DrawLine(
        sx1,
        sy1,
        rgba,
        sx2,
        sy2,
        rgba,
        width,
        0.5f
    );
    N3DSRenderer_noteC2DDraws(renderer, 1u);
}

static void N3DSRenderer_drawTriangle(Renderer* base, float x1, float y1, float x2, float y2, float x3, float y3, bool outline) {
    if (!Renderer_isFiniteFloat(x1) || !Renderer_isFiniteFloat(y1) ||
        !Renderer_isFiniteFloat(x2) || !Renderer_isFiniteFloat(y2) ||
        !Renderer_isFiniteFloat(x3) || !Renderer_isFiniteFloat(y3)) {
        return;
    }

    if (outline) {
        N3DSRenderer_drawLine(base, x1, y1, x2, y2, 1.0f, base->drawColor, base->drawAlpha);
        N3DSRenderer_drawLine(base, x2, y2, x3, y3, 1.0f, base->drawColor, base->drawAlpha);
        N3DSRenderer_drawLine(base, x3, y3, x1, y1, 1.0f, base->drawColor, base->drawAlpha);
        return;
    }

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    float sx1 = N3DSRenderer_transformScreenX(renderer, x1);
    float sy1 = N3DSRenderer_transformScreenY(renderer, y1);
    float sx2 = N3DSRenderer_transformScreenX(renderer, x2);
    float sy2 = N3DSRenderer_transformScreenY(renderer, y2);
    float sx3 = N3DSRenderer_transformScreenX(renderer, x3);
    float sy3 = N3DSRenderer_transformScreenY(renderer, y3);
    if (!Renderer_isFiniteFloat(sx1) || !Renderer_isFiniteFloat(sy1) ||
        !Renderer_isFiniteFloat(sx2) || !Renderer_isFiniteFloat(sy2) ||
        !Renderer_isFiniteFloat(sx3) || !Renderer_isFiniteFloat(sy3)) {
        return;
    }

    float minX = sx1 < sx2 ? sx1 : sx2;
    if (sx3 < minX) minX = sx3;
    float maxX = sx1 > sx2 ? sx1 : sx2;
    if (sx3 > maxX) maxX = sx3;
    float minY = sy1 < sy2 ? sy1 : sy2;
    if (sy3 < minY) minY = sy3;
    float maxY = sy1 > sy2 ? sy1 : sy2;
    if (sy3 > maxY) maxY = sy3;
    if (N3DSRenderer_isScreenRectOffscreen(renderer, minX, minY, maxX - minX, maxY - minY)) {
        return;
    }

    // Tiny circles and snapped coordinates can collapse adjacent fan triangles to zero area.
    // Citro2D appears to dislike those degenerate cases, so skip them before issuing the draw.
    float doubledArea =
        ((sx2 - sx1) * (sy3 - sy1)) -
        ((sy2 - sy1) * (sx3 - sx1));
    if (fabsf(doubledArea) < 0.01f) {
        return;
    }

    u32 rgba = N3DSRenderer_makeColor(base->drawColor, base->drawAlpha);
    C2D_DrawTriangle(
        sx1,
        sy1,
        rgba,
        sx2,
        sy2,
        rgba,
        sx3,
        sy3,
        rgba,
        0.5f
    );
    N3DSRenderer_noteC2DDraws(renderer, 1u);
}

static void N3DSRenderer_drawLineColor(Renderer* base, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    if (!Renderer_isFiniteFloat(x1) || !Renderer_isFiniteFloat(y1) ||
        !Renderer_isFiniteFloat(x2) || !Renderer_isFiniteFloat(y2) ||
        !Renderer_isFiniteFloat(width) || !Renderer_isFiniteFloat(alpha)) {
        return;
    }

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    float sx1 = N3DSRenderer_transformScreenX(renderer, x1);
    float sy1 = N3DSRenderer_transformScreenY(renderer, y1);
    float sx2 = N3DSRenderer_transformScreenX(renderer, x2);
    float sy2 = N3DSRenderer_transformScreenY(renderer, y2);
    if (!Renderer_isFiniteFloat(sx1) || !Renderer_isFiniteFloat(sy1) ||
        !Renderer_isFiniteFloat(sx2) || !Renderer_isFiniteFloat(sy2)) {
        return;
    }

    width = fabsf(width);
    if (width < 1.0f) width = 1.0f;

    float dx = sx2 - sx1;
    float dy = sy2 - sy1;
    if ((dx * dx + dy * dy) <= 0.0001f) {
        C2D_DrawRectSolid(sx1, sy1, 0.5f, width, width, N3DSRenderer_makeColor(color1, alpha));
        N3DSRenderer_noteC2DDraws(renderer, 1u);
        return;
    }

    C2D_DrawLine(
        sx1,
        sy1,
        N3DSRenderer_makeColor(color1, alpha),
        sx2,
        sy2,
        N3DSRenderer_makeColor(color2, alpha),
        width,
        0.5f
    );
    N3DSRenderer_noteC2DDraws(renderer, 1u);
}

static void N3DSRenderer_drawTextCommon(Renderer* base, const char* text, float x, float y, float xscale, float yscale, float angleDeg, bool gradient, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha) {
    DataWin* dw = base->dataWin;
    if (base->drawFont < 0 || (uint32_t) base->drawFont >= dw->font.count) return;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    bool savedTextLinearFilterActive = renderer->textLinearFilterActive;
    renderer->textLinearFilterActive = renderer->topScreenGui2xActive;
    u64 textStartTick = svcGetSystemTick();
    Font* font = &dw->font.fonts[base->drawFont];
    float effectiveXScale = xscale * font->scaleX;
    float effectiveYScale = yscale * font->scaleY;
    if (!font->isSpriteFont && font->tpagIndex >= 0) {
        N3DSRenderer_traceTPAGUsage(renderer, N3DS_TRACE_KIND_FONT, font->tpagIndex);
    }
    int32_t len = (int32_t) strlen(text);
    const N3DSAtlasFragment* fontFragment = NULL;
    N3DSLoadedAtlasPage* fontPage = NULL;
    C2D_Image directFontImage;
    Tex3DS_SubTexture directFontBaseSubtex;
    bool useDirectFontAsset =
        !font->isSpriteFont &&
        N3DSRenderer_tryResolveDirectFontImage(renderer, base->drawFont, &directFontImage, &directFontBaseSubtex);
    bool useSingleFragmentFontFastPath =
        !useDirectFontAsset &&
        !font->isSpriteFont &&
        N3DSRenderer_tryResolveSingleFragmentFontPage(renderer, font->tpagIndex, &fontFragment, &fontPage);
    if (useDirectFontAsset || useSingleFragmentFontFastPath) {
        if (N3DSRenderer_tryDrawSingleGlyphTextFast(
                base,
                renderer,
                font,
                text,
                len,
                x,
                y,
                effectiveXScale,
                effectiveYScale,
                angleDeg,
                gradient,
                c1,
                alpha,
                useDirectFontAsset,
                useDirectFontAsset ? &directFontImage : NULL,
                useDirectFontAsset ? &directFontBaseSubtex : NULL,
                fontFragment,
                fontPage)) {
            (void) c2;
            (void) c3;
            (void) c4;
            renderer->frameTextTenthsMs += (uint32_t) lround(N3DSRenderer_ticksToMs(svcGetSystemTick() - textStartTick) * 10.0);
            renderer->textLinearFilterActive = savedTextLinearFilterActive;
            return;
        }

        const N3DSCachedTextLayout* layout = N3DSRenderer_getCachedTextLayout(renderer, font, base->drawFont, text);
        repeat(layout->glyphCount, i) {
            const N3DSCachedTextGlyph* cachedGlyph = &layout->glyphs[i];
            Tex3DS_SubTexture subtex;
            C2D_Image image;
            if (useDirectFontAsset) {
                subtex.width = cachedGlyph->sourceWidth;
                subtex.height = cachedGlyph->sourceHeight;
                float baseLeft = directFontBaseSubtex.left;
                float baseRight = directFontBaseSubtex.right;
                float baseTop = directFontBaseSubtex.top;
                float baseBottom = directFontBaseSubtex.bottom;
                float baseWidth = (float) directFontBaseSubtex.width;
                float baseHeight = (float) directFontBaseSubtex.height;
                subtex.left = baseLeft + (baseRight - baseLeft) * ((float) cachedGlyph->sourceX / baseWidth);
                subtex.right = baseLeft + (baseRight - baseLeft) * (((float) cachedGlyph->sourceX + (float) cachedGlyph->sourceWidth) / baseWidth);
                subtex.top = baseTop + (baseBottom - baseTop) * ((float) cachedGlyph->sourceY / baseHeight);
                subtex.bottom = baseTop + (baseBottom - baseTop) * (((float) cachedGlyph->sourceY + (float) cachedGlyph->sourceHeight) / baseHeight);
                image.tex = directFontImage.tex;
                image.subtex = &subtex;
            } else {
                image.tex = &fontPage->texture;
                image.subtex = &subtex;
                uint16_t px = (uint16_t) (fontFragment->x + cachedGlyph->sourceX);
                uint16_t py = (uint16_t) (fontFragment->y + cachedGlyph->sourceY);
                N3DSRenderer_fillSubTexture(&subtex, fontPage, px, py, cachedGlyph->sourceWidth, cachedGlyph->sourceHeight);
            }
            float drawX = x + cachedGlyph->localX * effectiveXScale;
            float drawY = y + cachedGlyph->localY * effectiveYScale;
            renderer->frameTextGlyphDraws++;
            if (fabsf(angleDeg) < 0.001f) {
                N3DSRenderer_drawImageFast(base, &image, drawX, drawY, effectiveXScale, effectiveYScale, gradient ? (uint32_t) c1 : base->drawColor, alpha);
            } else {
                N3DSRenderer_drawImage(base, &image, drawX, drawY, (float) cachedGlyph->sourceWidth * effectiveXScale, (float) cachedGlyph->sourceHeight * effectiveYScale, 0.0f, 0.0f, angleDeg, gradient ? (uint32_t) c1 : base->drawColor, alpha);
            }
        }
        (void) c2;
        (void) c3;
        (void) c4;
        renderer->frameTextTenthsMs += (uint32_t) lround(N3DSRenderer_ticksToMs(svcGetSystemTick() - textStartTick) * 10.0);
        renderer->textLinearFilterActive = savedTextLinearFilterActive;
        return;
    }
    int32_t lineCount = TextUtils_countLines(text, len);
    float totalHeight = (float) lineCount * TextUtils_lineStride(font) * effectiveYScale;
    float valignOffset = 0.0f;
    if (base->drawValign == 1) valignOffset = -totalHeight * 0.5f;
    else if (base->drawValign == 2) valignOffset = -totalHeight;

    float cursorY = y + valignOffset - (float) font->ascenderOffset * effectiveYScale;
    int32_t lineStart = 0;

    while (lineStart <= len) {
        int32_t lineEnd = lineStart;
        while (lineEnd < len && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineEnd - lineStart) * effectiveXScale;
        float cursorX = x;
        if (base->drawHalign == 1) cursorX -= lineWidth * 0.5f;
        else if (base->drawHalign == 2) cursorX -= lineWidth;

        int32_t pos = lineStart;
        while (pos < lineEnd) {
            uint16_t ch = TextUtils_decodeUtf8(text, lineEnd, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == NULL) continue;

            uint32_t glyphColor = gradient ? (uint32_t) c1 : base->drawColor;
            if (font->isSpriteFont) {
                int32_t glyphIndex = (int32_t) (glyph - font->glyphs);
                Sprite* sprite = &dw->sprt.sprites[font->spriteIndex];
                if (glyphIndex >= 0 && glyphIndex < (int32_t) sprite->textureCount) {
                    int32_t glyphTpagIndex = sprite->tpagIndices[glyphIndex];
                    if (glyphTpagIndex >= 0 && (uint32_t) glyphTpagIndex < dw->tpag.count) {
                        TexturePageItem* glyphTpag = &dw->tpag.items[glyphTpagIndex];
                        renderer->frameTextGlyphDraws++;
                        N3DSRenderer_drawSprite(
                            base,
                            glyphTpagIndex,
                            cursorX + (float) glyph->offset * effectiveXScale,
                            cursorY,
                            (float) glyphTpag->targetX,
                            (float) sprite->originY,
                            effectiveXScale,
                            effectiveYScale,
                            angleDeg,
                            glyphColor,
                            alpha
                        );
                    }
                }
            } else {
                renderer->frameTextGlyphDraws++;
                N3DSRenderer_drawSpritePart(
                    base,
                    font->tpagIndex,
                    glyph->sourceX,
                    glyph->sourceY,
                    glyph->sourceWidth,
                    glyph->sourceHeight,
                    cursorX + (float) glyph->offset * effectiveXScale,
                    cursorY,
                    effectiveXScale,
                    effectiveYScale,
                    angleDeg,
                    0.0f,
                    0.0f,
                    glyphColor,
                    alpha
                );
            }

            if (pos < lineEnd) {
                int32_t previewPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(text, lineEnd, &previewPos);
                cursorX += ((float) glyph->shift + TextUtils_getKerningOffset(glyph, nextCh)) * effectiveXScale;
            } else {
                cursorX += (float) glyph->shift * effectiveXScale;
            }
        }

        if (lineEnd >= len) break;
        lineStart = TextUtils_skipNewline(text, lineEnd, len);
        cursorY += TextUtils_lineStride(font) * effectiveYScale;
    }

    (void) c2;
    (void) c3;
    (void) c4;
    renderer->frameTextTenthsMs += (uint32_t) lround(N3DSRenderer_ticksToMs(svcGetSystemTick() - textStartTick) * 10.0);
    renderer->textLinearFilterActive = savedTextLinearFilterActive;
}

static void N3DSRenderer_drawText(Renderer* base, const char* text, float x, float y, float xscale, float yscale, float angleDeg) {
    N3DSRenderer_drawTextCommon(base, text, x, y, xscale, yscale, angleDeg, false, 0, 0, 0, 0, base->drawAlpha);
}

static void N3DSRenderer_drawTextColor(Renderer* base, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha) {
    N3DSRenderer_drawTextCommon(base, text, x, y, xscale, yscale, angleDeg, true, c1, c2, c3, c4, alpha);
}

static void N3DSRenderer_flush(Renderer* base) {
    if (base == NULL) return;
    N3DSRenderer_flushC2DQueue((N3DSRenderer*) base);
}

static void N3DSRenderer_clearScreen(Renderer* base, uint32_t color, float alpha) {
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    renderer->clearColor = color;
    renderer->clearAlpha = alpha;
    C2D_TargetClear(renderer->topTarget, N3DSRenderer_makeColor(color, alpha));
}

static int32_t N3DSRenderer_createSpriteFromSurface(Renderer* base, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    if (base == NULL || base->dataWin == NULL || surfaceID != -1 || w <= 0 || h <= 0) return -1;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    if (renderer->activeSceneTarget != N3DS_SCENE_TARGET_TOP) return -1;

    float screenRectX = 0.0f;
    float screenRectY = 0.0f;
    float screenRectW = 0.0f;
    float screenRectH = 0.0f;
    N3DSRenderer_transformScreenRect(renderer, (float) x, (float) y, (float) w, (float) h, &screenRectX, &screenRectY, &screenRectW, &screenRectH);

    int32_t screenLeft = (int32_t) floorf(screenRectX);
    int32_t screenTop = (int32_t) floorf(screenRectY);
    int32_t screenRight = (int32_t) ceilf(screenRectX + screenRectW);
    int32_t screenBottom = (int32_t) ceilf(screenRectY + screenRectH);
    if (screenLeft < 0) screenLeft = 0;
    if (screenTop < 0) screenTop = 0;
    if (screenRight > N3DS_TOP_WIDTH) screenRight = N3DS_TOP_WIDTH;
    if (screenBottom > N3DS_TOP_HEIGHT) screenBottom = N3DS_TOP_HEIGHT;
    if (screenLeft >= screenRight || screenTop >= screenBottom) return -1;

    uint16_t captureWidth = (uint16_t) (screenRight - screenLeft);
    uint16_t captureHeight = (uint16_t) (screenBottom - screenTop);
    size_t pixelCount = (size_t) captureWidth * (size_t) captureHeight;
    uint16_t* pixels = (uint16_t*) linearAlloc(pixelCount * sizeof(uint16_t));
    if (pixels == NULL) return -1;

    N3DSRenderer_flushC2DQueue(renderer);
    C2D_Flush();
    uint8_t* framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (framebuffer == NULL) {
        linearFree(pixels);
        return -1;
    }

    uint16_t transparentKey = 0;
    bool haveTransparentKey = false;
    repeat(captureHeight, row) {
        int32_t screenY = screenTop + (int32_t) row;
        repeat(captureWidth, col) {
            int32_t screenX = screenLeft + (int32_t) col;
            size_t fbOffset = ((size_t) screenX * (size_t) N3DS_TOP_HEIGHT + (size_t) ((N3DS_TOP_HEIGHT - 1) - screenY)) * 3u;
            uint8_t r = framebuffer[fbOffset + 0u];
            uint8_t g = framebuffer[fbOffset + 1u];
            uint8_t b = framebuffer[fbOffset + 2u];
            uint16_t rgba5551 = (uint16_t) ((((uint16_t) r >> 3) << 11) | (((uint16_t) g >> 3) << 6) | (((uint16_t) b >> 3) << 1) | 1u);
            if (!haveTransparentKey) {
                transparentKey = rgba5551;
                haveTransparentKey = true;
            }
            if (removeback && rgba5551 == transparentKey) {
                rgba5551 &= (uint16_t) ~1u;
            }
            pixels[(size_t) row * (size_t) captureWidth + (size_t) col] = rgba5551;
        }
    }

    int32_t spriteIndex = (int32_t) DataWin_allocSpriteSlot(base->dataWin, base->dataWin->sprt.parsedCount);
    int32_t tpagIndex = N3DSRenderer_allocDynamicCaptureTPAG(renderer);
    if (spriteIndex < 0 || tpagIndex < 0) {
        linearFree(pixels);
        return -1;
    }

    N3DSDynamicCaptureTPAG* capture = &renderer->dynamicCaptureTPAGs[(uint32_t) tpagIndex - renderer->baseTPAGCount];
    if (!C3D_TexInit(&capture->texture, captureWidth, captureHeight, GPU_RGBA5551)) {
        linearFree(pixels);
        return -1;
    }

    C3D_TexSetFilter(&capture->texture, smooth ? GPU_LINEAR : GPU_NEAREST, smooth ? GPU_LINEAR : GPU_NEAREST);
    C3D_TexSetWrap(&capture->texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexUpload(&capture->texture, pixels);
    C3D_TexFlush(&capture->texture);
    linearFree(pixels);

    capture->used = true;
    capture->ownerSpriteIndex = spriteIndex;
    capture->logicalWidth = (uint16_t) w;
    capture->logicalHeight = (uint16_t) h;
    capture->subtex.width = captureWidth;
    capture->subtex.height = captureHeight;
    capture->subtex.left = 0.0f;
    capture->subtex.top = 1.0f;
    capture->subtex.right = 1.0f;
    capture->subtex.bottom = 0.0f;
    capture->image.tex = &capture->texture;
    capture->image.subtex = &capture->subtex;

    Sprite* sprite = &base->dataWin->sprt.sprites[spriteIndex];
    const char* preservedName = sprite->name;
    TexturePageItem* tpag = &base->dataWin->tpag.items[tpagIndex];
    memset(sprite, 0, sizeof(*sprite));
    sprite->name = preservedName;
    sprite->width = (uint32_t) w;
    sprite->height = (uint32_t) h;
    sprite->transparent = removeback;
    sprite->smooth = smooth;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1u;
    sprite->tpagIndices = safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = tpagIndex;

    memset(tpag, 0, sizeof(*tpag));
    tpag->sourceWidth = (uint16_t) w;
    tpag->sourceHeight = (uint16_t) h;
    tpag->targetWidth = (uint16_t) w;
    tpag->targetHeight = (uint16_t) h;
    tpag->boundingWidth = (uint16_t) w;
    tpag->boundingHeight = (uint16_t) h;
    tpag->texturePageId = -1;
    return spriteIndex;
}

static void N3DSRenderer_deleteSprite(Renderer* base, int32_t spriteIndex) {
    if (base == NULL || base->dataWin == NULL) return;
    DataWin* dw = base->dataWin;
    if (spriteIndex < 0 || (uint32_t) spriteIndex >= dw->sprt.count) return;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return;

    bool releasedDynamicCapture = false;
    repeat(renderer->dynamicCaptureTPAGCount, i) {
        N3DSDynamicCaptureTPAG* capture = &renderer->dynamicCaptureTPAGs[i];
        if (!capture->used || capture->ownerSpriteIndex != spriteIndex) continue;
        N3DSRenderer_freeDynamicCaptureTPAG(renderer, capture);
        TexturePageItem* tpag = &dw->tpag.items[renderer->baseTPAGCount + (uint32_t) i];
        memset(tpag, 0, sizeof(*tpag));
        tpag->texturePageId = -1;
        releasedDynamicCapture = true;
    }
    if (!releasedDynamicCapture) return;

    const char* preservedName = sprite->name;
    free(sprite->tpagIndices);
    sprite->tpagIndices = NULL;
    repeat(sprite->maskCount, maskIndex) {
        free(sprite->masks[maskIndex]);
    }
    free(sprite->masks);
    memset(sprite, 0, sizeof(*sprite));
    sprite->name = preservedName;
}

static void N3DSRenderer_gpuSetBlendMode(Renderer* base, int32_t mode) {
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    renderer->blendEnabled = true;
    renderer->blendEquation = mode;
    renderer->blendSrcFactor = (mode == bm_add) ? bm_src_alpha : bm_src_alpha;
    renderer->blendDstFactor = (mode == bm_add) ? bm_one : bm_inv_src_alpha;
    N3DSRenderer_applyBlendState(renderer);
}

static void N3DSRenderer_gpuSetBlendModeExt(Renderer* base, int32_t sfactor, int32_t dfactor) {
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    renderer->blendEnabled = true;
    renderer->blendEquation = bm_normal;
    renderer->blendSrcFactor = sfactor;
    renderer->blendDstFactor = dfactor;
    N3DSRenderer_applyBlendState(renderer);
}

static void N3DSRenderer_gpuSetBlendEnable(Renderer* base, bool enable) {
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    renderer->blendEnabled = enable;
    N3DSRenderer_applyBlendState(renderer);
}

static void N3DSRenderer_gpuSetAlphaTestEnable(Renderer* base, bool enable) {
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    renderer->alphaTestEnabled = enable;
    N3DSRenderer_applyAlphaState(renderer);
}

static void N3DSRenderer_gpuSetAlphaTestRef(Renderer* base, uint8_t ref) {
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    renderer->alphaTestRef = ref;
    N3DSRenderer_applyAlphaState(renderer);
}

static void N3DSRenderer_gpuSetColorWriteEnable(MAYBE_UNUSED Renderer* base, MAYBE_UNUSED bool red, MAYBE_UNUSED bool green, MAYBE_UNUSED bool blue, MAYBE_UNUSED bool alpha) {}
static void N3DSRenderer_gpuSetFog(MAYBE_UNUSED Renderer* base, MAYBE_UNUSED bool enable, MAYBE_UNUSED uint32_t color) {}

static int32_t N3DSRenderer_createSurface(MAYBE_UNUSED Renderer* base, MAYBE_UNUSED int32_t width, MAYBE_UNUSED int32_t height) { return -1; }
static bool N3DSRenderer_surfaceExists(MAYBE_UNUSED Renderer* base, MAYBE_UNUSED int32_t surfaceID) { return surfaceID == -1; }
static bool N3DSRenderer_setSurfaceTarget(MAYBE_UNUSED Renderer* base, MAYBE_UNUSED int32_t surfaceID) { return false; }
static bool N3DSRenderer_resetSurfaceTarget(MAYBE_UNUSED Renderer* base) { return false; }
static float N3DSRenderer_getSurfaceWidth(MAYBE_UNUSED Renderer* base, MAYBE_UNUSED int32_t surfaceID) { return 0.0f; }
static float N3DSRenderer_getSurfaceHeight(MAYBE_UNUSED Renderer* base, MAYBE_UNUSED int32_t surfaceID) { return 0.0f; }
static void N3DSRenderer_drawSurface(MAYBE_UNUSED Renderer* base, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED float angleDeg, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {}
static void N3DSRenderer_drawSurfacePart(MAYBE_UNUSED Renderer* base, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED int32_t x, MAYBE_UNUSED int32_t y, MAYBE_UNUSED int32_t left, MAYBE_UNUSED int32_t top, MAYBE_UNUSED int32_t width, MAYBE_UNUSED int32_t height, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {}
static void N3DSRenderer_drawSurfaceStretched(MAYBE_UNUSED Renderer* base, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float width, MAYBE_UNUSED float height) {}
static void N3DSRenderer_surfaceResize(MAYBE_UNUSED Renderer* base, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED int32_t width, MAYBE_UNUSED int32_t height) {}
static void N3DSRenderer_surfaceFree(MAYBE_UNUSED Renderer* base, MAYBE_UNUSED int32_t surfaceID) {}
static void N3DSRenderer_surfaceCopy(MAYBE_UNUSED Renderer* base, MAYBE_UNUSED int32_t DestSurfaceID, MAYBE_UNUSED int32_t DestX, MAYBE_UNUSED int32_t DestY, MAYBE_UNUSED int32_t SrcSurfaceID, MAYBE_UNUSED int32_t SrcX, MAYBE_UNUSED int32_t SrcY, MAYBE_UNUSED int32_t SrcW, MAYBE_UNUSED int32_t SrcH, MAYBE_UNUSED bool part) {}

static RendererVtable N3DSRenderer_vtable = {
    .init = N3DSRenderer_init,
    .destroy = N3DSRenderer_destroy,
    .beginFrame = N3DSRenderer_beginFrame,
    .endFrame = N3DSRenderer_endFrame,
    .beginView = N3DSRenderer_beginView,
    .endView = N3DSRenderer_endView,
    .beginGUI = N3DSRenderer_beginGUI,
    .endGUI = N3DSRenderer_endGUI,
    .drawSprite = N3DSRenderer_drawSprite,
    .drawSpritePart = N3DSRenderer_drawSpritePart,
    .drawSpritePos = N3DSRenderer_drawSpritePos,
    .drawRectangle = N3DSRenderer_drawRectangle,
    .drawLine = N3DSRenderer_drawLine,
    .drawTriangle = N3DSRenderer_drawTriangle,
    .drawLineColor = N3DSRenderer_drawLineColor,
    .drawText = N3DSRenderer_drawText,
    .drawTextColor = N3DSRenderer_drawTextColor,
    .flush = N3DSRenderer_flush,
    .clearScreen = N3DSRenderer_clearScreen,
    .createSpriteFromSurface = N3DSRenderer_createSpriteFromSurface,
    .deleteSprite = N3DSRenderer_deleteSprite,
    .gpuSetBlendMode = N3DSRenderer_gpuSetBlendMode,
    .gpuSetBlendModeExt = N3DSRenderer_gpuSetBlendModeExt,
    .gpuSetBlendEnable = N3DSRenderer_gpuSetBlendEnable,
    .gpuSetAlphaTestEnable = N3DSRenderer_gpuSetAlphaTestEnable,
    .gpuSetAlphaTestRef = N3DSRenderer_gpuSetAlphaTestRef,
    .gpuSetColorWriteEnable = N3DSRenderer_gpuSetColorWriteEnable,
    .gpuSetFog = N3DSRenderer_gpuSetFog,
    .drawTile = N3DSRenderer_drawTile,
    .prewarmRoom = N3DSRenderer_prewarmRoom,
    .drawTiled = N3DSRenderer_drawTiled,
    .drawTiledPart = N3DSRenderer_drawTiledPart,
    .createSurface = N3DSRenderer_createSurface,
    .surfaceExists = N3DSRenderer_surfaceExists,
    .setSurfaceTarget = N3DSRenderer_setSurfaceTarget,
    .resetSurfaceTarget = N3DSRenderer_resetSurfaceTarget,
    .getSurfaceWidth = N3DSRenderer_getSurfaceWidth,
    .getSurfaceHeight = N3DSRenderer_getSurfaceHeight,
    .drawSurface = N3DSRenderer_drawSurface,
    .drawSurfacePart = N3DSRenderer_drawSurfacePart,
    .drawSurfaceStretched = N3DSRenderer_drawSurfaceStretched,
    .surfaceResize = N3DSRenderer_surfaceResize,
    .surfaceFree = N3DSRenderer_surfaceFree,
    .surfaceCopy = N3DSRenderer_surfaceCopy,
};

Renderer* N3DSRenderer_create(void) {
    N3DSRenderer* renderer = safeCalloc(1, sizeof(N3DSRenderer));
    renderer->base.vtable = &N3DSRenderer_vtable;
    renderer->base.drawColor = 0xFFFFFF;
    renderer->base.drawAlpha = 1.0f;
    renderer->base.drawFont = -1;
    renderer->base.drawHalign = 0;
    renderer->base.drawValign = 0;
    renderer->base.circlePrecision = 24;
    renderer->resolvedAssetPathCache = NULL;
    sh_new_strdup(renderer->resolvedAssetPathCache);
    renderer->packedDirectAssetMap = NULL;
    sh_new_strdup(renderer->packedDirectAssetMap);
    return (Renderer*) renderer;
}

bool N3DSRenderer_isReady(Renderer* base) {
    if (base == NULL) return false;
    return ((N3DSRenderer*) base)->atlasLoaded;
}

const char* N3DSRenderer_getStartupError(Renderer* base) {
    if (base == NULL) return "Renderer was not created";
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    if (renderer->startupError[0] == '\0') return NULL;
    return renderer->startupError;
}

uint32_t N3DSRenderer_getResidentAtlasVRAMBytes(Renderer* base) {
    if (base == NULL) return 0;
    return ((N3DSRenderer*) base)->residentAtlasVRAMBytes;
}

uint32_t N3DSRenderer_getResidentAtlasVRAMLimitBytes(Renderer* base) {
    if (base == NULL) return 0;
    N3DSRenderer* renderer = (N3DSRenderer*) base;
    return renderer->residentAtlasVRAMLimitBytes;
}

uint32_t N3DSRenderer_getResidentAtlasPageCount(Renderer* base) {
    if (base == NULL) return 0;
    return ((N3DSRenderer*) base)->residentAtlasPageCount;
}

uint32_t N3DSRenderer_getResidentAtlasPageLimit(Renderer* base) {
    if (base == NULL) return 0;
    return ((N3DSRenderer*) base)->residentAtlasPageLimit;
}

uint32_t N3DSRenderer_getResidentDirectAssetVRAMBytes(Renderer* base) {
    if (base == NULL) return 0;
    return ((N3DSRenderer*) base)->residentDirectAssetVRAMBytes;
}

uint32_t N3DSRenderer_getResidentDirectAssetVRAMLimitBytes(Renderer* base) {
    if (base == NULL) return 0;
    return ((N3DSRenderer*) base)->residentDirectAssetVRAMLimitBytes;
}

uint32_t N3DSRenderer_getFrameFragmentDraws(Renderer* base) {
    if (base == NULL) return 0;
    return ((N3DSRenderer*) base)->frameFragmentDraws;
}

uint32_t N3DSRenderer_getFrameSpriteDrawCalls(Renderer* base) {
    if (base == NULL) return 0;
    return ((N3DSRenderer*) base)->frameSpriteDrawCalls;
}

uint32_t N3DSRenderer_getFrameSpritePartDrawCalls(Renderer* base) {
    if (base == NULL) return 0;
    return ((N3DSRenderer*) base)->frameSpritePartDrawCalls;
}

uint32_t N3DSRenderer_getFrameDirectSpriteHits(Renderer* base) {
    if (base == NULL) return 0;
    return ((N3DSRenderer*) base)->frameDirectSpriteHits;
}

uint32_t N3DSRenderer_getFrameDirectAssetLoads(Renderer* base) {
    if (base == NULL) return 0;
    return ((N3DSRenderer*) base)->frameDirectAssetLoads;
}

uint32_t N3DSRenderer_getFrameTextureSwitches(Renderer* base) {
    if (base == NULL) return 0;
    return ((N3DSRenderer*) base)->frameTextureSwitches;
}

uint32_t N3DSRenderer_getFrameTextGlyphDraws(Renderer* base) {
    if (base == NULL) return 0;
    return ((N3DSRenderer*) base)->frameTextGlyphDraws;
}

uint32_t N3DSRenderer_getFrameTextTenthsMs(Renderer* base) {
    if (base == NULL) return 0;
    return ((N3DSRenderer*) base)->frameTextTenthsMs;
}

int32_t N3DSRenderer_findTileEntryIndex(Renderer* base, int32_t backgroundIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH) {
    if (base == NULL) return -1;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    uint32_t entryIndex = 0;
    if (N3DSRenderer_findTileEntryByKey(renderer, backgroundIndex, srcX, srcY, srcW, srcH, &entryIndex) == NULL) {
        return -1;
    }
    return (int32_t) entryIndex;
}

bool N3DSRenderer_drawCachedTileEntry(Renderer* base, int32_t tileEntryIndex, float drawX, float drawY, float xscale, float yscale, uint32_t color, float alpha) {
    if (base == NULL) return false;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    if (tileEntryIndex < 0 || (uint32_t) tileEntryIndex >= renderer->tileEntryCount) return false;
    return N3DSRenderer_drawPackedTileEntry(base, renderer, &renderer->tileEntries[tileEntryIndex], drawX, drawY, xscale, yscale, color, alpha);
}

int32_t N3DSRenderer_createTileLayerChunkCache(Renderer* base, int32_t roomWidth, int32_t roomHeight, const TileLayerRenderCache* cache) {
    if (base == NULL || cache == NULL || cache->rows == NULL || cache->cells == NULL) return -1;
    if (roomWidth <= 0 || roomHeight <= 0 || cache->tileWidth == 0 || cache->tileHeight == 0) return -1;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    uint32_t chunkCols = ((uint32_t) roomWidth + (N3DS_TILE_LAYER_CHUNK_SIZE - 1u)) / N3DS_TILE_LAYER_CHUNK_SIZE;
    uint32_t chunkRows = ((uint32_t) roomHeight + (N3DS_TILE_LAYER_CHUNK_SIZE - 1u)) / N3DS_TILE_LAYER_CHUNK_SIZE;
    uint32_t chunkCount = chunkCols * chunkRows;
    if (chunkCount == 0) return -1;

    uint64_t estimatedBytes = (uint64_t) chunkCount * (uint64_t) N3DS_TILE_LAYER_CHUNK_TEXTURE_BYTES;
    if (estimatedBytes > (uint64_t) N3DS_TILE_LAYER_CHUNK_VRAM_BUDGET ||
        (uint64_t) renderer->tileLayerChunkVRAMBytes + estimatedBytes > (uint64_t) N3DS_TILE_LAYER_CHUNK_VRAM_BUDGET) {
        return -1;
    }

    int32_t cacheIndex = -1;
    size_t existingCount = arrlenu(renderer->tileLayerChunkCaches);
    repeat(existingCount, i) {
        if (!renderer->tileLayerChunkCaches[i].used) {
            cacheIndex = (int32_t) i;
            break;
        }
    }
    if (cacheIndex < 0) {
        N3DSTileLayerChunkCache newCache = {0};
        arrput(renderer->tileLayerChunkCaches, newCache);
        cacheIndex = (int32_t) arrlen(renderer->tileLayerChunkCaches) - 1;
    }

    N3DSTileLayerChunkCache* chunkCache = &renderer->tileLayerChunkCaches[cacheIndex];
    N3DSRenderer_freeTileLayerChunkCache(renderer, chunkCache);
    chunkCache->used = true;
    chunkCache->chunkCount = chunkCount;
    chunkCache->vramBytes = 0;
    chunkCache->chunks = safeCalloc(chunkCount, sizeof(N3DSTileLayerChunk));

    float savedFrameScaleX = renderer->frameScaleX;
    float savedFrameScaleY = renderer->frameScaleY;
    float savedFrameOffsetX = renderer->frameOffsetX;
    float savedFrameOffsetY = renderer->frameOffsetY;
    float savedPortOffsetX = renderer->portOffsetX;
    float savedPortOffsetY = renderer->portOffsetY;
    int32_t savedViewX = renderer->viewX;
    int32_t savedViewY = renderer->viewY;
    float savedViewScaleX = renderer->viewScaleX;
    float savedViewScaleY = renderer->viewScaleY;
    uint8_t savedSceneTarget = renderer->activeSceneTarget;

    C2D_Flush();
    renderer->pendingC2DDraws = 0;
    renderer->lastDrawTexture = NULL;
    renderer->frameScaleX = 1.0f;
    renderer->frameScaleY = 1.0f;
    renderer->frameOffsetX = 0.0f;
    renderer->frameOffsetY = 0.0f;
    renderer->portOffsetX = 0.0f;
    renderer->portOffsetY = 0.0f;
    renderer->viewX = 0;
    renderer->viewY = 0;
    renderer->viewScaleX = 1.0f;
    renderer->viewScaleY = 1.0f;
    N3DSRenderer_applyBlendState(renderer);
    N3DSRenderer_applyAlphaState(renderer);

    repeat(chunkRows, chunkRow) {
        repeat(chunkCols, chunkCol) {
            uint32_t chunkIndexU = (uint32_t) chunkRow * chunkCols + (uint32_t) chunkCol;
            N3DSTileLayerChunk* chunk = &chunkCache->chunks[chunkIndexU];
            chunk->x = (int32_t) chunkCol * (int32_t) N3DS_TILE_LAYER_CHUNK_SIZE;
            chunk->y = (int32_t) chunkRow * (int32_t) N3DS_TILE_LAYER_CHUNK_SIZE;
            chunk->width = (uint16_t) (((uint32_t) roomWidth - (uint32_t) chunk->x) > N3DS_TILE_LAYER_CHUNK_SIZE ? N3DS_TILE_LAYER_CHUNK_SIZE : ((uint32_t) roomWidth - (uint32_t) chunk->x));
            chunk->height = (uint16_t) (((uint32_t) roomHeight - (uint32_t) chunk->y) > N3DS_TILE_LAYER_CHUNK_SIZE ? N3DS_TILE_LAYER_CHUNK_SIZE : ((uint32_t) roomHeight - (uint32_t) chunk->y));
            if (chunk->width == 0 || chunk->height == 0) continue;

            if (!C3D_TexInit(&chunk->texture, N3DS_TILE_LAYER_CHUNK_SIZE, N3DS_TILE_LAYER_CHUNK_SIZE, GPU_RGBA5551)) {
                continue;
            }
            chunk->allocated = true;
            C3D_TexSetFilter(&chunk->texture, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&chunk->texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            chunk->target = C3D_RenderTargetCreateFromTex(&chunk->texture, GPU_TEXFACE_2D, 0, -1);
            if (chunk->target == NULL) {
                C3D_TexDelete(&chunk->texture);
                memset(&chunk->texture, 0, sizeof(chunk->texture));
                chunk->allocated = false;
                continue;
            }

            chunk->subtex.width = chunk->width;
            chunk->subtex.height = chunk->height;
            chunk->subtex.left = 0.0f;
            chunk->subtex.top = 1.0f;
            chunk->subtex.right = (float) chunk->width / (float) N3DS_TILE_LAYER_CHUNK_SIZE;
            chunk->subtex.bottom = 1.0f - ((float) chunk->height / (float) N3DS_TILE_LAYER_CHUNK_SIZE);
            chunk->image.tex = &chunk->texture;
            chunk->image.subtex = &chunk->subtex;

            C2D_SceneBegin(chunk->target);
            renderer->activeSceneTarget = N3DS_SCENE_TARGET_NONE;
            C2D_TargetClear(chunk->target, C2D_Color32(0, 0, 0, 0));

            int32_t startX = chunk->x / (int32_t) cache->tileWidth;
            int32_t startY = chunk->y / (int32_t) cache->tileHeight;
            int32_t endX = (int32_t) (((int64_t) chunk->x + (int64_t) chunk->width + (int64_t) cache->tileWidth - 1) / (int64_t) cache->tileWidth);
            int32_t endY = (int32_t) (((int64_t) chunk->y + (int64_t) chunk->height + (int64_t) cache->tileHeight - 1) / (int64_t) cache->tileHeight);
            if (startX < 0) startX = 0;
            if (startY < 0) startY = 0;
            if (endX > (int32_t) cache->tilesX) endX = (int32_t) cache->tilesX;
            if (endY > (int32_t) cache->tilesY) endY = (int32_t) cache->tilesY;

            bool drewAny = false;
            for (int32_t ty = startY; ty < endY; ty++) {
                const TileLayerCacheRow* rowCache = &cache->rows[ty];
                if (rowCache->count == 0) continue;

                const TileLayerCacheCell* rowCells = &cache->cells[rowCache->start];
                uint32_t startCell = 0;
                while (startCell < rowCache->count && (int32_t) rowCells[startCell].tileX < startX) startCell++;
                for (uint32_t ci = startCell; ci < rowCache->count; ci++) {
                    const TileLayerCacheCell* cachedCell = &rowCells[ci];
                    if ((int32_t) cachedCell->tileX >= endX) break;

                    float xscale = cachedCell->mirror ? -1.0f : 1.0f;
                    float yscale = cachedCell->flip ? -1.0f : 1.0f;
                    float dstX = (float) ((int32_t) cachedCell->tileX * (int32_t) cache->tileWidth - chunk->x) + (cachedCell->mirror ? (float) cache->tileWidth : 0.0f);
                    float dstY = (float) (ty * (int32_t) cache->tileHeight - chunk->y) + (cachedCell->flip ? (float) cache->tileHeight : 0.0f);

                    bool drewCell = false;
                    if (cachedCell->n3dsTileEntryIndex >= 0 && (uint32_t) cachedCell->n3dsTileEntryIndex < renderer->tileEntryCount) {
                        drewCell = N3DSRenderer_drawPackedTileEntry(
                            base,
                            renderer,
                            &renderer->tileEntries[cachedCell->n3dsTileEntryIndex],
                            dstX,
                            dstY,
                            xscale,
                            yscale,
                            0x00FFFFFFu,
                            1.0f
                        );
                    }

                    if (!drewCell && cachedCell->tpagIndex >= 0) {
                        drewCell = N3DSRenderer_tryDrawTileRect(
                            base,
                            cachedCell->tpagIndex,
                            (int32_t) cachedCell->srcX,
                            (int32_t) cachedCell->srcY,
                            (int32_t) cache->tileWidth,
                            (int32_t) cache->tileHeight,
                            dstX,
                            dstY,
                            xscale,
                            yscale,
                            0x00FFFFFFu,
                            1.0f
                        );
                    }

                    if (drewCell) drewAny = true;
                }
            }

            C2D_Flush();
            renderer->pendingC2DDraws = 0;
            renderer->lastDrawTexture = NULL;
            if (!drewAny) {
                if (chunk->target != NULL) {
                    C3D_RenderTargetDelete(chunk->target);
                    chunk->target = NULL;
                }
                C3D_TexDelete(&chunk->texture);
                memset(&chunk->texture, 0, sizeof(chunk->texture));
                chunk->allocated = false;
            } else {
                chunkCache->vramBytes += N3DS_TILE_LAYER_CHUNK_TEXTURE_BYTES;
            }
        }
    }

    renderer->tileLayerChunkVRAMBytes += chunkCache->vramBytes;

    renderer->frameScaleX = savedFrameScaleX;
    renderer->frameScaleY = savedFrameScaleY;
    renderer->frameOffsetX = savedFrameOffsetX;
    renderer->frameOffsetY = savedFrameOffsetY;
    renderer->portOffsetX = savedPortOffsetX;
    renderer->portOffsetY = savedPortOffsetY;
    renderer->viewX = savedViewX;
    renderer->viewY = savedViewY;
    renderer->viewScaleX = savedViewScaleX;
    renderer->viewScaleY = savedViewScaleY;
    N3DSRenderer_applyBlendState(renderer);
    N3DSRenderer_applyAlphaState(renderer);
    if (savedSceneTarget == N3DS_SCENE_TARGET_BOTTOM) {
        N3DSRenderer_sceneBeginTarget(renderer, N3DS_SCENE_TARGET_BOTTOM, true);
    } else {
        N3DSRenderer_sceneBeginTarget(renderer, N3DS_SCENE_TARGET_TOP, true);
    }

    return cacheIndex;
}

bool N3DSRenderer_drawTileLayerChunkCache(Renderer* base, int32_t cacheId, float layerOffsetX, float layerOffsetY, float alpha) {
    if (base == NULL) return false;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    if (cacheId < 0 || (size_t) cacheId >= arrlenu(renderer->tileLayerChunkCaches)) return false;
    N3DSTileLayerChunkCache* cache = &renderer->tileLayerChunkCaches[cacheId];
    if (!cache->used || cache->chunks == NULL) return false;

    float viewRight = (renderer->viewScaleX != 0.0f) ? ((float) renderer->viewX + ((float) N3DS_TOP_WIDTH / fabsf(renderer->viewScaleX))) : (float) renderer->viewX;
    float viewBottom = (renderer->viewScaleY != 0.0f) ? ((float) renderer->viewY + ((float) N3DS_TOP_HEIGHT / fabsf(renderer->viewScaleY))) : (float) renderer->viewY;
    bool drewAny = false;

    repeat(cache->chunkCount, i) {
        N3DSTileLayerChunk* chunk = &cache->chunks[i];
        if (!chunk->allocated || chunk->target == NULL) continue;

        float localX = (float) chunk->x + layerOffsetX;
        float localY = (float) chunk->y + layerOffsetY;
        if (localX + (float) chunk->width <= (float) renderer->viewX ||
            localY + (float) chunk->height <= (float) renderer->viewY ||
            localX >= viewRight ||
            localY >= viewBottom) {
            continue;
        }

        renderer->frameSpriteDrawCalls++;
        N3DSRenderer_drawImageFast(base, &chunk->image, localX, localY, 1.0f, 1.0f, 0x00FFFFFFu, alpha);
        drewAny = true;
    }

    return drewAny;
}

void N3DSRenderer_destroyTileLayerChunkCache(Renderer* base, int32_t cacheId) {
    if (base == NULL) return;

    N3DSRenderer* renderer = (N3DSRenderer*) base;
    if (cacheId < 0 || (size_t) cacheId >= arrlenu(renderer->tileLayerChunkCaches)) return;
    N3DSRenderer_freeTileLayerChunkCache(renderer, &renderer->tileLayerChunkCaches[cacheId]);
}
