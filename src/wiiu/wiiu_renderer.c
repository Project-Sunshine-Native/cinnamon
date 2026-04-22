#include "wiiu_renderer.h"

#include "textured_quad_gsh.h"

#include <gx2/clear.h>
#include <gx2/display.h>
#include <gx2/aperture.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/enum.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <gx2r/surface.h>
#include <coreinit/memdefaultheap.h>

#include <math.h>
#include <limits.h>
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../matrix_math.h"
#include "../stb_image.h"
#include "../text_utils.h"
#include "../utils.h"

#define WIIU_MAX_QUADS 4096
#define WIIU_VERTICES_PER_QUAD 6

// The game always renders into a fixed 640x480 offscreen colour buffer.
// The GPU then blits this to each scan buffer (TV / DRC) using the POINT
// sampler, giving a hardware nearest-neighbour integer upscale.
#define WIIU_GAME_WIDTH  640u
#define WIIU_GAME_HEIGHT 480u

// DRC scan buffer is always 854x480.
#define WIIU_DRC_WIDTH  854u
#define WIIU_DRC_HEIGHT 480u

// Detect the TV scan-buffer resolution from the system scan mode.
// This mirrors the logic in libwhb/src/gfx.c so our blit quad can be
// sized correctly.
static void WiiURenderer_detectTVResolution(WiiURenderer* renderer) {
    switch (GX2GetSystemTVScanMode()) {
        case GX2_TV_SCAN_MODE_480I:
        case GX2_TV_SCAN_MODE_480P:
            if (GX2GetSystemTVAspectRatio() == GX2_ASPECT_RATIO_16_9) {
                renderer->tvWidth  = 854;
                renderer->tvHeight = 480;
            } else {
                renderer->tvWidth  = 640;
                renderer->tvHeight = 480;
            }
            break;
        case GX2_TV_SCAN_MODE_1080I:
        case GX2_TV_SCAN_MODE_1080P:
            renderer->tvWidth  = 1920;
            renderer->tvHeight = 1080;
            break;
        case GX2_TV_SCAN_MODE_720P:
        default:
            renderer->tvWidth  = 1280;
            renderer->tvHeight = 720;
            break;
    }
}

// Compute clip-space coordinates for the integer-scaled blit rect centred
// inside a scan buffer of (scanW x scanH).
// Returns the (x0,y0)-(x1,y1) clip corners for the fullscreen blit quad.
static void WiiURenderer_blitRect(uint32_t scanW, uint32_t scanH,
                                   float* x0, float* y0, float* x1, float* y1) {
    uint32_t scaleX = scanW / WIIU_GAME_WIDTH;
    uint32_t scaleY = scanH / WIIU_GAME_HEIGHT;
    uint32_t scale  = scaleX < scaleY ? scaleX : scaleY;
    if (scale < 1) scale = 1;

    uint32_t rectW = WIIU_GAME_WIDTH  * scale;
    uint32_t rectH = WIIU_GAME_HEIGHT * scale;
    uint32_t offX  = (scanW - rectW) / 2u;
    uint32_t offY  = (scanH - rectH) / 2u;

    // Convert pixel offsets to clip space [-1, 1].
    *x0 = ((float) offX                / (float) scanW) * 2.0f - 1.0f;
    *y0 = 1.0f - ((float)(offY + rectH) / (float) scanH) * 2.0f;
    *x1 = ((float)(offX + rectW)        / (float) scanW) * 2.0f - 1.0f;
    *y1 = 1.0f - ((float) offY          / (float) scanH) * 2.0f;
}

__attribute__((weak)) void WiiURenderer_platformBootLog(const char* message) {
    (void) message;
}

static void WiiURenderer_bootLog(const char* message) {
    WiiURenderer_platformBootLog(message);
}

static void* WiiURenderer_gpuAlloc(size_t alignment, size_t size) {
    void* ptr = MEMAllocFromDefaultHeapEx((uint32_t) size, (int32_t) alignment);
    requireMessage(ptr != NULL, "Wii U GPU allocation failed");
    return ptr;
}

static void WiiURenderer_gpuFree(void* ptr) {
    if (ptr != NULL) {
        MEMFreeToDefaultHeap(ptr);
    }
}

static double WiiURenderer_elapsedMs(const struct timespec* start, const struct timespec* end) {
    double seconds = (double) (end->tv_sec - start->tv_sec);
    double nanos = (double) (end->tv_nsec - start->tv_nsec);
    return seconds * 1000.0 + nanos / 1000000.0;
}

static float WiiURenderer_snapPixel(float value) {
    return floorf(value + 0.5f);
}

void WiiURenderer_refreshOutputState(WiiURenderer* renderer) {
    if (renderer == NULL) return;

    // Re-detect in case the user changed the TV mode at runtime.
    WiiURenderer_detectTVResolution(renderer);

    GX2SetTVScale(renderer->tvWidth, renderer->tvHeight);
    GX2SetDRCScale(WIIU_DRC_WIDTH, WIIU_DRC_HEIGHT);
}



static void WiiURenderer_destroyTexturePage(WiiUTexturePage* page) {
    if (page->texture.surface.image != NULL) {
        WiiURenderer_gpuFree(page->texture.surface.image);
    }
    memset(page, 0, sizeof(*page));
}


static bool WiiURenderer_initLinearTexture(GX2Texture* texture, uint32_t width, uint32_t height) {
    memset(texture, 0, sizeof(*texture));
    texture->surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    texture->surface.width = width;
    texture->surface.height = height;
    texture->surface.depth = 1;
    texture->surface.mipLevels = 1;
    texture->surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    texture->surface.aa = GX2_AA_MODE1X;
    texture->surface.use = GX2_SURFACE_USE_TEXTURE;
    texture->surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    GX2CalcSurfaceSizeAndAlignment(&texture->surface);
    texture->surface.image = WiiURenderer_gpuAlloc(texture->surface.alignment, texture->surface.imageSize);
    memset(texture->surface.image, 0, texture->surface.imageSize);
    texture->viewFirstMip = 0;
    texture->viewNumMips = 1;
    texture->viewFirstSlice = 0;
    texture->viewNumSlices = 1;
    texture->compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
    GX2InitTextureRegs(texture);
    return true;
}

