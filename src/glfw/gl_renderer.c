#include "gl_renderer.h"
#include "matrix_math.h"
#include "text_utils.h"

#include <glad/glad.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_image.h"
#include "stb_ds.h"
#include "utils.h"

// ===[ Constants ]===
#define MAX_QUADS 4096
#define FLOATS_PER_VERTEX 8  // x, y, u, v, r, g, b, a
#define VERTICES_PER_QUAD 4
#define INDICES_PER_QUAD 6

// ===[ Shader Sources ]===
static const char* vertexShaderSource =
    "#version 410 core\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aTexCoord;\n"
    "layout(location = 2) in vec4 aColor;\n"
    "uniform mat4 uProjection;\n"
    "out vec2 vTexCoord;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "    vColor = aColor;\n"
    "}\n";

static const char* fragmentShaderSource =
    "#version 410 core\n"
    "in vec2 vTexCoord;\n"
    "in vec4 vColor;\n"
    "uniform sampler2D uTexture;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = texture(uTexture, vTexCoord) * vColor;\n"
    "}\n";

// ===[ Shader Compilation ]===

static GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        fprintf(stderr, "GL: Shader compilation failed: %s\n", infoLog);
        abort();
    }
    return shader;
}

static GLuint linkProgram(GLuint vertShader, GLuint fragShader) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vertShader);
    glAttachShader(program, fragShader);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        fprintf(stderr, "GL: Shader linking failed: %s\n", infoLog);
        abort();
    }
    return program;
}

// ===[ Batch Flush ]===

static void flushBatch(GLRenderer* gl) {
    if (gl->quadCount == 0) return;

    int32_t vertexCount = gl->quadCount * VERTICES_PER_QUAD;
    int32_t indexCount = gl->quadCount * INDICES_PER_QUAD;

    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertexCount * FLOATS_PER_VERTEX * sizeof(float), gl->vertexData);

    glBindTexture(GL_TEXTURE_2D, gl->currentTextureId);
    glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);

    gl->quadCount = 0;
}

// ===[ Vtable Implementations ]===

