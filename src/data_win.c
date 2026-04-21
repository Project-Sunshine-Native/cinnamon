#include "data_win.h"
#include "binary_reader.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_ds.h"
#include "utils.h"

#ifdef __3DS__
#include <3ds/os.h>
#endif

// ===[ HELPERS ]===

// Reads a uint32 absolute file offset, resolves it into the pre-loaded STRG buffer,
// and returns a pointer to the null-terminated string content at that offset.
static const char* readStringPtr(BinaryReader* reader, DataWin* dw) {
    uint32_t offset = BinaryReader_readUint32(reader);
    if (offset == 0) return nullptr;
    return (const char*) (dw->strgBuffer + (offset - dw->strgBufferBase));
}

// Reads a pointer list header: count + absolute-offset pointers.
// Caller must free the returned array.
static uint32_t* readPointerTable(BinaryReader* reader, uint32_t* outCount) {
    *outCount = BinaryReader_readUint32(reader);
    if (*outCount == 0) return nullptr;
    uint32_t* ptrs = safeMalloc(*outCount * sizeof(uint32_t));
    repeat(*outCount, i) {
        ptrs[i] = BinaryReader_readUint32(reader);
    }
    return ptrs;
}

// Reads a PointerList of EventAction entries. Used by TMLN and OBJT.
static EventAction* readEventActions(BinaryReader* reader, DataWin* dw, uint32_t* outCount) {
    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    *outCount = count;
    if (count == 0) { free(ptrs); return nullptr; }

    EventAction* actions = safeMalloc(count * sizeof(EventAction));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        actions[i].libID = BinaryReader_readUint32(reader);
        actions[i].id = BinaryReader_readUint32(reader);
        actions[i].kind = BinaryReader_readUint32(reader);
        actions[i].useRelative = BinaryReader_readBool32(reader);
        actions[i].isQuestion = BinaryReader_readBool32(reader);
        actions[i].useApplyTo = BinaryReader_readBool32(reader);
        actions[i].exeType = BinaryReader_readUint32(reader);
        actions[i].actionName = readStringPtr(reader, dw);
        actions[i].codeId = BinaryReader_readInt32(reader);
        actions[i].argumentCount = BinaryReader_readUint32(reader);
        actions[i].who = BinaryReader_readInt32(reader);
        actions[i].relative = BinaryReader_readBool32(reader);
        actions[i].isNot = BinaryReader_readBool32(reader);
        actions[i].unknownAlwaysZero = BinaryReader_readUint32(reader);
    }
    free(ptrs);
    return actions;
}

// ===[ PATH INTERNAL COMPUTATION ]===
// Matches HTML5 yyPath.js algorithm exactly.

// Dynamic array of InternalPathPoints for building during computation
static InternalPathPoint* tempIntPoints = nullptr;
static uint32_t tempIntPointCount = 0;

static void addInternalPoint(double x, double y, double speed) {
    InternalPathPoint pt = { .x = x, .y = y, .speed = speed, .l = 0.0 };
    arrput(tempIntPoints, pt);
    tempIntPointCount++;
}

// Recursive midpoint subdivision for smooth curves (yyPath.js:225-242)
static void handlePiece(int depth, double x1, double y1, double s1, double x2, double y2, double s2, double x3, double y3, double s3) {
    if (depth == 0) return;

    double mx = (x1 + x2 + x2 + x3) / 4.0;
    double my = (y1 + y2 + y2 + y3) / 4.0;
    double ms = (s1 + s2 + s2 + s3) / 4.0;

    if ((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1) > 16.0) {
        handlePiece(depth - 1, x1, y1, s1, (x2 + x1) / 2.0, (y2 + y1) / 2.0, (s2 + s1) / 2.0, mx, my, ms);
    }

    addInternalPoint(mx, my, ms);

    if ((x2 - x3) * (x2 - x3) + (y2 - y3) * (y2 - y3) > 16.0) {
        handlePiece(depth - 1, mx, my, ms, (x3 + x2) / 2.0, (y3 + y2) / 2.0, (s3 + s2) / 2.0, x3, y3, s3);
    }
}

void GamePath_computeInternal(GamePath* path) {
    // Reset temp state
    arrfree(tempIntPoints);
    tempIntPoints = nullptr;
    tempIntPointCount = 0;

    if (path->pointCount == 0) {
        path->internalPointCount = 0;
        path->internalPoints = nullptr;
        path->length = 0.0;
        return;
    }

    if (path->isSmooth) {
        // ComputeCurved (yyPath.js:254-292)
        if (!path->isClosed) {
            addInternalPoint(path->points[0].x, path->points[0].y, path->points[0].speed);
        }

        int n;
        if (path->isClosed) {
            n = (int) path->pointCount - 1;
        } else {
            n = (int) path->pointCount - 3;
        }

        repeat(n + 1, i) {
            PathPoint* p1 = &path->points[i % path->pointCount];
            PathPoint* p2 = &path->points[(i + 1) % path->pointCount];
            PathPoint* p3 = &path->points[(i + 2) % path->pointCount];
            handlePiece((int) path->precision,
                        (p1->x + p2->x) / 2.0, (p1->y + p2->y) / 2.0, (p1->speed + p2->speed) / 2.0,
                        p2->x, p2->y, p2->speed,
                        (p2->x + p3->x) / 2.0, (p2->y + p3->y) / 2.0, (p2->speed + p3->speed) / 2.0);
        }

        if (!path->isClosed) {
            PathPoint* last = &path->points[path->pointCount - 1];
            addInternalPoint(last->x, last->y, last->speed);
        } else {
            // Closed smooth: append the first internal point again
            addInternalPoint(tempIntPoints[0].x, tempIntPoints[0].y, tempIntPoints[0].speed);
        }
    } else {
        // ComputeLinear (yyPath.js:192-204)
        repeat(path->pointCount, i) {
            addInternalPoint(path->points[i].x, path->points[i].y, path->points[i].speed);
        }
        if (path->isClosed) {
            addInternalPoint(path->points[0].x, path->points[0].y, path->points[0].speed);
        }
    }

    // ComputeLength (yyPath.js:150-160)
    path->internalPointCount = tempIntPointCount;
    path->internalPoints = safeMalloc(tempIntPointCount * sizeof(InternalPathPoint));
    memcpy(path->internalPoints, tempIntPoints, tempIntPointCount * sizeof(InternalPathPoint));
    arrfree(tempIntPoints);
    tempIntPoints = nullptr;
    tempIntPointCount = 0;

    path->length = 0.0;
    if (path->internalPointCount > 0) {
        path->internalPoints[0].l = 0.0;
        repeat(path->internalPointCount - 1, j) {
            uint32_t i = j + 1;
            double dx = path->internalPoints[i].x - path->internalPoints[i - 1].x;
            double dy = path->internalPoints[i].y - path->internalPoints[i - 1].y;
            path->length += sqrt(dx * dx + dy * dy);
            path->internalPoints[i].l = path->length;
        }
    }
}

// Get interpolated position at t in [0,1] (yyPath.js:362-409)
PathPositionResult GamePath_getPosition(GamePath* path, double t) {
    PathPositionResult result = { .x = 0.0, .y = 0.0, .speed = 0.0 };

    if (path->internalPointCount == 0) return result;

    if (path->internalPointCount == 1 || path->length == 0.0 || 0.0 >= t) {
        result.x = path->internalPoints[0].x;
        result.y = path->internalPoints[0].y;
        result.speed = path->internalPoints[0].speed;
        return result;
    }

    if (t >= 1.0) {
        InternalPathPoint* last = &path->internalPoints[path->internalPointCount - 1];
        result.x = last->x;
        result.y = last->y;
        result.speed = last->speed;
        return result;
    }

    // Get the right interval via linear scan
    double l = path->length * t;
    uint32_t pos = 0;
    while (path->internalPointCount - 2 > pos && l >= path->internalPoints[pos + 1].l) {
        pos++;
    }

    InternalPathPoint* node = &path->internalPoints[pos];
    double lRem = l - node->l;
    double w = path->internalPoints[pos + 1].l - node->l;

    if (w != 0.0) {
        InternalPathPoint* next = &path->internalPoints[pos + 1];
        result.x = node->x + lRem * (next->x - node->x) / w;
        result.y = node->y + lRem * (next->y - node->y) / w;
        result.speed = node->speed + lRem * (next->speed - node->speed) / w;
    } else {
        result.x = node->x;
        result.y = node->y;
        result.speed = node->speed;
    }

    return result;
}

// ===[ CHUNK PARSERS ]===

static void parseGEN8(BinaryReader* reader, DataWin* dw) {
    Gen8* g = &dw->gen8;
    g->isDebuggerDisabled = BinaryReader_readUint8(reader);
    g->bytecodeVersion = BinaryReader_readUint8(reader);
    BinaryReader_skip(reader, 2); // padding
    g->fileName = readStringPtr(reader, dw);
    g->config = readStringPtr(reader, dw);
    g->lastObj = BinaryReader_readUint32(reader);
    g->lastTile = BinaryReader_readUint32(reader);
    g->gameID = BinaryReader_readUint32(reader);
    BinaryReader_readBytes(reader, g->directPlayGuid, 16);
    g->name = readStringPtr(reader, dw);
    g->major = BinaryReader_readUint32(reader);
    g->minor = BinaryReader_readUint32(reader);
    g->release = BinaryReader_readUint32(reader);
    g->build = BinaryReader_readUint32(reader);
    g->defaultWindowWidth = BinaryReader_readUint32(reader);
    g->defaultWindowHeight = BinaryReader_readUint32(reader);
    g->info = BinaryReader_readUint32(reader);
    g->licenseCRC32 = BinaryReader_readUint32(reader);
    BinaryReader_readBytes(reader, g->licenseMD5, 16);
    g->timestamp = BinaryReader_readUint64(reader);
    g->displayName = readStringPtr(reader, dw);
    g->activeTargets = BinaryReader_readUint64(reader);
    g->functionClassifications = BinaryReader_readUint64(reader);
    g->steamAppID = BinaryReader_readInt32(reader);
    if (g->bytecodeVersion >= 14) {
        g->debuggerPort = BinaryReader_readUint32(reader);
    }

    // Room order SimpleList
    g->roomOrderCount = BinaryReader_readUint32(reader);
    if (g->roomOrderCount > 0) {
        g->roomOrder = safeMalloc(g->roomOrderCount * sizeof(int32_t));
        repeat(g->roomOrderCount, i) {
            g->roomOrder[i] = BinaryReader_readInt32(reader);
        }
    } else {
        g->roomOrder = nullptr;
    }

    if (g->major >= 2) {
        BinaryReader_skip(reader, 8); // firstRandom (int64)
        BinaryReader_skip(reader, 8*4); // 4 Random Entries (one int64 or two int32)

        g->gms2FPS = BinaryReader_readFloat32(reader);
        BinaryReader_skip(reader, 4); // AllowStatistics (bool32)
        BinaryReader_skip(reader, 16); // GameGUID (16 Bytes, unknown it's use)
    }

    // Seed the detected version from GEN8.
    // Later chunk parsers may bump these upward when they identify newer-format features, because since GM:S 2 the value in the GEN8 chunk is not accurate.
    dw->detectedFormat.major = g->major;
    dw->detectedFormat.minor = g->minor;
    dw->detectedFormat.release = g->release;
    dw->detectedFormat.build = g->build;
}

