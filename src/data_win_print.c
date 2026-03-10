#include "data_win.h"

#include <stdio.h>
#include <string.h>

#include "utils.h"

void DataWin_printDebugSummary(DataWin* dataWin) {
    printf("===== data.win Summary =====\n\n");

    // GEN8
    Gen8* g = &dataWin->gen8;
    printf("-- GEN8 (General Info) --\n");
    printf("  Game Name:        %s\n", g->name ? g->name : "(null)");
    printf("  Display Name:     %s\n", g->displayName ? g->displayName : "(null)");
    printf("  File Name:        %s\n", g->fileName ? g->fileName : "(null)");
    printf("  Config:           %s\n", g->config ? g->config : "(null)");
    printf("  Bytecode Version: %u\n", g->bytecodeVersion);
    printf("  Game ID:          %u\n", g->gameID);
    printf("  Version:          %u.%u.%u.%u\n", g->major, g->minor, g->release, g->build);
    printf("  Window Size:      %ux%u\n", g->defaultWindowWidth, g->defaultWindowHeight);
    printf("  Steam App ID:     %d\n", g->steamAppID);
    printf("  Room Order:       %u rooms\n", g->roomOrderCount);
    printf("\n");

    // OPTN
    printf("-- OPTN (Options) --\n");
    printf("  Constants:        %u\n", dataWin->optn.constantCount);
    if (dataWin->optn.constantCount > 0) {
        uint32_t show = dataWin->optn.constantCount < 3 ? dataWin->optn.constantCount : 3;
        forEachIndexed(OptnConstant, constant, idx, dataWin->optn.constants, show) {
            printf("    [%u] %s = %s\n", idx, constant->name ? constant->name : "?", constant->value ? constant->value : "?");
        }
        if (dataWin->optn.constantCount > 3) printf("    ... and %u more\n", dataWin->optn.constantCount - 3);
    }
    printf("\n");

    // LANG
    printf("-- LANG (Languages) --\n");
    printf("  Languages:        %u\n", dataWin->lang.languageCount);
    printf("  Entries:          %u\n", dataWin->lang.entryCount);
    printf("\n");

    // EXTN
    printf("-- EXTN (Extensions) --\n");
    printf("  Extensions:       %u\n", dataWin->extn.count);
    forEachIndexed(Extension, ext, idx, dataWin->extn.extensions, dataWin->extn.count) {
        printf("    [%u] %s (%u files)\n", idx, ext->name ? ext->name : "?", ext->fileCount);
    }
    printf("\n");

    // SOND
    printf("-- SOND (Sounds) --\n");
    printf("  Sounds:           %u\n", dataWin->sond.count);
    if (dataWin->sond.count > 0) {
        uint32_t show = dataWin->sond.count < 3 ? dataWin->sond.count : 3;
        forEachIndexed(Sound, snd, idx, dataWin->sond.sounds, show) {
            printf("    [%u] %s (%s)\n", idx, snd->name ? snd->name : "?", snd->type ? snd->type : "?");
        }
        if (dataWin->sond.count > 3) printf("    ... and %u more\n", dataWin->sond.count - 3);
    }
    printf("\n");

    // AGRP
    printf("-- AGRP (Audio Groups) --\n");
    printf("  Audio Groups:     %u\n", dataWin->agrp.count);
    forEachIndexed(AudioGroup, ag, idx, dataWin->agrp.audioGroups, dataWin->agrp.count) {
        printf("    [%u] %s\n", idx, ag->name ? ag->name : "?");
    }
    printf("\n");

    // SPRT
    printf("-- SPRT (Sprites) --\n");
    printf("  Sprites:          %u\n", dataWin->sprt.count);
    if (dataWin->sprt.count > 0) {
        uint32_t show = dataWin->sprt.count < 3 ? dataWin->sprt.count : 3;
        forEachIndexed(Sprite, spr, idx, dataWin->sprt.sprites, show) {
            printf("    [%u] %s (%ux%u, %u frames)\n", idx, spr->name ? spr->name : "?", spr->width, spr->height, spr->textureCount);
        }
        if (dataWin->sprt.count > 3) printf("    ... and %u more\n", dataWin->sprt.count - 3);
    }
    printf("\n");

    // BGND
    printf("-- BGND (Backgrounds) --\n");
    printf("  Backgrounds:      %u\n", dataWin->bgnd.count);
    if (dataWin->bgnd.count > 0) {
        uint32_t show = dataWin->bgnd.count < 3 ? dataWin->bgnd.count : 3;
        forEachIndexed(Background, bg, idx, dataWin->bgnd.backgrounds, show) {
            printf("    [%u] %s\n", idx, bg->name ? bg->name : "?");
        }
        if (dataWin->bgnd.count > 3) printf("    ... and %u more\n", dataWin->bgnd.count - 3);
    }
    printf("\n");

    // PATH
    printf("-- PATH (Paths) --\n");
    printf("  Paths:            %u\n", dataWin->path.count);
    printf("\n");

    // SCPT
    printf("-- SCPT (Scripts) --\n");
    printf("  Scripts:          %u\n", dataWin->scpt.count);
    if (dataWin->scpt.count > 0) {
        uint32_t show = dataWin->scpt.count < 3 ? dataWin->scpt.count : 3;
        forEachIndexed(Script, scr, idx, dataWin->scpt.scripts, show) {
            printf("    [%u] %s -> code[%d]\n", idx, scr->name ? scr->name : "?", scr->codeId);
        }
        if (dataWin->scpt.count > 3) printf("    ... and %u more\n", dataWin->scpt.count - 3);
    }
    printf("\n");

    // GLOB
    printf("-- GLOB (Global Init Scripts) --\n");
    printf("  Init Scripts:     %u\n", dataWin->glob.count);
    printf("\n");

    // SHDR
    printf("-- SHDR (Shaders) --\n");
    printf("  Shaders:          %u\n", dataWin->shdr.count);
    forEachIndexed(Shader, shdr, idx, dataWin->shdr.shaders, dataWin->shdr.count) {
        printf("    [%u] %s (version %d)\n", idx, shdr->name ? shdr->name : "?", shdr->version);
    }
    printf("\n");

    // FONT
    printf("-- FONT (Fonts) --\n");
    printf("  Fonts:            %u\n", dataWin->font.count);
    forEachIndexed(Font, fnt, idx, dataWin->font.fonts, dataWin->font.count) {
        printf("    [%u] %s (%s, em=%u, %u glyphs)\n", idx, fnt->name ? fnt->name : "?", fnt->displayName ? fnt->displayName : "?", fnt->emSize, fnt->glyphCount);
    }
    printf("\n");

    // TMLN
    printf("-- TMLN (Timelines) --\n");
    printf("  Timelines:        %u\n", dataWin->tmln.count);
    printf("\n");

    // OBJT
    printf("-- OBJT (Game Objects) --\n");
    printf("  Objects:          %u\n", dataWin->objt.count);
    if (dataWin->objt.count > 0) {
        uint32_t show = dataWin->objt.count < 3 ? dataWin->objt.count : 3;
        forEachIndexed(GameObject, obj, idx, dataWin->objt.objects, show) {
            uint32_t totalEvents = 0;
            repeat(OBJT_EVENT_TYPE_COUNT, e) {
                totalEvents += obj->eventLists[e].eventCount;
            }
            printf("    [%u] %s (sprite=%d, depth=%d, %u events)\n", idx, obj->name ? obj->name : "?", obj->spriteId, obj->depth, totalEvents);
        }
        if (dataWin->objt.count > 3) printf("    ... and %u more\n", dataWin->objt.count - 3);
    }
    printf("\n");

    // ROOM
    printf("-- ROOM (Rooms) --\n");
    printf("  Rooms:            %u\n", dataWin->room.count);
    if (dataWin->room.count > 0) {
        uint32_t show = dataWin->room.count < 3 ? dataWin->room.count : 3;
        forEachIndexed(Room, rm, idx, dataWin->room.rooms, show) {
            printf("    [%u] %s (%ux%u, %u objects, %u tiles)\n", idx, rm->name ? rm->name : "?", rm->width, rm->height, rm->gameObjectCount, rm->tileCount);
        }
        if (dataWin->room.count > 3) printf("    ... and %u more\n", dataWin->room.count - 3);
    }
    printf("\n");

    // TPAG
    printf("-- TPAG (Texture Page Items) --\n");
    printf("  Items:            %u\n", dataWin->tpag.count);
    printf("\n");

    // CODE
    printf("-- CODE (Code Entries) --\n");
    printf("  Entries:          %u\n", dataWin->code.count);
    if (dataWin->code.count > 0) {
        uint32_t show = dataWin->code.count < 3 ? dataWin->code.count : 3;
        forEachIndexed(CodeEntry, entry, idx, dataWin->code.entries, show) {
            printf("    [%u] %s (%u bytes, %u locals, %u args)\n", idx, entry->name ? entry->name : "?", entry->length, entry->localsCount, entry->argumentsCount);
        }
        if (dataWin->code.count > 3) printf("    ... and %u more\n", dataWin->code.count - 3);
    }
    printf("\n");

    // VARI
    printf("-- VARI (Variables) --\n");
    printf("  Variables:        %u\n", dataWin->vari.variableCount);
    printf("  Max Locals:       %u\n", dataWin->vari.maxLocalVarCount);
    if (dataWin->vari.variableCount > 0) {
        uint32_t show = dataWin->vari.variableCount < 3 ? dataWin->vari.variableCount : 3;
        forEachIndexed(Variable, var, idx, dataWin->vari.variables, show) {
            printf("    [%u] %s (type=%d, id=%d, %u refs)\n", idx, var->name ? var->name : "?", var->instanceType, var->varID, var->occurrences);
        }
        if (dataWin->vari.variableCount > 3) printf("    ... and %u more\n", dataWin->vari.variableCount - 3);
    }
    printf("\n");

    // FUNC
    printf("-- FUNC (Functions) --\n");
    printf("  Functions:        %u\n", dataWin->func.functionCount);
    printf("  Code Locals:      %u\n", dataWin->func.codeLocalsCount);
    if (dataWin->func.functionCount > 0) {
        uint32_t show = dataWin->func.functionCount < 3 ? dataWin->func.functionCount : 3;
        forEachIndexed(Function, fn, idx, dataWin->func.functions, show) {
            printf("    [%u] %s (%u refs)\n", idx, fn->name ? fn->name : "?", fn->occurrences);
        }
        if (dataWin->func.functionCount > 3) printf("    ... and %u more\n", dataWin->func.functionCount - 3);
    }
    printf("\n");

    // STRG
    printf("-- STRG (Strings) --\n");
    printf("  Strings:          %u\n", dataWin->strg.count);
    if (dataWin->strg.count > 0) {
        uint32_t show = dataWin->strg.count < 5 ? dataWin->strg.count : 5;
        repeat(show, i) {
            const char* str = dataWin->strg.strings[i];
            // Truncate long strings for display
            if (str) {
                size_t len = strlen(str);
                if (len > 60) {
                    printf("    [%u] \"%.60s...\" (%zu chars)\n", i, str, len);
                } else {
                    printf("    [%u] \"%s\"\n", i, str);
                }
            } else {
                printf("    [%u] (null)\n", i);
            }
        }
        if (dataWin->strg.count > 5) printf("    ... and %u more\n", dataWin->strg.count - 5);
    }
    printf("\n");

    // TXTR
    printf("-- TXTR (Textures) --\n");
    printf("  Textures:         %u\n", dataWin->txtr.count);
    if (dataWin->txtr.count > 0) {
        forEachIndexed(Texture, tex, idx, dataWin->txtr.textures, dataWin->txtr.count) {
            printf("    [%u] offset=0x%08X size=%u bytes\n", idx, tex->blobOffset, tex->blobSize);
        }
    }
    printf("\n");

    // AUDO
    printf("-- AUDO (Audio) --\n");
    printf("  Audio Entries:    %u\n", dataWin->audo.count);
    if (dataWin->audo.count > 0) {
        uint32_t show = dataWin->audo.count < 3 ? dataWin->audo.count : 3;
        forEachIndexed(AudioEntry, ae, idx, dataWin->audo.entries, show) {
            printf("    [%u] offset=0x%08X size=%u bytes\n", idx, ae->dataOffset, ae->dataSize);
        }
        if (dataWin->audo.count > 3) printf("    ... and %u more\n", dataWin->audo.count - 3);
    }
    printf("\n");

    printf("-- Room Instances --\n");
    forEach(Room, room, dataWin->room.rooms, dataWin->room.count) {
        printf("Room %s\n", room->name);

        forEachIndexed(RoomGameObject, roomGameObject, idx, room->gameObjects, room->gameObjectCount) {
            int32_t objectDefinitionId = roomGameObject->objectDefinition;
            GameObject* objectDefinition = &dataWin->objt.objects[objectDefinitionId];
            printf("  Object %d (%s, x=%d, y=%d)\n", objectDefinitionId, objectDefinition->name, roomGameObject->x, roomGameObject->y);
        }
    }

    // Overall summary
    printf("===== DataWin parse complete =====\n");
}
