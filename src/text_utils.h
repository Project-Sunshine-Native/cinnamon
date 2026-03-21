#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "data_win.h"
#include "utils.h"

// ===[ Text Utility Functions ]===
// Platform-agnostic text measurement and processing helpers.
// Used by both the renderer (for drawing text) and the VM (for string_width/string_height).

static inline FontGlyph* TextUtils_findGlyph(Font* font, uint16_t ch) {
    repeat(font->glyphCount, i) {
        if (font->glyphs[i].character == ch) return &font->glyphs[i];
    }
    return NULL;
}

static inline float TextUtils_getKerningOffset(FontGlyph* glyph, uint16_t nextCh) {
    repeat(glyph->kerningCount, k) {
        if (glyph->kerning[k].character == (int16_t) nextCh) {
            return glyph->kerning[k].shiftModifier;
        }
    }
    return 0;
}

// Decodes a single UTF-8 codepoint from str at *pos, advances *pos past the consumed bytes.
// Returns the codepoint as uint16_t (sufficient for BMP glyphs). Returns 0xFFFD for invalid sequences.
static inline uint16_t TextUtils_decodeUtf8(const char* str, int32_t len, int32_t* pos) {
    uint8_t b = (uint8_t) str[*pos];
    if (128 > b) {
        // ASCII (0xxxxxxx)
        (*pos)++;
        return b;
    } else if ((b & 0xE0) == 0xC0) {
        // 2-byte sequence (110xxxxx 10xxxxxx)
        if (len > *pos + 1 && ((uint8_t) str[*pos + 1] & 0xC0) == 0x80) {
            uint16_t cp = ((b & 0x1F) << 6) | ((uint8_t) str[*pos + 1] & 0x3F);
            *pos += 2;
            return cp;
        }
    } else if ((b & 0xF0) == 0xE0) {
        // 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx)
        if (len > *pos + 2 && ((uint8_t) str[*pos + 1] & 0xC0) == 0x80 && ((uint8_t) str[*pos + 2] & 0xC0) == 0x80) {
            uint16_t cp = ((b & 0x0F) << 12) | (((uint8_t) str[*pos + 1] & 0x3F) << 6) | ((uint8_t) str[*pos + 2] & 0x3F);
            *pos += 3;
            return cp;
        }
    } else if ((b & 0xF8) == 0xF0) {
        // 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx) - truncated to uint16_t
        if (len > *pos + 3 && ((uint8_t) str[*pos + 1] & 0xC0) == 0x80 && ((uint8_t) str[*pos + 2] & 0xC0) == 0x80 && ((uint8_t) str[*pos + 3] & 0xC0) == 0x80) {
            *pos += 4;
            return 0xFFFD; // Beyond BMP, return replacement character
        }
    }
    // Invalid or truncated sequence
    (*pos)++;
    return 0xFFFD;
}

static inline float TextUtils_measureLineWidth(Font* font, const char* line, int32_t len) {
    float width = 0;
    int32_t pos = 0;
    while (len > pos) {
        uint16_t ch = TextUtils_decodeUtf8(line, len, &pos);
        FontGlyph* glyph = TextUtils_findGlyph(font, ch);
        if (glyph == NULL) continue;

        width += glyph->shift;

        if (len > pos) {
            int32_t savedPos = pos;
            uint16_t nextCh = TextUtils_decodeUtf8(line, len, &pos);
            pos = savedPos;
            width += TextUtils_getKerningOffset(glyph, nextCh);
        }
    }
    return width;
}

// Preprocesses GML text: converts unescaped # to \n, and \# to literal #.
// Returns a heap-allocated string that must be freed by the caller.
static inline char* TextUtils_preprocessGmlText(const char* text) {
    int32_t len = (int32_t) strlen(text);
    char* result = safeMalloc(len + 1);
    int32_t out = 0;

    repeat(len, i) {
        if (text[i] == '#') {
            if (out > 0 && result[out - 1] == '\\') {
                // \# -> replace the already-written backslash with literal #
                result[out - 1] = '#';
            } else {
                result[out++] = '\n';
            }
        } else {
            result[out++] = text[i];
        }
    }
    result[out] = '\0';
    return result;
}

// Returns true if c is \r or \n
static inline bool TextUtils_isNewlineChar(char c) {
    return c == '\n' || c == '\r';
}

// Returns true if c is ' ' or \t
static inline bool TextUtils_isWhitespaceChar(char c) {
    return c == ' ' || c == '\t';
}

// Counts the number of lines in preprocessed text, treating \r\n and \n\r as single breaks
static inline int32_t TextUtils_countLines(const char* text, int32_t len) {
    int32_t count = 1;
    for (int32_t i = 0; len > i; i++) {
        if (TextUtils_isNewlineChar(text[i])) {
            count++;
            // Treat \r\n or \n\r as a single line break
            if (len > i + 1 && TextUtils_isNewlineChar(text[i + 1]) && text[i] != text[i + 1]) {
                i++;
            }
        }
    }
    return count;
}

// Advances lineStart past the newline at lineEnd, treating \r\n and \n\r as single breaks
static inline int32_t TextUtils_skipNewline(const char* text, int32_t lineEnd, int32_t textLen) {
    int32_t next = lineEnd + 1;
    if (textLen > next && TextUtils_isNewlineChar(text[next]) && text[lineEnd] != text[next]) {
        next++;
    }
    return next;
}

static char* TextUtils_trimTrailingWhitespace(char* str) {
    size_t len = strlen(str);
    while (len > 0 && (TextUtils_isWhitespaceChar(str[len - 1]) || TextUtils_isNewlineChar(str[len - 1]))) {
        len--;
    }
    str[len] = '\0';
    return str;
}