static void parseOPTN(BinaryReader* reader, DataWin* dw) {
    Optn* o = &dw->optn;

    int32_t marker = BinaryReader_readInt32(reader);
    if (marker != (int32_t)0x80000000) {
        fprintf(stderr, "OPTN: expected new format marker 0x80000000, got 0x%08X\n", (uint32_t)marker);
        exit(1);
    }

    int32_t shaderExtVersion = BinaryReader_readInt32(reader);
    (void)shaderExtVersion; // always 2

    o->info = BinaryReader_readUint64(reader);
    o->scale = BinaryReader_readInt32(reader);
    o->windowColor = BinaryReader_readUint32(reader);
    o->colorDepth = BinaryReader_readUint32(reader);
    o->resolution = BinaryReader_readUint32(reader);
    o->frequency = BinaryReader_readUint32(reader);
    o->vertexSync = BinaryReader_readUint32(reader);
    o->priority = BinaryReader_readUint32(reader);
    o->backImage = BinaryReader_readUint32(reader);
    o->frontImage = BinaryReader_readUint32(reader);
    o->loadImage = BinaryReader_readUint32(reader);
    o->loadAlpha = BinaryReader_readUint32(reader);

    // Constants SimpleList
    o->constantCount = BinaryReader_readUint32(reader);
    if (o->constantCount > 0) {
        o->constants = safeMalloc(o->constantCount * sizeof(OptnConstant));
        repeat(o->constantCount, i) {
            o->constants[i].name = readStringPtr(reader, dw);
            o->constants[i].value = readStringPtr(reader, dw);
        }
    } else {
        o->constants = nullptr;
    }
}

static void parseLANG(BinaryReader* reader, DataWin* dw) {
    Lang* l = &dw->lang;
    l->unknown1 = BinaryReader_readUint32(reader);
    l->languageCount = BinaryReader_readUint32(reader);
    l->entryCount = BinaryReader_readUint32(reader);

    // Entry IDs
    if (l->entryCount > 0) {
        l->entryIds = safeMalloc(l->entryCount * sizeof(const char*));
        repeat(l->entryCount, i) {
            l->entryIds[i] = readStringPtr(reader, dw);
        }
    } else {
        l->entryIds = nullptr;
    }

    // Languages
    if (l->languageCount > 0) {
        l->languages = safeMalloc(l->languageCount * sizeof(Language));
        repeat(l->languageCount, i) {
            l->languages[i].name = readStringPtr(reader, dw);
            l->languages[i].region = readStringPtr(reader, dw);
            l->languages[i].entryCount = l->entryCount;
            if (l->entryCount > 0) {
                l->languages[i].entries = safeMalloc(l->entryCount * sizeof(const char*));
                repeat(l->entryCount, j) {
                    l->languages[i].entries[j] = readStringPtr(reader, dw);
                }
            } else {
                l->languages[i].entries = nullptr;
            }
        }
    } else {
        l->languages = nullptr;
    }
}

static void parseEXTN(BinaryReader* reader, DataWin* dw) {
    Extn* e = &dw->extn;

    uint32_t extCount;
    uint32_t* extPtrs = readPointerTable(reader, &extCount);
    e->count = extCount;

    if (extCount == 0) { free(extPtrs); e->extensions = nullptr; return; }

    e->extensions = safeMalloc(extCount * sizeof(Extension));
    repeat(extCount, i) {
        BinaryReader_seek(reader, extPtrs[i]);
        Extension* ext = &e->extensions[i];
        ext->folderName = readStringPtr(reader, dw);
        ext->name = readStringPtr(reader, dw);
        ext->className = readStringPtr(reader, dw);

        // Files PointerList
        uint32_t fileCount;
        uint32_t* filePtrs = readPointerTable(reader, &fileCount);
        ext->fileCount = fileCount;

        if (fileCount > 0) {
            ext->files = safeMalloc(fileCount * sizeof(ExtensionFile));
            repeat(fileCount, j) {
                BinaryReader_seek(reader, filePtrs[j]);
                ExtensionFile* file = &ext->files[j];
                file->filename = readStringPtr(reader, dw);
                file->cleanupScript = readStringPtr(reader, dw);
                file->initScript = readStringPtr(reader, dw);
                file->kind = BinaryReader_readUint32(reader);

                // Functions PointerList
                uint32_t funcCount;
                uint32_t* funcPtrs = readPointerTable(reader, &funcCount);
                file->functionCount = funcCount;

                if (funcCount > 0) {
                    file->functions = safeMalloc(funcCount * sizeof(ExtensionFunction));
                    repeat(funcCount, k) {
                        BinaryReader_seek(reader, funcPtrs[k]);
                        ExtensionFunction* func = &file->functions[k];
                        func->name = readStringPtr(reader, dw);
                        func->id = BinaryReader_readUint32(reader);
                        func->kind = BinaryReader_readUint32(reader);
                        func->retType = BinaryReader_readUint32(reader);
                        func->extName = readStringPtr(reader, dw);

                        // Arguments SimpleList
                        func->argumentCount = BinaryReader_readUint32(reader);
                        if (func->argumentCount > 0) {
                            func->arguments = safeMalloc(func->argumentCount * sizeof(uint32_t));
                            repeat(func->argumentCount, a) {
                                func->arguments[a] = BinaryReader_readUint32(reader);
                            }
                        } else {
                            func->arguments = nullptr;
                        }
                    }
                } else {
                    file->functions = nullptr;
                }
                free(funcPtrs);
            }
        } else {
            ext->files = nullptr;
        }
        free(filePtrs);
    }
    free(extPtrs);

    // Product ID data (16 bytes per extension, bytecodeVersion >= 14)
    // Skipped -- we seek to chunkEnd after parsing
}

static void parseSOND(BinaryReader* reader, DataWin* dw) {
    Sond* s = &dw->sond;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    s->count = count;

    if (count == 0) { free(ptrs); s->sounds = nullptr; return; }

    s->sounds = safeMalloc(count * sizeof(Sound));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        Sound* snd = &s->sounds[i];
        snd->name = readStringPtr(reader, dw);
        snd->flags = BinaryReader_readUint32(reader);
        snd->type = readStringPtr(reader, dw);
        snd->file = readStringPtr(reader, dw);
        snd->effects = BinaryReader_readUint32(reader);
        snd->volume = BinaryReader_readFloat32(reader);
        snd->pitch = BinaryReader_readFloat32(reader);

        // AudioGroup or preload field at offset +28
        // For GMS 1.4.x (bytecodeVersion >= 14) with Regular flag: resource_id
        if ((snd->flags & 0x64) == 0x64) {
            snd->audioGroup = BinaryReader_readInt32(reader);
        } else {
            int32_t preload = BinaryReader_readInt32(reader);
            (void)preload;
            snd->audioGroup = 0; // default audio group
        }

        snd->audioFile = BinaryReader_readInt32(reader);
    }
    free(ptrs);
}

static void parseAGRP(BinaryReader* reader, DataWin* dw) {
    Agrp* a = &dw->agrp;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    a->count = count;

    if (count == 0) { free(ptrs); a->audioGroups = nullptr; return; }

    a->audioGroups = safeMalloc(count * sizeof(AudioGroup));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        a->audioGroups[i].name = readStringPtr(reader, dw);
    }
    free(ptrs);
}

static void parseSPRT(BinaryReader* reader, DataWin* dw, bool skipLoadingPreciseMasksForNonPreciseSprites) {
    Sprt* s = &dw->sprt;
    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    s->count = count;

    if (count == 0) { free(ptrs); s->sprites = nullptr; return; }

    s->sprites = safeMalloc(count * sizeof(Sprite));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        Sprite* spr = &s->sprites[i];
        spr->name = readStringPtr(reader, dw);
        spr->width = BinaryReader_readUint32(reader);
        spr->height = BinaryReader_readUint32(reader);
        spr->marginLeft = BinaryReader_readInt32(reader);
        spr->marginRight = BinaryReader_readInt32(reader);
        spr->marginBottom = BinaryReader_readInt32(reader);
        spr->marginTop = BinaryReader_readInt32(reader);
        spr->transparent = BinaryReader_readBool32(reader);
        spr->smooth = BinaryReader_readBool32(reader);
        spr->preload = BinaryReader_readBool32(reader);
        spr->bboxMode = BinaryReader_readUint32(reader);
        spr->sepMasks = BinaryReader_readUint32(reader);
        spr->originX = BinaryReader_readInt32(reader);
        spr->originY = BinaryReader_readInt32(reader);

        // Detect special type vs normal: peek next int32
        int32_t check = BinaryReader_readInt32(reader);
        if (check == -1) {
            spr->specialType = true;
            spr->sVersion = BinaryReader_readUint32(reader);
            spr->sSpriteType = BinaryReader_readUint32(reader);
            if (DataWin_isVersionAtLeast(dw, 2, 0, 0, 0)) {
                spr->gms2PlaybackSpeed = BinaryReader_readFloat32(reader);
                spr->gms2PlaybackSpeedType = BinaryReader_readUint32(reader);
                if (spr->sVersion >= 2) {
                    BinaryReader_skip(reader, 4); //sequenceOffset;
                    if (spr->sVersion >= 3) {
                       BinaryReader_skip(reader, 4); // nineSliceOffset;
                    }
                }
                check = BinaryReader_readUint32(reader);
            }
        }

        // 'check' is the texture count (start of SimpleList)
        spr->textureCount = (uint32_t)check;
        if (spr->textureCount > 0) {
            spr->textureOffsets = safeMalloc(spr->textureCount * sizeof(uint32_t));
            repeat(spr->textureCount, j) {
                spr->textureOffsets[j] = BinaryReader_readUint32(reader);
            }
        } else {
            spr->textureOffsets = nullptr;
        }

        // Collision mask data
        // sepMasks: 0 = axis-aligned rect (no mask data stored in some cases)
        //           1 = precise per-frame masks
        //           2 = rotated rect (no mask data)
        // Mask format: each bit = 1 pixel, MSB first, row-major
        // Width in bytes = (spriteWidth + 7) / 8, total = widthInBytes * spriteHeight
        // After all masks, data is padded to 4-byte alignment
        uint32_t maskDataCount = BinaryReader_readUint32(reader);
        spr->maskCount = maskDataCount;
        if (maskDataCount > 0 && spr->width > 0 && spr->height > 0) {
            uint32_t bytesPerRow = (spr->width + 7) / 8;
            uint32_t bytesPerMask = bytesPerRow * spr->height;

            if (spr->sepMasks == 1 || !skipLoadingPreciseMasksForNonPreciseSprites) {
                spr->masks = safeMalloc(maskDataCount * sizeof(uint8_t*));
                repeat(maskDataCount, j) {
                    spr->masks[j] = safeMalloc(bytesPerMask);
                    BinaryReader_readBytes(reader, spr->masks[j], bytesPerMask);
                }
            } else {
                BinaryReader_skip(reader, bytesPerMask * maskDataCount);
                spr->masks = nullptr;
            }
            // Pad the TOTAL mask data to 4-byte alignment (not per-mask)
            uint32_t totalMaskBytes = bytesPerMask * maskDataCount;
            uint32_t remainder = totalMaskBytes % 4;
            if (remainder != 0) {
                BinaryReader_skip(reader, 4 - remainder);
            }
        } else {
            spr->masks = nullptr;
        }
    }
    
    // Build sprtOffsetMap: absolute file offset -> SPRT index
    // TODO: This is only needed for GMS2
    repeat(count, i) {
        hmput(dw->sprtOffsetMap, ptrs[i], (int32_t) i);
    }
    free(ptrs);
}