static bool WiiURenderer_uploadTexturePage(WiiUTexturePage* page, const uint8_t* pixels, uint32_t width, uint32_t height) {
    WiiURenderer_destroyTexturePage(page);
    if (!WiiURenderer_initLinearTexture(&page->texture, width, height)) return false;

    uint8_t* dst = (uint8_t*) page->texture.surface.image;
    requireMessage(dst != NULL, "failed to access texture surface image");
    memset(dst, 0, page->texture.surface.imageSize);
    uint32_t dstPitchBytes = page->texture.surface.pitch * 4u;
    for (uint32_t y = 0; y < height; ++y) {
        memcpy(
            dst + (size_t) y * (size_t) dstPitchBytes,
            pixels + (size_t) y * (size_t) width * 4u,
            (size_t) width * 4u
        );
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, page->texture.surface.image, page->texture.surface.imageSize);
    page->ready = true;
    return true;
}

static void WiiURenderer_loadTexturePages(WiiURenderer* renderer, DataWin* dataWin) {
    renderer->texturePageCount = dataWin->txtr.count;
    renderer->texturePages = safeCalloc(renderer->texturePageCount, sizeof(WiiUTexturePage));

    repeat(renderer->texturePageCount, i) {
        uint8_t* pngData = DataWin_loadTexture(dataWin, i);
        if (pngData == NULL) continue;

        int width = 0;
        int height = 0;
        int channels = 0;
        uint8_t* pixels = stbi_load_from_memory(
            pngData,
            (int) dataWin->txtr.textures[i].blobSize,
            &width,
            &height,
            &channels,
            4
        );
        if (pixels == NULL) continue;

        WiiURenderer_uploadTexturePage(&renderer->texturePages[i], pixels, (uint32_t) width, (uint32_t) height);
        stbi_image_free(pixels);
    }

    {
        const uint8_t whitePixel[4] = { 255, 255, 255, 255 };
        WiiURenderer_uploadTexturePage(&renderer->whiteTexture, whitePixel, 1, 1);
    }
}

static void WiiURenderer_freeTexturePages(WiiURenderer* renderer) {
    repeat(renderer->texturePageCount, i) {
        WiiURenderer_destroyTexturePage(&renderer->texturePages[i]);
    }
    free(renderer->texturePages);
    renderer->texturePages = NULL;
    renderer->texturePageCount = 0;
    WiiURenderer_destroyTexturePage(&renderer->whiteTexture);
}

static void WiiURenderer_destroyVertexBuffer(WiiURenderer* renderer) {
    if (renderer->batchVertices != NULL && GX2RBufferExists(&renderer->batchVertexBuffer)) {
        GX2RUnlockBufferEx(&renderer->batchVertexBuffer, 0);
    }
    if (GX2RBufferExists(&renderer->batchVertexBuffer)) {
        GX2RDestroyBufferEx(&renderer->batchVertexBuffer, 0);
    }
    renderer->batchVertices = NULL;
    renderer->batchVertexCapacity = 0;
}

static void WiiURenderer_ensureVertexCapacity(WiiURenderer* renderer, uint32_t neededVertices) {
    uint32_t needed = renderer->batchVertexCount + neededVertices;
    if (needed <= renderer->batchVertexCapacity) return;

    uint32_t newCapacity = renderer->batchVertexCapacity == 0 ? WIIU_MAX_QUADS * WIIU_VERTICES_PER_QUAD : renderer->batchVertexCapacity;
    while (newCapacity < needed) {
        newCapacity *= 2;
    }

    GX2RBuffer newVertexBuffer = { 0 };
    newVertexBuffer.flags =
        GX2R_RESOURCE_BIND_VERTEX_BUFFER |
        GX2R_RESOURCE_USAGE_CPU_WRITE |
        GX2R_RESOURCE_USAGE_GPU_READ |
        GX2R_RESOURCE_USAGE_FORCE_MEM2;
    newVertexBuffer.elemSize = sizeof(WiiUBatchVertex);
    newVertexBuffer.elemCount = newCapacity;
    requireMessage(GX2RCreateBuffer(&newVertexBuffer), "failed to create GX2R vertex buffer");

    WiiUBatchVertex* newVertices = (WiiUBatchVertex*) GX2RLockBufferEx(&newVertexBuffer, 0);
    requireMessage(newVertices != NULL, "failed to access GX2R vertex buffer");

    if (renderer->batchVertices != NULL && renderer->batchVertexCount > 0) {
        memcpy(newVertices, renderer->batchVertices, (size_t) renderer->batchVertexCount * sizeof(WiiUBatchVertex));
    }

    WiiURenderer_destroyVertexBuffer(renderer);
    renderer->batchVertexBuffer = newVertexBuffer;
    renderer->batchVertices = newVertices;
    renderer->batchVertexCapacity = newCapacity;
}

static void WiiURenderer_mapVertexBuffer(WiiURenderer* renderer) {
    if (renderer->batchVertices == NULL && GX2RBufferExists(&renderer->batchVertexBuffer)) {
        renderer->batchVertices = (WiiUBatchVertex*) GX2RLockBufferEx(&renderer->batchVertexBuffer, 0);
        requireMessage(renderer->batchVertices != NULL, "failed to lock GX2R vertex buffer");
    }
}

static void WiiURenderer_pushCommand(WiiURenderer* renderer, const WiiUQuadCommand* command) {
    if (renderer->commandCount >= renderer->commandCapacity) {
        renderer->commandCapacity = renderer->commandCapacity == 0 ? 1024 : renderer->commandCapacity * 2;
        renderer->commands = safeRealloc(renderer->commands, (size_t) renderer->commandCapacity * sizeof(WiiUQuadCommand));
    }
    renderer->commands[renderer->commandCount++] = *command;
}

static void WiiURenderer_bindShader(WiiURenderer* renderer) {
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
    GX2SetShaderModeEx(
        GX2_SHADER_MODE_UNIFORM_BLOCK,
        GX2GetVertexShaderGPRs(renderer->shaderGroup.vertexShader),
        GX2GetVertexShaderStackEntries(renderer->shaderGroup.vertexShader),
        0,
        0,
        GX2GetPixelShaderGPRs(renderer->shaderGroup.pixelShader),
        GX2GetPixelShaderStackEntries(renderer->shaderGroup.pixelShader)
    );
    GX2SetFetchShader(&renderer->shaderGroup.fetchShader);
    GX2SetVertexShader(renderer->shaderGroup.vertexShader);
    GX2SetPixelShader(renderer->shaderGroup.pixelShader);
    GX2SetPixelSampler(&renderer->sampler, renderer->textureUnit);
    GX2SetStreamOutEnable(FALSE);
}