static void glInit(Renderer* renderer, DataWin* dataWin) {
    GLRenderer* gl = (GLRenderer*) renderer;
    renderer->dataWin = dataWin;

    // Compile shaders
    GLuint vertShader = compileShader(GL_VERTEX_SHADER, vertexShaderSource);
    GLuint fragShader = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    gl->shaderProgram = linkProgram(vertShader, fragShader);
    glDeleteShader(vertShader);
    glDeleteShader(fragShader);

    gl->uProjection = glGetUniformLocation(gl->shaderProgram, "uProjection");
    gl->uTexture = glGetUniformLocation(gl->shaderProgram, "uTexture");

    // Create VAO/VBO/EBO
    glGenVertexArrays(1, &gl->vao);
    glGenBuffers(1, &gl->vbo);
    glGenBuffers(1, &gl->ebo);

    glBindVertexArray(gl->vao);

    // VBO: sized for max quads
    int32_t vboSize = MAX_QUADS * VERTICES_PER_QUAD * FLOATS_PER_VERTEX * (int32_t) sizeof(float);
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBufferData(GL_ARRAY_BUFFER, vboSize, nullptr, GL_DYNAMIC_DRAW);

    // EBO: pre-fill with quad index pattern (0,1,2,2,3,0 repeated)
    int32_t eboSize = MAX_QUADS * INDICES_PER_QUAD * (int32_t) sizeof(uint32_t);
    uint32_t* indices = safeMalloc(eboSize);
    for (int32_t i = 0; MAX_QUADS > i; i++) {
        uint32_t base = (uint32_t) i * 4;
        indices[i * 6 + 0] = base + 0;
        indices[i * 6 + 1] = base + 1;
        indices[i * 6 + 2] = base + 2;
        indices[i * 6 + 3] = base + 2;
        indices[i * 6 + 4] = base + 3;
        indices[i * 6 + 5] = base + 0;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, eboSize, indices, GL_STATIC_DRAW);
    free(indices);

    // Vertex attributes: pos(2f), texcoord(2f), color(4f)
    int32_t stride = FLOATS_PER_VERTEX * (int32_t) sizeof(float);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*) 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*) (2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*) (4 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);

    // Allocate CPU-side vertex buffer
    gl->vertexData = safeMalloc(MAX_QUADS * VERTICES_PER_QUAD * FLOATS_PER_VERTEX * sizeof(float));

    // Load textures from TXTR pages
    gl->textureCount = dataWin->txtr.count;
    gl->glTextures = safeMalloc(gl->textureCount * sizeof(GLuint));
    gl->textureWidths = safeMalloc(gl->textureCount * sizeof(int32_t));
    gl->textureHeights = safeMalloc(gl->textureCount * sizeof(int32_t));

    glGenTextures((GLsizei) gl->textureCount, gl->glTextures);

    for (uint32_t i = 0; gl->textureCount > i; i++) {
        Texture* txtr = &dataWin->txtr.textures[i];
        uint8_t* pngData = txtr->blobData;
        uint32_t pngSize = txtr->blobSize;

        int w, h, channels;
        uint8_t* pixels = stbi_load_from_memory(pngData, (int) pngSize, &w, &h, &channels, 4);
        if (pixels == nullptr) {
            fprintf(stderr, "GL: Failed to decode TXTR page %u\n", i);
            gl->textureWidths[i] = 0;
            gl->textureHeights[i] = 0;
            continue;
        }

        gl->textureWidths[i] = w;
        gl->textureHeights[i] = h;

        glBindTexture(GL_TEXTURE_2D, gl->glTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        stbi_image_free(pixels);
        fprintf(stderr, "GL: Loaded TXTR page %u (%dx%d)\n", i, w, h);
    }

    // Create 1x1 white pixel texture for primitive drawing (rectangles, lines, etc.)
    glGenTextures(1, &gl->whiteTexture);
    glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);
    uint8_t whitePixel[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Enable blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    gl->quadCount = 0;
    gl->currentTextureId = 0;

    // Create FBO (texture will be allocated/resized in beginFrame)
    glGenFramebuffers(1, &gl->fbo);
    gl->fboTexture = 0;
    gl->fboWidth = 0;
    gl->fboHeight = 0;

    // Save original counts so we know which slots are from data.win vs dynamic
    gl->originalTexturePageCount = gl->textureCount;
    gl->originalTpagCount = dataWin->tpag.count;
    gl->originalSpriteCount = dataWin->sprt.count;

    fprintf(stderr, "GL: Renderer initialized (%u texture pages)\n", gl->textureCount);
}

static void glDestroy(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;

    if (gl->fboTexture != 0) glDeleteTextures(1, &gl->fboTexture);
    glDeleteFramebuffers(1, &gl->fbo);
    glDeleteTextures(1, &gl->whiteTexture);

    glDeleteTextures((GLsizei) gl->textureCount, gl->glTextures);
    glDeleteProgram(gl->shaderProgram);
    glDeleteVertexArrays(1, &gl->vao);
    glDeleteBuffers(1, &gl->vbo);
    glDeleteBuffers(1, &gl->ebo);

    free(gl->glTextures);
    free(gl->textureWidths);
    free(gl->textureHeights);
    free(gl->vertexData);
    free(gl);
}

static void glBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    GLRenderer* gl = (GLRenderer*) renderer;

    gl->quadCount = 0;
    gl->currentTextureId = 0;
    gl->windowW = windowW;
    gl->windowH = windowH;
    gl->gameW = gameW;
    gl->gameH = gameH;

    // Resize FBO to game resolution if needed
    if (gameW != gl->fboWidth || gameH != gl->fboHeight) {
        if (gl->fboTexture != 0) glDeleteTextures(1, &gl->fboTexture);

        glGenTextures(1, &gl->fboTexture);
        glBindTexture(GL_TEXTURE_2D, gl->fboTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gameW, gameH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glBindFramebuffer(GL_FRAMEBUFFER, gl->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl->fboTexture, 0);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "GL: Framebuffer incomplete (status=0x%X)\n", status);
        }

        gl->fboWidth = gameW;
        gl->fboHeight = gameH;
        fprintf(stderr, "GL: FBO resized to %dx%d\n", gameW, gameH);
    }

    // Bind FBO and clear
    glBindFramebuffer(GL_FRAMEBUFFER, gl->fbo);
    glViewport(0, 0, gameW, gameH);
}

