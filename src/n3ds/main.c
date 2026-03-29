#define NULL ((void *)0)

// src/n3ds/main.c

#include "../data_win.h"
#include "../vm.h"

#include <citro2d.h>
#include <3ds.h>

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <malloc.h>

#include "runner_keyboard.h"
#include "runner.h"
#include "input_recording.h"

// TODO: IMPLIMENT THIS!!!
#include "n3ds_renderer.h"

#include "n3ds_file_system.h"
#include "n3ds_audio_system.h"
#include "stb_ds.h"
#include "stb_image_write.h"
#include "profiler.h"

#include "utils.h"

#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>

typedef struct
{
    u32 hwKey;  // 3DS button
    int gmlKey; // GML keycode
    bool held;  // currently held this frame
} KeyMap;

// ===[ COMMAND LINE ARGUMENTS ]===
typedef struct
{
    int key;
    // We need this dummy value, think that the ds_map is like a Java HashMap NOT a HashSet
    // (Which is funny, because in Java HashSets are backed by HashMaps lol)
    bool value;
} FrameSetEntry;

typedef struct
{
    const char *dataWinPath;
    const char *screenshotPattern;
    FrameSetEntry *screenshotFrames;
    FrameSetEntry *dumpFrames;
    FrameSetEntry *dumpJsonFrames;
    const char *dumpJsonFilePattern;
    StringBooleanEntry *varReadsToBeTraced;
    StringBooleanEntry *varWritesToBeTraced;
    StringBooleanEntry *functionCallsToBeTraced;
    StringBooleanEntry *alarmsToBeTraced;
    StringBooleanEntry *instanceLifecyclesToBeTraced;
    StringBooleanEntry *eventsToBeTraced;
    StringBooleanEntry *opcodesToBeTraced;
    StringBooleanEntry *stackToBeTraced;
    StringBooleanEntry *disassemble;
    StringBooleanEntry *tilesToBeTraced;
    bool headless;
    bool traceFrames;
    bool printRooms;
    bool printDeclaredFunctions;
    int exitAtFrame;
    double speedMultiplier;
    int seed;
    bool hasSeed;
    bool debug;
    bool traceEventInherited;
    const char *recordInputsPath;
    const char *playbackInputsPath;
} CommandLineArgs;

static void freeCommandLineArgs(CommandLineArgs *args)
{
    hmfree(args->screenshotFrames);
    hmfree(args->dumpFrames);
    hmfree(args->dumpJsonFrames);
    shfree(args->varReadsToBeTraced);
    shfree(args->varWritesToBeTraced);
    shfree(args->functionCallsToBeTraced);
    shfree(args->alarmsToBeTraced);
    shfree(args->instanceLifecyclesToBeTraced);
    shfree(args->eventsToBeTraced);
    shfree(args->opcodesToBeTraced);
    shfree(args->stackToBeTraced);
    shfree(args->disassemble);
    shfree(args->tilesToBeTraced);
}

// ===[ SCREENSHOT ]===
/*
static void captureScreenshot(const char* filenamePattern, int frameNumber, int width, int height) {
    char filename[512];
    snprintf(filename, sizeof(filename), filenamePattern, frameNumber);

    int stride = width * 4;
    unsigned char* pixels = safeMalloc(stride * height);
    if (pixels == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory for screenshot (%dx%d)\n", width, height);
        return;
    }

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // OpenGL reads bottom-to-top, but PNG is top-to-bottom.
    // Use stb's negative stride trick: point to the last row and use a negative stride to flip vertically.
    unsigned char* lastRow = pixels + (height - 1) * stride;
    stbi_write_png(filename, width, height, 4, lastRow, -stride);

    free(pixels);
    printf("Screenshot saved: %s\n", filename);
}
*/

static void cleanup(Runner *runner, VMContext *vm, DataWin *dataWin, Renderer *renderer, InputRecording *inputRec)
{
    // Save & free input recording if active
    if (inputRec)
    {
        if (inputRec->isRecording)
            InputRecording_save(inputRec);
        InputRecording_free(inputRec);
        inputRec = NULL;
    }

    if (runner && runner->audioSystem)
    {
        runner->audioSystem->vtable->destroy(runner->audioSystem);
        runner->audioSystem = NULL;
    }

    // Free game/app objects first
    if (runner)
        Runner_free(runner);
    if (vm)
        VM_free(vm);
    if (dataWin)
        DataWin_free(dataWin);

    // Destroy renderer last, before shutting down C2D/C3D
    if (renderer && renderer->vtable)
        renderer->vtable->destroy(renderer);

    // Shut down libraries
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    fsExit();
}

// ===[ KEYBOARD INPUT ]===