static void parseBGND(BinaryReader* reader, DataWin* dw) {
    Bgnd* b = &dw->bgnd;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    b->count = count;

    if (count == 0) { free(ptrs); b->backgrounds = nullptr; return; }

    b->backgrounds = safeCalloc(count, sizeof(Background));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        Background* bg = &b->backgrounds[i];
        bg->name = readStringPtr(reader, dw);
        bg->transparent = BinaryReader_readBool32(reader);
        bg->smooth = BinaryReader_readBool32(reader);
        bg->preload = BinaryReader_readBool32(reader);
        bg->textureOffset = BinaryReader_readUint32(reader);
        if (DataWin_isVersionAtLeast(dw, 2, 0, 0, 0)) {
            bg->gms2UnknownAlways2 = BinaryReader_readUint32(reader);
            bg->gms2TileWidth = BinaryReader_readUint32(reader);
            bg->gms2TileHeight = BinaryReader_readUint32(reader);
            if (DataWin_isVersionAtLeast(dw, 2024, 14, 0, 1)) {
                bg->gms2TileSeparationX = BinaryReader_readUint32(reader);
                bg->gms2TileSeparationY = BinaryReader_readUint32(reader);
            }
            bg->gms2OutputBorderX = BinaryReader_readUint32(reader);
            bg->gms2OutputBorderY = BinaryReader_readUint32(reader);
            bg->gms2TileColumns = BinaryReader_readUint32(reader);
            bg->gms2ItemsPerTileCount = BinaryReader_readUint32(reader);
            bg->gms2TileCount = BinaryReader_readUint32(reader);
            bg->gms2ExportedSpriteIndex = BinaryReader_readInt32(reader);
            bg->gms2FrameLength = BinaryReader_readInt64(reader);
            int tileIdCount = bg->gms2TileCount * bg->gms2ItemsPerTileCount;
            bg->gms2TileIds = malloc(tileIdCount*sizeof(uint32_t));
            repeat(tileIdCount, j) {
                bg->gms2TileIds[j] = BinaryReader_readUint32(reader);
            }
        }
    }
    free(ptrs);
}

static void parsePATH(BinaryReader* reader, DataWin* dw) {
    PathChunk* p = &dw->path;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    p->count = count;

    if (count == 0) { free(ptrs); p->paths = nullptr; return; }

    p->paths = safeMalloc(count * sizeof(GamePath));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        GamePath* path = &p->paths[i];
        path->name = readStringPtr(reader, dw);
        path->isSmooth = BinaryReader_readBool32(reader);
        path->isClosed = BinaryReader_readBool32(reader);
        path->precision = BinaryReader_readUint32(reader);

        // Points SimpleList
        path->pointCount = BinaryReader_readUint32(reader);
        if (path->pointCount > 0) {
            path->points = safeMalloc(path->pointCount * sizeof(PathPoint));
            repeat(path->pointCount, j) {
                path->points[j].x = BinaryReader_readFloat32(reader);
                path->points[j].y = BinaryReader_readFloat32(reader);
                path->points[j].speed = BinaryReader_readFloat32(reader);
            }
        } else {
            path->points = nullptr;
        }

        // Precompute internal representation for path following
        GamePath_computeInternal(path);
    }
    free(ptrs);
}

static void parseSCPT(BinaryReader* reader, DataWin* dw) {
    Scpt* s = &dw->scpt;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    s->count = count;

    if (count == 0) { free(ptrs); s->scripts = nullptr; return; }

    s->scripts = safeMalloc(count * sizeof(Script));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        s->scripts[i].name = readStringPtr(reader, dw);
        s->scripts[i].codeId = BinaryReader_readInt32(reader);
    }
    free(ptrs);
}

static void parseGLOB(BinaryReader* reader, DataWin* dw) {
    Glob* g = &dw->glob;

    g->count = BinaryReader_readUint32(reader);
    if (g->count > 0) {
        g->codeIds = safeMalloc(g->count * sizeof(int32_t));
        repeat(g->count, i) {
            g->codeIds[i] = BinaryReader_readInt32(reader);
        }
    } else {
        g->codeIds = nullptr;
    }
}

static void parseSHDR(BinaryReader* reader, DataWin* dw) {
    Shdr* s = &dw->shdr;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    s->count = count;

    if (count == 0) { free(ptrs); s->shaders = nullptr; return; }

    s->shaders = safeMalloc(count * sizeof(Shader));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        Shader* sh = &s->shaders[i];
        sh->name = readStringPtr(reader, dw);
        sh->type = BinaryReader_readUint32(reader) & 0x7FFFFFFF;
        sh->glslES_Vertex = readStringPtr(reader, dw);
        sh->glslES_Fragment = readStringPtr(reader, dw);
        sh->glsl_Vertex = readStringPtr(reader, dw);
        sh->glsl_Fragment = readStringPtr(reader, dw);
        sh->hlsl9_Vertex = readStringPtr(reader, dw);
        sh->hlsl9_Fragment = readStringPtr(reader, dw);
        sh->hlsl11_VertexOffset = BinaryReader_readUint32(reader);
        sh->hlsl11_PixelOffset = BinaryReader_readUint32(reader);

        // Vertex attributes SimpleList
        sh->vertexAttributeCount = BinaryReader_readUint32(reader);
        if (sh->vertexAttributeCount > 0) {
            sh->vertexAttributes = safeMalloc(sh->vertexAttributeCount * sizeof(const char*));
            repeat(sh->vertexAttributeCount, j) {
                sh->vertexAttributes[j] = readStringPtr(reader, dw);
            }
        } else {
            sh->vertexAttributes = nullptr;
        }

        // Version field (bytecodeVersion > 13)
        sh->version = BinaryReader_readInt32(reader);

        sh->pssl_VertexOffset = BinaryReader_readUint32(reader);
        sh->pssl_VertexLen = BinaryReader_readUint32(reader);
        sh->pssl_PixelOffset = BinaryReader_readUint32(reader);
        sh->pssl_PixelLen = BinaryReader_readUint32(reader);
        sh->cgVita_VertexOffset = BinaryReader_readUint32(reader);
        sh->cgVita_VertexLen = BinaryReader_readUint32(reader);
        sh->cgVita_PixelOffset = BinaryReader_readUint32(reader);
        sh->cgVita_PixelLen = BinaryReader_readUint32(reader);

        if (sh->version >= 2) {
            sh->cgPS3_VertexOffset = BinaryReader_readUint32(reader);
            sh->cgPS3_VertexLen = BinaryReader_readUint32(reader);
            sh->cgPS3_PixelOffset = BinaryReader_readUint32(reader);
            sh->cgPS3_PixelLen = BinaryReader_readUint32(reader);
        } else {
            sh->cgPS3_VertexOffset = 0;
            sh->cgPS3_VertexLen = 0;
            sh->cgPS3_PixelOffset = 0;
            sh->cgPS3_PixelLen = 0;
        }

        // Blob data follows but we skip it (pointer list seeking handles position)
    }
    free(ptrs);
}

static void parseFONT(BinaryReader* reader, DataWin* dw) {
    FontChunk* f = &dw->font;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    f->count = count;

    if (count == 0) { free(ptrs); f->fonts = nullptr; return; }

    f->fonts = safeMalloc(count * sizeof(Font));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        Font* font = &f->fonts[i];
        font->name = readStringPtr(reader, dw);
        font->displayName = readStringPtr(reader, dw);
        font->emSize = BinaryReader_readUint32(reader);
        font->bold = BinaryReader_readBool32(reader);
        font->italic = BinaryReader_readBool32(reader);
        font->rangeStart = BinaryReader_readUint16(reader);
        font->charset = BinaryReader_readUint8(reader);
        font->antiAliasing = BinaryReader_readUint8(reader);
        font->rangeEnd = BinaryReader_readUint32(reader);
        font->textureOffset = BinaryReader_readUint32(reader);
        font->scaleX = BinaryReader_readFloat32(reader);
        font->scaleY = BinaryReader_readFloat32(reader);
        font->ascenderOffset = 0; // default for BC < 17
        if (dw->gen8.bytecodeVersion >= 17) {
            font->ascenderOffset = BinaryReader_readInt32(reader);
        }
        font->isSpriteFont = false;
        font->spriteIndex = -1;

        // Glyphs PointerList
        uint32_t glyphCount;
        uint32_t* glyphPtrs = readPointerTable(reader, &glyphCount);
        font->glyphCount = glyphCount;

        uint32_t maxGlyphHeight = 0;
        if (glyphCount > 0) {
            font->glyphs = safeMalloc(glyphCount * sizeof(FontGlyph));
            repeat(glyphCount, j) {
                BinaryReader_seek(reader, glyphPtrs[j]);
                FontGlyph* glyph = &font->glyphs[j];
                glyph->character = BinaryReader_readUint16(reader);
                glyph->sourceX = BinaryReader_readUint16(reader);
                glyph->sourceY = BinaryReader_readUint16(reader);
                glyph->sourceWidth = BinaryReader_readUint16(reader);
                glyph->sourceHeight = BinaryReader_readUint16(reader);
                glyph->shift = BinaryReader_readInt16(reader);
                glyph->offset = BinaryReader_readInt16(reader);

                if (glyph->sourceHeight > maxGlyphHeight) maxGlyphHeight = glyph->sourceHeight;

                // Kerning SimpleListShort (uint16 count)
                glyph->kerningCount = BinaryReader_readUint16(reader);
                if (glyph->kerningCount > 0) {
                    glyph->kerning = safeMalloc(glyph->kerningCount * sizeof(KerningPair));
                    for (uint16_t k = 0; glyph->kerningCount > k; k++) {
                        glyph->kerning[k].character = BinaryReader_readInt16(reader);
                        glyph->kerning[k].shiftModifier = BinaryReader_readInt16(reader);
                    }
                } else {
                    glyph->kerning = nullptr;
                }
            }
        } else {
            font->glyphs = nullptr;
        }
        font->maxGlyphHeight = maxGlyphHeight;
        free(glyphPtrs);
    }
    free(ptrs);

    // 512 bytes of trailing padding -- skipped by chunkEnd seek
}

