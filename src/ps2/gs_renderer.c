#include "gs_renderer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <kernel.h>

#include "binary_reader.h"
#include "binary_utils.h"
#include "utils.h"
#include "text_utils.h"
#include "ps2_utils.h"
#include "matrix_math.h"

// ===[ Constants ]===
#define ATLAS_WIDTH 512
#define ATLAS_HEIGHT 512
#define PS2_SCREEN_WIDTH 640.0f
#define PS2_SCREEN_HEIGHT 448.0f
#define TEX_HEADER_SIZE 128
#define CLUT4_ENTRY_SIZE 64    // 16 colors * 4 bytes
#define CLUT8_ENTRY_SIZE 1024  // 256 colors * 4 bytes

// ===[ File Loading Helper ]===

// Loads an entire file from host into a memalign'd buffer. Returns size via outSize.
// Aborts on failure.
static uint8_t* loadFileRaw(const char* path, uint32_t* outSize) {
    char* textureBinPath = PS2Utils_createDevicePath(path);

    FILE* f = fopen(textureBinPath, "rb");
    if (f == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to open %s\n", path);
        abort();
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // 128-byte aligned for DMA transfers
    uint8_t* data = (uint8_t*) memalign(128, (size_t) size);
    if (data == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to allocate %ld bytes for %s\n", size, path);
        fclose(f);
        abort();
    }

    size_t read = fread(data, 1, (size_t) size, f);
    fclose(f);

    if (read != (size_t) size) {
        fprintf(stderr, "GsRenderer: Short read on %s (expected %ld, got %zu)\n", path, size, read);
        abort();
    }

    *outSize = (uint32_t) size;
    free(textureBinPath);
    return data;
}

// ===[ Atlas Loading ]===
static void loadAtlas(GsRenderer* gs) {
    char* atlasBinPath = PS2Utils_createDevicePath("ATLAS.BIN");
    FILE* f = fopen(atlasBinPath, "rb");
    if (f == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to open %s\n", atlasBinPath);
        abort();
    }

    fseek(f, 0, SEEK_END);
    size_t fileSize = (size_t) ftell(f);
    fseek(f, 0, SEEK_SET);

    BinaryReader reader = BinaryReader_create(f, fileSize);

    uint8_t version = BinaryReader_readUint8(&reader);
    if (version != 0) {
        fprintf(stderr, "GsRenderer: Unsupported ATLAS.BIN version %u\n", version);
        abort();
    }

    gs->atlasTPAGCount = BinaryReader_readUint16(&reader);
    gs->atlasTileCount = BinaryReader_readUint16(&reader);
    gs->atlasCount = BinaryReader_readUint16(&reader);

    // Parse atlas offset table
    gs->atlasOffsets = safeMalloc(gs->atlasCount * sizeof(uint32_t));
    repeat(gs->atlasCount, i) {
        gs->atlasOffsets[i] = BinaryReader_readUint32(&reader);
    }

    // Parse TPAG entries
    gs->atlasTPAGEntries = safeMalloc(gs->atlasTPAGCount * sizeof(AtlasTPAGEntry));

    repeat(gs->atlasTPAGCount, i) {
        AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[i];
        entry->atlasId = BinaryReader_readUint16(&reader);
        entry->atlasX = BinaryReader_readUint16(&reader);
        entry->atlasY = BinaryReader_readUint16(&reader);
        entry->width = BinaryReader_readUint16(&reader);
        entry->height = BinaryReader_readUint16(&reader);
        entry->cropX = BinaryReader_readUint16(&reader);
        entry->cropY = BinaryReader_readUint16(&reader);
        entry->cropW = BinaryReader_readUint16(&reader);
        entry->cropH = BinaryReader_readUint16(&reader);
        entry->clutIndex = BinaryReader_readUint16(&reader);
        entry->bpp = BinaryReader_readUint8(&reader);
    }

    // Parse tile entries
    gs->atlasTileEntries = safeMalloc(gs->atlasTileCount * sizeof(AtlasTileEntry));

    repeat(gs->atlasTileCount, i) {
        AtlasTileEntry* entry = &gs->atlasTileEntries[i];
        entry->bgDef = BinaryReader_readInt16(&reader);
        entry->srcX = BinaryReader_readUint16(&reader);
        entry->srcY = BinaryReader_readUint16(&reader);
        entry->srcW = BinaryReader_readUint16(&reader);
        entry->srcH = BinaryReader_readUint16(&reader);
        entry->atlasId = BinaryReader_readUint16(&reader);
        entry->atlasX = BinaryReader_readUint16(&reader);
        entry->atlasY = BinaryReader_readUint16(&reader);
        entry->width = BinaryReader_readUint16(&reader);
        entry->height = BinaryReader_readUint16(&reader);
        entry->cropX = BinaryReader_readUint16(&reader);
        entry->cropY = BinaryReader_readUint16(&reader);
        entry->cropW = BinaryReader_readUint16(&reader);
        entry->cropH = BinaryReader_readUint16(&reader);
        entry->clutIndex = BinaryReader_readUint16(&reader);
        entry->bpp = BinaryReader_readUint8(&reader);
    }

    fclose(f);

    gs->atlasBpp = safeCalloc(gs->atlasCount, sizeof(uint8_t));
    gs->atlasToChunk = safeMalloc(gs->atlasCount * sizeof(int16_t));
    repeat(gs->atlasCount, i) {
        gs->atlasToChunk[i] = -1;
    }

    // Build bpp table from TPAG and tile entries
    repeat(gs->atlasTPAGCount, i) {
        AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[i];
        if (entry->atlasId != 0xFFFF && gs->atlasCount > entry->atlasId) {
            gs->atlasBpp[entry->atlasId] = entry->bpp;
        }
    }
    repeat(gs->atlasTileCount, i) {
        AtlasTileEntry* entry = &gs->atlasTileEntries[i];
        if (entry->atlasId != 0xFFFF && gs->atlasCount > entry->atlasId) {
            gs->atlasBpp[entry->atlasId] = entry->bpp;
        }
    }

    fprintf(stderr, "GsRenderer: ATLAS.BIN loaded - %u TPAG entries, %u tile entries, %u atlases\n", gs->atlasTPAGCount, gs->atlasTileCount, gs->atlasCount);

    free(atlasBinPath);
}

// ===[ CLUT Loading and VRAM Upload ]===
// Each CLUT is uploaded individually to its own VRAM address. This is necessary because
// the PS2 GS VRAM has a block-swizzled layout - bulk-uploading stacked CLUTs and computing
// linear offsets for CBP does NOT work (the BITBLT write path and CLUT read path use
// block-based addressing, so CLUTs don't land at simple linear offsets within a bulk upload).
static void loadAndUploadCLUTs(GsRenderer* gs) {
    GSGLOBAL* gsGlobal = gs->gsGlobal;

    // 128-byte aligned temp buffer for DMA transfers (reused for each CLUT send)
    // Large enough for one 8bpp CLUT (1024 bytes)
    uint8_t* tempBuf = (uint8_t*) memalign(128, CLUT8_ENTRY_SIZE);
    if (tempBuf == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to allocate CLUT temp buffer\n");
        abort();
    }

    // Load and upload CLUT4 (4bpp palettes: 16 colors * 4 bytes = 64 bytes each)
    {
        uint32_t clut4FileSize;
        uint8_t* clut4Data = loadFileRaw("CLUT4.BIN", &clut4FileSize);
        gs->clut4Count = clut4FileSize / CLUT4_ENTRY_SIZE;
        fprintf(stderr, "GsRenderer: CLUT4.BIN loaded - %u CLUTs (%u bytes)\n", gs->clut4Count, clut4FileSize);

        gs->clut4VramAddrs = safeMalloc(gs->clut4Count * sizeof(uint32_t));

        repeat(gs->clut4Count, i) {
            // gsKit uploads 4bpp CLUTs as 8x2 CT32 (16 entries in 8-wide, 2-tall grid)
            uint32_t vramSize = gsKit_texture_size(8, 2, GS_PSM_CT32);
            uint32_t vramAddr = gsKit_vram_alloc(gsGlobal, vramSize, GSKIT_ALLOC_USERBUFFER);
            if (vramAddr == GSKIT_ALLOC_ERROR) {
                fprintf(stderr, "GsRenderer: Failed to allocate VRAM for CLUT4 index %u\n", i);
                abort();
            }

            // Copy to aligned temp buffer for DMA
            memcpy(tempBuf, clut4Data + i * CLUT4_ENTRY_SIZE, CLUT4_ENTRY_SIZE);
            gsKit_texture_send((u32*) tempBuf, 8, 2, vramAddr, GS_PSM_CT32, 1, GS_CLUT_PALLETE);
            gs->clut4VramAddrs[i] = vramAddr;
        }

        fprintf(stderr, "GsRenderer: CLUT4 uploaded (%u CLUTs)\n", gs->clut4Count);
        free(clut4Data);
    }

    // Load and upload CLUT8 (8bpp palettes: 256 colors * 4 bytes = 1024 bytes each)
    {
        uint32_t clut8FileSize;
        uint8_t* clut8Data = loadFileRaw("CLUT8.BIN", &clut8FileSize);
        gs->clut8Count = clut8FileSize / CLUT8_ENTRY_SIZE;
        fprintf(stderr, "GsRenderer: CLUT8.BIN loaded - %u CLUTs (%u bytes)\n", gs->clut8Count, clut8FileSize);

        gs->clut8VramAddrs = safeMalloc(gs->clut8Count * sizeof(uint32_t));

        repeat(gs->clut8Count, i) {
            // gsKit uploads 8bpp CLUTs as 16x16 CT32 (256 entries in 16-wide, 16-tall grid)
            uint32_t vramSize = gsKit_texture_size(16, 16, GS_PSM_CT32);
            uint32_t vramAddr = gsKit_vram_alloc(gsGlobal, vramSize, GSKIT_ALLOC_USERBUFFER);
            if (vramAddr == GSKIT_ALLOC_ERROR) {
                fprintf(stderr, "GsRenderer: Failed to allocate VRAM for CLUT8 index %u\n", i);
                abort();
            }

            // 8bpp CLUTs are 1024 bytes; source is 128-byte aligned (1024 is a multiple of 128)
            gsKit_texture_send((u32*) (clut8Data + i * CLUT8_ENTRY_SIZE), 16, 16, vramAddr, GS_PSM_CT32, 1, GS_CLUT_PALLETE);
            gs->clut8VramAddrs[i] = vramAddr;
        }

        fprintf(stderr, "GsRenderer: CLUT8 uploaded (%u CLUTs)\n", gs->clut8Count);
        free(clut8Data);
    }

    free(tempBuf);

    fprintf(stderr, "GsRenderer: VRAM after CLUTs: 0x%08X / 0x%08X\n", gsGlobal->CurrentPointer, GS_VRAM_SIZE);
}

// ===[ VRAM Texture Cache (Buddy System with LRU Eviction) ]===
// Manages a pool of 128KB VRAM chunks for atlas textures.
// 4bpp atlases use 1 chunk, 8bpp atlases use 2 consecutive chunks.

#define FONTM_RESERVED_VRAM 65536 // 64KB reserved for gsKit's TexManager (FONTM debug overlay)

// Initializes the chunk pool from the remaining VRAM after CLUTs.
// Reserves 64KB at the end for gsKit's TexManager (used by FONTM).
// Our chunk pool occupies the middle, between CLUTs and the FONTM region.
//
// VRAM layout: [Framebuffers] [CLUTs] [Chunk Pool ...] [64KB FONTM]
static void initTextureCache(GsRenderer* gs) {
    gs->textureVramBase = gs->gsGlobal->CurrentPointer;
    uint32_t availableVram = GS_VRAM_SIZE - gs->textureVramBase - FONTM_RESERVED_VRAM;
    gs->chunkCount = availableVram / VRAM_CHUNK_SIZE;

    gs->chunks = safeMalloc(gs->chunkCount * sizeof(VRAMChunk));
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        chunk->atlasId = -1;
        chunk->lastUsed = 0;
    }

    gs->frameCounter = 1;

    // Advance CurrentPointer past our chunk pool so the TexManager only
    // manages the 64KB FONTM region at the end of VRAM.
    gs->gsGlobal->CurrentPointer = gs->textureVramBase + gs->chunkCount * VRAM_CHUNK_SIZE;
    gsKit_TexManager_init(gs->gsGlobal);

    uint32_t fontmVram = GS_VRAM_SIZE - gs->gsGlobal->CurrentPointer;
    fprintf(stderr, "GsRenderer: Texture cache initialized - %u chunks (%u KB each), base 0x%08X, %u KB for textures, %u KB for FONTM\n", gs->chunkCount, VRAM_CHUNK_SIZE / 1024, gs->textureVramBase, gs->chunkCount * (VRAM_CHUNK_SIZE / 1024), fontmVram / 1024);
}