static void glBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    GLRenderer* gl = (GLRenderer*) renderer;

    gl->quadCount = 0;
    gl->currentTextureId = 0;

    // Set viewport and scissor to the port rectangle within the FBO
    // FBO uses game resolution, port coordinates are in game space
    // OpenGL viewport Y is bottom-up, game Y is top-down
    int32_t glPortY = gl->gameH - portY - portH;
    glViewport(portX, glPortY, portW, portH);
    glEnable(GL_SCISSOR_TEST);
    glScissor(portX, glPortY, portW, portH);

    // Build orthographic projection (Y-down for GML coordinate system)
    Matrix4f projection;
    Matrix4f_identity(&projection);
    Matrix4f_ortho(&projection, (float) viewX, (float) (viewX + viewW), (float) (viewY + viewH), (float) viewY, -1.0f, 1.0f);

    if (viewAngle != 0.0f) {
        // GML view_angle: rotate camera by this angle (degrees, counter-clockwise)
        // To rotate the camera, we rotate the world in the opposite direction around the view center
        float cx = (float) viewX + (float) viewW / 2.0f;
        float cy = (float) viewY + (float) viewH / 2.0f;
        Matrix4f rot;
        Matrix4f_identity(&rot);
        Matrix4f_translate(&rot, cx, cy, 0.0f);
        float angleRad = viewAngle * (float) M_PI / 180.0f;
        Matrix4f_rotateZ(&rot, -angleRad);
        Matrix4f_translate(&rot, -cx, -cy, 0.0f);
        Matrix4f result;
        Matrix4f_multiply(&result, &projection, &rot);
        projection = result;
    }

    glUseProgram(gl->shaderProgram);
    glUniformMatrix4fv(gl->uProjection, 1, GL_FALSE, projection.m);
    glUniform1i(gl->uTexture, 0);
    glActiveTexture(GL_TEXTURE0);

    glBindVertexArray(gl->vao);
}

static void glEndView(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
    glDisable(GL_SCISSOR_TEST);
}

static void glEndFrame(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    glBindVertexArray(0);

    // Blit the full game-resolution FBO to the window
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, gl->fboWidth, gl->fboHeight, 0, 0, gl->windowW, gl->windowH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void glRendererFlush(Renderer* renderer) {
    flushBatch((GLRenderer*) renderer);
}

static void glDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return;

    GLuint texId = gl->glTextures[pageId];
    int32_t texW = gl->textureWidths[pageId];
    int32_t texH = gl->textureHeights[pageId];
    if (texW == 0 || texH == 0) return;

    // Flush if texture changed or batch full
    if (gl->quadCount > 0 && gl->currentTextureId != texId) {
        flushBatch(gl);
    }
    if (gl->quadCount >= MAX_QUADS) {
        flushBatch(gl);
    }
    gl->currentTextureId = texId;

    // Compute normalized UVs from TPAG source rect
    float u0 = (float) tpag->sourceX / (float) texW;
    float v0 = (float) tpag->sourceY / (float) texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / (float) texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / (float) texH;

    // Compute local quad corners (relative to origin, with target offset)
    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->sourceWidth;
    float localY1 = localY0 + (float) tpag->sourceHeight;

    // Build 2D transform: T(x,y) * R(-angleDeg) * S(xscale, yscale)
    // GML rotation is counter-clockwise, OpenGL rotation is counter-clockwise, but
    // since we have Y-down, we negate the angle to get the correct visual rotation
    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    // Transform 4 corners
    float x0, y0, x1, y1, x2, y2, x3, y3;
    Matrix4f_transformPoint(&transform, localX0, localY0, &x0, &y0); // top-left
    Matrix4f_transformPoint(&transform, localX1, localY0, &x1, &y1); // top-right
    Matrix4f_transformPoint(&transform, localX1, localY1, &x2, &y2); // bottom-right
    Matrix4f_transformPoint(&transform, localX0, localY1, &x3, &y3); // bottom-left

    // Convert BGR color to RGB floats
    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    // Write 4 vertices into batch buffer
    float* verts = gl->vertexData + gl->quadCount * VERTICES_PER_QUAD * FLOATS_PER_VERTEX;

    // Vertex 0: top-left
    verts[0] = x0; verts[1] = y0; verts[2] = u0; verts[3] = v0;
    verts[4] = r;  verts[5] = g;  verts[6] = b;  verts[7] = alpha;

    // Vertex 1: top-right
    verts[8]  = x1; verts[9]  = y1; verts[10] = u1; verts[11] = v0;
    verts[12] = r;  verts[13] = g;  verts[14] = b;  verts[15] = alpha;

    // Vertex 2: bottom-right
    verts[16] = x2; verts[17] = y2; verts[18] = u1; verts[19] = v1;
    verts[20] = r;  verts[21] = g;  verts[22] = b;  verts[23] = alpha;

    // Vertex 3: bottom-left
    verts[24] = x3; verts[25] = y3; verts[26] = u0; verts[27] = v1;
    verts[28] = r;  verts[29] = g;  verts[30] = b;  verts[31] = alpha;

    gl->quadCount++;
}