static void parseTMLN(BinaryReader* reader, DataWin* dw) {
    Tmln* t = &dw->tmln;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    t->count = count;

    if (count == 0) { free(ptrs); t->timelines = nullptr; return; }

    t->timelines = safeMalloc(count * sizeof(Timeline));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        Timeline* tl = &t->timelines[i];
        tl->name = readStringPtr(reader, dw);
        tl->momentCount = BinaryReader_readUint32(reader);

        if (tl->momentCount > 0) {
            tl->moments = safeMalloc(tl->momentCount * sizeof(TimelineMoment));

            // Pass 1: Read step + event pointer pairs
            uint32_t* eventPtrs = safeMalloc(tl->momentCount * sizeof(uint32_t));
            repeat(tl->momentCount, j) {
                tl->moments[j].step = BinaryReader_readUint32(reader);
                eventPtrs[j] = BinaryReader_readUint32(reader);
            }

            // Pass 2: Parse event action lists
            repeat(tl->momentCount, j) {
                BinaryReader_seek(reader, eventPtrs[j]);
                tl->moments[j].actions = readEventActions(reader, dw, &tl->moments[j].actionCount);
            }
            free(eventPtrs);
        } else {
            tl->moments = nullptr;
        }
    }
    free(ptrs);
}

static void parseOBJT(BinaryReader* reader, DataWin* dw) {
    Objt* o = &dw->objt;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    o->count = count;

    if (count == 0) { free(ptrs); o->objects = nullptr; return; }

    o->objects = safeMalloc(count * sizeof(GameObject));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        GameObject* obj = &o->objects[i];
        obj->name = readStringPtr(reader, dw);
        obj->spriteId = BinaryReader_readInt32(reader);
        obj->visible = BinaryReader_readBool32(reader);
        obj->solid = BinaryReader_readBool32(reader);
        obj->depth = BinaryReader_readInt32(reader);
        obj->persistent = BinaryReader_readBool32(reader);
        obj->parentId = BinaryReader_readInt32(reader);
        obj->textureMaskId = BinaryReader_readInt32(reader);
        obj->usesPhysics = BinaryReader_readBool32(reader);
        obj->isSensor = BinaryReader_readBool32(reader);
        obj->collisionShape = BinaryReader_readUint32(reader);
        obj->density = BinaryReader_readFloat32(reader);
        obj->restitution = BinaryReader_readFloat32(reader);
        obj->group = BinaryReader_readUint32(reader);
        obj->linearDamping = BinaryReader_readFloat32(reader);
        obj->angularDamping = BinaryReader_readFloat32(reader);
        obj->physicsVertexCount = BinaryReader_readInt32(reader);
        obj->friction = BinaryReader_readFloat32(reader);
        obj->awake = BinaryReader_readBool32(reader);
        obj->kinematic = BinaryReader_readBool32(reader);

        // Physics vertices
        if (obj->physicsVertexCount > 0) {
            obj->physicsVertices = safeMalloc(obj->physicsVertexCount * sizeof(PhysicsVertex));
            for (int32_t j = 0; obj->physicsVertexCount > j; j++) {
                obj->physicsVertices[j].x = BinaryReader_readFloat32(reader);
                obj->physicsVertices[j].y = BinaryReader_readFloat32(reader);
            }
        } else {
            obj->physicsVertices = nullptr;
        }

        // Events: UndertalePointerList<UndertalePointerList<Event>>
        // Outer pointer list: one entry per event type
        // Inner pointer list: events for that type
        uint32_t eventTypeCount;
        uint32_t* eventTypePtrs = readPointerTable(reader, &eventTypeCount);

        for (uint32_t eventType = 0; eventTypeCount > eventType && OBJT_EVENT_TYPE_COUNT > eventType; eventType++) {
            BinaryReader_seek(reader, eventTypePtrs[eventType]);

            // Inner pointer list: events for this type
            uint32_t eventCount;
            uint32_t* eventPtrs = readPointerTable(reader, &eventCount);

            obj->eventLists[eventType].eventCount = eventCount;

            if (eventCount > 0) {
                obj->eventLists[eventType].events = safeMalloc(eventCount * sizeof(ObjectEvent));
                repeat(eventCount, j) {
                    BinaryReader_seek(reader, eventPtrs[j]);
                    obj->eventLists[eventType].events[j].eventSubtype = BinaryReader_readUint32(reader);
                    obj->eventLists[eventType].events[j].actions = readEventActions(reader, dw, &obj->eventLists[eventType].events[j].actionCount);
                }
            } else {
                obj->eventLists[eventType].events = nullptr;
            }

            free(eventPtrs);
        }

        // Zero-fill any unused event type slots
        for (uint32_t eventType = eventTypeCount; OBJT_EVENT_TYPE_COUNT > eventType; eventType++) {
            obj->eventLists[eventType].eventCount = 0;
            obj->eventLists[eventType].events = nullptr;
        }

        free(eventTypePtrs);
    }
    free(ptrs);
}

// ===[ Room payload parsing helpers ]===
// Each of these assumes the caller has seeked to the start of the relevant PointerList (where the uint32 "count" of the list is).
// They allocate and populate the corresponding fields on Room.
// They are used by both the eager parse path and the lazy load path (DataWin_loadRoomPayload).

static void readRoomBackgrounds(BinaryReader* reader, Room* room) {
    uint32_t bgCount;
    uint32_t* bgPtrs = readPointerTable(reader, &bgCount);
    room->backgrounds = safeMalloc(8 * sizeof(RoomBackground));
    uint32_t fillEnd = bgCount < 8 ? bgCount : 8;
    for (uint32_t j = 0; fillEnd > j; j++) {
        BinaryReader_seek(reader, bgPtrs[j]);
        RoomBackground* bg = &room->backgrounds[j];
        bg->enabled = BinaryReader_readBool32(reader);
        bg->foreground = BinaryReader_readBool32(reader);
        bg->backgroundDefinition = BinaryReader_readInt32(reader);
        bg->x = BinaryReader_readInt32(reader);
        bg->y = BinaryReader_readInt32(reader);
        bg->tileX = BinaryReader_readInt32(reader);
        bg->tileY = BinaryReader_readInt32(reader);
        bg->speedX = BinaryReader_readInt32(reader);
        bg->speedY = BinaryReader_readInt32(reader);
        bg->stretch = BinaryReader_readBool32(reader);
    }
    for (uint32_t j = fillEnd; 8 > j; j++) {
        memset(&room->backgrounds[j], 0, sizeof(RoomBackground));
    }
    free(bgPtrs);
}

static void readRoomViews(BinaryReader* reader, Room* room) {
    uint32_t viewCount;
    uint32_t* viewPtrsArr = readPointerTable(reader, &viewCount);
    room->views = safeMalloc(8 * sizeof(RoomView));
    for (uint32_t j = 0; viewCount > j && 8 > j; j++) {
        BinaryReader_seek(reader, viewPtrsArr[j]);
        RoomView* view = &room->views[j];
        view->enabled = BinaryReader_readBool32(reader);
        view->viewX = BinaryReader_readInt32(reader);
        view->viewY = BinaryReader_readInt32(reader);
        view->viewWidth = BinaryReader_readInt32(reader);
        view->viewHeight = BinaryReader_readInt32(reader);
        view->portX = BinaryReader_readInt32(reader);
        view->portY = BinaryReader_readInt32(reader);
        view->portWidth = BinaryReader_readInt32(reader);
        view->portHeight = BinaryReader_readInt32(reader);
        view->borderX = BinaryReader_readUint32(reader);
        view->borderY = BinaryReader_readUint32(reader);
        view->speedX = BinaryReader_readInt32(reader);
        view->speedY = BinaryReader_readInt32(reader);
        view->objectId = BinaryReader_readInt32(reader);
    }
    for (uint32_t j = viewCount; 8 > j; j++) {
        memset(&room->views[j], 0, sizeof(RoomView));
    }
    free(viewPtrsArr);
}

static void readRoomGameObjects(BinaryReader* reader, DataWin* dw, Room* room) {
    uint32_t objCount;
    uint32_t* objPtrs = readPointerTable(reader, &objCount);
    room->gameObjectCount = objCount;
    if (objCount > 0) {
        room->gameObjects = safeMalloc(objCount * sizeof(RoomGameObject));
        repeat(objCount, j) {
            BinaryReader_seek(reader, objPtrs[j]);
            RoomGameObject* go = &room->gameObjects[j];
            go->x = BinaryReader_readInt32(reader);
            go->y = BinaryReader_readInt32(reader);
            go->objectDefinition = BinaryReader_readInt32(reader);
            go->instanceID = BinaryReader_readUint32(reader);
            go->creationCode = BinaryReader_readInt32(reader);
            go->scaleX = BinaryReader_readFloat32(reader);
            go->scaleY = BinaryReader_readFloat32(reader);
            if (DataWin_isVersionAtLeast(dw, 2, 2, 2, 302)) {
                go->imageSpeed = BinaryReader_readFloat32(reader);
                go->imageIndex = BinaryReader_readInt32(reader);
            } else {
                go->imageSpeed = 1.0f;
                go->imageIndex = 0;
            }
            go->color = BinaryReader_readUint32(reader);
            go->rotation = BinaryReader_readFloat32(reader);
            if (dw->gen8.bytecodeVersion >= 16) {
                go->preCreateCode = BinaryReader_readInt32(reader);
            } else {
                go->preCreateCode = -1;
            }
        }
    } else {
        room->gameObjects = nullptr;
    }
    free(objPtrs);
}

static void readRoomTiles(BinaryReader* reader, DataWin* dw, Room* room) {
    uint32_t tileCount;
    uint32_t* tilePtrs = readPointerTable(reader, &tileCount);
    room->tileCount = tileCount;
    if (tileCount > 0) {
        room->tiles = safeMalloc(tileCount * sizeof(RoomTile));
        repeat(tileCount, j) {
            BinaryReader_seek(reader, tilePtrs[j]);
            RoomTile* tile = &room->tiles[j];
            tile->x = BinaryReader_readInt32(reader);
            tile->y = BinaryReader_readInt32(reader);
            tile->useSpriteDefinition = DataWin_isVersionAtLeast(dw, 2, 0, 0, 0);
            tile->backgroundDefinition = BinaryReader_readInt32(reader);
            tile->sourceX = BinaryReader_readInt32(reader);
            tile->sourceY = BinaryReader_readInt32(reader);
            tile->width = BinaryReader_readUint32(reader);
            tile->height = BinaryReader_readUint32(reader);
            tile->tileDepth = BinaryReader_readInt32(reader);
            tile->instanceID = BinaryReader_readUint32(reader);
            tile->scaleX = BinaryReader_readFloat32(reader);
            tile->scaleY = BinaryReader_readFloat32(reader);
            tile->color = BinaryReader_readUint32(reader);
        }
    } else {
        room->tiles = nullptr;
    }
    free(tilePtrs);
}