// Find the first run of consecutive free chunks.
// Returns the index of the first chunk, or -1 if not found.
static int32_t findConsecutiveFreeChunks(GsRenderer* gs, int chunksNeeded) {
    int consecutive = 0;
    forEachIndexed(VRAMChunk, chunk, i, gs->chunks, gs->chunkCount) {
        if (0 > chunk->atlasId) {
            consecutive++;
            if (consecutive >= chunksNeeded) {
                return (int32_t) (i - (uint32_t) chunksNeeded + 1);
            }
        } else {
            consecutive = 0;
        }
    }
    return -1;
}

// Count total free chunks.
static uint32_t countFreeChunks(GsRenderer* gs) {
    uint32_t count = 0;
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        if (0 > chunk->atlasId)
            count++;
    }
    return count;
}

// Find the atlas with the oldest lastUsed time (LRU victim).
// Returns the atlasId, or -1 if no loaded atlases.
static int16_t findLRUVictim(GsRenderer* gs) {
    uint64_t oldest = UINT64_MAX;
    int16_t victimAtlas = -1;
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        if (chunk->atlasId >= 0 && oldest > chunk->lastUsed) {
            oldest = chunk->lastUsed;
            victimAtlas = chunk->atlasId;
        }
    }
    return victimAtlas;
}

// Evict an atlas from the cache, freeing its chunk(s).
static void evictAtlas(GsRenderer* gs, int16_t atlasId) {
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        if (chunk->atlasId == atlasId) {
            chunk->atlasId = -1;
            chunk->lastUsed = 0;
        }
    }

    if (atlasId >= 0 && gs->atlasCount > (uint16_t) atlasId) {
        gs->atlasToChunk[atlasId] = -1;
    }

    uint32_t availableChunks = countFreeChunks(gs);
    fprintf(stderr, "GsRenderer: Evicted atlas %d from VRAM (available chunks = %d)\n", atlasId, availableChunks);
}

// Defragment the texture cache by evicting all loaded atlases.
// They will be reloaded on-demand as needed during subsequent draw calls.
static void defragTextureCache(GsRenderer* gs) {
    fprintf(stderr, "GsRenderer: Defragmenting VRAM texture cache...\n");

    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        chunk->atlasId = -1;
        chunk->lastUsed = 0;
    }

    repeat(gs->atlasCount, i) {
        gs->atlasToChunk[i] = -1;
    }

    fprintf(stderr, "GsRenderer: Defrag complete - all %u chunks freed\n", gs->chunkCount);
}