static int32_t glfwKeyToGml(int glfwKey)
{
    // TODO: Replace with Citro2d input handling

    /*
    // Letters: GLFW_KEY_A (65) -> 65 (same as GML)
    if (glfwKey >= GLFW_KEY_A && glfwKey <= GLFW_KEY_Z) return glfwKey;
    // Numbers: GLFW_KEY_0 (48) -> 48
    if (glfwKey >= GLFW_KEY_0 && glfwKey <= GLFW_KEY_9) return glfwKey;
    // Special keys need mapping
    switch (glfwKey) {
        case GLFW_KEY_ESCAPE:        return VK_ESCAPE;
        case GLFW_KEY_ENTER:         return VK_ENTER;
        case GLFW_KEY_TAB:           return VK_TAB;
        case GLFW_KEY_BACKSPACE:     return VK_BACKSPACE;
        case GLFW_KEY_SPACE:         return VK_SPACE;
        case GLFW_KEY_LEFT_SHIFT:
        case GLFW_KEY_RIGHT_SHIFT:   return VK_SHIFT;
        case GLFW_KEY_LEFT_CONTROL:
        case GLFW_KEY_RIGHT_CONTROL: return VK_CONTROL;
        case GLFW_KEY_LEFT_ALT:
        case GLFW_KEY_RIGHT_ALT:     return VK_ALT;
        case GLFW_KEY_UP:            return VK_UP;
        case GLFW_KEY_DOWN:          return VK_DOWN;
        case GLFW_KEY_LEFT:          return VK_LEFT;
        case GLFW_KEY_RIGHT:         return VK_RIGHT;
        case GLFW_KEY_F1:            return VK_F1;
        case GLFW_KEY_F2:            return VK_F2;
        case GLFW_KEY_F3:            return VK_F3;
        case GLFW_KEY_F4:            return VK_F4;
        case GLFW_KEY_F5:            return VK_F5;
        case GLFW_KEY_F6:            return VK_F6;
        case GLFW_KEY_F7:            return VK_F7;
        case GLFW_KEY_F8:            return VK_F8;
        case GLFW_KEY_F9:            return VK_F9;
        case GLFW_KEY_F10:           return VK_F10;
        case GLFW_KEY_F11:           return VK_F11;
        case GLFW_KEY_F12:           return VK_F12;
        case GLFW_KEY_INSERT:        return VK_INSERT;
        case GLFW_KEY_DELETE:        return VK_DELETE;
        case GLFW_KEY_HOME:          return VK_HOME;
        case GLFW_KEY_END:           return VK_END;
        case GLFW_KEY_PAGE_UP:       return VK_PAGEUP;
        case GLFW_KEY_PAGE_DOWN:     return VK_PAGEDOWN;
        default:                     return -1; // Unknown
    }
    */

    return -1;
}

static InputRecording *globalInputRecording = NULL;

/*
static void keyCallback(C3D_RenderTarget* window, int key, int scancode, int action, int mods) {
    (void) scancode; (void) mods;
    Runner* runner = (Runner*) glfwGetWindowUserPointer(window);
    // During playback, suppress real keyboard input (window events like close still work)
    if (InputRecording_isPlaybackActive(globalInputRecording)) return;
    int32_t gmlKey = glfwKeyToGml(key);
    if (0 > gmlKey) return;
    if (action == GLFW_PRESS) RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
    else if (action == GLFW_RELEASE) RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
    // GLFW_REPEAT is ignored (GML doesn't use key repeat)
}
*/

static void LogToSD(const char *text)
{
    // Open file in append mode on SD card
    FILE *f = fopen("sdmc:/cinnamon/log.txt", "a");
    if (f)
    {
        fprintf(f, "%s\n", text);
        fclose(f);
    }
}

void ShowErrorAndExit(const char *msg)
{
    // Flush log to SD card before touching the display — if gfx is already
    // initialised this is a no-op at the driver level but keeps our log intact.
    fflush(stdout);

    // init display (safe to call even if already initialized in some contexts,
    // but note: if this is called before gfxInitDefault in the early boot path
    // the first gfxInitDefault wins; calling it twice is a no-op in libctru)
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);

    C2D_TextBuf buf = C2D_TextBufNew(4096);
    C2D_Text text;
    C2D_TextParse(&text, buf, msg);
    C2D_TextOptimize(&text);

    while (aptMainLoop())
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(top);

        C2D_DrawText(&text,
                     C2D_WithColor,
                     8.0f, 8.0f,
                     0.0f,
                     0.5f, 0.5f,
                     C2D_Color32(255, 255, 255, 255));

        C3D_FrameEnd(0);
    }

    // cleanup
    C2D_TextBufDelete(buf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();

    cleanup(NULL, NULL, NULL, NULL, globalInputRecording);

    exit(0);
}

void list(const char *path, int depth)
{
    DIR *dir = opendir(path);
    if (!dir)
        return;

    struct dirent *entry;

    while ((entry = readdir(dir)))
    {
        // skip . and ..
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;

        // indentation
        for (int i = 0; i < depth; i++)
            printf("  ");

        printf("%s\n", entry->d_name);

        // build full path
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);

        // check if directory
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
        {
            list(full, depth + 1);
        }
    }

    closedir(dir);
}

// ===[ LOADING BAR ]===
// Must be at file scope — GCC nested functions use stack trampolines to capture
// locals, but trampolines require an executable stack.  ARM/3DS marks the stack
// non-executable, so a nested function callback would fault immediately on real
// hardware.  Using a file-scope static + void* userData avoids the trampoline.

typedef struct
{
    C3D_RenderTarget *top;
    C2D_TextBuf textBuf;
    C2D_Text *text;
    int lastChunkIndex;
} LoadingBarState;