static void readRoomLayers(BinaryReader* reader, DataWin* dw, Room* room) {
    uint32_t layerCount;
    uint32_t* layerPtrs = readPointerTable(reader, &layerCount);
    room->layerCount = layerCount;

    if (layerCount == 0) {
        room->layers = nullptr;
        free(layerPtrs);
        return;
    }

    room->layers = safeMalloc(layerCount * sizeof(RoomLayer));
    repeat(layerCount, j) {
        BinaryReader_seek(reader, layerPtrs[j]);
        RoomLayer* layer = &room->layers[j];
        layer->name = readStringPtr(reader, dw);
        layer->id = BinaryReader_readUint32(reader);
        layer->type = BinaryReader_readUint32(reader);
        layer->depth = BinaryReader_readInt32(reader);
        layer->xOffset = BinaryReader_readFloat32(reader);
        layer->yOffset = BinaryReader_readFloat32(reader);
        layer->hSpeed = BinaryReader_readFloat32(reader);
        layer->vSpeed = BinaryReader_readFloat32(reader);
        layer->visible = BinaryReader_readBool32(reader);
        layer->assetsData = nullptr;
        layer->backgroundData = nullptr;
        layer->instancesData = nullptr;
        layer->tilesData = nullptr;
        switch (layer->type) {
            case RoomLayerType_Path:
                break; // Nothing to do

            case RoomLayerType_Assets: {
                RoomLayerAssetsData* assets = safeMalloc(sizeof(RoomLayerAssetsData));
                uint32_t legacyTilesPtr = BinaryReader_readUint32(reader);
                uint32_t spritesPtr = BinaryReader_readUint32(reader);

                BinaryReader_seek(reader, legacyTilesPtr);
                uint32_t *innerTilePtrs = readPointerTable(reader, &assets->legacyTileCount);
                if (assets->legacyTileCount > 0) {
                    assets->legacyTiles = safeMalloc(assets->legacyTileCount * sizeof(RoomTile));
                    repeat(assets->legacyTileCount, k) {
                        BinaryReader_seek(reader, innerTilePtrs[k]);
                        RoomTile* tile = &assets->legacyTiles[k];
                        tile->x = BinaryReader_readInt32(reader);
                        tile->y = BinaryReader_readInt32(reader);
                        tile->useSpriteDefinition = DataWin_isVersionAtLeast(dw, 2, 0, 0, 0);
                        tile->backgroundDefinition = BinaryReader_readInt32(reader);
                        tile->sourceX = BinaryReader_readInt32(reader);
                        tile->sourceY = BinaryReader_readInt32(reader);
                        tile->width = BinaryReader_readUint32(reader);
                        tile->height = BinaryReader_readUint32(reader);
                        tile->tileDepth = BinaryReader_readInt32(reader);
                        tile->instanceID = BinaryReader_readUint32(reader);
                        tile->scaleX = BinaryReader_readFloat32(reader);
                        tile->scaleY = BinaryReader_readFloat32(reader);
                        tile->color = BinaryReader_readUint32(reader);
                    }
                } else {
                    assets->legacyTiles = nullptr;
                }
                free(innerTilePtrs);

                BinaryReader_seek(reader, spritesPtr);
                uint32_t *spritePtrs = readPointerTable(reader, &assets->spriteCount);
                if (assets->spriteCount > 0) {
                    assets->sprites = safeMalloc(assets->spriteCount * sizeof(SpriteInstance));
                    repeat(assets->spriteCount, k) {
                        BinaryReader_seek(reader, spritePtrs[k]);
                        SpriteInstance* sprite = &assets->sprites[k];
                        sprite->name = readStringPtr(reader, dw);
                        sprite->spriteIndex = BinaryReader_readInt32(reader);
                        sprite->x = BinaryReader_readInt32(reader);
                        sprite->y = BinaryReader_readInt32(reader);
                        sprite->scaleX = BinaryReader_readFloat32(reader);
                        sprite->scaleY = BinaryReader_readFloat32(reader);
                        sprite->color = BinaryReader_readUint32(reader);
                        sprite->animationSpeed = BinaryReader_readFloat32(reader);
                        sprite->animationSpeedType = BinaryReader_readUint32(reader);
                        sprite->frameIndex = BinaryReader_readFloat32(reader);
                        sprite->rotation = BinaryReader_readFloat32(reader);
                    }
                } else {
                    assets->sprites = nullptr;
                }
                free(spritePtrs);

                layer->assetsData = assets;
                break;
            }

            case RoomLayerType_Background: {
                RoomLayerBackgroundData* bg = safeMalloc(sizeof(RoomLayerBackgroundData));
                bg->visible = BinaryReader_readBool32(reader);
                bg->foreground = BinaryReader_readBool32(reader);
                bg->spriteIndex = BinaryReader_readInt32(reader);
                bg->hTiled = BinaryReader_readBool32(reader);
                bg->vTiled = BinaryReader_readBool32(reader);
                bg->stretch = BinaryReader_readBool32(reader);
                bg->color = BinaryReader_readUint32(reader);
                bg->firstFrame = BinaryReader_readFloat32(reader);
                bg->animSpeed = BinaryReader_readFloat32(reader);
                bg->animSpeedType = BinaryReader_readUint32(reader);
                layer->backgroundData = bg;
                break;
            }
            case RoomLayerType_Instances: {
                RoomLayerInstancesData* inst = safeMalloc(sizeof(RoomLayerInstancesData));
                inst->instanceCount = BinaryReader_readUint32(reader);
                if (inst->instanceCount > 0) {
                    inst->instanceIds = safeMalloc(inst->instanceCount * sizeof(uint32_t));
                    repeat(inst->instanceCount, k) {
                        inst->instanceIds[k] = BinaryReader_readUint32(reader);
                    }
                } else {
                    inst->instanceIds = nullptr;
                }
                layer->instancesData = inst;
                break;
            }
            case RoomLayerType_Tiles: {
                RoomLayerTilesData* tiles = safeMalloc(sizeof(RoomLayerTilesData));
                tiles->backgroundIndex = BinaryReader_readInt32(reader);
                tiles->tilesX = BinaryReader_readUint32(reader);
                tiles->tilesY = BinaryReader_readUint32(reader);
                uint32_t totalTiles = tiles->tilesX * tiles->tilesY;
                if (totalTiles > 0) {
                    tiles->tileData = safeMalloc(totalTiles * sizeof(uint32_t));
                    repeat(totalTiles, k) {
                        tiles->tileData[k] = BinaryReader_readUint32(reader);
                    }
                } else {
                    tiles->tileData = nullptr;
                }
                layer->tilesData = tiles;
                break;
            }
            default: {
                fprintf(stderr, "Unsupported Room Layer Type %u\n", layer->type);
                exit(0);
            }
        }
    }
    free(layerPtrs);
}

// Reads all 5 payload sections for a single room via the given reader.
// Assumes the caller has populated room->*FileOffset from the header pass.
static void readRoomPayload(BinaryReader* reader, DataWin* dw, Room* room) {
    require(!room->payloadLoaded);

    BinaryReader_seek(reader, room->backgroundsFileOffset);
    readRoomBackgrounds(reader, room);

    BinaryReader_seek(reader, room->viewsFileOffset);
    readRoomViews(reader, room);

    BinaryReader_seek(reader, room->gameObjectsFileOffset);
    readRoomGameObjects(reader, dw, room);

    BinaryReader_seek(reader, room->tilesFileOffset);
    readRoomTiles(reader, dw, room);

    room->layerCount = 0;
    room->layers = nullptr;
    if (room->layersFileOffset != 0) {
        BinaryReader_seek(reader, room->layersFileOffset);
        readRoomLayers(reader, dw, room);
    }

    room->payloadLoaded = true;
}

// Returns true when "name" is in the eager-load set.
static bool isRoomNameInEagerList(const char* name, StringBooleanEntry* eagerSet) {
    if (name == nullptr || eagerSet == nullptr) return false;
    return shgeti(eagerSet, name) >= 0;
}

static void parseROOM(BinaryReader* reader, DataWin* dw, bool lazyLoadRooms, StringBooleanEntry* eagerlyLoadedRooms) {
    RoomChunk* rc = &dw->room;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    rc->count = count;

    if (count == 0) { free(ptrs); rc->rooms = nullptr; return; }

    // Detect whether RoomGameObject includes ImageSpeed/ImageIndex fields (added in GMS 2.2.2.302).
    // UndertaleModTool detects this via the distance between the first two game object pointers: 40 bytes = legacy format, 48 bytes = new format with ImageSpeed+ImageIndex.
    // We skip if we already know that we are at or above 2.2.2.302.
    if (DataWin_isVersionAtLeast(dw, 2, 0, 0, 0) && !DataWin_isVersionAtLeast(dw, 2, 2, 2, 302)) {
        repeat(count, i) {
            BinaryReader_seek(reader, ptrs[i]);
            // Room header layout (before gameObjectsPtr): name, caption, width, height, speed, persistent,
            // bgColor, drawBgColor, creationCodeId, flags, backgroundsPtr, viewsPtr = 12 uint32s.
            BinaryReader_skip(reader, 12 * 4);
            uint32_t gameObjectsPtr = BinaryReader_readUint32(reader);
            BinaryReader_seek(reader, gameObjectsPtr);
            uint32_t objCount = BinaryReader_readUint32(reader);
            if (objCount >= 2) {
                uint32_t firstPtr = BinaryReader_readUint32(reader);
                uint32_t secondPtr = BinaryReader_readUint32(reader);
                if (secondPtr - firstPtr == 48) {
                    DataWin_bumpVersionTo(dw, 2, 2, 2, 302);
                }
                break;
            }
        }
    }

    rc->rooms = safeCalloc(count, sizeof(Room));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        Room* room = &rc->rooms[i];

        // ===[ Header pass ]===
        room->name = readStringPtr(reader, dw);
        room->caption = readStringPtr(reader, dw);
        room->width = BinaryReader_readUint32(reader);
        room->height = BinaryReader_readUint32(reader);
        room->speed = BinaryReader_readUint32(reader);
        room->persistent = BinaryReader_readBool32(reader);
        room->backgroundColor = BinaryReader_readUint32(reader);
        room->drawBackgroundColor = BinaryReader_readBool32(reader);
        room->creationCodeId = BinaryReader_readInt32(reader);
        room->flags = BinaryReader_readUint32(reader);
        room->backgroundsFileOffset = BinaryReader_readUint32(reader);
        room->viewsFileOffset = BinaryReader_readUint32(reader);
        room->gameObjectsFileOffset = BinaryReader_readUint32(reader);
        room->tilesFileOffset = BinaryReader_readUint32(reader);
        room->world = BinaryReader_readBool32(reader);
        room->top = BinaryReader_readUint32(reader);
        room->left = BinaryReader_readUint32(reader);
        room->right = BinaryReader_readUint32(reader);
        room->bottom = BinaryReader_readUint32(reader);
        room->gravityX = BinaryReader_readFloat32(reader);
        room->gravityY = BinaryReader_readFloat32(reader);
        room->metersPerPixel = BinaryReader_readFloat32(reader);
        if (DataWin_isVersionAtLeast(dw, 2024, 13, 0, 0)) {
            // skip instanceCreationOrderIDs
            int icCount = BinaryReader_readInt32(reader);
            BinaryReader_skip(reader, sizeof(int32_t) * icCount);
        }
        room->layersFileOffset = 0;
        if (DataWin_isVersionAtLeast(dw, 2, 0, 0, 0)) {
            room->layersFileOffset = BinaryReader_readUint32(reader);
            if (DataWin_isVersionAtLeast(dw, 2, 3, 0, 0)) {
                BinaryReader_skip(reader, 4); // sequencesPtr
            }
        }

        room->payloadLoaded = false;
        room->eagerlyLoaded = false;
        room->backgrounds = nullptr;
        room->views = nullptr;
        room->gameObjects = nullptr;
        room->gameObjectCount = 0;
        room->tiles = nullptr;
        room->tileCount = 0;
        room->layers = nullptr;
        room->layerCount = 0;

        // Load the room payload if needed
        bool eager = !lazyLoadRooms || isRoomNameInEagerList(room->name, eagerlyLoadedRooms);
        if (eager) {
            readRoomPayload(reader, dw, room);
            if (lazyLoadRooms) {
                room->eagerlyLoaded = true;
            }
        }
    }
    free(ptrs);
}