// Allocate consecutive chunks for an atlas. Evicts LRU victims or defrags if needed.
// Returns the first chunk index, or -1 if VRAM is truly exhausted.
static int32_t allocateChunks(GsRenderer* gs, int chunksNeeded) {
    // Attempt 1: find free consecutive chunks
    int32_t idx = findConsecutiveFreeChunks(gs, chunksNeeded);
    if (idx >= 0) return idx;

    // Attempt 2: evict LRU victims one at a time until space is found
    repeat(gs->chunkCount, attempts) {
        int16_t victim = findLRUVictim(gs);
        if (0 > victim)
            break;

        evictAtlas(gs, victim);

        idx = findConsecutiveFreeChunks(gs, chunksNeeded);

        if (idx >= 0)
            return idx;
    }

    // Attempt 3: defrag - evict ALL and let them reload on demand
    // Handles fragmentation where enough free chunks exist but aren't consecutive
    if (countFreeChunks(gs) >= (uint32_t) chunksNeeded) {
        defragTextureCache(gs);
        idx = findConsecutiveFreeChunks(gs, chunksNeeded);

        if (idx >= 0)
            return idx;
    }

    // VRAM truly exhausted
    return -1;
}

// ===[ EE RAM Atlas Cache (Bump Allocator with LRU Eviction + Compaction) ]===
// Caches compressed atlas data in a 4 MiB EE RAM buffer to avoid repeated
// CDVD reads when atlases are evicted from VRAM and need to be re-uploaded.

#define EE_CACHE_CAPACITY (4 * 1024 * 1024) // 4 MiB

// Initialize the EE RAM cache. Called from gsInit after opening TEXTURES.BIN.
static void initEeCache(GsRenderer* gs) {
    gs->eeCacheCapacity = EE_CACHE_CAPACITY;
    gs->eeCacheBumpPtr = 0;
    gs->eeCache = (uint8_t*) memalign(128, EE_CACHE_CAPACITY);
    if (gs->eeCache == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to allocate %u bytes for EE cache\n", EE_CACHE_CAPACITY);
        abort();
    }

    gs->eeCacheEntries = safeMalloc(gs->atlasCount * sizeof(EeAtlasCacheEntry));
    repeat(gs->atlasCount, i) {
        gs->eeCacheEntries[i].atlasId = -1;
        gs->eeCacheEntries[i].offset = 0;
        gs->eeCacheEntries[i].size = 0;
        gs->eeCacheEntries[i].lastUsed = 0;
    }

    // Compute on-disk sizes from offset table
    gs->atlasDataSizes = safeMalloc(gs->atlasCount * sizeof(uint32_t));

    // Get total file size for the last atlas
    fseek(gs->texturesFile, 0, SEEK_END);
    uint32_t texturesFileSize = (uint32_t) ftell(gs->texturesFile);

    repeat(gs->atlasCount, i) {
        if (gs->atlasCount - 1 > i) {
            gs->atlasDataSizes[i] = gs->atlasOffsets[i + 1] - gs->atlasOffsets[i];
        } else {
            gs->atlasDataSizes[i] = texturesFileSize - gs->atlasOffsets[i];
        }
    }
}

// Preload atlases sequentially into the EE cache until the buffer is full.
static void preloadEeCache(GsRenderer* gs) {
    uint32_t preloaded = 0;

    repeat(gs->atlasCount, i) {
        uint32_t dataSize = gs->atlasDataSizes[i];
        if (gs->eeCacheBumpPtr + dataSize > gs->eeCacheCapacity) {
            break;
        }

        fseek(gs->texturesFile, (long) gs->atlasOffsets[i], SEEK_SET);
        size_t bytesRead = fread(gs->eeCache + gs->eeCacheBumpPtr, 1, dataSize, gs->texturesFile);
        if (bytesRead != dataSize) {
            fprintf(stderr, "GsRenderer: EE cache preload short read for atlas %u (expected %u, got %zu)\n", i, dataSize, bytesRead);
            break;
        }

        gs->eeCacheEntries[i].atlasId = (int16_t) i;
        gs->eeCacheEntries[i].offset = gs->eeCacheBumpPtr;
        gs->eeCacheEntries[i].size = dataSize;
        gs->eeCacheEntries[i].lastUsed = gs->frameCounter;

        gs->eeCacheBumpPtr += dataSize;
        preloaded++;
    }

    fprintf(stderr, "GsRenderer: EE cache initialized - %u MB, %u atlases preloaded (%u KB used)\n", EE_CACHE_CAPACITY / (1024 * 1024), preloaded, gs->eeCacheBumpPtr / 1024);
}

// Look up an atlas in the EE cache. Returns pointer to cached data or nullptr.
static uint8_t* eeCacheLookup(GsRenderer* gs, uint16_t atlasId) {
    if (atlasId >= gs->atlasCount) return nullptr;
    if (0 > gs->eeCacheEntries[atlasId].atlasId) return nullptr;

    gs->eeCacheEntries[atlasId].lastUsed = gs->frameCounter;
    return gs->eeCache + gs->eeCacheEntries[atlasId].offset;
}

// Compact the EE cache by closing gaps from evicted entries.
static void compactEeCache(GsRenderer* gs) {
    // Collect live entries sorted by offset using insertion sort
    // (max 146 atlases, so a stack array + insertion sort is fine)
    uint16_t liveIds[256]; // More than enough for 146 atlases
    uint32_t liveCount = 0;

    repeat(gs->atlasCount, i) {
        if (gs->eeCacheEntries[i].atlasId >= 0) {
            // Insertion sort by offset
            uint32_t insertPos = liveCount;
            while (insertPos > 0 && gs->eeCacheEntries[liveIds[insertPos - 1]].offset > gs->eeCacheEntries[i].offset) {
                liveIds[insertPos] = liveIds[insertPos - 1];
                insertPos--;
            }
            liveIds[insertPos] = (uint16_t) i;
            liveCount++;
        }
    }

    // Walk and memmove each entry down to close gaps
    uint32_t writePtr = 0;
    repeat(liveCount, i) {
        EeAtlasCacheEntry* entry = &gs->eeCacheEntries[liveIds[i]];
        if (entry->offset != writePtr) {
            memmove(gs->eeCache + writePtr, gs->eeCache + entry->offset, entry->size);
            entry->offset = writePtr;
        }
        writePtr += entry->size;
    }

    gs->eeCacheBumpPtr = writePtr;
}

// Evict LRU entries until spaceNeeded bytes are available. Returns true on success.
static bool eeCacheEvictLRU(GsRenderer* gs, uint32_t spaceNeeded) {
    // Calculate total live bytes to determine how much space we can free
    uint32_t liveBytes = 0;
    repeat(gs->atlasCount, i) {
        if (gs->eeCacheEntries[i].atlasId >= 0) {
            liveBytes += gs->eeCacheEntries[i].size;
        }
    }

    // Evict LRU entries until enough space would be freed after compaction
    while (gs->eeCacheCapacity - liveBytes < spaceNeeded) {
        // Find entry with smallest lastUsed
        uint64_t oldest = UINT64_MAX;
        int16_t victimId = -1;

        repeat(gs->atlasCount, i) {
            if (gs->eeCacheEntries[i].atlasId >= 0 && oldest > gs->eeCacheEntries[i].lastUsed) {
                oldest = gs->eeCacheEntries[i].lastUsed;
                victimId = (int16_t) i;
            }
        }

        if (0 > victimId) {
            break;
        }

        liveBytes -= gs->eeCacheEntries[victimId].size;
        gs->eeCacheEntries[victimId].atlasId = -1;
    }

    compactEeCache(gs);

    return gs->eeCacheCapacity - gs->eeCacheBumpPtr >= spaceNeeded;
}

