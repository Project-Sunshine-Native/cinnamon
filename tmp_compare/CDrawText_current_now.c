static void CDrawText(Renderer* renderer, const char* text,
    float x, float y, float xscale, float yscale, float angleDeg)
{
    CRenderer3DS* C = (CRenderer3DS*) renderer;
    DataWin* dw = renderer->dataWin;
    CFlushQueuedRects(C);
    u64 _lagTXT0 = C->lagMode ? svcGetSystemTick() : 0;

    if (renderer->drawFont < 0 || (uint32_t)renderer->drawFont >= dw->font.count) {
        if (C->lagMode) { C->lagTextTicks += svcGetSystemTick() - _lagTXT0; C->lagTextN++; }
        return;
    }

    Font* font = &dw->font.fonts[renderer->drawFont];

    // Resolve which texture page the font lives on
    int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (fontTpagIndex < 0 || (uint32_t)fontTpagIndex >= dw->tpag.count) {
        if (C->lagMode) { C->lagTextTicks += svcGetSystemTick() - _lagTXT0; C->lagTextN++; }
        return;
    }

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
                        dstX, dstY, dstW, dstH, 0.0f,
                        C2D_Color32(BGR_R(renderer->drawColor), BGR_G(renderer->drawColor),
                                    BGR_B(renderer->drawColor),
                                    (uint8_t)(renderer->drawAlpha * 255.0f)),
                    1.0f);
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
    if (C->lagMode) { C->lagTextTicks += svcGetSystemTick() - _lagTXT0; C->lagTextN++; }
}
//
// Evict all non-pinned region cache entries across every page.  Called when
// the game switches rooms so stale sprite textures from the previous room are
// freed, making linear RAM available for the new room's assets.
//
// Pinned entries (font glyphs preloaded at init) are kept - they compact to
// the front of each page's region array so the slot space they vacate is
// immediately reusable.