static void parseTPAG(BinaryReader* reader, DataWin* dw) {
    Tpag* t = &dw->tpag;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    t->count = count;

    if (count == 0) { free(ptrs); t->items = nullptr; return; }

    t->items = safeMalloc(count * sizeof(TexturePageItem));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        TexturePageItem* item = &t->items[i];
        item->sourceX = BinaryReader_readUint16(reader);
        item->sourceY = BinaryReader_readUint16(reader);
        item->sourceWidth = BinaryReader_readUint16(reader);
        item->sourceHeight = BinaryReader_readUint16(reader);
        item->targetX = BinaryReader_readUint16(reader);
        item->targetY = BinaryReader_readUint16(reader);
        item->targetWidth = BinaryReader_readUint16(reader);
        item->targetHeight = BinaryReader_readUint16(reader);
        item->boundingWidth = BinaryReader_readUint16(reader);
        item->boundingHeight = BinaryReader_readUint16(reader);
        item->texturePageId = BinaryReader_readInt16(reader);
    }

    // Build tpagOffsetMap: absolute file offset -> TPAG index
    repeat(count, i) {
        hmput(dw->tpagOffsetMap, ptrs[i], (int32_t) i);
    }

    free(ptrs);
}

static void parseCODE(BinaryReader* reader, DataWin* dw, uint32_t chunkLength, size_t chunkDataStart) {
    Code* c = &dw->code;

    if (chunkLength == 0) {
        // YYC-compiled game, no bytecode
        c->count = 0;
        c->entries = nullptr;
        return;
    }

    // Standard pointer list at chunk start. Each entry has a relative offset
    // (bytecodeRelAddr) that points to the actual bytecode blob elsewhere in the chunk.

    uint32_t codeCount;
    uint32_t* codePtrs = readPointerTable(reader, &codeCount);
    c->count = codeCount;

    if (codeCount == 0) { free(codePtrs); c->entries = nullptr; return; }

    c->entries = safeMalloc(codeCount * sizeof(CodeEntry));
    repeat(codeCount, i) {
        BinaryReader_seek(reader, codePtrs[i]);
        CodeEntry* entry = &c->entries[i];
        entry->name = readStringPtr(reader, dw);
        entry->length = BinaryReader_readUint32(reader);
        entry->localsCount = BinaryReader_readUint16(reader);
        entry->argumentsCount = BinaryReader_readUint16(reader);

        // bytecodeRelAddr is relative to the position of this field
        size_t relAddrFieldPos = BinaryReader_getPosition(reader);
        int32_t bytecodeRelAddr = BinaryReader_readInt32(reader);
        entry->bytecodeAbsoluteOffset = (uint32_t)((int64_t)relAddrFieldPos + bytecodeRelAddr);

        entry->offset = BinaryReader_readUint32(reader);
    }
    free(codePtrs);

    // Compute bytecode blob range and load into owned buffer.
    // The bytecode blob starts at the minimum bytecodeAbsoluteOffset and
    // extends to the end of the CODE chunk.
    uint32_t blobStart = c->entries[0].bytecodeAbsoluteOffset;
    repeat(codeCount, i) {
        if (c->entries[i].bytecodeAbsoluteOffset < blobStart) {
            blobStart = c->entries[i].bytecodeAbsoluteOffset;
        }
    }
    size_t chunkEnd = chunkDataStart + chunkLength;
    size_t blobSize = chunkEnd - blobStart;

    dw->bytecodeBufferBase = blobStart;
    dw->bytecodeBuffer = BinaryReader_readBytesAt(reader, blobStart, blobSize);
}

static void parseVARI(BinaryReader* reader, DataWin* dw, uint32_t chunkLength) {
    Vari* v = &dw->vari;

    v->varCount1 = BinaryReader_readUint32(reader);
    v->varCount2 = BinaryReader_readUint32(reader);
    v->maxLocalVarCount = BinaryReader_readUint32(reader);

    // Variable entries are packed sequentially (no pointer table)
    // Number of entries = (chunkLength - 12) / 20
    v->variableCount = (chunkLength - 12) / 20;

    if (v->variableCount > 0) {
        v->variables = safeMalloc(v->variableCount * sizeof(Variable));
        repeat(v->variableCount, i) {
            Variable* var = &v->variables[i];
            var->name = readStringPtr(reader, dw);
            var->instanceType = BinaryReader_readInt32(reader);
            var->varID = BinaryReader_readInt32(reader);
            var->occurrences = BinaryReader_readUint32(reader);
            var->firstAddress = BinaryReader_readUint32(reader);
        }
    } else {
        v->variables = nullptr;
    }
}

static void parseFUNC(BinaryReader* reader, DataWin* dw) {
    Func* f = &dw->func;

    // Part 1: Functions SimpleList
    f->functionCount = BinaryReader_readUint32(reader);
    if (f->functionCount > 0) {
        f->functions = safeMalloc(f->functionCount * sizeof(Function));
        repeat(f->functionCount, i) {
            f->functions[i].name = readStringPtr(reader, dw);
            f->functions[i].occurrences = BinaryReader_readUint32(reader);
            uint32_t rawAddr = BinaryReader_readUint32(reader);
            // In GMS 2.3+, firstAddress points to the operand word (instruction + 4), not the instruction itself
            if (DataWin_isVersionAtLeast(dw, 2, 3, 0, 0) && rawAddr != (uint32_t) -1) {
                rawAddr -= 4;
            }
            f->functions[i].firstAddress = rawAddr;
        }
    } else {
        f->functions = nullptr;
    }

    // Part 2: Code Locals SimpleList
    f->codeLocalsCount = BinaryReader_readUint32(reader);
    if (f->codeLocalsCount > 0) {
        f->codeLocals = safeMalloc(f->codeLocalsCount * sizeof(CodeLocals));
        repeat(f->codeLocalsCount, i) {
            CodeLocals* cl = &f->codeLocals[i];
            cl->localVarCount = BinaryReader_readUint32(reader);
            cl->name = readStringPtr(reader, dw);

            if (cl->localVarCount > 0) {
                cl->locals = safeMalloc(cl->localVarCount * sizeof(LocalVar));
                repeat(cl->localVarCount, j) {
                    cl->locals[j].varID = BinaryReader_readUint32(reader);
                    cl->locals[j].name = readStringPtr(reader, dw);
                }
            } else {
                cl->locals = nullptr;
            }
        }
    } else {
        f->codeLocals = nullptr;
    }
}

static void parseSTRG(BinaryReader* reader, DataWin* dw) {
    Strg* s = &dw->strg;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    s->count = count;

    if (count == 0) { free(ptrs); s->strings = nullptr; return; }

    s->strings = safeMalloc(count * sizeof(const char*));
    repeat(count, i) {
        // Pointer table points to the string's length prefix.
        // The actual string content starts 4 bytes after.
        s->strings[i] = (const char*)(dw->strgBuffer + (ptrs[i] + 4 - dw->strgBufferBase));
    }
    free(ptrs);
}

static void parseTXTR(BinaryReader* reader, DataWin* dw, size_t chunkEnd) {
    Txtr* t = &dw->txtr;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    t->count = count;

    if (count == 0) { free(ptrs); t->textures = nullptr; return; }

    // Read metadata entries
    bool hasGeneratedMips = DataWin_isVersionAtLeast(dw, 2, 0, 0, 0);
    t->textures = safeMalloc(count * sizeof(Texture));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        t->textures[i].scaled = BinaryReader_readUint32(reader);
        if (hasGeneratedMips) {
            t->textures[i].generatedMips = BinaryReader_readUint32(reader);
        } else {
            t->textures[i].generatedMips = 0;
        }
        t->textures[i].blobOffset = BinaryReader_readUint32(reader);
        t->textures[i].blobData = nullptr;
    }
    free(ptrs);

    // Compute blob sizes from successive offsets
    repeat(count, i) {
        if (t->textures[i].blobOffset == 0) {
            t->textures[i].blobSize = 0; // external texture
            continue;
        }
        if (count > i + 1 && t->textures[i + 1].blobOffset != 0) {
            // Check for integer underflow
            if (t->textures[i + 1].blobOffset >= t->textures[i].blobOffset) {
                t->textures[i].blobSize = t->textures[i + 1].blobOffset - t->textures[i].blobOffset;
            } else {
                fprintf(stderr, "Warning: texture %u has offset > next texture offset (likely corruption)\n", i);
                t->textures[i].blobSize = 0;
            }
        } else {
            // Check if offset is within chunk bounds
            if (t->textures[i].blobOffset >= chunkEnd) {
                fprintf(stderr, "Warning: texture %u offset exceeds chunk bounds\n", i);
                t->textures[i].blobSize = 0;
            } else {
                t->textures[i].blobSize = (uint32_t)(chunkEnd - t->textures[i].blobOffset);
            }
        }
    }

    // Load blob data with size validation
    #define MAX_TEXTURE_BLOB_SIZE (16 * 1024 * 1024)
    repeat(count, i) {
        if (t->textures[i].blobOffset == 0 || t->textures[i].blobSize == 0) continue;
        
        // Cap unreasonable sizes
        if (t->textures[i].blobSize > MAX_TEXTURE_BLOB_SIZE) {
            fprintf(stderr, "Warning: texture %u blob size %u exceeds 16MB limit, capping\n", i, t->textures[i].blobSize);
            t->textures[i].blobSize = MAX_TEXTURE_BLOB_SIZE;
        }
        
        t->textures[i].blobData = BinaryReader_readBytesAt(reader, t->textures[i].blobOffset, t->textures[i].blobSize);
        t->textures[i].loaded = (t->textures[i].blobData != NULL);
    }
}