// Insert atlas data into the EE cache. Evicts LRU entries if needed.
static void eeCacheInsert(GsRenderer* gs, uint16_t atlasId, const uint8_t* data, uint32_t size) {
    if (size > gs->eeCacheCapacity) {
        // Atlas too large to ever fit in the cache
        return;
    }

    if (gs->eeCacheBumpPtr + size > gs->eeCacheCapacity) {
        if (!eeCacheEvictLRU(gs, size)) {
            fprintf(stderr, "GsRenderer: EE cache eviction failed for atlas %u (%u bytes)\n", atlasId, size);
            return;
        }
    }

    memcpy(gs->eeCache + gs->eeCacheBumpPtr, data, size);

    gs->eeCacheEntries[atlasId].atlasId = (int16_t) atlasId;
    gs->eeCacheEntries[atlasId].offset = gs->eeCacheBumpPtr;
    gs->eeCacheEntries[atlasId].size = size;
    gs->eeCacheEntries[atlasId].lastUsed = gs->frameCounter;

    gs->eeCacheBumpPtr += size;
}

// Upload atlas pixel data from TEXTURES.BIN to the given VRAM chunk(s).
static void uploadAtlasToChunk(GsRenderer* gs, uint16_t atlasId, int32_t firstChunk) {
    // Try EE RAM cache first (avoids slow CDVD reads)
    uint8_t* cached = eeCacheLookup(gs, atlasId);
    const char* atlasSource = "RAM";

    if (cached == nullptr) {
        // Cache miss: read from TEXTURES.BIN and insert into EE cache
        uint32_t dataSize = gs->atlasDataSizes[atlasId];
        uint8_t* tempBuf = (uint8_t*) memalign(128, dataSize);
        if (tempBuf == nullptr) {
            fprintf(stderr, "GsRenderer: Failed to allocate %u bytes for atlas %u temp buffer\n", dataSize, atlasId);
            abort();
        }

        fseek(gs->texturesFile, (long) gs->atlasOffsets[atlasId], SEEK_SET);
        size_t bytesRead = fread(tempBuf, 1, dataSize, gs->texturesFile);
        if (bytesRead != dataSize) {
            fprintf(stderr, "GsRenderer: Short read for atlas %u (expected %u, got %zu)\n", atlasId, dataSize, bytesRead);
            abort();
        }

        eeCacheInsert(gs, atlasId, tempBuf, dataSize);
        free(tempBuf);

        atlasSource = "disk";
        cached = eeCacheLookup(gs, atlasId);
        if (cached == nullptr) {
            // EE cache insert failed (atlas too large?), fall back to direct file read
            fprintf(stderr, "GsRenderer: EE cache insert failed for atlas %u, reading directly from CDVD\n", atlasId);
            fseek(gs->texturesFile, (long) gs->atlasOffsets[atlasId], SEEK_SET);

            uint8_t header[TEX_HEADER_SIZE];
            fread(header, 1, TEX_HEADER_SIZE, gs->texturesFile);

            uint16_t width = BinaryUtils_readUint16(header + 1);
            uint16_t height = BinaryUtils_readUint16(header + 3);
            uint8_t bpp = BinaryUtils_readUint8(header + 5);
            uint32_t pixelDataSize = BinaryUtils_readUint32(header + 6);
            uint8_t compressionType = BinaryUtils_readUint8(header + 10);

            uint8_t* rawData = (uint8_t*) memalign(128, pixelDataSize);
            fread(rawData, 1, pixelDataSize, gs->texturesFile);

            // Decompress + upload (duplicated for fallback path)
            uint8_t* pixelData;
            if (compressionType == 1) {
                uint32_t uncompressedSize = (bpp == 4) ? (uint32_t)((width * height + 1) / 2) : (uint32_t)(width * height);
                pixelData = (uint8_t*) memalign(128, uncompressedSize);
                uint32_t srcPos = 0, dstPos = 0;
                while (pixelDataSize > srcPos + 1 && uncompressedSize > dstPos) {
                    uint8_t runLength = rawData[srcPos++];
                    uint8_t value = rawData[srcPos++];
                    for (uint8_t j = 0; runLength > j && uncompressedSize > dstPos; j++) {
                        pixelData[dstPos++] = value;
                    }
                }
                free(rawData);
            } else {
                pixelData = rawData;
            }

            uint8_t psm = (bpp == 4) ? GS_PSM_T4 : GS_PSM_T8;
            uint32_t tbw = ATLAS_WIDTH / 64;
            uint32_t vramAddr = gs->textureVramBase + (uint32_t) firstChunk * VRAM_CHUNK_SIZE;
            gsKit_texture_send((u32*) pixelData, ATLAS_WIDTH, ATLAS_HEIGHT, vramAddr, psm, tbw, GS_CLUT_TEXTURE);

            int chunksUsed = (bpp == 8) ? 2 : 1;
            repeat(chunksUsed, i) {
                gs->chunks[firstChunk + i].atlasId = (int16_t) atlasId;
                gs->chunks[firstChunk + i].lastUsed = gs->frameCounter;
            }
            gs->atlasToChunk[atlasId] = (int16_t) firstChunk;
            free(pixelData);
            return;
        }
    }

    // Parse header from cached data
    uint8_t* header = cached;

    uint8_t version = header[0];
    if (version != 0) {
        fprintf(stderr, "GsRenderer: Unsupported TEX version %u for atlas %u\n", version, atlasId);
        abort();
    }

    uint16_t width = BinaryUtils_readUint16(header + 1);
    uint16_t height = BinaryUtils_readUint16(header + 3);
    uint8_t bpp = BinaryUtils_readUint8(header + 5);
    uint32_t pixelDataSize = BinaryUtils_readUint32(header + 6);
    uint8_t compressionType = BinaryUtils_readUint8(header + 10);

    if (width != ATLAS_WIDTH || height != ATLAS_HEIGHT) {
        fprintf(stderr, "GsRenderer: Atlas %u unexpected dimensions %ux%u (expected %ux%u)\n", atlasId, width, height, ATLAS_WIDTH, ATLAS_HEIGHT);
        abort();
    }

    if (bpp != 4 && bpp != 8) {
        fprintf(stderr, "GsRenderer: Atlas %u unsupported bpp %u\n", atlasId, bpp);
        abort();
    }

    // Copy compressed pixel data into DMA-aligned buffer
    uint8_t* rawData = (uint8_t*) memalign(128, pixelDataSize);
    if (rawData == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to allocate %u bytes for atlas %u pixel data\n", pixelDataSize, atlasId);
        abort();
    }
    memcpy(rawData, cached + TEX_HEADER_SIZE, pixelDataSize);

    // Decompress if needed
    uint8_t* pixelData;
    if (compressionType == 1) {
        // RLE decompression
        uint32_t uncompressedSize = (bpp == 4) ? (uint32_t)((width * height + 1) / 2) : (uint32_t)(width * height);
        pixelData = (uint8_t*) memalign(128, uncompressedSize);
        if (pixelData == nullptr) {
            fprintf(stderr, "GsRenderer: Failed to allocate %u bytes for atlas %u decompressed data\n", uncompressedSize, atlasId);
            abort();
        }

        uint32_t srcPos = 0;
        uint32_t dstPos = 0;
        while (pixelDataSize > srcPos + 1 && uncompressedSize > dstPos) {
            uint8_t runLength = rawData[srcPos++];
            uint8_t value = rawData[srcPos++];
            for (uint8_t j = 0; runLength > j && uncompressedSize > dstPos; j++) {
                pixelData[dstPos++] = value;
            }
        }

        free(rawData);
    } else {
        // Uncompressed, use data directly
        pixelData = rawData;
    }

    // Upload pixel data to VRAM at the chunk's address
    uint8_t psm = (bpp == 4) ? GS_PSM_T4 : GS_PSM_T8;
    uint32_t tbw = ATLAS_WIDTH / 64;
    uint32_t vramAddr = gs->textureVramBase + (uint32_t) firstChunk * VRAM_CHUNK_SIZE;

    gsKit_texture_send((u32*) pixelData, ATLAS_WIDTH, ATLAS_HEIGHT, vramAddr, psm, tbw, GS_CLUT_TEXTURE);

    // Update chunk state
    int chunksUsed = (bpp == 8) ? 2 : 1;
    repeat(chunksUsed, i) {
        gs->chunks[firstChunk + i].atlasId = (int16_t) atlasId;
        gs->chunks[firstChunk + i].lastUsed = gs->frameCounter;
    }
    gs->atlasToChunk[atlasId] = (int16_t) firstChunk;

    fprintf(stderr, "GsRenderer: Atlas %u uploaded to chunk %d (VRAM 0x%08X, %ubpp, src: %s)\n", atlasId, firstChunk, vramAddr, bpp, atlasSource);

    free(pixelData);
}