static void glDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return;

    GLuint texId = gl->glTextures[pageId];
    int32_t texW = gl->textureWidths[pageId];
    int32_t texH = gl->textureHeights[pageId];
    if (texW == 0 || texH == 0) return;

    // Flush if texture changed or batch full
    if (gl->quadCount > 0 && gl->currentTextureId != texId) flushBatch(gl);
    if (gl->quadCount >= MAX_QUADS) flushBatch(gl);
    gl->currentTextureId = texId;

    // Compute UVs for the sub-region within the atlas
    float u0 = (float) (tpag->sourceX + srcOffX) / (float) texW;
    float v0 = (float) (tpag->sourceY + srcOffY) / (float) texH;
    float u1 = (float) (tpag->sourceX + srcOffX + srcW) / (float) texW;
    float v1 = (float) (tpag->sourceY + srcOffY + srcH) / (float) texH;

    // Quad corners (no origin offset, no transform - draw_sprite_part ignores sprite origin)
    float x0 = x;
    float y0 = y;
    float x1 = x + (float) srcW * xscale;
    float y1 = y + (float) srcH * yscale;

    // Convert BGR color to RGB floats
    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    // Write 4 vertices into batch buffer
    float* verts = gl->vertexData + gl->quadCount * VERTICES_PER_QUAD * FLOATS_PER_VERTEX;

    // Vertex 0: top-left
    verts[0] = x0; verts[1] = y0; verts[2] = u0; verts[3] = v0;
    verts[4] = r;  verts[5] = g;  verts[6] = b;  verts[7] = alpha;

    // Vertex 1: top-right
    verts[8]  = x1; verts[9]  = y0; verts[10] = u1; verts[11] = v0;
    verts[12] = r;  verts[13] = g;  verts[14] = b;  verts[15] = alpha;

    // Vertex 2: bottom-right
    verts[16] = x1; verts[17] = y1; verts[18] = u1; verts[19] = v1;
    verts[20] = r;  verts[21] = g;  verts[22] = b;  verts[23] = alpha;

    // Vertex 3: bottom-left
    verts[24] = x0; verts[25] = y1; verts[26] = u0; verts[27] = v1;
    verts[28] = r;  verts[29] = g;  verts[30] = b;  verts[31] = alpha;

    gl->quadCount++;
}

// Emits a single colored quad into the batch using the white pixel texture
static void emitColoredQuad(GLRenderer* gl, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    if (gl->quadCount > 0 && gl->currentTextureId != gl->whiteTexture) {
        flushBatch(gl);
    }
    if (gl->quadCount >= MAX_QUADS) {
        flushBatch(gl);
    }
    gl->currentTextureId = gl->whiteTexture;

    float* verts = gl->vertexData + gl->quadCount * VERTICES_PER_QUAD * FLOATS_PER_VERTEX;

    // All UVs point to (0.5, 0.5) center of the 1x1 white texture
    // Vertex 0: top-left
    verts[0] = x0; verts[1] = y0; verts[2] = 0.5f; verts[3] = 0.5f;
    verts[4] = r;  verts[5] = g;  verts[6] = b;    verts[7] = a;

    // Vertex 1: top-right
    verts[8]  = x1; verts[9]  = y0; verts[10] = 0.5f; verts[11] = 0.5f;
    verts[12] = r;  verts[13] = g;  verts[14] = b;    verts[15] = a;

    // Vertex 2: bottom-right
    verts[16] = x1; verts[17] = y1; verts[18] = 0.5f; verts[19] = 0.5f;
    verts[20] = r;  verts[21] = g;  verts[22] = b;    verts[23] = a;

    // Vertex 3: bottom-left
    verts[24] = x0; verts[25] = y1; verts[26] = 0.5f; verts[27] = 0.5f;
    verts[28] = r;  verts[29] = g;  verts[30] = b;    verts[31] = a;

    gl->quadCount++;
}