static void WiiURenderer_setCommonState(bool blendEnabled) {
    GX2SetColorControl(GX2_LOGIC_OP_COPY, blendEnabled ? 0x1 : 0x0, FALSE, TRUE);
    GX2SetBlendControl(
        GX2_RENDER_TARGET_0,
        GX2_BLEND_MODE_SRC_ALPHA,
        GX2_BLEND_MODE_INV_SRC_ALPHA,
        GX2_BLEND_COMBINE_MODE_ADD,
        blendEnabled ? GX2_ENABLE : GX2_DISABLE,
        GX2_BLEND_MODE_ONE,
        GX2_BLEND_MODE_INV_SRC_ALPHA,
        GX2_BLEND_COMBINE_MODE_ADD
    );
    GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, 0, 0, 0, 0, 0, 0, 0);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, 0.0f);
}

static void WiiURenderer_flushVerticesWithTexture(WiiURenderer* renderer, uint32_t vertexCount, GX2Texture* texture) {
    if (vertexCount == 0 || texture == NULL) return;

    GX2SetPixelTexture(texture, renderer->textureUnit);

    if (renderer->batchVertices != NULL) {
        GX2RUnlockBufferEx(&renderer->batchVertexBuffer, 0);
        renderer->batchVertices = NULL;
    }
    GX2RInvalidateBuffer(&renderer->batchVertexBuffer, 0);
    GX2RSetAttributeBuffer(&renderer->batchVertexBuffer, 0, sizeof(WiiUBatchVertex), offsetof(WiiUBatchVertex, x));
    GX2RSetAttributeBuffer(&renderer->batchVertexBuffer, 1, sizeof(WiiUBatchVertex), offsetof(WiiUBatchVertex, u));
    GX2RSetAttributeBuffer(&renderer->batchVertexBuffer, 2, sizeof(WiiUBatchVertex), offsetof(WiiUBatchVertex, r));
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, vertexCount, 0, 1);
    GX2DrawDone();

    renderer->perfFlushCount++;
}

static void WiiURenderer_flushVertices(WiiURenderer* renderer, uint32_t vertexCount, int32_t texturePageId) {
    if (vertexCount == 0 || texturePageId < -1) return;

    GX2Texture* texture = texturePageId < 0
        ? &renderer->whiteTexture.texture
        : &renderer->texturePages[texturePageId].texture;
    WiiURenderer_flushVerticesWithTexture(renderer, vertexCount, texture);
}

static WiiUVec2 WiiURenderer_worldToGame(WiiURenderer* renderer, float worldX, float worldY) {
    WiiUVec2 result;
    result.x = (float) renderer->portX + (worldX - (float) renderer->viewX) * renderer->viewScaleX;
    result.y = (float) renderer->portY + (worldY - (float) renderer->viewY) * renderer->viewScaleY;
    return result;
}

// Game coordinates → NDC for the 640x480 offscreen render target.
// The offscreen target IS the game buffer, so this is just a straightforward
// pixel-to-clip mapping with no layout offset needed.
static WiiUVec2 WiiURenderer_gameToClip(float x, float y) {
    WiiUVec2 result;
    result.x = (x / (float) WIIU_GAME_WIDTH)  * 2.0f - 1.0f;
    result.y = 1.0f - (y / (float) WIIU_GAME_HEIGHT) * 2.0f;
    return result;
}

static void WiiURenderer_emitVertex(WiiURenderer* renderer, WiiUVec2 clip, float u, float v, uint32_t color, float alpha) {
    uint32_t index = renderer->batchVertexCount++;
    WiiUBatchVertex* vertex = &renderer->batchVertices[index];

    vertex->x = clip.x;
    vertex->y = clip.y;
    vertex->z = 0.0f;
    vertex->w = 1.0f;
    vertex->u = u;
    vertex->v = v;
    vertex->r = (float) BGR_R(color) / 255.0f;
    vertex->g = (float) BGR_G(color) / 255.0f;
    vertex->b = (float) BGR_B(color) / 255.0f;
    vertex->a = alpha;
}

static void WiiURenderer_appendQuad(
    WiiURenderer* renderer,
    int32_t texturePageId,
    WiiUVec2 p00,
    WiiUVec2 p10,
    WiiUVec2 p01,
    float u0,
    float v0,
    float u1,
    float v1,
    uint32_t color,
    float alpha
) {
    if (renderer->frameWidth <= 0 || renderer->frameHeight <= 0) return;

    WiiUQuadCommand command;
    memset(&command, 0, sizeof(command));
    command.texturePageId = texturePageId;
    command.gradient = false;
    command.p00 = p00;
    command.p10 = p10;
    command.p01 = p01;
    command.u0 = u0;
    command.v0 = v0;
    command.u1 = u1;
    command.v1 = v1;
    command.color0 = color;
    command.color1 = color;
    command.alpha = alpha;
    WiiURenderer_pushCommand(renderer, &command);
    renderer->queuedQuadCount++;
}

static void WiiURenderer_appendGradientQuad(
    WiiURenderer* renderer,
    int32_t texturePageId,
    WiiUVec2 p00,
    WiiUVec2 p10,
    WiiUVec2 p01,
    float u0,
    float v0,
    float u1,
    float v1,
    uint32_t color0,
    uint32_t color1,
    float alpha
) {
    if (renderer->frameWidth <= 0 || renderer->frameHeight <= 0) return;

    WiiUQuadCommand command;
    memset(&command, 0, sizeof(command));
    command.texturePageId = texturePageId;
    command.gradient = true;
    command.p00 = p00;
    command.p10 = p10;
    command.p01 = p01;
    command.u0 = u0;
    command.v0 = v0;
    command.u1 = u1;
    command.v1 = v1;
    command.color0 = color0;
    command.color1 = color1;
    command.alpha = alpha;
    WiiURenderer_pushCommand(renderer, &command);
    renderer->queuedQuadCount++;
}