static void parseAUDO(BinaryReader* reader, DataWin* dw) {
    Audo* a = &dw->audo;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    a->count = count;

    if (count == 0) { free(ptrs); a->entries = nullptr; return; }

    a->entries = safeMalloc(count * sizeof(AudioEntry));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        a->entries[i].dataSize = BinaryReader_readUint32(reader);
        a->entries[i].dataOffset = (uint32_t)BinaryReader_getPosition(reader);
        // Load audio data into owned buffer
        if (a->entries[i].dataSize > 0) {
            a->entries[i].data = safeMalloc(a->entries[i].dataSize);
            BinaryReader_readBytes(reader, a->entries[i].data, a->entries[i].dataSize);
        } else {
            a->entries[i].data = nullptr;
        }
    }
    free(ptrs);
}

// ===[ MAIN PARSE FUNCTION ]===

DataWin* DataWin_parse(const char* filePath, DataWinParserOptions options) {
    FILE* file = fopen(filePath, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open file: %s\n", filePath);
        exit(1);
    }

    // Use a large read buffer to reduce the number of physical reads
    // This is critical for slow I/O devices like the PS2 CDVD drive, where each fread
    // call would otherwise trigger a separate disc read of just a few sectors
    setvbuf(file, nullptr, _IOFBF, 128 * 1024);

    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (fileSize <= 0) {
        fprintf(stderr, "Invalid file size: %ld\n", fileSize);
        fclose(file);
        exit(1);
    }

    // Allocate and zero-initialize DataWin
    DataWin* dw = safeCalloc(1, sizeof(DataWin));

    BinaryReader reader = BinaryReader_create(file, (size_t) fileSize);

    // Validate FORM header
    char formMagic[4];
    BinaryReader_readBytes(&reader, formMagic, 4);
    if (memcmp(formMagic, "FORM", 4) != 0) {
        fprintf(stderr, "Invalid file: expected FORM magic, got '%.4s'\n", formMagic);
        free(dw);
        fclose(file);
        exit(1);
    }

    uint32_t formLength = BinaryReader_readUint32(&reader);
    (void) formLength;

    // Pass 1: Count total chunks and find STRG chunk offset.
    // All other chunks reference strings from STRG, so it must be loaded first.
    // We also check if the CODE chunk exists.
    int totalChunks = 0;
    bool codeExists = false;
    BinaryReader_seek(&reader, 8); // reset to after FORM header

    while ((size_t) fileSize > BinaryReader_getPosition(&reader)) {
        if (BinaryReader_getPosition(&reader) + 8 > (size_t) fileSize) break;

        char chunkName[5] = {0};
        BinaryReader_readBytes(&reader, chunkName, 4);
        uint32_t chunkLength = BinaryReader_readUint32(&reader);
        size_t chunkDataStart = BinaryReader_getPosition(&reader);

        if (options.parseStrg && memcmp(chunkName, "STRG", 4) == 0) {
            dw->strgBufferBase = chunkDataStart;
            dw->strgBuffer = BinaryReader_readBytesAt(&reader, chunkDataStart, chunkLength);
        }

        if ((memcmp(chunkName, "CODE", 4) == 0) && chunkLength > 0) {
            codeExists = true;
        }
        BinaryReader_seek(&reader, chunkDataStart + chunkLength);
        totalChunks++;
    }

    if (!codeExists && options.parseCode) {
        fprintf(stderr, "CODE chunk does not exist or is empty! This usually means you're loading a YYC game.\n");
        fclose(file);
        exit(1);
    }

    // Pass 2: Parse all chunks
    // For each chunk that will be parsed, we bulk-read the entire chunk into memory first
    // and then parse from the memory buffer. This dramatically reduces the number of physical
    // reads on slow I/O devices like the PS2 CDVD drive.
    BinaryReader_seek(&reader, 8); // skip past FORM header
    int chunkIndex = 0;
    while ((size_t) fileSize > BinaryReader_getPosition(&reader)) {
        if (BinaryReader_getPosition(&reader) + 8 > (size_t) fileSize) break;

        char chunkName[5] = {0};
        BinaryReader_readBytes(&reader, chunkName, 4);
        uint32_t chunkLength = BinaryReader_readUint32(&reader);
        size_t chunkDataStart = BinaryReader_getPosition(&reader);
        size_t chunkEnd = chunkDataStart + chunkLength;

        if (options.progressCallback) {
            options.progressCallback(chunkName, chunkIndex, totalChunks, dw, options.progressCallbackUserData);
        }

        // Determine if this chunk will be parsed (and thus needs bulk loading)
        bool shouldParse =
            (options.parseGen8 && memcmp(chunkName, "GEN8", 4) == 0) ||
            (options.parseOptn && memcmp(chunkName, "OPTN", 4) == 0) ||
            (options.parseLang && memcmp(chunkName, "LANG", 4) == 0) ||
            (options.parseExtn && memcmp(chunkName, "EXTN", 4) == 0) ||
            (options.parseSond && memcmp(chunkName, "SOND", 4) == 0) ||
            (options.parseAgrp && memcmp(chunkName, "AGRP", 4) == 0) ||
            (options.parseSprt && memcmp(chunkName, "SPRT", 4) == 0) ||
            (options.parseBgnd && memcmp(chunkName, "BGND", 4) == 0) ||
            (options.parsePath && memcmp(chunkName, "PATH", 4) == 0) ||
            (options.parseScpt && memcmp(chunkName, "SCPT", 4) == 0) ||
            (options.parseGlob && memcmp(chunkName, "GLOB", 4) == 0) ||
            (options.parseShdr && memcmp(chunkName, "SHDR", 4) == 0) ||
            (options.parseFont && memcmp(chunkName, "FONT", 4) == 0) ||
            (options.parseTmln && memcmp(chunkName, "TMLN", 4) == 0) ||
            (options.parseObjt && memcmp(chunkName, "OBJT", 4) == 0) ||
            (options.parseRoom && memcmp(chunkName, "ROOM", 4) == 0) ||
            (options.parseTpag && memcmp(chunkName, "TPAG", 4) == 0) ||
            (options.parseCode && memcmp(chunkName, "CODE", 4) == 0) ||
            (options.parseVari && memcmp(chunkName, "VARI", 4) == 0) ||
            (options.parseFunc && memcmp(chunkName, "FUNC", 4) == 0) ||
            (options.parseStrg && memcmp(chunkName, "STRG", 4) == 0) ||
            (options.parseTxtr && memcmp(chunkName, "TXTR", 4) == 0) ||
            (options.parseAudo && memcmp(chunkName, "AUDO", 4) == 0);

        // Bulk-read the chunk data into memory for fast parsing
        uint8_t* chunkBuffer = nullptr;
        if (shouldParse && chunkLength > 0) {
            chunkBuffer = safeMalloc(chunkLength);
            size_t read = fread(chunkBuffer, 1, chunkLength, reader.file);
            if (read != chunkLength) {
                fprintf(stderr, "DataWin: short read on chunk %.4s (expected %u, got %zu)\n", chunkName, chunkLength, read);
                exit(1);
            }
            BinaryReader_setBuffer(&reader, chunkBuffer, chunkDataStart, chunkLength);
        }

        if (options.parseGen8 && memcmp(chunkName, "GEN8", 4) == 0) {
            parseGEN8(&reader, dw);
        } else if (options.parseOptn && memcmp(chunkName, "OPTN", 4) == 0) {
            parseOPTN(&reader, dw);
        } else if (options.parseLang && memcmp(chunkName, "LANG", 4) == 0) {
            parseLANG(&reader, dw);
        } else if (options.parseExtn && memcmp(chunkName, "EXTN", 4) == 0) {
            parseEXTN(&reader, dw);
        } else if (options.parseSond && memcmp(chunkName, "SOND", 4) == 0) {
            parseSOND(&reader, dw);
        } else if (options.parseAgrp && memcmp(chunkName, "AGRP", 4) == 0) {
            parseAGRP(&reader, dw);
        } else if (options.parseSprt && memcmp(chunkName, "SPRT", 4) == 0) {
            parseSPRT(&reader, dw, options.skipLoadingPreciseMasksForNonPreciseSprites);
        } else if (options.parseBgnd && memcmp(chunkName, "BGND", 4) == 0) {
            parseBGND(&reader, dw);
        } else if (options.parsePath && memcmp(chunkName, "PATH", 4) == 0) {
            parsePATH(&reader, dw);
        } else if (options.parseScpt && memcmp(chunkName, "SCPT", 4) == 0) {
            parseSCPT(&reader, dw);
        } else if (options.parseGlob && memcmp(chunkName, "GLOB", 4) == 0) {
            parseGLOB(&reader, dw);
        } else if (options.parseShdr && memcmp(chunkName, "SHDR", 4) == 0) {
            parseSHDR(&reader, dw);
        } else if (options.parseFont && memcmp(chunkName, "FONT", 4) == 0) {
            parseFONT(&reader, dw);
        } else if (options.parseTmln && memcmp(chunkName, "TMLN", 4) == 0) {
            parseTMLN(&reader, dw);
        } else if (options.parseObjt && memcmp(chunkName, "OBJT", 4) == 0) {
            parseOBJT(&reader, dw);
        } else if (options.parseRoom && memcmp(chunkName, "ROOM", 4) == 0) {
            parseROOM(&reader, dw, options.lazyLoadRooms, options.eagerlyLoadedRooms);
        } else if (memcmp(chunkName, "DAFL", 4) == 0) {
            // Empty chunk, nothing to parse
        } else if (memcmp(chunkName, "EMBI", 4) == 0) {
            // Embedded Images chunk
        } else if (memcmp(chunkName, "TGIN", 4) == 0) {
            // Texture Group Info chunk (bytecodeVersion >= 17)
        } else if (memcmp(chunkName, "ACRV", 4) == 0) {
            // Animation Curves chunk (GMS 2.3+)
            DataWin_bumpVersionTo(dw, 2, 3, 0, 0);
        } else if (memcmp(chunkName, "SEQN", 4) == 0) {
            // Sequences chunk (GMS 2.3+)
            DataWin_bumpVersionTo(dw, 2, 3, 0, 0);
        } else if (memcmp(chunkName, "TAGS", 4) == 0) {
            // Tags chunk (GMS 2.3+)
            DataWin_bumpVersionTo(dw, 2, 3, 0, 0);
        } else if (memcmp(chunkName, "FEDS", 4) == 0) {
            // Filter Effects Data chunk (GMS 2.3.6+)
            DataWin_bumpVersionTo(dw, 2, 3, 6, 0);
        } else if (options.parseTpag && memcmp(chunkName, "TPAG", 4) == 0) {
            parseTPAG(&reader, dw);
        } else if (options.parseCode && memcmp(chunkName, "CODE", 4) == 0) {
            parseCODE(&reader, dw, chunkLength, chunkDataStart);
        } else if (options.parseVari && memcmp(chunkName, "VARI", 4) == 0) {
            parseVARI(&reader, dw, chunkLength);
        } else if (options.parseFunc && memcmp(chunkName, "FUNC", 4) == 0) {
            parseFUNC(&reader, dw);
        } else if (options.parseStrg && memcmp(chunkName, "STRG", 4) == 0) {
            parseSTRG(&reader, dw);
        } else if (options.parseTxtr && memcmp(chunkName, "TXTR", 4) == 0) {
            parseTXTR(&reader, dw, chunkEnd);
        } else if (options.parseAudo && memcmp(chunkName, "AUDO", 4) == 0) {
            parseAUDO(&reader, dw);
        } else if (memcmp(chunkName, "AUDO", 4) == 0) {
            /* parseAudo == false: skip (e.g. 3DS uses external CWAV; loading blobs would OOM). */
        } else {
            printf("Unknown chunk: %.4s (length %u at offset 0x%zX)\n", chunkName, chunkLength, chunkDataStart - 8);
        }

        // Free the chunk buffer and revert to FILE*-based reads for the next header
        if (chunkBuffer != nullptr) {
            BinaryReader_clearBuffer(&reader);
            free(chunkBuffer);
        }

        // Seek to chunk end (skip any unread data or trailing padding)
        fseek(reader.file, (long) chunkEnd, SEEK_SET);
        chunkIndex++;
    }

    // GMS2: apply default FPS to rooms with speed=0
    if (dw->gen8.gms2FPS > 0) {
        repeat(dw->room.count, i) {
            if (dw->room.rooms[i].speed == 0) {
                dw->room.rooms[i].speed = (uint32_t) dw->gen8.gms2FPS;
            }
        }
    }

    // If lazy-loading rooms, keep the file handle open for DataWin_loadRoomPayload, otherwise close it now
    dw->lazyLoadRooms = options.lazyLoadRooms;
    if (options.lazyLoadRooms) {
        dw->lazyLoadFile = file;
        dw->lazyLoadFilePath = safeStrdup(filePath);
    } else {
        dw->lazyLoadFile = nullptr;
        dw->lazyLoadFilePath = nullptr;
        fclose(file);
    }
    // Expose file handle via the 3DS-compatible dw->file alias (may be NULL if not lazy-loading)
    dw->file = dw->lazyLoadFile;

    return dw;
}