// Ensure an atlas is loaded into VRAM, using LRU eviction if needed.
// Returns true on success, false on failure.
static bool ensureAtlasLoaded(GsRenderer* gs, uint16_t atlasId) {
    if (atlasId >= gs->atlasCount) {
        fprintf(stderr, "GsRenderer: Atlas ID %u out of range (max %u)\n", atlasId, gs->atlasCount - 1);
        return false;
    }

    // Already loaded? Just touch LRU timestamp
    if (gs->atlasToChunk[atlasId] >= 0) {
        int16_t firstChunk = gs->atlasToChunk[atlasId];
        uint8_t bpp = gs->atlasBpp[atlasId];
        int chunksUsed = (bpp == 8) ? 2 : 1;
        repeat(chunksUsed, i) {
            gs->chunks[firstChunk + i].lastUsed = gs->frameCounter;
        }
        return true;
    }

    // Determine how many chunks we need
    uint8_t bpp = gs->atlasBpp[atlasId];
    if (bpp != 4 && bpp != 8) {
        fprintf(stderr, "GsRenderer: Atlas %u has unknown bpp %u\n", atlasId, bpp);
        return false;
    }
    int chunksNeeded = (bpp == 8) ? 2 : 1;

    // Allocate chunks (may evict or defrag)
    int32_t chunkIdx = allocateChunks(gs, chunksNeeded);
    if (0 > chunkIdx) {
        fprintf(stderr, "GsRenderer: VRAM exhausted! Cannot allocate %d chunk(s) for atlas %u (%ubpp)\n", chunksNeeded, atlasId, bpp);
        abort();
    }

    // Load TEX file and upload to the allocated chunk(s)
    uploadAtlasToChunk(gs, atlasId, chunkIdx);
    return true;
}

// ===[ GSTEXTURE setup for a given TPAG entry ]===
// Configures a GSTEXTURE struct for rendering a specific atlas region.
// The GSTEXTURE points to the atlas's VRAM location and the appropriate CLUT.
static bool setupTextureForTPAG(GsRenderer* gs, GSTEXTURE* tex, int32_t tpagIndex) {
    if (0 > tpagIndex || (uint32_t) tpagIndex >= gs->atlasTPAGCount) return false;

    AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[tpagIndex];
    if (entry->atlasId == 0xFFFF) return false;

    // Ensure the atlas texture is loaded into VRAM (may trigger LRU eviction)
    if (!ensureAtlasLoaded(gs, entry->atlasId))
        return false;

    // Compute VRAM address from chunk index
    int16_t chunkIdx = gs->atlasToChunk[entry->atlasId];
    uint32_t vramAddr = gs->textureVramBase + (uint32_t) chunkIdx * VRAM_CHUNK_SIZE;

    memset(tex, 0, sizeof(GSTEXTURE));
    tex->Width = ATLAS_WIDTH;
    tex->Height = ATLAS_HEIGHT;
    tex->TBW = ATLAS_WIDTH / 64;
    tex->Vram = vramAddr;
    tex->Filter = GS_FILTER_NEAREST;
    tex->ClutStorageMode = GS_CLUT_STORAGE_CSM1;

    if (entry->bpp == 4) {
        tex->PSM = GS_PSM_T4;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut4Count) {
            fprintf(stderr, "GsRenderer: CLUT4 index %u out of range (max %u) for TPAG %d\n", entry->clutIndex, gs->clut4Count - 1, tpagIndex);
            abort();
        }

        tex->VramClut = gs->clut4VramAddrs[entry->clutIndex];
    } else {
        tex->PSM = GS_PSM_T8;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut8Count) {
            fprintf(stderr, "GsRenderer: CLUT8 index %u out of range (max %u) for TPAG %d\n", entry->clutIndex, gs->clut8Count - 1, tpagIndex);
            abort();
        }

        tex->VramClut = gs->clut8VramAddrs[entry->clutIndex];
    }

    return true;
}

// ===[ Tile Lookup and Texture Setup ]===

// Finds a tile entry by (bgDef, srcX, srcY, srcW, srcH). Returns nullptr if not found.
static AtlasTileEntry* findTileEntry(GsRenderer* gs, int16_t bgDef, uint16_t srcX, uint16_t srcY, uint16_t srcW, uint16_t srcH) {
    forEach(AtlasTileEntry, entry, gs->atlasTileEntries, gs->atlasTileCount) {
        if (entry->bgDef == bgDef && entry->srcX == srcX && entry->srcY == srcY && entry->srcW == srcW && entry->srcH == srcH) {
            return entry;
        }
    }
    return nullptr;
}

// Configures a GSTEXTURE for rendering a tile entry. Same logic as setupTextureForTPAG but for AtlasTileEntry.
static bool setupTextureForTile(GsRenderer* gs, GSTEXTURE* tex, AtlasTileEntry* entry) {
    if (entry->atlasId == 0xFFFF) return false;

    if (!ensureAtlasLoaded(gs, entry->atlasId))
        return false;

    int16_t chunkIdx = gs->atlasToChunk[entry->atlasId];
    uint32_t vramAddr = gs->textureVramBase + (uint32_t) chunkIdx * VRAM_CHUNK_SIZE;

    memset(tex, 0, sizeof(GSTEXTURE));
    tex->Width = ATLAS_WIDTH;
    tex->Height = ATLAS_HEIGHT;
    tex->TBW = ATLAS_WIDTH / 64;
    tex->Vram = vramAddr;
    tex->Filter = GS_FILTER_NEAREST;
    tex->ClutStorageMode = GS_CLUT_STORAGE_CSM1;

    if (entry->bpp == 4) {
        tex->PSM = GS_PSM_T4;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut4Count) {
            fprintf(stderr, "GsRenderer: CLUT4 index %u out of range (max %u) for tile (bg=%d)\n", entry->clutIndex, gs->clut4Count - 1, entry->bgDef);
            abort();
        }

        tex->VramClut = gs->clut4VramAddrs[entry->clutIndex];
    } else {
        tex->PSM = GS_PSM_T8;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut8Count) {
            fprintf(stderr, "GsRenderer: CLUT8 index %u out of range (max %u) for tile (bg=%d)\n", entry->clutIndex, gs->clut8Count - 1, entry->bgDef);
            abort();
        }

        tex->VramClut = gs->clut8VramAddrs[entry->clutIndex];
    }

    return true;
}

// ===[ Vtable Implementations ]===