static void WiiURenderer_buildQuadVertices(WiiURenderer* renderer, const WiiUQuadCommand* command) {
    WiiURenderer_ensureVertexCapacity(renderer, WIIU_VERTICES_PER_QUAD);
    WiiURenderer_mapVertexBuffer(renderer);
    WiiUVec2 c00 = WiiURenderer_gameToClip(command->p00.x, command->p00.y);
    WiiUVec2 c10 = WiiURenderer_gameToClip(command->p10.x, command->p10.y);
    WiiUVec2 c01 = WiiURenderer_gameToClip(command->p01.x, command->p01.y);
    WiiUVec2 p11 = { command->p10.x + (command->p01.x - command->p00.x), command->p10.y + (command->p01.y - command->p00.y) };
    WiiUVec2 c11 = WiiURenderer_gameToClip(p11.x, p11.y);

    if (command->gradient) {
        WiiURenderer_emitVertex(renderer, c00, command->u0, command->v0, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c10, command->u1, command->v0, command->color1, command->alpha);
        WiiURenderer_emitVertex(renderer, c01, command->u0, command->v1, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c01, command->u0, command->v1, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c10, command->u1, command->v0, command->color1, command->alpha);
        WiiURenderer_emitVertex(renderer, c11, command->u1, command->v1, command->color1, command->alpha);
    } else {
        WiiURenderer_emitVertex(renderer, c00, command->u0, command->v0, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c10, command->u1, command->v0, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c01, command->u0, command->v1, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c01, command->u0, command->v1, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c10, command->u1, command->v0, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c11, command->u1, command->v1, command->color0, command->alpha);
    }
}

// Render all queued game commands into the 640x480 offscreen colour buffer,
// which must already be bound as the current render target.
static void WiiURenderer_renderGameCommands(WiiURenderer* renderer) {
    GX2SetViewport(0.0f, 0.0f, (float) WIIU_GAME_WIDTH, (float) WIIU_GAME_HEIGHT, 0.0f, 1.0f);
    GX2SetScissor(0, 0, WIIU_GAME_WIDTH, WIIU_GAME_HEIGHT);
    renderer->batchVertexCount = 0;

    if (renderer->commandCount == 0) return;

    WiiURenderer_bindShader(renderer);
    WiiURenderer_setCommonState(true);
    int32_t currentTexturePageId = INT32_MIN;

    repeat(renderer->commandCount, i) {
        WiiUQuadCommand* command = &renderer->commands[i];
        if (renderer->batchVertexCount > 0 &&
            (command->texturePageId != currentTexturePageId ||
             renderer->batchVertexCount + WIIU_VERTICES_PER_QUAD > renderer->batchVertexCapacity)) {
            WiiURenderer_flushVertices(renderer, renderer->batchVertexCount, currentTexturePageId);
            renderer->batchVertexCount = 0;
        }

        currentTexturePageId = command->texturePageId;
        WiiURenderer_buildQuadVertices(renderer, command);
    }

    if (renderer->batchVertexCount > 0) {
        WiiURenderer_flushVertices(renderer, renderer->batchVertexCount, currentTexturePageId);
        renderer->batchVertexCount = 0;
    }
}

// Blit the 640x480 offscreen colour buffer to the currently-bound scan buffer
// (TV or DRC) using a single fullscreen quad.  The quad is sized to the
// largest integer multiple of 640x480 that fits inside (scanW x scanH) and
// centred.  The existing POINT sampler gives nearest-neighbour upscaling.
static void WiiURenderer_blitOffscreenToScanBuffer(WiiURenderer* renderer, uint32_t scanW, uint32_t scanH) {
    float x0, y0, x1, y1;
    WiiURenderer_blitRect(scanW, scanH, &x0, &y0, &x1, &y1);

    GX2SetViewport(0.0f, 0.0f, (float) scanW, (float) scanH, 0.0f, 1.0f);
    GX2SetScissor(0, 0, scanW, scanH);

    WiiURenderer_bindShader(renderer);
    WiiURenderer_setCommonState(false);  // no blending for the final blit

    WiiURenderer_ensureVertexCapacity(renderer, WIIU_VERTICES_PER_QUAD);
    WiiURenderer_mapVertexBuffer(renderer);
    renderer->batchVertexCount = 0;

    // Full white tint, UV covers the entire 640x480 texture.
    // Offscreen color buffers are sampled upside-down relative to our
    // top-left game coordinate system, so flip V during the blit pass.
    const uint32_t white = 0xFFFFFF;
    const float    alpha = 1.0f;
    WiiURenderer_emitVertex(renderer, (WiiUVec2){x0, y1}, 0.0f, 0.0f, white, alpha); // BL
    WiiURenderer_emitVertex(renderer, (WiiUVec2){x1, y1}, 1.0f, 0.0f, white, alpha); // BR
    WiiURenderer_emitVertex(renderer, (WiiUVec2){x0, y0}, 0.0f, 1.0f, white, alpha); // TL
    WiiURenderer_emitVertex(renderer, (WiiUVec2){x0, y0}, 0.0f, 1.0f, white, alpha); // TL
    WiiURenderer_emitVertex(renderer, (WiiUVec2){x1, y1}, 1.0f, 0.0f, white, alpha); // BR
    WiiURenderer_emitVertex(renderer, (WiiUVec2){x1, y0}, 1.0f, 1.0f, white, alpha); // TR

    WiiURenderer_flushVerticesWithTexture(renderer, renderer->batchVertexCount, &renderer->offscreenTexture);
    renderer->batchVertexCount = 0;
}

static bool WiiURenderer_initOffscreenBuffer(WiiURenderer* renderer) {
    // Allocate a 640x480 tiled colour buffer as the offscreen render target.
    memset(&renderer->offscreenColorBuffer, 0, sizeof(renderer->offscreenColorBuffer));
    renderer->offscreenColorBuffer.surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
    renderer->offscreenColorBuffer.surface.width     = WIIU_GAME_WIDTH;
    renderer->offscreenColorBuffer.surface.height    = WIIU_GAME_HEIGHT;
    renderer->offscreenColorBuffer.surface.depth     = 1;
    renderer->offscreenColorBuffer.surface.mipLevels = 1;
    renderer->offscreenColorBuffer.surface.format    = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    renderer->offscreenColorBuffer.surface.aa        = GX2_AA_MODE1X;
    renderer->offscreenColorBuffer.surface.use       = GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER;
    renderer->offscreenColorBuffer.surface.tileMode  = GX2_TILE_MODE_DEFAULT;
    GX2CalcSurfaceSizeAndAlignment(&renderer->offscreenColorBuffer.surface);
    GX2InitColorBufferRegs(&renderer->offscreenColorBuffer);

    renderer->offscreenColorBuffer.surface.image =
        WiiURenderer_gpuAlloc(renderer->offscreenColorBuffer.surface.alignment,
                              renderer->offscreenColorBuffer.surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_COLOR_BUFFER,
                  renderer->offscreenColorBuffer.surface.image,
                  renderer->offscreenColorBuffer.surface.imageSize);

    // Build a GX2Texture view of the same surface so we can sample it during blit.
    memset(&renderer->offscreenTexture, 0, sizeof(renderer->offscreenTexture));
    renderer->offscreenTexture.surface        = renderer->offscreenColorBuffer.surface;
    renderer->offscreenTexture.viewFirstMip   = 0;
    renderer->offscreenTexture.viewNumMips    = 1;
    renderer->offscreenTexture.viewFirstSlice = 0;
    renderer->offscreenTexture.viewNumSlices  = 1;
    renderer->offscreenTexture.compMap        = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
    GX2InitTextureRegs(&renderer->offscreenTexture);

    renderer->offscreenReady = true;
    return true;
}