static void glDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    GLRenderer* gl = (GLRenderer*) renderer;

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    if (outline) {
        // Draw 4 one-pixel-wide edges: top, bottom, left, right
        emitColoredQuad(gl, x1, y1, x2 + 1, y1 + 1, r, g, b, alpha); // top
        emitColoredQuad(gl, x1, y2, x2 + 1, y2 + 1, r, g, b, alpha); // bottom
        emitColoredQuad(gl, x1, y1 + 1, x1 + 1, y2, r, g, b, alpha); // left
        emitColoredQuad(gl, x2, y1 + 1, x2 + 1, y2, r, g, b, alpha); // right
    } else {
        // Filled rectangle: GML adds +1 to width/height for filled rects
        emitColoredQuad(gl, x1, y1, x2 + 1, y2 + 1, r, g, b, alpha);
    }
}

// ===[ Line Drawing ]===

static void glDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    // Compute perpendicular offset for line thickness
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    // Emit quad as 4 vertices forming a rectangle along the line
    if (gl->quadCount > 0 && gl->currentTextureId != gl->whiteTexture) {
        flushBatch(gl);
    }
    if (gl->quadCount >= MAX_QUADS) {
        flushBatch(gl);
    }
    gl->currentTextureId = gl->whiteTexture;

    float* verts = gl->vertexData + gl->quadCount * VERTICES_PER_QUAD * FLOATS_PER_VERTEX;

    // Vertex 0: start + perpendicular
    verts[0] = x1 + px; verts[1] = y1 + py; verts[2] = 0.5f; verts[3] = 0.5f;
    verts[4] = r; verts[5] = g; verts[6] = b; verts[7] = alpha;

    // Vertex 1: start - perpendicular
    verts[8] = x1 - px; verts[9] = y1 - py; verts[10] = 0.5f; verts[11] = 0.5f;
    verts[12] = r; verts[13] = g; verts[14] = b; verts[15] = alpha;

    // Vertex 2: end - perpendicular
    verts[16] = x2 - px; verts[17] = y2 - py; verts[18] = 0.5f; verts[19] = 0.5f;
    verts[20] = r; verts[21] = g; verts[22] = b; verts[23] = alpha;

    // Vertex 3: end + perpendicular
    verts[24] = x2 + px; verts[25] = y2 + py; verts[26] = 0.5f; verts[27] = 0.5f;
    verts[28] = r; verts[29] = g; verts[30] = b; verts[31] = alpha;

    gl->quadCount++;
}

static void glDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;

    float r1 = (float) BGR_R(color1) / 255.0f;
    float g1 = (float) BGR_G(color1) / 255.0f;
    float b1 = (float) BGR_B(color1) / 255.0f;

    float r2 = (float) BGR_R(color2) / 255.0f;
    float g2 = (float) BGR_G(color2) / 255.0f;
    float b2 = (float) BGR_B(color2) / 255.0f;

    // Compute perpendicular offset for line thickness
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    // Emit quad with per-vertex colors (color1 at start, color2 at end)
    if (gl->quadCount > 0 && gl->currentTextureId != gl->whiteTexture) {
        flushBatch(gl);
    }
    if (gl->quadCount >= MAX_QUADS) {
        flushBatch(gl);
    }
    gl->currentTextureId = gl->whiteTexture;

    float* verts = gl->vertexData + gl->quadCount * VERTICES_PER_QUAD * FLOATS_PER_VERTEX;

    // Vertex 0: start + perpendicular (color1)
    verts[0] = x1 + px; verts[1] = y1 + py; verts[2] = 0.5f; verts[3] = 0.5f;
    verts[4] = r1; verts[5] = g1; verts[6] = b1; verts[7] = alpha;

    // Vertex 1: start - perpendicular (color1)
    verts[8] = x1 - px; verts[9] = y1 - py; verts[10] = 0.5f; verts[11] = 0.5f;
    verts[12] = r1; verts[13] = g1; verts[14] = b1; verts[15] = alpha;

    // Vertex 2: end - perpendicular (color2)
    verts[16] = x2 - px; verts[17] = y2 - py; verts[18] = 0.5f; verts[19] = 0.5f;
    verts[20] = r2; verts[21] = g2; verts[22] = b2; verts[23] = alpha;

    // Vertex 3: end + perpendicular (color2)
    verts[24] = x2 + px; verts[25] = y2 + py; verts[26] = 0.5f; verts[27] = 0.5f;
    verts[28] = r2; verts[29] = g2; verts[30] = b2; verts[31] = alpha;

    gl->quadCount++;
}

