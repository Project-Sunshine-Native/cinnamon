static bool uploadRegion(TexCachePage* page, RegionCacheEntry* entry, uint32_t pageIdx) {
    entry->texW = gpuTexDim(entry->srcW);
    entry->texH = gpuTexDim(entry->srcH);

    if (!C3D_TexInit(&entry->tex,
                     (uint16_t)entry->texW, (uint16_t)entry->texH,
                     GPU_RGBA8)) {
        LOG_ERR("CRenderer3DS: C3D_TexInit failed page %lu region %ux%u @ (%u,%u)\n",
                (unsigned long)pageIdx,
                (unsigned)entry->srcW, (unsigned)entry->srcH,
                (unsigned)entry->srcX, (unsigned)entry->srcY);
        entry->loadFailed = true;
        return false;
    }

    size_t bufSize = (size_t)entry->texW * entry->texH * 4;
    uint8_t* swizzle = (uint8_t*)linearAlloc(bufSize);
    if (!swizzle) {
        LOG_ERR("CRenderer3DS: linearAlloc(%lu KB) failed for swizzle page %lu\n",
                (unsigned long)(bufSize / 1024), (unsigned long)pageIdx);
        C3D_TexDelete(&entry->tex);
        entry->loadFailed = true;
        return false;
    }
    memset(swizzle, 0, bufSize);

    bool ok = false;

    if (page->pixels) {
        // Full page already in RAM (font preload path or fallback decode).
        // Use it directly — no extra allocation needed.
        linearToTile(swizzle,
                     page->pixels,
                     entry->srcX, entry->srcY,
                     entry->srcW, entry->srcH,
                     page->atlasW,
                     entry->texW, entry->texH);
        ok = true;
    } else {
        // Hot path during gameplay: read only this sprite's rows from the SD
        // pixel cache.  Peak allocation is srcW*srcH*4 (e.g. 16 KB for 64x64)
        // instead of atlasW*atlasH*4 (up to 16 MB for a 2048x2048 page).
        size_t spriteBytes = (size_t)entry->srcW * entry->srcH * 4;
        uint8_t* spritePixels = (uint8_t*)malloc(spriteBytes);
        if (spritePixels) {
            if (readRegionFromSDCache(page, pageIdx,
                                      entry->srcX, entry->srcY,
                                      entry->srcW, entry->srcH,
                                      spritePixels)) {
                // spritePixels is a packed srcW×srcH buffer starting at (0,0)
                linearToTile(swizzle,
                             spritePixels,
                             0, 0,
                             entry->srcW, entry->srcH,
                             entry->srcW,          // fullSrcW = packed width
                             entry->texW, entry->texH);
                ok = true;
            } else {
                // Fallback for first-run or stale SD cache: decode once and then
                // pull this region from page->pixels.
                if (ensurePageDecoded(page, pageIdx) && page->pixels) {
                    linearToTile(swizzle,
                                 page->pixels,
                                 entry->srcX, entry->srcY,
                                 entry->srcW, entry->srcH,
                                 page->atlasW,
                                 entry->texW, entry->texH);
                    ok = true;
                } else {
                    LOG_ERR("CRenderer3DS: failed to load region page %lu (%u,%u,%u,%u)\n",
                            (unsigned long)pageIdx,
                            entry->srcX, entry->srcY, entry->srcW, entry->srcH);
                }
            }
            free(spritePixels);
        } else {
            LOG_ERR("CRenderer3DS: malloc failed for sprite pixel buf (%lu KB)\n",
                    (unsigned long)(spriteBytes / 1024));
        }
    }

    if (!ok) {
        linearFree(swizzle);
        C3D_TexDelete(&entry->tex);
        entry->loadFailed = true;
        return false;
    }

    memcpy(entry->tex.data, swizzle, bufSize);
    GSPGPU_FlushDataCache(entry->tex.data, bufSize);
    linearFree(swizzle);

    C3D_TexSetFilter(&entry->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&entry->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    entry->loaded = true;
    return true;
}