static void progressCb(const char *chunkName, int chunkIndex, int totalChunks,
                       DataWin *dw, void *userData)
{
    (void)dw;
    LoadingBarState *s = (LoadingBarState *)userData;

    // Skip duplicate index — DataWin_parse calls once per chunk
    if (chunkIndex == s->lastChunkIndex)
        return;
    s->lastChunkIndex = chunkIndex;

    char label[64];
    snprintf(label, sizeof(label), "Loading... %.4s (%d/%d)",
             chunkName, chunkIndex + 1, totalChunks > 0 ? totalChunks : 1);

    C2D_TextBufClear(s->textBuf);
    C2D_TextParse(s->text, s->textBuf, label);
    C2D_TextOptimize(s->text);

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(s->top, C2D_Color32(20, 20, 40, 255));
    C2D_SceneBegin(s->top);

    float barX = 20.0f, barY = 110.0f;
    float barW = 360.0f, barH = 20.0f;
    // Background track
    C2D_DrawRectSolid(barX, barY, 0.5f, barW, barH, C2D_Color32(60, 60, 80, 255));
    // Fill
    float progress = (totalChunks > 0)
                         ? (float)(chunkIndex + 1) / (float)totalChunks
                         : 0.0f;
    if (progress > 1.0f)
        progress = 1.0f;
    C2D_DrawRectSolid(barX, barY, 0.6f, barW * progress, barH, C2D_Color32(80, 160, 255, 255));
    // Label above bar
    C2D_DrawText(s->text, C2D_WithColor, barX, barY - 18.0f, 0.7f,
                 0.5f, 0.5f, C2D_Color32(255, 255, 255, 255));

    C3D_FrameEnd(0);
}