// ===[ Text Drawing ]===

static void glDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg) {
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dw->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];

    // Resolve font texture page
    int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (0 > fontTpagIndex) return;

    TexturePageItem* fontTpag = &dw->tpag.items[fontTpagIndex];
    int16_t pageId = fontTpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return;

    GLuint texId = gl->glTextures[pageId];
    int32_t texW = gl->textureWidths[pageId];
    int32_t texH = gl->textureHeights[pageId];
    if (texW == 0 || texH == 0) return;

    uint32_t color = renderer->drawColor;
    float alpha = renderer->drawAlpha;
    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    // Preprocess: convert # to \n (and \# to literal #)
    char* processed = TextUtils_preprocessGmlText(text);
    int32_t textLen = (int32_t) strlen(processed);

    // Count lines, treating \r\n and \n\r as single breaks
    int32_t lineCount = TextUtils_countLines(processed, textLen);

    // Vertical alignment offset
    float totalHeight = (float) lineCount * (float) font->emSize;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    // Build transform matrix
    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    // Iterate through lines
    float cursorY = valignOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        // Horizontal alignment offset for this line
        float lineWidth = TextUtils_measureLineWidth(font, processed + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;

        // Render each glyph in the line
        int32_t pos = 0;
        while (lineLen > pos) {
            uint16_t ch = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == nullptr) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            // Flush if texture changed or batch full
            if (gl->quadCount > 0 && gl->currentTextureId != texId) flushBatch(gl);
            if (gl->quadCount >= MAX_QUADS) flushBatch(gl);
            gl->currentTextureId = texId;

            // Compute UVs from glyph position in the font's atlas
            float u0 = (float) (fontTpag->sourceX + glyph->sourceX) / (float) texW;
            float v0 = (float) (fontTpag->sourceY + glyph->sourceY) / (float) texH;
            float u1 = (float) (fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / (float) texW;
            float v1 = (float) (fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / (float) texH;

            // Local quad position (before transform)
            float localX0 = cursorX + glyph->offset;
            float localY0 = cursorY;
            float localX1 = localX0 + (float) glyph->sourceWidth;
            float localY1 = localY0 + (float) glyph->sourceHeight;

            // Transform corners
            float px0, py0, px1, py1, px2, py2, px3, py3;
            Matrix4f_transformPoint(&transform, localX0, localY0, &px0, &py0);
            Matrix4f_transformPoint(&transform, localX1, localY0, &px1, &py1);
            Matrix4f_transformPoint(&transform, localX1, localY1, &px2, &py2);
            Matrix4f_transformPoint(&transform, localX0, localY1, &px3, &py3);

            // Write 4 vertices
            float* verts = gl->vertexData + gl->quadCount * VERTICES_PER_QUAD * FLOATS_PER_VERTEX;

            verts[0] = px0; verts[1] = py0; verts[2] = u0; verts[3] = v0;
            verts[4] = r;   verts[5] = g;   verts[6] = b;  verts[7] = alpha;

            verts[8]  = px1; verts[9]  = py1; verts[10] = u1; verts[11] = v0;
            verts[12] = r;   verts[13] = g;   verts[14] = b;  verts[15] = alpha;

            verts[16] = px2; verts[17] = py2; verts[18] = u1; verts[19] = v1;
            verts[20] = r;   verts[21] = g;   verts[22] = b;  verts[23] = alpha;

            verts[24] = px3; verts[25] = py3; verts[26] = u0; verts[27] = v1;
            verts[28] = r;   verts[29] = g;   verts[30] = b;  verts[31] = alpha;

            gl->quadCount++;

            // Advance cursor by glyph shift + kerning
            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += (float) font->emSize;
        // Skip past the newline, treating \r\n and \n\r as single breaks
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }

    free(processed);
}

// ===[ Dynamic Sprite Creation/Deletion ]===

// Sentinel base for fake TPAG offsets used by dynamic sprites
#define DYNAMIC_TPAG_OFFSET_BASE 0xD0000000u

// Finds a free dynamic texture page slot (glTextures[i] == 0), or appends a new one.
static uint32_t findOrAllocTexturePageSlot(GLRenderer* gl) {
    // Scan dynamic range for a reusable slot
    for (uint32_t i = gl->originalTexturePageCount; gl->textureCount > i; i++) {
        if (gl->glTextures[i] == 0) return i;
    }
    // No free slot found, grow the arrays
    uint32_t newPageId = gl->textureCount;
    gl->textureCount++;
    gl->glTextures = safeRealloc(gl->glTextures, gl->textureCount * sizeof(GLuint));
    gl->textureWidths = safeRealloc(gl->textureWidths, gl->textureCount * sizeof(int32_t));
    gl->textureHeights = safeRealloc(gl->textureHeights, gl->textureCount * sizeof(int32_t));
    gl->glTextures[newPageId] = 0;
    gl->textureWidths[newPageId] = 0;
    gl->textureHeights[newPageId] = 0;
    return newPageId;
}

// Finds a free dynamic TPAG slot (texturePageId == -1), or appends a new one.
static uint32_t findOrAllocTpagSlot(DataWin* dw, uint32_t originalTpagCount) {
    for (uint32_t i = originalTpagCount; dw->tpag.count > i; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    uint32_t newIndex = dw->tpag.count;
    dw->tpag.count++;
    dw->tpag.items = safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;
    return newIndex;
}

// Finds a free dynamic Sprite slot (textureCount == 0), or appends a new one.
static uint32_t findOrAllocSpriteSlot(DataWin* dw, uint32_t originalSpriteCount) {
    for (uint32_t i = originalSpriteCount; dw->sprt.count > i; i++) {
        if (dw->sprt.sprites[i].textureCount == 0) return i;
    }
    uint32_t newIndex = dw->sprt.count;
    dw->sprt.count++;
    dw->sprt.sprites = safeRealloc(dw->sprt.sprites, dw->sprt.count * sizeof(Sprite));
    memset(&dw->sprt.sprites[newIndex], 0, sizeof(Sprite));
    return newIndex;
}

static int32_t glCreateSpriteFromSurface(Renderer* renderer, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 >= w || 0 >= h) return -1;

    // Flush any pending draws before reading pixels
    flushBatch(gl);

    // Read pixels from the FBO (application_surface)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->fbo);

    uint8_t* pixels = safeMalloc((size_t) w * (size_t) h * 4);
    if (pixels == nullptr) return -1;

    // OpenGL Y is bottom-up, GML Y is top-down, so flip the Y coordinate
    int32_t glY = gl->fboHeight - y - h;
    glReadPixels(x, glY, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // Flip vertically (OpenGL reads bottom-to-top)
    size_t rowBytes = (size_t) w * 4;
    uint8_t* rowTemp = safeMalloc(rowBytes);
    repeat(h / 2, row) {
        uint8_t* top = pixels + row * rowBytes;
        uint8_t* bot = pixels + (h - 1 - row) * rowBytes;
        memcpy(rowTemp, top, rowBytes);
        memcpy(top, bot, rowBytes);
        memcpy(bot, rowTemp, rowBytes);
    }
    free(rowTemp);

    // Create a new GL texture from the captured pixels
    GLuint newTexId;
    glGenTextures(1, &newTexId);
    glBindTexture(GL_TEXTURE_2D, newTexId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    free(pixels);

    // Find or allocate slots for texture page, TPAG, and sprite
    uint32_t pageId = findOrAllocTexturePageSlot(gl);
    gl->glTextures[pageId] = newTexId;
    gl->textureWidths[pageId] = w;
    gl->textureHeights[pageId] = h;

    uint32_t tpagIndex = findOrAllocTpagSlot(dw, gl->originalTpagCount);
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t) w;
    tpag->sourceHeight = (uint16_t) h;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t) w;
    tpag->targetHeight = (uint16_t) h;
    tpag->boundingWidth = (uint16_t) w;
    tpag->boundingHeight = (uint16_t) h;
    tpag->texturePageId = (int16_t) pageId;

    // Register a fake offset in the tpagOffsetMap so DataWin_resolveTPAG works
    uint32_t fakeOffset = DYNAMIC_TPAG_OFFSET_BASE + tpagIndex;
    hmput(dw->tpagOffsetMap, fakeOffset, (int32_t) tpagIndex);

    uint32_t spriteIndex = findOrAllocSpriteSlot(dw, gl->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    sprite->name = "dynamic_sprite";
    sprite->width = (uint32_t) w;
    sprite->height = (uint32_t) h;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->textureOffsets = safeMalloc(sizeof(uint32_t));
    sprite->textureOffsets[0] = fakeOffset;
    sprite->maskCount = 0;
    sprite->masks = nullptr;

    fprintf(stderr, "GL: Created dynamic sprite %u (%dx%d) from surface at (%d,%d)\n", spriteIndex, w, h, x, y);
    return (int32_t) spriteIndex;
}

static void glDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > spriteIndex || dw->sprt.count <= (uint32_t) spriteIndex) return;

    // Refuse to delete original data.win sprites
    if (gl->originalSpriteCount > (uint32_t) spriteIndex) {
        fprintf(stderr, "GL: Cannot delete data.win sprite %d\n", spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return; // already deleted

    // Clean up GL texture, TPAG entries, and tpagOffsetMap entries
    repeat(sprite->textureCount, i) {
        uint32_t offset = sprite->textureOffsets[i];
        if (offset >= DYNAMIC_TPAG_OFFSET_BASE) {
            int32_t tpagIdx = DataWin_resolveTPAG(dw, offset);
            if (tpagIdx >= 0) {
                TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
                int16_t pageId = tpag->texturePageId;
                if (pageId >= 0 && gl->textureCount > (uint32_t) pageId) {
                    glDeleteTextures(1, &gl->glTextures[pageId]);
                    gl->glTextures[pageId] = 0;
                }
                // Mark TPAG slot as free for reuse
                tpag->texturePageId = -1;
            }
            // Remove the fake offset from the lookup map
            hmdel(dw->tpagOffsetMap, offset);
        }
    }

    // Clear the sprite entry so it won't be drawn and can be reused
    free(sprite->textureOffsets);
    memset(sprite, 0, sizeof(Sprite));

    fprintf(stderr, "GL: Deleted sprite %d\n", spriteIndex);
}

// ===[ Vtable ]===

static RendererVtable glVtable = {
    .init = glInit,
    .destroy = glDestroy,
    .beginFrame = glBeginFrame,
    .endFrame = glEndFrame,
    .beginView = glBeginView,
    .endView = glEndView,
    .drawSprite = glDrawSprite,
    .drawSpritePart = glDrawSpritePart,
    .drawRectangle = glDrawRectangle,
    .drawLine = glDrawLine,
    .drawLineColor = glDrawLineColor,
    .drawText = glDrawText,
    .flush = glRendererFlush,
    .createSpriteFromSurface = glCreateSpriteFromSurface,
    .deleteSprite = glDeleteSprite,
    .drawTile = nullptr,
};

// ===[ Public API ]===

Renderer* GLRenderer_create(void) {
    GLRenderer* gl = safeCalloc(1, sizeof(GLRenderer));
    gl->base.vtable = &glVtable;
    gl->base.drawColor = 0xFFFFFF; // white (BGR)
    gl->base.drawAlpha = 1.0f;
    gl->base.drawFont = -1;
    gl->base.drawHalign = 0;
    gl->base.drawValign = 0;
    return (Renderer*) gl;
}
