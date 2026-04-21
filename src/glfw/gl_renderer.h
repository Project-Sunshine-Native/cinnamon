#pragma once

#include "renderer.h"
#include <glad/glad.h>

// ===[ GLRenderer Struct ]===
// Exposed in the header so platform-specific code (main.c) can access FBO fields for screenshots.
typedef struct {
    Renderer base; // Must be first field for struct embedding

    GLuint shaderProgram;
    GLint uProjection;
    GLint uTexture;

    GLuint vao, vbo, ebo;
    float* vertexData; // MAX_QUADS * VERTICES_PER_QUAD * FLOATS_PER_VERTEX floats

    int32_t quadCount;
    GLuint currentTextureId;

    GLuint* glTextures;       // one GL texture per TXTR page
    int32_t* textureWidths;   // needed for UV normalization
    int32_t* textureHeights;
    uint32_t textureCount;

    GLuint whiteTexture; // 1x1 white pixel for drawing primitives (rectangles, lines, etc.)

    // FBO for render-to-texture (game renders here, then blitted to screen)
    GLuint fbo;
    GLuint fboTexture;
    int32_t fboWidth;
    int32_t fboHeight;
    int32_t windowW; // stored from beginFrame for endFrame blit
    int32_t windowH;
    int32_t gameW; // game resolution (for FBO sizing)
    int32_t gameH;

    // Original counts from data.win (dynamic slots start at these indices)
    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;
} GLRenderer;

Renderer* GLRenderer_create(void);