static void WiiURenderer_destroyOffscreenBuffer(WiiURenderer* renderer) {
    if (renderer->offscreenColorBuffer.surface.image != NULL) {
        WiiURenderer_gpuFree(renderer->offscreenColorBuffer.surface.image);
    }
    memset(&renderer->offscreenColorBuffer, 0, sizeof(renderer->offscreenColorBuffer));
    memset(&renderer->offscreenTexture,     0, sizeof(renderer->offscreenTexture));
    renderer->offscreenReady = false;
}

static bool WiiURenderer_initShaderPipeline(WiiURenderer* renderer) {
    memset(&renderer->shaderGroup, 0, sizeof(renderer->shaderGroup));
    if (!WHBGfxLoadGFDShaderGroup(&renderer->shaderGroup, 0, resources_wiiu_shaders_textured_quad_gsh)) {
        WiiURenderer_bootLog("wiiu_renderer: failed to load shader group");
        return false;
    }
    if (!WHBGfxInitShaderAttribute(&renderer->shaderGroup, "aPosition", 0, offsetof(WiiUBatchVertex, x), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32)) {
        WiiURenderer_bootLog("wiiu_renderer: failed to init aPosition");
        WHBGfxFreeShaderGroup(&renderer->shaderGroup);
        memset(&renderer->shaderGroup, 0, sizeof(renderer->shaderGroup));
        return false;
    }
    if (!WHBGfxInitShaderAttribute(&renderer->shaderGroup, "aTexCoord", 0, offsetof(WiiUBatchVertex, u), GX2_ATTRIB_FORMAT_FLOAT_32_32)) {
        WiiURenderer_bootLog("wiiu_renderer: failed to init aTexCoord");
        WHBGfxFreeShaderGroup(&renderer->shaderGroup);
        memset(&renderer->shaderGroup, 0, sizeof(renderer->shaderGroup));
        return false;
    }
    if (!WHBGfxInitShaderAttribute(&renderer->shaderGroup, "aColour", 0, offsetof(WiiUBatchVertex, r), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32)) {
        WiiURenderer_bootLog("wiiu_renderer: failed to init aColour");
        WHBGfxFreeShaderGroup(&renderer->shaderGroup);
        memset(&renderer->shaderGroup, 0, sizeof(renderer->shaderGroup));
        return false;
    }
    if (!WHBGfxInitFetchShader(&renderer->shaderGroup)) {
        WiiURenderer_bootLog("wiiu_renderer: failed to init fetch shader");
        WHBGfxFreeShaderGroup(&renderer->shaderGroup);
        memset(&renderer->shaderGroup, 0, sizeof(renderer->shaderGroup));
        return false;
    }

    renderer->textureUnit = 0;
    GX2InitSampler(&renderer->sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSamplerZMFilter(&renderer->sampler, GX2_TEX_Z_FILTER_MODE_NONE, GX2_TEX_MIP_FILTER_MODE_NONE);
    renderer->shaderReady = true;
    return true;
}

static void WiiURenderer_init(Renderer* base, DataWin* dataWin) {
    WiiURenderer_bootLog("wiiu_renderer: init begin");
    WiiURenderer* renderer = (WiiURenderer*) base;
    base->dataWin = dataWin;

    if (!WiiURenderer_initShaderPipeline(renderer)) {
        return;
    }

    // Detect TV output resolution before the first refreshOutputState so the
    // present layout is correct from the very first frame.
    WiiURenderer_detectTVResolution(renderer);
    WiiURenderer_refreshOutputState(renderer);

    {
        char tvLog[128];
        snprintf(tvLog, sizeof(tvLog),
            "wiiu_renderer: TV scan mode detected: %ux%u (point-filter integer upscale)",
            renderer->tvWidth, renderer->tvHeight);
        WiiURenderer_bootLog(tvLog);
    }

    renderer->clearR = 0;
    renderer->clearG = 0;
    renderer->clearB = 0;

    if (!WiiURenderer_initOffscreenBuffer(renderer)) {
        WiiURenderer_bootLog("wiiu_renderer: failed to create offscreen buffer");
        return;
    }

    WiiURenderer_loadTexturePages(renderer, dataWin);
    WiiURenderer_bootLog("wiiu_renderer: gpu path ready");
    WiiURenderer_bootLog("wiiu_renderer: init end");
}

static void WiiURenderer_destroy(Renderer* base) {
    WiiURenderer* renderer = (WiiURenderer*) base;

    WiiURenderer_freeTexturePages(renderer);
    WiiURenderer_destroyOffscreenBuffer(renderer);
    WiiURenderer_destroyVertexBuffer(renderer);
    if (renderer->shaderReady) {
        WHBGfxFreeShaderGroup(&renderer->shaderGroup);
    }
    free(renderer->commands);
    free(renderer);
}

static void WiiURenderer_beginFrame(Renderer* base, [[maybe_unused]] uint32_t clearColor, [[maybe_unused]] uint32_t speed, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    (void) windowW;
    (void) windowH;

    WiiURenderer* renderer = (WiiURenderer*) base;
    renderer->frameWidth = gameW;
    renderer->frameHeight = gameH;
    renderer->viewX = 0;
    renderer->viewY = 0;
    renderer->viewW = gameW;
    renderer->viewH = gameH;
    renderer->portX = 0;
    renderer->portY = 0;
    renderer->portW = gameW;
    renderer->portH = gameH;
    renderer->viewScaleX = 1.0f;
    renderer->viewScaleY = 1.0f;
    renderer->commandCount = 0;
    renderer->batchVertexCount = 0;
    renderer->queuedQuadCount = 0;
}

static void WiiURenderer_beginView(Renderer* base, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle, [[maybe_unused]] uint32_t viewIndex) {
    (void) viewAngle;

    WiiURenderer* renderer = (WiiURenderer*) base;
    renderer->viewX = viewX;
    renderer->viewY = viewY;
    renderer->viewW = viewW;
    renderer->viewH = viewH;
    renderer->portX = portX;
    renderer->portY = portY;
    renderer->portW = portW;
    renderer->portH = portH;
    renderer->viewScaleX = (float) portW / (float) viewW;
    renderer->viewScaleY = (float) portH / (float) viewH;
}

static void WiiURenderer_endView(Renderer* base) {
    (void) base;
}

static void WiiURenderer_endFrame(Renderer* base) {
    WiiURenderer* renderer = (WiiURenderer*) base;
    GX2ColorBuffer* tvScan = WHBGfxGetTVColourBuffer();
    GX2ColorBuffer* drcScan = WHBGfxGetDRCColourBuffer();
    if (!renderer->shaderReady || tvScan == NULL || drcScan == NULL) {
        WiiURenderer_bootLog("wiiu_renderer: missing scan buffers");
        return;
    }

    if (renderer->debugFrameIndex < 12) {
        char summary[160];
        snprintf(
            summary,
            sizeof(summary),
            "wiiu_renderer: frame=%u cmds=%u quads=%u frame=%dx%d view=%d,%d %dx%d port=%d,%d %dx%d",
            renderer->debugFrameIndex,
            renderer->commandCount,
            renderer->queuedQuadCount,
            renderer->frameWidth,
            renderer->frameHeight,
            renderer->viewX,
            renderer->viewY,
            renderer->viewW,
            renderer->viewH,
            renderer->portX,
            renderer->portY,
            renderer->portW,
            renderer->portH
        );
        WiiURenderer_bootLog(summary);

        uint32_t sampleCount = renderer->commandCount < 8 ? renderer->commandCount : 8;
        for (uint32_t i = 0; i < sampleCount; ++i) {
            WiiUQuadCommand* command = &renderer->commands[i];
            char detail[256];
            snprintf(
                detail,
                sizeof(detail),
                "wiiu_renderer: cmd[%u] tex=%d uv=(%.4f,%.4f)-(%.4f,%.4f) p00=(%.1f,%.1f) p10=(%.1f,%.1f) p01=(%.1f,%.1f) alpha=%.3f color=%06X grad=%d",
                i,
                command->texturePageId,
                command->u0,
                command->v0,
                command->u1,
                command->v1,
                command->p00.x,
                command->p00.y,
                command->p10.x,
                command->p10.y,
                command->p01.x,
                command->p01.y,
                command->alpha,
                command->color0 & 0xFFFFFFu,
                command->gradient ? 1 : 0
            );
            WiiURenderer_bootLog(detail);
        }
    }

    struct timespec tvStart;
    struct timespec tvEnd;
    struct timespec drcStart;
    struct timespec drcEnd;

    WHBGfxBeginRender();
    WiiURenderer_refreshOutputState(renderer);

    // ── Pass 1: render game commands into the 640x480 offscreen buffer ──────
    // We use WHBGfxBeginRenderTV as a convenient way to get a valid GX2 context,
    // then immediately redirect the render target to our offscreen buffer.
    clock_gettime(CLOCK_MONOTONIC, &tvStart);
    WHBGfxBeginRenderTV();

    GX2SetColorBuffer(&renderer->offscreenColorBuffer, GX2_RENDER_TARGET_0);
    GX2SetViewport(0.0f, 0.0f, (float) WIIU_GAME_WIDTH, (float) WIIU_GAME_HEIGHT, 0.0f, 1.0f);
    GX2SetScissor(0, 0, WIIU_GAME_WIDTH, WIIU_GAME_HEIGHT);

    // Clear to the game background colour.
    GX2ClearColor(&renderer->offscreenColorBuffer,
        (float) renderer->clearR / 255.0f,
        (float) renderer->clearG / 255.0f,
        (float) renderer->clearB / 255.0f,
        1.0f);
    GX2SetContextState(WHBGfxGetTVContextState());
    GX2SetColorBuffer(&renderer->offscreenColorBuffer, GX2_RENDER_TARGET_0);

    WiiURenderer_renderGameCommands(renderer);
    GX2DrawDone();

    // Make the offscreen surface readable as a texture for the blit pass.
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE,
                  renderer->offscreenColorBuffer.surface.image,
                  renderer->offscreenColorBuffer.surface.imageSize);

    // Restore TV scan buffer as render target, then blit.
    GX2SetColorBuffer(WHBGfxGetTVColourBuffer(), GX2_RENDER_TARGET_0);
    GX2SetViewport(0.0f, 0.0f, (float) renderer->tvWidth, (float) renderer->tvHeight, 0.0f, 1.0f);
    GX2SetScissor(0, 0, renderer->tvWidth, renderer->tvHeight);
    WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    WiiURenderer_blitOffscreenToScanBuffer(renderer, renderer->tvWidth, renderer->tvHeight);

    WHBGfxFinishRenderTV();
    clock_gettime(CLOCK_MONOTONIC, &tvEnd);

    // ── Pass 2: blit the same offscreen buffer to the DRC ───────────────────
    clock_gettime(CLOCK_MONOTONIC, &drcStart);
    WHBGfxBeginRenderDRC();
    WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    WiiURenderer_blitOffscreenToScanBuffer(renderer, WIIU_DRC_WIDTH, WIIU_DRC_HEIGHT);
    WHBGfxFinishRenderDRC();
    GX2DrawDone();
    clock_gettime(CLOCK_MONOTONIC, &drcEnd);

    WHBGfxFinishRender();

    renderer->perfRenderTvMs += WiiURenderer_elapsedMs(&tvStart, &tvEnd);
    renderer->perfRenderDrcMs += WiiURenderer_elapsedMs(&drcStart, &drcEnd);
    renderer->perfQuadCount += renderer->queuedQuadCount;
    renderer->perfFrameCount++;

    if (renderer->perfFrameCount >= 60) {
        char perfBuffer[256];
        snprintf(
            perfBuffer,
            sizeof(perfBuffer),
            "wiiu_renderer: avg over %u frames tv=%.2fms drc=%.2fms total=%.2fms quads=%u flush=%u",
            renderer->perfFrameCount,
            renderer->perfRenderTvMs / (double) renderer->perfFrameCount,
            renderer->perfRenderDrcMs / (double) renderer->perfFrameCount,
            (renderer->perfRenderTvMs + renderer->perfRenderDrcMs) / (double) renderer->perfFrameCount,
            renderer->perfQuadCount / renderer->perfFrameCount,
            renderer->perfFlushCount / renderer->perfFrameCount
        );
        WiiURenderer_bootLog(perfBuffer);
        renderer->perfFrameCount = 0;
        renderer->perfRenderTvMs = 0.0;
        renderer->perfRenderDrcMs = 0.0;
        renderer->perfFlushCount = 0;
        renderer->perfQuadCount = 0;
    }

    renderer->debugFrameIndex++;
}

