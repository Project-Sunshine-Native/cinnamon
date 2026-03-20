#pragma once

#include "renderer.h"
#include <gsKit.h>

// ===[ Atlas Entry (from ATLAS.BIN TPAG entries) ]===
typedef struct {
    uint16_t atlasId;   // TEX atlas index (0xFFFF = not mapped)
    uint16_t atlasX;    // X offset within the atlas
    uint16_t atlasY;    // Y offset within the atlas
    uint16_t width;     // Image width in the atlas (post-crop, post-resize)
    uint16_t height;    // Image height in the atlas (post-crop, post-resize)
    uint16_t cropX;     // X offset of cropped content within original bounding box
    uint16_t cropY;     // Y offset of cropped content within original bounding box
    uint16_t cropW;     // Pre-resize width of the cropped content
    uint16_t cropH;     // Pre-resize height of the cropped content
    uint16_t clutIndex; // CLUT index within the corresponding CLUT file
    uint8_t bpp;        // 4 or 8
} AtlasTPAGEntry;

// ===[ Atlas Tile Entry (from ATLAS.BIN tile entries) ]===
typedef struct {
    int16_t bgDef;      // Background definition index
    uint16_t srcX;      // Source X in the original background image
    uint16_t srcY;      // Source Y in the original background image
    uint16_t srcW;      // Original tile width in pixels
    uint16_t srcH;      // Original tile height in pixels
    uint16_t atlasId;   // TEX atlas index (0xFFFF = not mapped)
    uint16_t atlasX;    // X offset within the atlas
    uint16_t atlasY;    // Y offset within the atlas
    uint16_t width;     // Tile width in the atlas (post-crop, post-resize)
    uint16_t height;    // Tile height in the atlas (post-crop, post-resize)
    uint16_t cropX;     // X offset of cropped content within original tile
    uint16_t cropY;     // Y offset of cropped content within original tile
    uint16_t cropW;     // Pre-resize width of the cropped content
    uint16_t cropH;     // Pre-resize height of the cropped content
    uint16_t clutIndex; // CLUT index within the corresponding CLUT file
    uint8_t bpp;        // 4 or 8
} AtlasTileEntry;

// ===[ VRAM Chunk (buddy system unit) ]===
// Each chunk is 128KB of VRAM (fits one 4bpp 512x512 atlas).
// An 8bpp atlas uses 2 consecutive chunks.
#define VRAM_CHUNK_SIZE 131072 // 128KB = gsKit_texture_size(512, 512, GS_PSM_T4)

typedef struct {
    int16_t atlasId;    // Which atlas occupies this chunk (-1 = free)
    uint64_t lastUsed;  // Frame number when last accessed
} VRAMChunk;

// ===[ EE RAM Atlas Cache Entry ]===
// Caches compressed atlas data (header + pixel data) in EE RAM to avoid repeated CDVD reads.
typedef struct {
    int16_t atlasId;    // Which atlas (-1 = free)
    uint32_t offset;    // Byte offset within eeCache buffer
    uint32_t size;      // Total bytes stored (128-byte header + compressed pixel data)
    uint64_t lastUsed;  // Frame counter for LRU
} EeAtlasCacheEntry;

// ===[ GsRenderer Struct ]===
typedef struct {
    Renderer base; // Must be first field for struct embedding

    GSGLOBAL* gsGlobal;

    // View transform state
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
    int32_t viewX;
    int32_t viewY;

    // Z counter for depth ordering
    uint16_t zCounter;

    // ATLAS.BIN data
    uint16_t atlasTPAGCount;
    uint16_t atlasTileCount;
    AtlasTPAGEntry* atlasTPAGEntries;
    AtlasTileEntry* atlasTileEntries;

    // CLUT VRAM addresses (one per CLUT, individually uploaded)
    uint32_t clut4Count;       // Number of 4bpp CLUTs
    uint32_t* clut4VramAddrs;  // Per-CLUT VRAM addresses [clut4Count]

    uint32_t clut8Count;       // Number of 8bpp CLUTs
    uint32_t* clut8VramAddrs;  // Per-CLUT VRAM addresses [clut8Count]

    // TEXTURES.BIN file handle (kept open for on-demand atlas loading)
    FILE* texturesFile;
    uint32_t* atlasOffsets;    // Byte offset of each atlas within TEXTURES.BIN [atlasCount]

    // VRAM texture cache (buddy system with LRU eviction)
    uint32_t textureVramBase;  // Start of texture region in VRAM (after framebuffers + CLUTs)
    uint32_t chunkCount;       // Number of 128KB chunks available
    VRAMChunk* chunks;         // Per-chunk state [chunkCount]
    int16_t* atlasToChunk;     // atlasId -> first chunk index (-1 = not loaded) [atlasCount]
    uint16_t atlasCount;       // Number of atlas IDs from ATLAS.BIN header
    uint8_t* atlasBpp;         // Bits per pixel per atlas (4 or 8), from ATLAS.BIN [atlasCount]
    uint64_t frameCounter;     // Incremented each frame for LRU tracking

    // EE RAM atlas cache (stores compressed atlas data to avoid repeated CDVD reads)
    uint8_t* eeCache;                  // 4 MiB contiguous buffer
    uint32_t eeCacheCapacity;          // Total size (4 * 1024 * 1024)
    uint32_t eeCacheBumpPtr;           // End of live data
    EeAtlasCacheEntry* eeCacheEntries; // Per-atlas cache state [atlasCount]
    uint32_t* atlasDataSizes;          // On-disk size per atlas (header + compressed data) [atlasCount]
} GsRenderer;

Renderer* GsRenderer_create(GSGLOBAL* gsGlobal);
