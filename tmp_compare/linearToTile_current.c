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
                    // citro2d automatically does this
                    //uint32_t flippedLy = (texH - 1) - ly;

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
                            uint32_t srcOff = ((srcY0 + ly) * fullSrcW + (srcX0 + lx)) * 4;
                            dst[dstOff + 0] = src[srcOff + 3]; // A
                            dst[dstOff + 1] = src[srcOff + 2]; // B
                            dst[dstOff + 2] = src[srcOff + 1]; // G
                            dst[dstOff + 3] = src[srcOff + 0]; // R
                        } else {
                            dst[dstOff + 0] = 0;
                            dst[dstOff + 1] = 0;
                            dst[dstOff + 2] = 0;
                            dst[dstOff + 3] = 0;
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