static void WiiURenderer_drawSprite(Renderer* base, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    WiiURenderer* renderer = (WiiURenderer*) base;
    DataWin* dataWin = base->dataWin;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) return;

    TexturePageItem* tpag = &dataWin->tpag.items[tpagIndex];
    if (tpag->texturePageId < 0 || (uint32_t) tpag->texturePageId >= renderer->texturePageCount) return;
    if (!renderer->texturePages[tpag->texturePageId].ready) return;

    GX2Texture* page = &renderer->texturePages[tpag->texturePageId].texture;
    x = WiiURenderer_snapPixel(x);
    y = WiiURenderer_snapPixel(y);
    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->sourceWidth;
    float localY1 = localY0 + (float) tpag->sourceHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    float w0x, w0y, w1x, w1y, w2x, w2y;
    Matrix4f_transformPoint(&transform, localX0, localY0, &w0x, &w0y);
    Matrix4f_transformPoint(&transform, localX1, localY0, &w1x, &w1y);
    Matrix4f_transformPoint(&transform, localX0, localY1, &w2x, &w2y);

    WiiURenderer_appendQuad(
        renderer,
        tpag->texturePageId,
        WiiURenderer_worldToGame(renderer, w0x, w0y),
        WiiURenderer_worldToGame(renderer, w1x, w1y),
        WiiURenderer_worldToGame(renderer, w2x, w2y),
        (float) tpag->sourceX / (float) page->surface.width,
        (float) tpag->sourceY / (float) page->surface.height,
        (float) (tpag->sourceX + tpag->sourceWidth) / (float) page->surface.width,
        (float) (tpag->sourceY + tpag->sourceHeight) / (float) page->surface.height,
        color,
        alpha
    );
}

