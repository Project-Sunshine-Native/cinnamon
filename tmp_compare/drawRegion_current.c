static void drawRegion(CRenderer3DS* C,
                        uint32_t pageIdx,
                        float srcX,  float srcY,
                        float srcW,  float srcH,
                        float dstX,  float dstY,
                        float dstW,  float dstH,
                        float angle, u32 color,
                        float blend)
{
    bool flipX = dstW < 0.0f;
    bool flipY = dstH < 0.0f;
    if (flipX) {
        dstX += dstW;
        dstW = -dstW;
    }
    if (flipY) {
        dstY += dstH;
        dstH = -dstH;
    }

    if (isRotatedRectOffscreen(C, dstX, dstY, dstW, dstH, angle)) return;

    CFlushQueuedRects(C);

    DBG_LOG("drawRegion: Called for page %lu: src=(%.1f,%.1f,%.1f,%.1f) dst=(%.1f,%.1f,%.1f,%.1f)\n",
            (unsigned long)pageIdx, srcX, srcY, srcW, srcH, dstX, dstY, dstW, dstH);

    if (pageIdx >= C->pageCacheCount) {
        DBG_LOG("drawRegion: ERROR - page index %lu >= cache count %lu\n",
               (unsigned long)pageIdx, (unsigned long)C->pageCacheCount);
        return;
    }
    
    TexCachePage* page = &C->pageCache[pageIdx];
    if (page->loadFailed) {
        DBG_LOG("drawRegion: ERROR - page %lu marked as loadFailed\n", (unsigned long)pageIdx);
        return;
    }

    page->lastUsedFrame = C->frameCounter;

    // Clamp source rect to atlas bounds
    if (srcX < 0.0f) { 
        float d = -srcX * dstW / srcW; 
        dstX += d; dstW -= d; 
        srcW += srcX; 
        srcX = 0.0f; 
    }
    if (srcY < 0.0f) { 
        float d = -srcY * dstH / srcH; 
        dstY += d; dstH -= d; 
        srcH += srcY; 
        srcY = 0.0f;
    }
    {
        float overX = (srcX + srcW) - (float)page->atlasW;
        float overY = (srcY + srcH) - (float)page->atlasH;
        if (overX > 0.0f) { dstW -= overX * dstW / srcW; srcW -= overX; }
        if (overY > 0.0f) { dstH -= overY * dstH / srcH; srcH -= overY; }
    }
    if (srcW <= 0.0f || srcH <= 0.0f) return;

    u64 _lagRegT0 = C->lagMode ? svcGetSystemTick() : 0;

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

            RegionCacheEntry* entry = regionLookup(page, iSrcX, iSrcY, iSrcW, iSrcH,
                                                   C->frameCounter);
            if (!entry) {
                // No full-page decode needed — uploadRegion reads directly from
                // the SD pixel cache row-by-row.  ensurePageDecoded is only
                // called if page->pixels is somehow already populated (e.g.
                // during the init font-preload window).
                entry = regionAlloc(page, iSrcX, iSrcY, iSrcW, iSrcH, C->frameCounter);
                if (!entry) goto next_chunk;

                uploadRegion(page, entry, pageIdx);
                if (!entry->loaded) goto next_chunk;
            }

            if (!entry->loaded) goto next_chunk;

            {
                float scaleW = (float)iSrcW / (float)entry->texW;
                float scaleH = (float)iSrcH / (float)entry->texH;

                Tex3DS_SubTexture subtex = {
                    .width  = iSrcW,
                    .height = iSrcH,
                    .left   = flipX ? scaleW : 0.0f,
                    .right  = flipX ? 0.0f : scaleW,
                    .top    = flipY ? (1.0f - scaleH) : 1.0f,
                    .bottom = flipY ? 1.0f : (1.0f - scaleH),
                };

                if (entry->tex.data == NULL) goto next_chunk;

                C2D_Image image = { .tex = &entry->tex, .subtex = &subtex };

                C2D_ImageTint tint;
                float tintBlend = blend;
                if (tintBlend < 0.0f) tintBlend = 0.0f;
                if (tintBlend > 1.0f) tintBlend = 1.0f;
                C2D_PlainImageTint(&tint, color, tintBlend);
                DBG_LOG("[DRAW_REGION] color=%08lx blend=%.2f chunkSrc=%dx%d dst=%.1f,%.1f\n",
                    (unsigned long)color, tintBlend, chunkW, chunkH, dstX, dstY);

                float absScaleX = fabsf(pixScaleX);
                float absScaleY = fabsf(pixScaleY);
                float chunkDestX = flipX
                    ? dstX + (srcX + srcW - (chunkX + chunkW)) * absScaleX
                    : dstX + (chunkX - srcX) * absScaleX;
                float chunkDestY = flipY
                    ? dstY + (srcY + srcH - (chunkY + chunkH)) * absScaleY
                    : dstY + (chunkY - srcY) * absScaleY;
                float chunkDestW = chunkW * absScaleX;
                float chunkDestH = chunkH * absScaleY;

                float drawX = chunkDestX;
                float drawY = chunkDestY;
                float centerX = 0.0f;
                float centerY = 0.0f;
                if (angle != 0.0f) {
                    centerX = chunkDestW * 0.5f;
                    centerY = chunkDestH * 0.5f;
                    // C2D applies center as an origin offset from pos; keep
                    // the visual top-left fixed by advancing pos to the center.
                    drawX += centerX;
                    drawY += centerY;
                }

                C2D_DrawParams params = {
                    .pos    = { drawX, drawY, chunkDestW, chunkDestH },
                    .center = { centerX, centerY },
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

    if (C->lagMode) {
        C->lagRegionTicks += svcGetSystemTick() - _lagRegT0;
        C->lagRegionN++;
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