// ===[ FREE ]===

void DataWin_free(DataWin* dw) {
    if (!dw) return;

    // GEN8
    free(dw->gen8.roomOrder);

    // OPTN
    free(dw->optn.constants);

    // LANG
    free(dw->lang.entryIds);
    if (dw->lang.languages) {
        repeat(dw->lang.languageCount, i) {
            free(dw->lang.languages[i].entries);
        }
        free(dw->lang.languages);
    }

    // EXTN
    if (dw->extn.extensions) {
        repeat(dw->extn.count, i) {
            Extension* ext = &dw->extn.extensions[i];
            if (ext->files) {
                repeat(ext->fileCount, j) {
                    ExtensionFile* file = &ext->files[j];
                    if (file->functions) {
                        repeat(file->functionCount, k) {
                            free(file->functions[k].arguments);
                        }
                        free(file->functions);
                    }
                }
                free(ext->files);
            }
        }
        free(dw->extn.extensions);
    }

    // SOND
    free(dw->sond.sounds);

    // AGRP
    free(dw->agrp.audioGroups);

    // SPRT
    if (dw->sprt.sprites) {
        repeat(dw->sprt.count, i) {
            free(dw->sprt.sprites[i].textureOffsets);
            if (dw->sprt.sprites[i].masks != nullptr) {
                repeat(dw->sprt.sprites[i].maskCount, j) {
                    free(dw->sprt.sprites[i].masks[j]);
                }
                free(dw->sprt.sprites[i].masks);
            }
        }
        free(dw->sprt.sprites);
        hmfree(dw->sprtOffsetMap);
    }


    // BGND
    if (dw->bgnd.backgrounds) {
        repeat(dw->bgnd.count, i) {
            free(dw->bgnd.backgrounds[i].gms2TileIds);
        }
    }
    free(dw->bgnd.backgrounds);

    // PATH
    if (dw->path.paths) {
        repeat(dw->path.count, i) {
            free(dw->path.paths[i].points);
            free(dw->path.paths[i].internalPoints);
        }
        free(dw->path.paths);
    }

    // SCPT
    free(dw->scpt.scripts);

    // GLOB
    free(dw->glob.codeIds);

    // SHDR
    if (dw->shdr.shaders) {
        repeat(dw->shdr.count, i) {
            free(dw->shdr.shaders[i].vertexAttributes);
        }
        free(dw->shdr.shaders);
    }

    // FONT
    if (dw->font.fonts) {
        repeat(dw->font.count, i) {
            Font* font = &dw->font.fonts[i];
            if (font->glyphs) {
                repeat(font->glyphCount, j) {
                    free(font->glyphs[j].kerning);
                }
                free(font->glyphs);
            }
        }
        free(dw->font.fonts);
    }

    // TMLN
    if (dw->tmln.timelines) {
        repeat(dw->tmln.count, i) {
            Timeline* tl = &dw->tmln.timelines[i];
            if (tl->moments) {
                repeat(tl->momentCount, j) {
                    free(tl->moments[j].actions);
                }
                free(tl->moments);
            }
        }
        free(dw->tmln.timelines);
    }

    // OBJT
    if (dw->objt.objects) {
        repeat(dw->objt.count, i) {
            GameObject* obj = &dw->objt.objects[i];
            free(obj->physicsVertices);
            repeat(OBJT_EVENT_TYPE_COUNT, e) {
                ObjectEventList* list = &obj->eventLists[e];
                if (list->events) {
                    repeat(list->eventCount, j) {
                        free(list->events[j].actions);
                    }
                    free(list->events);
                }
            }
        }
        free(dw->objt.objects);
    }

    // ROOM
    if (dw->room.rooms) {
        repeat(dw->room.count, i) {
            DataWin_freeRoomPayload(&dw->room.rooms[i]);
        }
        free(dw->room.rooms);
    }

    // TPAG
    free(dw->tpag.items);
    hmfree(dw->tpagOffsetMap);

    // CODE
    free(dw->code.entries);

    // VARI
    free(dw->vari.variables);

    // FUNC
    free(dw->func.functions);
    if (dw->func.codeLocals) {
        repeat(dw->func.codeLocalsCount, i) {
            free(dw->func.codeLocals[i].locals);
        }
        free(dw->func.codeLocals);
    }

    // STRG
    free(dw->strg.strings);

    // TXTR
    if (dw->txtr.textures) {
        repeat(dw->txtr.count, i) {
            free(dw->txtr.textures[i].blobData);
        }
        free(dw->txtr.textures);
    }

    // AUDO
    if (dw->audo.entries) {
        repeat(dw->audo.count, i) {
            free(dw->audo.entries[i].data);
        }
        free(dw->audo.entries);
    }

    // Owned buffers
    free(dw->strgBuffer);
    free(dw->bytecodeBuffer);

    // Close the lazy-load file handle (only open when lazyLoadRooms was enabled)
    if (dw->lazyLoadFile != nullptr) {
        fclose(dw->lazyLoadFile);
        dw->lazyLoadFile = nullptr;
    }
    free(dw->lazyLoadFilePath);

    free(dw);
}

// ===[ Lazy Room Payload ]===

void DataWin_freeRoomPayload(Room* room) {
    requireNotNull(room);
    free(room->backgrounds);
    room->backgrounds = nullptr;
    free(room->views);
    room->views = nullptr;
    free(room->gameObjects);
    room->gameObjects = nullptr;
    room->gameObjectCount = 0;
    free(room->tiles);
    room->tiles = nullptr;
    room->tileCount = 0;
    if (room->layerCount != 0 && room->layers != nullptr) {
        repeat(room->layerCount, j) {
            RoomLayer* layer = &room->layers[j];
            if (layer->assetsData) {
                free(layer->assetsData->legacyTiles);
                free(layer->assetsData->sprites);
                free(layer->assetsData);
            }
            if (layer->backgroundData) free(layer->backgroundData);
            if (layer->instancesData) {
                free(layer->instancesData->instanceIds);
                free(layer->instancesData);
            }
            if (layer->tilesData) {
                free(layer->tilesData->tileData);
                free(layer->tilesData);
            }
        }
    }
    free(room->layers);
    room->layers = nullptr;
    room->layerCount = 0;
    room->payloadLoaded = false;
}

void DataWin_loadRoomPayload(DataWin* dw, int32_t roomIndex) {
    require(roomIndex >= 0 && dw->room.count > (uint32_t) roomIndex);
    Room* room = &dw->room.rooms[roomIndex];
    if (room->payloadLoaded) return;
    requireMessage(dw->lazyLoadFile != nullptr, "DataWin_loadRoomPayload called without an open lazy-load FILE*");

    FILE* f = dw->lazyLoadFile;
    // Find file size at lazy-load time (only needed for BinaryReader bounds checking).
    long savedPos = ftell(f);
    fseek(f, 0, SEEK_END);
    size_t fileSize = (size_t) ftell(f);
    fseek(f, savedPos, SEEK_SET);

    BinaryReader lazyReader = BinaryReader_create(f, fileSize);
    readRoomPayload(&lazyReader, dw, room);
}

// ===[ TPAG Offset Resolution ]===

int32_t DataWin_resolveTPAG(DataWin* dw, uint32_t offset) {
    ptrdiff_t idx = hmgeti(dw->tpagOffsetMap, offset);
    if (0 > idx) return -1;
    return dw->tpagOffsetMap[idx].value;
}

// ===[ SPRT Offset Resolution ]===

int32_t DataWin_resolveSPRT(DataWin* dw, uint32_t offset) {
    ptrdiff_t idx = hmgeti(dw->sprtOffsetMap, offset);
    if (0 > idx) return -1;
    return dw->sprtOffsetMap[idx].value;
}

// ===[ Version Detection ]===

bool DataWin_isVersionAtLeast(const DataWin* dw, uint32_t major, uint32_t minor, uint32_t release, uint32_t build) {
    const DetectedFormat* f = &dw->detectedFormat;
    if (f->major != major) return f->major > major;
    if (f->minor != minor) return f->minor > minor;
    if (f->release != release) return f->release > release;
    return f->build >= build;
}

void DataWin_bumpVersionTo(DataWin* dw, uint32_t major, uint32_t minor, uint32_t release, uint32_t build) {
    if (DataWin_isVersionAtLeast(dw, major, minor, release, build)) return;
    dw->detectedFormat.major = major;
    dw->detectedFormat.minor = minor;
    dw->detectedFormat.release = release;
    dw->detectedFormat.build = build;
}