static void WiiURenderer_drawSpritePart(Renderer* base, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    WiiURenderer* renderer = (WiiURenderer*) base;
    DataWin* dataWin = base->dataWin;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) return;

    TexturePageItem* tpag = &dataWin->tpag.items[tpagIndex];
    if (tpag->texturePageId < 0 || (uint32_t) tpag->texturePageId >= renderer->texturePageCount) return;
    if (!renderer->texturePages[tpag->texturePageId].ready) return;

    GX2Texture* page = &renderer->texturePages[tpag->texturePageId].texture;
    x = WiiURenderer_snapPixel(x);
    y = WiiURenderer_snapPixel(y);
    float x1 = x + (float) srcW * xscale;
    float y1 = y + (float) srcH * yscale;
    WiiURenderer_appendQuad(
        renderer,
        tpag->texturePageId,
        WiiURenderer_worldToGame(renderer, x, y),
        WiiURenderer_worldToGame(renderer, x1, y),
        WiiURenderer_worldToGame(renderer, x, y1),
        (float) (tpag->sourceX + srcOffX) / (float) page->surface.width,
        (float) (tpag->sourceY + srcOffY) / (float) page->surface.height,
        (float) (tpag->sourceX + srcOffX + srcW) / (float) page->surface.width,
        (float) (tpag->sourceY + srcOffY + srcH) / (float) page->surface.height,
        color,
        alpha
    );
}

static void WiiURenderer_drawRectangle(Renderer* base, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    WiiURenderer* renderer = (WiiURenderer*) base;

    if (outline) {
        renderer->base.vtable->drawLine(base, x1, y1, x2, y1, 1.0f, color, alpha);
        renderer->base.vtable->drawLine(base, x2, y1, x2, y2, 1.0f, color, alpha);
        renderer->base.vtable->drawLine(base, x2, y2, x1, y2, 1.0f, color, alpha);
        renderer->base.vtable->drawLine(base, x1, y2, x1, y1, 1.0f, color, alpha);
        return;
    }

    WiiURenderer_appendQuad(
        renderer,
        -1,
        WiiURenderer_worldToGame(renderer, x1, y1),
        WiiURenderer_worldToGame(renderer, x2, y1),
        WiiURenderer_worldToGame(renderer, x1, y2),
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        color,
        alpha
    );
}

static void WiiURenderer_drawLine(Renderer* base, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    WiiURenderer* renderer = (WiiURenderer*) base;

    WiiUVec2 p0 = WiiURenderer_worldToGame(renderer, x1, y1);
    WiiUVec2 p1 = WiiURenderer_worldToGame(renderer, x2, y2);
    x1 = p0.x;
    y1 = p0.y;
    x2 = p1.x;
    y2 = p1.y;
    width *= fmaxf(renderer->viewScaleX, renderer->viewScaleY);

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0001f) {
        WiiURenderer_appendQuad(renderer, -1, (WiiUVec2) { x1, y1 }, (WiiUVec2) { x1 + 1.0f, y1 }, (WiiUVec2) { x1, y1 + 1.0f }, 0.0f, 0.0f, 1.0f, 1.0f, color, alpha);
        return;
    }

    float half = fmaxf(width, 1.0f) * 0.5f;
    float nx = -dy / len * half;
    float ny = dx / len * half;
    WiiURenderer_appendQuad(
        renderer,
        -1,
        (WiiUVec2) { x1 - nx, y1 - ny },
        (WiiUVec2) { x2 - nx, y2 - ny },
        (WiiUVec2) { x1 + nx, y1 + ny },
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        color,
        alpha
    );
}