static void gsInit(Renderer* renderer, DataWin* dataWin) {
    GsRenderer* gs = (GsRenderer*) renderer;

    renderer->dataWin = dataWin;
    renderer->drawColor = 0xFFFFFF;
    renderer->drawAlpha = 1.0f;
    renderer->drawFont = -1;
    renderer->drawHalign = 0;
    renderer->drawValign = 0;

    // Enable alpha blending
    gs->gsGlobal->PrimAlphaEnable = GS_SETTING_ON;

    // Alpha blend: (Cs - Cd) * As / 128 + Cd (standard source-over)
    gsKit_set_primalpha(gs->gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);

    // Load atlas metadata
    loadAtlas(gs);

    // Open TEXTURES.BIN and keep it open for on-demand atlas loading
    char* texturesBinPath = PS2Utils_createDevicePath("TEXTURES.BIN");
    gs->texturesFile = fopen(texturesBinPath, "rb");
    if (gs->texturesFile == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to open %s\n", texturesBinPath);
        abort();
    }
    setvbuf(gs->texturesFile, nullptr, _IOFBF, 128 * 1024);
    free(texturesBinPath);

    // Upload CLUTs to VRAM
    loadAndUploadCLUTs(gs);

    // Initialize the texture cache chunk pool (uses remaining VRAM after CLUTs)
    initTextureCache(gs);

    // Initialize EE RAM cache for compressed atlas data
    initEeCache(gs);
    preloadEeCache(gs);

    fprintf(stderr, "GsRenderer: Initialized (textured mode)\n");
}

static void gsDestroy(Renderer* renderer) {
    GsRenderer* gs = (GsRenderer*) renderer;
    if (gs->texturesFile != nullptr) {
        fclose(gs->texturesFile);
    }
    free(gs->atlasOffsets);
    free(gs->atlasTPAGEntries);
    free(gs->atlasTileEntries);
    free(gs->chunks);
    free(gs->atlasToChunk);
    free(gs->atlasBpp);
    free(gs->clut4VramAddrs);
    free(gs->clut8VramAddrs);
    free(gs->eeCache);
    free(gs->eeCacheEntries);
    free(gs->atlasDataSizes);
    free(gs);
}

static void gsBeginFrame(Renderer* renderer, [[maybe_unused]] int32_t gameW, [[maybe_unused]] int32_t gameH, [[maybe_unused]] int32_t windowW, [[maybe_unused]] int32_t windowH) {
    GsRenderer* gs = (GsRenderer*) renderer;
    gs->zCounter = 1;
    gs->frameCounter++;
}

static void gsEndFrame([[maybe_unused]] Renderer* renderer) {
    // No-op: flip happens in main loop
}

static void gsBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, [[maybe_unused]] int32_t portX, [[maybe_unused]] int32_t portY, [[maybe_unused]] int32_t portW, [[maybe_unused]] int32_t portH, [[maybe_unused]] float viewAngle) {
    GsRenderer* gs = (GsRenderer*) renderer;
    gs->viewX = viewX;
    gs->viewY = viewY;

    // Scale game view to PS2 screen (640x448 NTSC interlaced)
    if (viewW > 0 && viewH > 0) {
        gs->scaleX = 640.0f / (float) viewW;
        gs->scaleY = gs->scaleX;
    } else {
        gs->scaleX = 2.0f;
        gs->scaleY = 2.0f;
    }

    // Center vertically
    float renderedH = (float) viewH * gs->scaleY;
    gs->offsetX = 0.0f;
    gs->offsetY = (448.0f - renderedH) / 2.0f;
}

static void gsEndView([[maybe_unused]] Renderer* renderer) {
    // No-op
}