// ===[ MAIN ]===
int main(int argc, char *argv[])
{
    fsInit();
    romfsInit();
    gfxInitDefault();

    freopen("sdmc:/cinnamon/full_log.txt", "w", stdout); // create/overwrite
    setvbuf(stdout, NULL, _IOLBF, 0);

    // redirect stderr to the same file without truncating
    freopen("sdmc:/cinnamon/full_log.txt", "a", stderr); // append
    setvbuf(stderr, NULL, _IOLBF, 0);

    list("sdmc:/cinnamon", 0);

    LogToSD("Application Started");
    // args.dataWinPath = "sdmc:/cinnamon/data.win";
    // parseCommandLineArgs(&args, argc, argv);

    printf("Checking if %s exists...\n", "romfs:/cinnamon/data.win");

    LogToSD("Loading data.win...");

    FILE *f = fopen("romfs:/cinnamon/data.win", "rb");
    if (f)
    {
        printf("File %s found.\n", "romfs:/cinnamon/data.win");
        fclose(f);
    }
    else
    {
        fprintf(stderr, "Error: data.win not found at romfs:/cinnamon/data.win\n");
        LogToSD("Error: data.win not found at romfs:/cinnamon/data.win");
        ShowErrorAndExit("An error has occurred.\nPlease make sure data.win is located at: cinnamon/data.win\non your SD card!\nPress START to exit.");
        return 0;
    }

    // ===[ Graphics init — done BEFORE DataWin_parse so we can show a loading bar ]===
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    gfxSet3D(true);
    // consoleInit(GFX_BOTTOM, NULL);

    LogToSD("Initialized 3DS libraries (pre-parse)");

    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C2D_TextBuf loadingTextBuf = C2D_TextBufNew(256);
    C2D_Text loadingText;

    LoadingBarState lbState = {
        .top = top,
        .textBuf = loadingTextBuf,
        .text = &loadingText,
        .lastChunkIndex = -1,
    };

    printf("Loading %s...\n", "romfs:/cinnamon/data.win");

    DataWin *dataWin = DataWin_parse(
        "romfs:/cinnamon/data.win",
        (DataWinParserOptions){
            .parseGen8 = true,
            .parseOptn = true,
            .parseLang = true,
            .parseExtn = true,
            .parseSond = true,
            .parseAgrp = true,
            .parseSprt = true,
            .parseBgnd = true,
            .parsePath = true,
            .parseScpt = true,
            .parseGlob = true,
            .parseShdr = true,
            .parseFont = true,
            .parseTmln = true,
            .parseObjt = true,
            .parseRoom = true,
            .parseTpag = true,
            .parseCode = true,
            .parseVari = true,
            .parseFunc = true,
            .parseStrg = true,
            .parseTxtr = true,
            .parseAudo = true,
            .skipLoadingPreciseMasksForNonPreciseSprites = true,
            .progressCallback = progressCb,
            .progressCallbackUserData = &lbState,
        });

    // Loading bar resources are no longer needed
    C2D_TextBufDelete(loadingTextBuf);

    Gen8 *gen8 = &dataWin->gen8;
    printf("Loaded \"%s\" (%d) successfully!\n", gen8->name, gen8->gameID);
    LogToSD("Loaded data.win successfully!");

    // TODO: Replace with N3DS compatible print.
    /*
    #ifdef __linux__
        {
            struct mallinfo2 mi = mallinfo2();
            printf("Memory after data.win parsing: used=%zu bytes (%.1f KB)\n", mi.uordblks, mi.uordblks / 1024.0f);
        }
    #endif
    */

    // TODO: Find a use for the display name? It might not actually be useful since there is no "window"
    // in the traditional sense on the 3DS, but maybe it could be used for something cool, streetpass?
    /*
    // Build window title
    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "cinnamon - %s", gen8->displayName);
    */

    LogToSD("LOADING VM...");

    // Initialize VM
    VMContext *vm = VM_create(dataWin);

    LogToSD("VM OK");

    /*
    if (args.hasSeed) {
        srand((unsigned int) args.seed);
        vm->hasFixedSeed = true;
        printf("Using fixed RNG seed: %d\n", args.seed);
        LogToSD("Using fixed RNG seed");
    }

    if (args.printRooms) {
        forEachIndexed(Room, room, idx, dataWin->room.rooms, dataWin->room.count) {
            printf("[%d] %s ()\n", idx, room->name);

            forEachIndexed(RoomGameObject, roomGameObject, idx2, room->gameObjects, room->gameObjectCount) {
                GameObject* gameObject = &dataWin->objt.objects[roomGameObject->objectDefinition];
                printf(
                    "  [%d] %s (x=%d,y=%d,persistent=%d,solid=%d,spriteId=%d,preCreateCode=%d,creationCode=%d)\n",
                    idx2,
                    gameObject->name,
                    roomGameObject->x,
                    roomGameObject->y,
                    gameObject->persistent,
                    gameObject->solid,
                    gameObject->spriteId,
                    roomGameObject->preCreateCode,
                    roomGameObject->creationCode
                );
            }
        }
        VM_free(vm);
        DataWin_free(dataWin);
        return 0;
    }

    if (args.printDeclaredFunctions) {
        repeat(hmlen(vm->funcMap), i) {
            printf("[%d] %s\n", vm->funcMap[i].value, vm->funcMap[i].key);
        }
        VM_free(vm);
        DataWin_free(dataWin);
        return 0;
    }

    if (shlen(args.disassemble) > 0) {
        VM_buildCrossReferences(vm);
        if (shgeti(args.disassemble, "*") >= 0) {
            repeat(dataWin->code.count, i) {
                VM_disassemble(vm, (int32_t) i);
            }
        } else {
            for (ptrdiff_t i = 0; shlen(args.disassemble) > i; i++) {
                const char* name = args.disassemble[i].key;
                ptrdiff_t idx = shgeti(vm->funcMap, (char*) name);
                if (idx >= 0) {
                    VM_disassemble(vm, vm->funcMap[idx].value);
                } else {
                    fprintf(stderr, "Error: Script '%s' not found in funcMap\n", name);
                }
            }
        }
        VM_free(vm);
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 0;
    }
    */

    // Initialize the file system
    // TODO: Replace with sdcard reads and writes
    N3DSFileSystem *n3dsFileSystem = N3DSFileSystem_create("sdmc:/cinnamon/data.win");

    N3DSAudioSystem *n3dsAudio = N3DSAudioSystem_create();
    AudioSystem *audioSystem = (AudioSystem *)n3dsAudio;
    audioSystem->vtable->init(audioSystem, dataWin, (FileSystem *)n3dsFileSystem);

    // Initialize the runner
    LogToSD("LOADING RUNNER");
    Runner *runner = Runner_create(dataWin, vm, (FileSystem *)n3dsFileSystem);
    runner->audioSystem = audioSystem;
    // runner->debugMode = args.debug;
    LogToSD("RUNNER OK");

    // Set up input recording/playback (both can be active: playback then continue recording)
    /*
    if (args.playbackInputsPath != NULL) {
        globalInputRecording = InputRecording_createPlayer(args.playbackInputsPath, args.recordInputsPath);
    } else if (args.recordInputsPath != NULL) {
        globalInputRecording = InputRecording_createRecorder(args.recordInputsPath);
    }
    shcopyFromTo(args.varReadsToBeTraced, runner->vmContext->varReadsToBeTraced);
    shcopyFromTo(args.varWritesToBeTraced, runner->vmContext->varWritesToBeTraced);
    shcopyFromTo(args.functionCallsToBeTraced, runner->vmContext->functionCallsToBeTraced);
    shcopyFromTo(args.alarmsToBeTraced, runner->vmContext->alarmsToBeTraced);
    shcopyFromTo(args.instanceLifecyclesToBeTraced, runner->vmContext->instanceLifecyclesToBeTraced);
    shcopyFromTo(args.eventsToBeTraced, runner->vmContext->eventsToBeTraced);
    shcopyFromTo(args.opcodesToBeTraced, runner->vmContext->opcodesToBeTraced);
    shcopyFromTo(args.stackToBeTraced, runner->vmContext->stackToBeTraced);
    shcopyFromTo(args.tilesToBeTraced, runner->vmContext->tilesToBeTraced);
    runner->vmContext->traceEventInherited = args.traceEventInherited;
    */

    // Init GLFW
    /*
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }
    */

    // We will not be headless on the 3DS.
    /*
    if (args.headless) {
        C3D_RenderTargetHint(GLFW_VISIBLE, GLFW_FALSE);
    }
    */

    // Initalize sdmc access
    // (sdmcInit was already called before DataWin_parse for the loading bar)

    // gfxInitDefault / C3D_Init / C2D_Init / C2D_Prepare / consoleInit were
    // already called before DataWin_parse so the loading bar could be shown.
    // They must NOT be called again here.

    LogToSD("Initalized 3DS Libaries");

    // TODO: do something with (int) gen8->defaultWindowWidth, (int) gen8->defaultWindowHeight
    // To make it fit the screen correctly. REMEMBER TO PUT THIS IN A FUNCTION
    // FOR CALLING AFTER INITALIZATION
    // C3D_RenderTarget* window = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);

    /*
    C3D_RenderTarget* window = glfwCreateWindow((int) gen8->defaultWindowWidth, (int) gen8->defaultWindowHeight, windowTitle, NULL, NULL);
    if (window == NULL) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0); // Disable v-sync, we control timing ourselves

    */

    // This is all just unneeded for now
    // TODO: Look this over and see if we are
    // doing this in the N3DS code too!

    /*
    // Load OpenGL function pointers via GLAD
    if (!gladLoadGLLoader((GLADloadproc) glfwGetProcAddress)) {
        fprintf(stderr, "Failed to initialize GLAD\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    // Initialize the renderer
    Renderer* renderer = GLRenderer_create();
    renderer->vtable->init(renderer, dataWin);
    runner->renderer = renderer;

    // Set up keyboard input
    glfwSetWindowUserPointer(window, runner);
    glfwSetKeyCallback(window, keyCallback);
    */

    // Initialize the renderer
    Renderer *renderer = CRenderer3DS_create();
    renderer->vtable->init(renderer, dataWin);
    runner->renderer = renderer;

    CRenderer3DS *C = (CRenderer3DS *)renderer;

    C->top = top;

    LogToSD("Initalizing first room...");
    // Ensure SD cache directory exists before the renderer begins decoding textures
    mkdir("sdmc:/cinnamon/cache", 0777);
    // Initialize the first room and fire Game Start / Room Start events
    Runner_initFirstRoom(runner);
    LogToSD("First room loaded!");

    KeyMap keymap[] = {
        {KEY_UP, VK_UP},       // Move up
        {KEY_DOWN, VK_DOWN},   // Move down
        {KEY_LEFT, VK_LEFT},   // Move left
        {KEY_RIGHT, VK_RIGHT}, // Move right
        {KEY_A, VK_Z},         // A button triggers Z (confirm)
        {KEY_B, VK_X},         // B button triggers X (cancel)
        {KEY_X, VK_C},         // X button triggers C (menu)
        {KEY_ZL, VK_SHIFT},    // L button triggers Left Shift
        {KEY_ZR, VK_CONTROL},  // R button triggers Right Shift
    };

    // Main loop
    bool debugPaused = false;
    double lastFrameTimeMs = (double)osGetTime();
    uint64_t lastAudioUpdateMs = (uint64_t)osGetTime();
    // Lag profiling state machine: NORMAL(0) -> LAGGING(1) -> RECOVERING(2)
    int lagState = 0;

    CinnamonProfiler_init((uint32_t)CINNAMON_PROFILE_REPORT_EVERY, (double)CINNAMON_PROFILE_SPIKE_MS);

    while (aptMainLoop())
    {
        CinnamonProfiler_beginFrame((uint64_t)runner->frameCount);

        CinnamonProfiler_beginSection(CINNAMON_PROFILE_INPUT);
        // Clear last frame's pressed/released state, then poll new input events
        RunnerKeyboard_beginFrame(runner->keyboard);
        hidScanInput(); // glfwPollEvents();

        u32 kDown = hidKeysDown();
        u32 kUp = hidKeysUp();
        u32 kHeld = hidKeysHeld();

        bool shiftHeld = (kHeld & KEY_L) != 0;
        u32 debugMask = KEY_RIGHT | KEY_LEFT | KEY_X | KEY_A | KEY_B | KEY_L;

        // update held state + fire key events
        for (int i = 0; i < (int)(sizeof(keymap) / sizeof(keymap[0])); i++)
        {
            keymap[i].held = (kHeld & keymap[i].hwKey) != 0;

            if (shiftHeld && (keymap[i].hwKey & debugMask))
                continue;

            if (kDown & keymap[i].hwKey)
            {
                RunnerKeyboard_onKeyDown(runner->keyboard, keymap[i].gmlKey);
            }
            if (kUp & keymap[i].hwKey)
            {
                RunnerKeyboard_onKeyUp(runner->keyboard, keymap[i].gmlKey);
            }
        }

        if (shiftHeld)
        {
            DataWin *dw = runner->dataWin;

            if (kDown & KEY_RIGHT)
            {
                int32_t nextPos = runner->currentRoomOrderPosition + 1;
                if ((int32_t)dw->gen8.roomOrderCount > nextPos)
                {
                    runner->pendingRoom = dw->gen8.roomOrder[nextPos];
                    printf("Debug: next room -> %s\n", dw->room.rooms[runner->pendingRoom].name);
                }
                else
                {
                    printf("Debug: already at last room\n");
                }
            }

            if (kDown & KEY_LEFT)
            {
                int32_t prevPos = runner->currentRoomOrderPosition - 1;
                if (prevPos >= 0)
                {
                    runner->pendingRoom = dw->gen8.roomOrder[prevPos];
                    printf("Debug: prev room -> %s\n", dw->room.rooms[runner->pendingRoom].name);
                }
                else
                {
                    printf("Debug: already at first room\n");
                }
            }

            if (kDown & KEY_X)
            {
                int interactVarIndex = shgeti(runner->vmContext->globalVarNameMap, "interact");
                if (interactVarIndex >= 0)
                {
                    int32_t interactVarId = runner->vmContext->globalVarNameMap[interactVarIndex].value;
                    runner->vmContext->globalVars[interactVarId] = RValue_makeInt32(0);
                    printf("Debug: global.interact = 0\n");
                }
                else
                {
                    printf("Debug: global.interact not found\n");
                }
            }

            if (kDown & KEY_A)
            {
                int32_t battleObjectIndex = -1;
                for (int32_t objectIndex = 0; objectIndex < (int32_t)dw->objt.count; objectIndex++)
                {
                    if (strcmp(dw->objt.objects[objectIndex].name, "obj_battleblcon") == 0)
                    {
                        battleObjectIndex = objectIndex;
                        break;
                    }
                }

                if (battleObjectIndex >= 0)
                {
                    Runner_createInstance(runner, 0.0, 0.0, battleObjectIndex);
                    printf("Debug: created obj_battleblcon\n");
                }
                else
                {
                    printf("Debug: obj_battleblcon not found\n");
                }
            }

            if (kDown & KEY_B)
            {
                int phasingVarIndex = shgeti(runner->vmContext->globalVarNameMap, "phasing");
                if (phasingVarIndex >= 0)
                {
                    int32_t phasingVarId = runner->vmContext->globalVarNameMap[phasingVarIndex].value;
                    int32_t newPhasingValue = RValue_toInt32(runner->vmContext->globalVars[phasingVarId]) ? 0 : 1;
                    runner->vmContext->globalVars[phasingVarId] = RValue_makeInt32(newPhasingValue);
                    printf("Debug: global.phasing = %d\n", newPhasingValue);
                }
                else
                {
                    printf("Debug: global.phasing not found\n");
                }
            }
        }

        // Process input recording/playback (must happen after glfwPollEvents, before Runner_step)
        InputRecording_processFrame(globalInputRecording, runner->keyboard, runner->frameCount);
        CinnamonProfiler_endSection(CINNAMON_PROFILE_INPUT);

        // Debug key bindings
        // TODO: Add these to buttons
        /*
        if (runner->debugMode) {
            // Pause
            if (RunnerKeyboard_checkPressed(runner->keyboard, 'P')) {
                debugPaused = !debugPaused;
                fprintf(stderr, "Debug: %s\n", debugPaused ? "Paused" : "Resumed");
            }

            // Go to next room
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEUP)) {
                DataWin* dw = runner->dataWin;
                if ((int32_t) dw->gen8.roomOrderCount > runner->currentRoomOrderPosition + 1) {
                    int32_t nextIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition + 1];
                    runner->pendingRoom = nextIdx;
                    fprintf(stderr, "Debug: Going to next room -> %s\n", dw->room.rooms[nextIdx].name);
                }
            }

            // Go to previous room
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEDOWN)) {
                DataWin* dw = runner->dataWin;
                if (runner->currentRoomOrderPosition > 0) {
                    int32_t prevIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition - 1];
                    runner->pendingRoom = prevIdx;
                    fprintf(stderr, "Debug: Going to previous room -> %s\n", dw->room.rooms[prevIdx].name);
                }
            }

            // Dump runner state to console
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F12)) {
                fprintf(stderr, "Debug: Dumping runner state at frame %d\n", runner->frameCount);
                Runner_dumpState(runner);
            }

            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F11)) {
                fprintf(stderr, "Debug: Dumping runner state at frame %d\n", runner->frameCount);
                char* json = Runner_dumpStateJson(runner);

                if (args.dumpJsonFilePattern != NULL) {
                    char filename[512];
                    snprintf(filename, sizeof(filename), args.dumpJsonFilePattern, runner->frameCount);
                    FILE* f = fopen(filename, "w");
                    if (f != NULL) {
                        fwrite(json, 1, strlen(json), f);
                        fputc('\n', f);
                        fclose(f);
                        printf("JSON dump saved: %s\n", filename);
                    } else {
                        fprintf(stderr, "Error: Could not write JSON dump to '%s'\n", filename);
                    }
                } else {
                    printf("%s\n", json);
                }

                free(json);
            }

            // Reset global interact state because I HATE when I get stuck while moving through rooms
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F10)) {
                int32_t interactVarId = shget(runner->vmContext->globalVarNameMap, "interact");

                runner->vmContext->globalVars[interactVarId] = RValue_makeInt32(0);
                printf("Changed global.interact [%d] value!\n", interactVarId);
            }
        }
        */

        // Run the game step if the game is paused
        bool shouldStep = true;
        if (runner->debugMode && debugPaused)
        {
            shouldStep = RunnerKeyboard_checkPressed(runner->keyboard, 'O');
            if (shouldStep)
                fprintf(stderr, "Debug: Frame advance (frame %d)\n", runner->frameCount);
        }

        double frameStartTime = 0;

        if (shouldStep)
        {
            /*
            if (args.traceFrames) {
                frameStartTime = svcGetSystemTick();
                fprintf(stderr, "Frame %d (Start)\n", runner->frameCount);
            }
            */

            // Run one game step (Begin Step, Keyboard, Alarms, Step, End Step, room transitions)
            CinnamonProfiler_beginSection(CINNAMON_PROFILE_STEP);
            Runner_step(runner);
            CinnamonProfiler_endSection(CINNAMON_PROFILE_STEP);

            /*
            // Dump full runner state if this frame was requested
            if (hmget(args.dumpFrames, runner->frameCount)) {
                Runner_dumpState(runner);
            }

            // Dump runner state as JSON if this frame was requested
            if (hmget(args.dumpJsonFrames, runner->frameCount)) {
                char* json = Runner_dumpStateJson(runner);
                if (args.dumpJsonFilePattern != NULL) {
                    char filename[512];
                    snprintf(filename, sizeof(filename), args.dumpJsonFilePattern, runner->frameCount);
                    FILE* f = fopen(filename, "w");
                    if (f != NULL) {
                        fwrite(json, 1, strlen(json), f);
                        fputc('\n', f);
                        fclose(f);
                        printf("JSON dump saved: %s\n", filename);
                    } else {
                        fprintf(stderr, "Error: Could not write JSON dump to '%s'\n", filename);
                    }
                } else {
                    printf("%s\n", json);
                }
                free(json);
            }
            */
        }

        if (runner->audioSystem != NULL)
        {
            CinnamonProfiler_beginSection(CINNAMON_PROFILE_AUDIO);
            uint64_t nowMs = (uint64_t)osGetTime();
            float deltaTime = (float)(nowMs - lastAudioUpdateMs) / 1000.0f;
            if (deltaTime < 0.0f)
                deltaTime = 0.0f;
            if (deltaTime > 0.1f)
                deltaTime = 0.1f;
            lastAudioUpdateMs = nowMs;
            runner->audioSystem->vtable->update(runner->audioSystem, deltaTime);
            CinnamonProfiler_endSection(CINNAMON_PROFILE_AUDIO);
        }

        Room *activeRoom = runner->currentRoom;

        // Query actual framebuffer size (differs from window size on Wayland with fractional scaling)
        int fbWidth, fbHeight;
        // glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        fbWidth = (int)gen8->defaultWindowWidth;
        fbHeight = (int)gen8->defaultWindowHeight;

        // Clear the default framebuffer (window background) to black
        // TODO: Make sure this is correct (it should be)
        /*
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        */
        // C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        // C2D_TargetClear(window, C2D_Color32f(0.0f, 0.0f, 0.0f, 1.0f));
        // C2D_SceneBegin(window);

        int32_t gameW = (int32_t)gen8->defaultWindowWidth;
        int32_t gameH = (int32_t)gen8->defaultWindowHeight;

        CinnamonProfiler_beginSection(CINNAMON_PROFILE_RENDER);

        // Begin the frame via renderer vtable (if provided). This pairs with endFrame below
        if (runner->renderer != NULL && runner->renderer->vtable != NULL && runner->renderer->vtable->beginFrame != NULL)
        {
            runner->renderer->vtable->beginFrame(runner->renderer, C2D_Color32(BGR_R(runner->backgroundColor), BGR_G(runner->backgroundColor), BGR_B(runner->backgroundColor), 255), runner->currentRoom->speed, gameW, gameH, fbWidth, fbHeight);
        }

        // NOTE: do NOT call beginFrame a second time here — CBeginFrame calls C3D_FrameBegin
        // internally, and calling it twice per loop causes GPU command-buffer corruption on
        // real hardware (the duplicate C3D_FrameBegin was the source of the hardware crash).

        // Clear FBO with room background color
        if (runner->drawBackgroundColor)
        {
            int rInt = BGR_R(runner->backgroundColor);
            int gInt = BGR_G(runner->backgroundColor);
            int bInt = BGR_B(runner->backgroundColor);
            // glClearColor(rInt / 255.0f, gInt / 255.0f, bInt / 255.0f, 1.0f);
            // C2D_TargetClear(window, C2D_Color32f(rInt / 255.0f, gInt / 255.0f, bInt / 255.0f, 1.0f));
        }
        else
        {
            // glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        }
        // glClear(GL_COLOR_BUFFER_BIT);

        // Render each enabled view (or a default full-screen view if views are disabled)
        bool viewsEnabled = (activeRoom->flags & 1) != 0;
        bool anyViewRendered = false;

        // TODO: Render one of the views to the bottom screen!
        if (viewsEnabled)
        {
            repeat(8, vi)
            {
                if (!activeRoom->views[vi].enabled)
                    continue;

                int32_t viewX = activeRoom->views[vi].viewX;
                int32_t viewY = activeRoom->views[vi].viewY;
                int32_t viewW = activeRoom->views[vi].viewWidth;
                int32_t viewH = activeRoom->views[vi].viewHeight;
                int32_t portX = activeRoom->views[vi].portX;
                int32_t portY = activeRoom->views[vi].portY;
                int32_t portW = activeRoom->views[vi].portWidth;
                int32_t portH = activeRoom->views[vi].portHeight;
                float viewAngle = runner->viewAngles[vi];

                runner->viewCurrent = vi;
                // TODO: Add renderer, see first comment about  renderer
                renderer->vtable->beginView(renderer, viewX, viewY, viewW, viewH, portX * 400 / gameW, portY * 240 / gameH, portW * 400 / gameW, portH * 240 / gameH, viewAngle, vi);

                Runner_draw(runner);

                renderer->vtable->endView(renderer);
                anyViewRendered = true;
            }
        }

        if (!anyViewRendered)
        {
            // No views enabled or views disabled: render with default full-screen view
            runner->viewCurrent = 0;
            renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, 0, 0, gameW, gameH, 0.0f, 0);
            Runner_draw(runner);
            renderer->vtable->endView(renderer);
        }

        // Reset view_current to 0 so non-Draw events (Step, Alarm, Create) see view_current = 0
        runner->viewCurrent = 0;

        CinnamonProfiler_endSection(CINNAMON_PROFILE_RENDER);

        // Capture screenshot if this frame matches a requested frame
        /*
        bool shouldScreenshot = hmget(args.screenshotFrames, runner->frameCount);

        if (shouldScreenshot) {
            // Bind FBO so glReadPixels reads from the game's native-resolution texture
            GLRenderer* gl = (GLRenderer*) renderer;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->fbo);
            captureScreenshot(args.screenshotPattern, runner->frameCount, (int) gen8->defaultWindowWidth, (int) gen8->defaultWindowHeight);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        }

        if (args.exitAtFrame >= 0 && runner->frameCount >= args.exitAtFrame) {
            printf("Exiting at frame %d (--exit-at-frame)\n", runner->frameCount);
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        */

        /*
        if (shouldStep && args.traceFrames) {
            double frameElapsedMs = (svcGetSystemTick() - frameStartTime) * 1000.0;
            fprintf(stderr, "Frame %d (End, %.2f ms)\n", runner->frameCount, frameElapsedMs);
        }
        */

        // TODO: Find a way to swap buffers in citro3d
        // glfwSwapBuffers(window);

        // End the frame via renderer vtable when possible to avoid unmatched C3D_FrameEnd
        CinnamonProfiler_beginSection(CINNAMON_PROFILE_PRESENT);
        if (runner->renderer != NULL && runner->renderer->vtable != NULL && runner->renderer->vtable->endFrame != NULL)
        {
            runner->renderer->vtable->endFrame(runner->renderer);
        }
        else
        {
            C3D_FrameEnd(0);
        }
        CinnamonProfiler_endSection(CINNAMON_PROFILE_PRESENT);

        // Limit frame rate to room speed
        CinnamonProfiler_beginSection(CINNAMON_PROFILE_THROTTLE);
        if (runner->currentRoom->speed > 0)
        {
            // Pace using the room's target speed only. If we overrun the budget,
            // don't accumulate delay debt across future frames.
            double frameStartMs = lastFrameTimeMs;
            double targetFrameMs = 1000.0 / (double)runner->currentRoom->speed;
            double nextFrameTimeMs = frameStartMs + targetFrameMs;
            double nowMs = (double)osGetTime();
            double remainingMs = nextFrameTimeMs - nowMs;

            if (remainingMs > 1.5)
            {
                struct timespec ts = {
                    .tv_sec = 0,
                    .tv_nsec = (long)((remainingMs - 0.5) * 1000000.0)};
                nanosleep(&ts, NULL);
            }

            while ((double)osGetTime() < nextFrameTimeMs)
            {
                // Spin-wait for the final sub-millisecond slice.
            }

            nowMs = (double)osGetTime();

            // Lag state machine: enable per-function timing when fps < 27, stop when >= 30.
            {
                double totalFrameMs = nowMs - frameStartMs;
                double decimateThresholdMs = targetFrameMs / 0.8;

                if (totalFrameMs > decimateThresholdMs)
                {
                    runner->drawSpriteDecimationEnabled = true;
                    runner->drawSpriteDecimationPhase ^= 1u;
                }
                else
                {
                    runner->drawSpriteDecimationEnabled = false;
                    runner->drawSpriteDecimationPhase = 0;
                }

                // Activate lag mode when frame time exceeds targetFrameMs / 0.6
                // (i.e. running at < 60% of target speed).
                // Deactivate when frame time drops back to targetFrameMs / 0.7
                // (i.e. recovering to >= 70% of target speed) — hysteresis prevents thrashing.
                double lagOnThresholdMs  = targetFrameMs / 0.6;
                double lagOffThresholdMs = targetFrameMs / 0.7;
                if (lagState == 0)
                {
                    if (totalFrameMs > lagOnThresholdMs && runner->renderer)
                    {
                        lagState = 1;
                        CRenderer3DS_setLagMode(runner->renderer, true);
                    }
                }
                else if (lagState == 1)
                {
                    if (totalFrameMs <= lagOffThresholdMs && runner->renderer)
                    {
                        lagState = 2;
                        CRenderer3DS_setLagMode(runner->renderer, false);
                    }
                }
                else
                { // recovering
                    if (totalFrameMs > lagOnThresholdMs && runner->renderer)
                    {
                        lagState = 1;
                        CRenderer3DS_setLagMode(runner->renderer, true);
                    }
                    else if (totalFrameMs <= lagOffThresholdMs)
                    {
                        lagState = 0;
                    }
                }
            }

            lastFrameTimeMs = nowMs > nextFrameTimeMs ? nowMs : nextFrameTimeMs;
        }
        else
        {
            lastFrameTimeMs = (double)osGetTime();
        }
        CinnamonProfiler_endSection(CINNAMON_PROFILE_THROTTLE);

        CinnamonProfiler_endFrame();
    }

    // Save input recording if active, then free
    if (globalInputRecording != NULL)
    {
        if (globalInputRecording->isRecording)
        {
            InputRecording_save(globalInputRecording);
        }
        globalInputRecording = NULL;
    }

    // massive cleanup
    cleanup(runner, vm, dataWin, renderer, globalInputRecording);

    printf("Bye! :3\n");
    return 0;
}