static void WiiURenderer_drawLineColor(Renderer* base, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    WiiURenderer* renderer = (WiiURenderer*) base;

    WiiUVec2 p0 = WiiURenderer_worldToGame(renderer, x1, y1);
    WiiUVec2 p1 = WiiURenderer_worldToGame(renderer, x2, y2);
    x1 = p0.x;
    y1 = p0.y;
    x2 = p1.x;
    y2 = p1.y;
    width *= fmaxf(renderer->viewScaleX, renderer->viewScaleY);

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0001f) {
        renderer->base.vtable->drawRectangle(base, x1, y1, x1 + 1.0f, y1 + 1.0f, color1, alpha, false);
        return;
    }

    float half = fmaxf(width, 1.0f) * 0.5f;
    float nx = -dy / len * half;
    float ny = dx / len * half;
    WiiURenderer_appendGradientQuad(
        renderer,
        -1,
        (WiiUVec2) { x1 - nx, y1 - ny },
        (WiiUVec2) { x2 - nx, y2 - ny },
        (WiiUVec2) { x1 + nx, y1 + ny },
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        color1,
        color2,
        alpha
    );
}

static void WiiURenderer_drawText(Renderer* base, const char* text, float x, float y, float xscale, float yscale, float angleDeg) {
    WiiURenderer* renderer = (WiiURenderer*) base;
    DataWin* dataWin = base->dataWin;

    int32_t fontIndex = base->drawFont;
    if (fontIndex < 0 || (uint32_t) fontIndex >= dataWin->font.count) return;

    Font* font = &dataWin->font.fonts[fontIndex];
    int32_t fontTpagIndex = DataWin_resolveTPAG(dataWin, font->textureOffset);
    if (fontTpagIndex < 0 || (uint32_t) fontTpagIndex >= dataWin->tpag.count) return;

    TexturePageItem* fontTpag = &dataWin->tpag.items[fontTpagIndex];
    if (fontTpag->texturePageId < 0 || (uint32_t) fontTpag->texturePageId >= renderer->texturePageCount) return;
    if (!renderer->texturePages[fontTpag->texturePageId].ready) return;

    GX2Texture* page = &renderer->texturePages[fontTpag->texturePageId].texture;
    PreprocessedText processedPt = TextUtils_preprocessGmlText(text);
    const char* processed = processedPt.text;
    int32_t textLen = (int32_t) strlen(processed);
    int32_t lineCount = TextUtils_countLines(processed, textLen);
    float totalHeight = (float) lineCount * (float) font->emSize;
    float valignOffset = 0.0f;
    if (base->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (base->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    x = WiiURenderer_snapPixel(x);
    y = WiiURenderer_snapPixel(y);
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    float cursorY = valignOffset;
    int32_t lineStart = 0;
    repeat(lineCount, lineIdx) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(processed[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, processed + lineStart, lineLen);
        float halignOffset = 0.0f;
        if (base->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (base->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        int32_t pos = 0;
        while (pos < lineLen) {
            uint16_t ch = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == NULL) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            float localX0 = cursorX + glyph->offset;
            float localY0 = cursorY;
            float localX1 = localX0 + (float) glyph->sourceWidth;
            float localY1 = localY0 + (float) glyph->sourceHeight;

            float w0x, w0y, w1x, w1y, w2x, w2y;
            Matrix4f_transformPoint(&transform, localX0, localY0, &w0x, &w0y);
            Matrix4f_transformPoint(&transform, localX1, localY0, &w1x, &w1y);
            Matrix4f_transformPoint(&transform, localX0, localY1, &w2x, &w2y);

            WiiURenderer_appendQuad(
                renderer,
                fontTpag->texturePageId,
                WiiURenderer_worldToGame(renderer, w0x, w0y),
                WiiURenderer_worldToGame(renderer, w1x, w1y),
                WiiURenderer_worldToGame(renderer, w2x, w2y),
                (float) (fontTpag->sourceX + glyph->sourceX) / (float) page->surface.width,
                (float) (fontTpag->sourceY + glyph->sourceY) / (float) page->surface.height,
                (float) (fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / (float) page->surface.width,
                (float) (fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / (float) page->surface.height,
                base->drawColor,
                base->drawAlpha
            );

            cursorX += glyph->shift;
            if (pos < lineLen) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += (float) font->emSize;
        lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
    }

    PreprocessedText_free(processedPt);
}

static void WiiURenderer_flush(Renderer* base) {
    (void) base;
}

static void WiiURenderer_onRoomEnd(Renderer* base) {
    (void) base;
}

static int32_t WiiURenderer_createSpriteFromSurface(Renderer* base, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    (void) removeback;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) smooth;
    (void) xorig;
    (void) yorig;
    (void) base;
    WiiURenderer_bootLog("wiiu_renderer: createSpriteFromSurface unsupported on gpu path");
    return -1;
}

static void WiiURenderer_deleteSprite(Renderer* base, int32_t spriteIndex) {
    (void) base;
    (void) spriteIndex;
}

static RendererVtable WiiURendererVtable = {
    .init = WiiURenderer_init,
    .destroy = WiiURenderer_destroy,
    .beginFrame = WiiURenderer_beginFrame,
    .endFrame = WiiURenderer_endFrame,
    .beginView = WiiURenderer_beginView,
    .endView = WiiURenderer_endView,
    .drawSprite = WiiURenderer_drawSprite,
    .drawSpritePart = WiiURenderer_drawSpritePart,
    .drawRectangle = WiiURenderer_drawRectangle,
    .drawLine = WiiURenderer_drawLine,
    .drawLineColor = WiiURenderer_drawLineColor,
    .drawText = WiiURenderer_drawText,
    .flush = WiiURenderer_flush,
    .createSpriteFromSurface = WiiURenderer_createSpriteFromSurface,
    .deleteSprite = WiiURenderer_deleteSprite,
    .drawTile = NULL,
    .onRoomEnd = NULL,
    .onRoomStart = NULL,
    .onRoomEnd = WiiURenderer_onRoomEnd,
};

Renderer* WiiURenderer_create(void) {
    WiiURenderer_bootLog("wiiu_renderer: create begin");
    WiiURenderer* renderer = safeCalloc(1, sizeof(WiiURenderer));
    renderer->base.vtable = &WiiURendererVtable;
    renderer->base.drawColor = 0xFFFFFF;
    renderer->base.drawAlpha = 1.0f;
    renderer->base.drawFont = -1;
    WiiURenderer_bootLog("wiiu_renderer: create end");
    return (Renderer*) renderer;
}

void WiiURenderer_setClearColor(WiiURenderer* renderer, uint32_t color) {
    renderer->clearR = (uint8_t) BGR_R(color);
    renderer->clearG = (uint8_t) BGR_G(color);
    renderer->clearB = (uint8_t) BGR_B(color);
}

void WiiURenderer_runStartupSmokeTest(uint32_t frameCount) {
    (void) frameCount;
}