static void gsDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t) tpagIndex >= dw->tpag.count) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    // Get crop region from atlas entry (falls back to full bounding box if unmapped)
    float cropX = 0.0f, cropY = 0.0f;
    float cropW = (float) tpag->boundingWidth;
    float cropH = (float) tpag->boundingHeight;
    if (gs->atlasTPAGCount > (uint32_t) tpagIndex) {
        AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[tpagIndex];
        if (entry->atlasId != 0xFFFF) {
            cropX = (float) entry->cropX;
            cropY = (float) entry->cropY;
            cropW = (float) entry->cropW;
            cropH = (float) entry->cropH;
        }
    }

    // Compute 4 screen-space corners (tristrip Z-pattern: top-left, top-right, bottom-left, bottom-right)
    // sx0/sy0 = top-left, sx1/sy1 = top-right, sx2/sy2 = bottom-left, sx3/sy3 = bottom-right
    float sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
    bool hasRotation = angleDeg != 0.0f;

    if (hasRotation) {
        // Rotated: compute 4 transformed corners via matrix, same approach as the GLFW renderer
        // Position the cropped region within the original bounding box
        float localX0 = cropX - originX;
        float localY0 = cropY - originY;
        float localX1 = cropX + cropW - originX;
        float localY1 = cropY + cropH - originY;

        // Build 2D transform: T(x,y) * R(-angleDeg) * S(xscale, yscale)
        // Negate angle because Y-down coordinate system
        float angleRad = -angleDeg * ((float) M_PI / 180.0f);
        Matrix4f transform;
        Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

        float gx0, gy0, gx1, gy1, gx2, gy2, gx3, gy3;
        Matrix4f_transformPoint(&transform, localX0, localY0, &gx0, &gy0); // top-left
        Matrix4f_transformPoint(&transform, localX1, localY0, &gx1, &gy1); // top-right
        Matrix4f_transformPoint(&transform, localX0, localY1, &gx2, &gy2); // bottom-left
        Matrix4f_transformPoint(&transform, localX1, localY1, &gx3, &gy3); // bottom-right

        // Apply view offset and scale
        sx0 = (gx0 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy0 = (gy0 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        sx1 = (gx1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy1 = (gy1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        sx2 = (gx2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy2 = (gy2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        sx3 = (gx3 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy3 = (gy3 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    } else {
        // Axis-aligned: simple rect math
        // Position the cropped region within the original bounding box
        float gameX1 = x + (cropX - originX) * xscale;
        float gameY1 = y + (cropY - originY) * yscale;
        float gameX2 = x + (cropX + cropW - originX) * xscale;
        float gameY2 = y + (cropY + cropH - originY) * yscale;

        // Apply view offset and scale
        sx0 = (gameX1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy0 = (gameY1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        sx1 = (gameX2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy1 = (gameY1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        sx2 = (gameX1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy2 = (gameY2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
        sx3 = (gameX2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
        sy3 = (gameY2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    }

    // View frustum culling: skip if entirely off-screen (handles negative scales via min/max)
    float minSX = fminf(fminf(sx0, sx1), fminf(sx2, sx3));
    float maxSX = fmaxf(fmaxf(sx0, sx1), fmaxf(sx2, sx3));
    float minSY = fminf(fminf(sy0, sy1), fminf(sy2, sy3));
    float maxSY = fmaxf(fmaxf(sy0, sy1), fmaxf(sy2, sy3));
    if (maxSX < 0.0f || minSX > PS2_SCREEN_WIDTH || maxSY < 0.0f || minSY > PS2_SCREEN_HEIGHT)
        return;

    // Set up GSTEXTURE for this TPAG entry
    GSTEXTURE tex;
    if (!setupTextureForTPAG(gs, &tex, tpagIndex)) {
        // Fallback: draw colored quad if no atlas mapping
        uint8_t r = BGR_R(color);
        uint8_t g = BGR_G(color);
        uint8_t b = BGR_B(color);
        uint8_t a = alphaToGS(alpha);
        u64 fallbackColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);
        if (hasRotation) {
            gsKit_prim_quad(gs->gsGlobal, sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, gs->zCounter, fallbackColor);
        } else {
            gsKit_prim_sprite(gs->gsGlobal, sx0, sy0, sx3, sy3, gs->zCounter, fallbackColor);
        }
        gs->zCounter++;
        return;
    }

    AtlasTPAGEntry* atlasEntry = &gs->atlasTPAGEntries[tpagIndex];

    // The atlas entry has the actual sprite dimensions in the atlas (post-crop, post-resize).
    // The screen rect covers cropW x cropH game-space pixels, positioned at (cropX, cropY)
    // within the original bounding box. The GS hardware stretches the atlas texels to fill.

    // UV coords within the 512x512 atlas (in texels for gsKit)
    float u0 = (float) atlasEntry->atlasX;
    float v0 = (float) atlasEntry->atlasY;
    float u1 = u0 + (float) atlasEntry->width;
    float v1 = v0 + (float) atlasEntry->height;

    // GS modulate mode: Output = Texture * Vertex / 128
    // Scale vertex RGB from 0-255 to 0-128 so white (255) becomes 128 (1.0x multiplier)
    uint8_t r = BGR_R(color) >> 1;
    uint8_t g = BGR_G(color) >> 1;
    uint8_t b = BGR_B(color) >> 1;
    uint8_t a = alphaToGS(alpha);
    u64 gsColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    if (hasRotation) {
        // Tristrip Z-pattern: needs 4 vertices for rotated quads
        gsKit_prim_quad_texture(
            gs->gsGlobal,
            &tex,
            sx0, sy0, u0, v0, // top-left
            sx1, sy1, u1, v0, // top-right
            sx2, sy2, u0, v1, // bottom-left
            sx3, sy3, u1, v1, // bottom-right
            gs->zCounter,
            gsColor
            );
    } else {
        gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx0, sy0, u0, v0, sx3, sy3, u1, v1, gs->zCounter, gsColor);
    }
    gs->zCounter++;
}

static void gsDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;

    if (0 > tpagIndex || (uint32_t) tpagIndex >= renderer->dataWin->tpag.count) return;

    // Compute screen position
    float gameX1 = x - (float) gs->viewX;
    float gameY1 = y - (float) gs->viewY;
    float gameX2 = gameX1 + (float) srcW * xscale;
    float gameY2 = gameY1 + (float) srcH * yscale;

    float sx1 = gameX1 * gs->scaleX + gs->offsetX;
    float sy1 = gameY1 * gs->scaleY + gs->offsetY;
    float sx2 = gameX2 * gs->scaleX + gs->offsetX;
    float sy2 = gameY2 * gs->scaleY + gs->offsetY;

    // View frustum culling: skip if entirely off-screen (handles negative scales via min/max)
    float minSX = (sx1 < sx2) ? sx1 : sx2;
    float maxSX = (sx1 > sx2) ? sx1 : sx2;
    float minSY = (sy1 < sy2) ? sy1 : sy2;
    float maxSY = (sy1 > sy2) ? sy1 : sy2;
    if (maxSX < 0.0f || minSX > PS2_SCREEN_WIDTH || maxSY < 0.0f || minSY > PS2_SCREEN_HEIGHT) return;

    // Set up GSTEXTURE for this TPAG entry
    GSTEXTURE tex;
    if (!setupTextureForTPAG(gs, &tex, tpagIndex)) {
        // Fallback: draw colored rectangle
        uint8_t r = BGR_R(color);
        uint8_t g = BGR_G(color);
        uint8_t b = BGR_B(color);
        uint8_t a = alphaToGS(alpha);
        gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, GS_SETREG_RGBAQ(r, g, b, a, 0x00));
        gs->zCounter++;
        return;
    }

    AtlasTPAGEntry* atlasEntry = &gs->atlasTPAGEntries[tpagIndex];

    // Intersect the requested source rect with the crop region
    float cX = (float) atlasEntry->cropX;
    float cY = (float) atlasEntry->cropY;
    float cW = (float) atlasEntry->cropW;
    float cH = (float) atlasEntry->cropH;

    float intX1 = fmaxf((float) srcOffX, cX);
    float intY1 = fmaxf((float) srcOffY, cY);
    float intX2 = fminf((float)(srcOffX + srcW), cX + cW);
    float intY2 = fminf((float)(srcOffY + srcH), cY + cH);

    if (intX1 >= intX2 || intY1 >= intY2) return;

    // Adjust screen position if the crop clipped the start of the requested rect
    float clipOffX = intX1 - (float) srcOffX;
    float clipOffY = intY1 - (float) srcOffY;
    float visW = intX2 - intX1;
    float visH = intY2 - intY1;

    sx1 = (x + clipOffX * xscale - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    sy1 = (y + clipOffY * yscale - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    sx2 = (x + (clipOffX + visW) * xscale - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    sy2 = (y + (clipOffY + visH) * yscale - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    // Map intersection region to atlas UV space
    float ratioX = (cW > 0) ? ((float) atlasEntry->width / cW) : 1.0f;
    float ratioY = (cH > 0) ? ((float) atlasEntry->height / cH) : 1.0f;

    float u1 = (float) atlasEntry->atlasX + (intX1 - cX) * ratioX;
    float v1 = (float) atlasEntry->atlasY + (intY1 - cY) * ratioY;
    float u2 = u1 + visW * ratioX;
    float v2 = v1 + visH * ratioY;

    // GS modulate mode: Output = Texture * Vertex / 128
    uint8_t r = BGR_R(color) >> 1;
    uint8_t g = BGR_G(color) >> 1;
    uint8_t b = BGR_B(color) >> 1;
    uint8_t a = alphaToGS(alpha);
    u64 gsColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx1, sy1, u1, v1, sx2, sy2, u2, v2, gs->zCounter, gsColor);
    gs->zCounter++;
}

static void gsDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    GsRenderer* gs = (GsRenderer*) renderer;

    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = alphaToGS(alpha);

    float sx1 = (x1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (y1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx2 = (x2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy2 = (y2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    u64 rectColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    if (outline) {
        // Draw 4 one-pixel-wide edges: top, bottom, left, right
        float pw = gs->scaleX; // one pixel width in screen coords
        float ph = gs->scaleY; // one pixel height in screen coords
        gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2 + pw, sy1 + ph, gs->zCounter, rectColor); // top
        gs->zCounter++;
        gsKit_prim_sprite(gs->gsGlobal, sx1, sy2, sx2 + pw, sy2 + ph, gs->zCounter, rectColor); // bottom
        gs->zCounter++;
        gsKit_prim_sprite(gs->gsGlobal, sx1, sy1 + ph, sx1 + pw, sy2, gs->zCounter, rectColor); // left
        gs->zCounter++;
        gsKit_prim_sprite(gs->gsGlobal, sx2, sy1 + ph, sx2 + pw, sy2, gs->zCounter, rectColor); // right
        gs->zCounter++;
    } else {
        gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, rectColor);
        gs->zCounter++;
    }
}

static void gsDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, [[maybe_unused]] float width, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;

    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = alphaToGS(alpha);

    float sx1 = (x1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (y1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx2 = (x2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy2 = (y2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    u64 lineColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);
    gsKit_prim_line(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, lineColor);
    gs->zCounter++;
}

static void gsDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, [[maybe_unused]] float angleDeg) {
    GsRenderer* gs = (GsRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > renderer->drawFont || (uint32_t) renderer->drawFont >= dw->font.count) return;

    Font* font = &dw->font.fonts[renderer->drawFont];

    // Resolve font texture page and set up GSTEXTURE
    int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    bool hasTexture = false;
    GSTEXTURE tex;
    AtlasTPAGEntry* atlasEntry = nullptr;
    float ratioX = 1.0f;
    float ratioY = 1.0f;

    if (fontTpagIndex >= 0) {
        hasTexture = setupTextureForTPAG(gs, &tex, fontTpagIndex);
        if (hasTexture) {
            atlasEntry = &gs->atlasTPAGEntries[fontTpagIndex];
            TexturePageItem* fontTpag = &dw->tpag.items[fontTpagIndex];

            // Compute ratio between atlas size and original TPAG size (in case the preprocessor downscaled)
            float origW = (float) fontTpag->sourceWidth;
            float origH = (float) fontTpag->sourceHeight;
            ratioX = (origW > 0) ? ((float) atlasEntry->width / origW) : 1.0f;
            ratioY = (origH > 0) ? ((float) atlasEntry->height / origH) : 1.0f;
        }
    }

    // GS modulate mode: Output = Texture * Vertex / 128
    // Scale vertex RGB from 0-255 to 0-128 so white (255) becomes 1.0x multiplier
    uint32_t color = renderer->drawColor;
    uint8_t r = hasTexture ? (BGR_R(color) >> 1) : BGR_R(color);
    uint8_t g = hasTexture ? (BGR_G(color) >> 1) : BGR_G(color);
    uint8_t b = hasTexture ? (BGR_B(color) >> 1) : BGR_B(color);
    uint8_t a = alphaToGS(renderer->drawAlpha);
    u64 textColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    // Preprocess GML text (# -> \n, \# -> #)
    char* processed = TextUtils_preprocessGmlText(text);
    int32_t textLen = (int32_t) strlen(processed);

    // Vertical alignment
    int32_t lineCount = TextUtils_countLines(processed, textLen);
    float totalHeight = (float) lineCount * (float) font->emSize;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float cursorY = valignOffset;
    int32_t lineStart = 0;

    while (textLen >= lineStart) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) {
            lineEnd++;
        }

        int32_t lineLen = lineEnd - lineStart;
        const char* line = processed + lineStart;

        // Horizontal alignment
        float lineWidth = TextUtils_measureLineWidth(font, line, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;

        // Draw each glyph
        int32_t pos = 0;
        while (lineLen > pos) {
            uint16_t ch = TextUtils_decodeUtf8(line, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == nullptr) continue;

            if (glyph->sourceWidth > 0 && glyph->sourceHeight > 0) {
                float glyphX = x + (cursorX + (float) glyph->offset) * xscale * font->scaleX;
                float glyphY = y + cursorY * yscale * font->scaleY;
                float glyphW = (float) glyph->sourceWidth * xscale * font->scaleX;
                float glyphH = (float) glyph->sourceHeight * yscale * font->scaleY;

                float sx1 = (glyphX - (float) gs->viewX) * gs->scaleX + gs->offsetX;
                float sy1 = (glyphY - (float) gs->viewY) * gs->scaleY + gs->offsetY;
                float sx2 = (glyphX + glyphW - (float) gs->viewX) * gs->scaleX + gs->offsetX;
                float sy2 = (glyphY + glyphH - (float) gs->viewY) * gs->scaleY + gs->offsetY;

                if (hasTexture) {
                    // Compute UV coordinates: map glyph position within the font TPAG to atlas space
                    float u1 = (float) atlasEntry->atlasX + (float) glyph->sourceX * ratioX;
                    float v1 = (float) atlasEntry->atlasY + (float) glyph->sourceY * ratioY;
                    float u2 = u1 + (float) glyph->sourceWidth * ratioX;
                    float v2 = v1 + (float) glyph->sourceHeight * ratioY;

                    gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx1, sy1, u1, v1, sx2, sy2, u2, v2, gs->zCounter, textColor);
                } else {
                    // Fallback: draw colored rectangle if font texture is not available
                    gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, textColor);
                }
            }

            cursorX += (float) glyph->shift;

            // Kerning
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(line, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        gs->zCounter++;

        // Next line
        cursorY += (float) font->emSize;
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        } else {
            break;
        }
    }

    free(processed);
}

static void gsFlush([[maybe_unused]] Renderer* renderer) {
    // No-op: gsKit queues commands, executed in main loop
}

static int32_t gsCreateSpriteFromSurface([[maybe_unused]] Renderer* renderer, [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y, [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h, [[maybe_unused]] bool removeback, [[maybe_unused]] bool smooth, [[maybe_unused]] int32_t xorig, [[maybe_unused]] int32_t yorig) {
    fprintf(stderr, "GsRenderer: createSpriteFromSurface not supported on PS2\n");
    return -1;
}

static void gsDeleteSprite([[maybe_unused]] Renderer* renderer, [[maybe_unused]] int32_t spriteIndex) {
    // No-op
}

static void gsDrawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY) {
    GsRenderer* gs = (GsRenderer*) renderer;

    // Look up the tile in the atlas tile entries
    AtlasTileEntry* tileEntry = findTileEntry(gs, (int16_t) tile->backgroundDefinition, (uint16_t) tile->sourceX, (uint16_t) tile->sourceY, (uint16_t) tile->width, (uint16_t) tile->height);
    if (tileEntry == nullptr)
        return;

    // Set up GSTEXTURE for this tile entry
    GSTEXTURE tex;
    if (!setupTextureForTile(gs, &tex, tileEntry))
        return;

    // Compute screen rect in game coordinates
    float drawX = (float) tile->x + offsetX;
    float drawY = (float) tile->y + offsetY;
    float drawW = (float) tile->width * tile->scaleX;
    float drawH = (float) tile->height * tile->scaleY;

    float sx1 = (drawX - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (drawY - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx2 = (drawX + drawW - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy2 = (drawY + drawH - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    // View frustum culling
    float minSX = (sx1 < sx2) ? sx1 : sx2;
    float maxSX = (sx1 > sx2) ? sx1 : sx2;
    float minSY = (sy1 < sy2) ? sy1 : sy2;
    float maxSY = (sy1 > sy2) ? sy1 : sy2;
    if (maxSX < 0.0f || minSX > PS2_SCREEN_WIDTH || maxSY < 0.0f || minSY > PS2_SCREEN_HEIGHT)
        return;

    // UV coordinates in atlas texels
    float u1 = (float) tileEntry->atlasX;
    float v1 = (float) tileEntry->atlasY;
    float u2 = u1 + (float) tileEntry->width;
    float v2 = v1 + (float) tileEntry->height;

    // Extract alpha from tile color high byte, default to 1.0 if 0
    uint8_t alphaByte = (tile->color >> 24) & 0xFF;
    float alpha = (alphaByte == 0) ? 1.0f : (float) alphaByte / 255.0f;
    uint32_t bgr = tile->color & 0x00FFFFFF;

    // GS modulate mode: scale RGB from 0-255 to 0-128
    uint8_t r = BGR_R(bgr) >> 1;
    uint8_t g = BGR_G(bgr) >> 1;
    uint8_t b = BGR_B(bgr) >> 1;
    uint8_t a = alphaToGS(alpha);
    u64 gsColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx1, sy1, u1, v1, sx2, sy2, u2, v2, gs->zCounter, gsColor);
    gs->zCounter++;
}

// ===[ Vtable ]===

static RendererVtable gsVtable = {
    .init = gsInit,
    .destroy = gsDestroy,
    .beginFrame = gsBeginFrame,
    .endFrame = gsEndFrame,
    .beginView = gsBeginView,
    .endView = gsEndView,
    .drawSprite = gsDrawSprite,
    .drawSpritePart = gsDrawSpritePart,
    .drawRectangle = gsDrawRectangle,
    .drawLine = gsDrawLine,
    .drawText = gsDrawText,
    .flush = gsFlush,
    .createSpriteFromSurface = gsCreateSpriteFromSurface,
    .deleteSprite = gsDeleteSprite,
    .drawTile = gsDrawTile,
};

// ===[ Public API ]===

Renderer* GsRenderer_create(GSGLOBAL* gsGlobal) {
    GsRenderer* gs = safeCalloc(1, sizeof(GsRenderer));
    gs->base.vtable = &gsVtable;
    gs->gsGlobal = gsGlobal;
    gs->scaleX = 2.0f;
    gs->scaleY = 2.0f;
    gs->zCounter = 1;
    return (Renderer*) gs;
}
