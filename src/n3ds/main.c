#define nullptr ((void*)0)

#include "data_win.h"
#include "vm.h"

// Glad and GLFW is not needed for N3DS
// These will be replaced with Citro2d
//#include <glad/glad.h>
//#include <GLFW/glfw3.h>

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
// #include "gl_renderer.h"

#include "n3ds_file_system.h"
#include "stb_ds.h"
#include "stb_image_write.h"

#include "utils.h"

// ===[ COMMAND LINE ARGUMENTS ]===
typedef struct {
    int key;
    // We need this dummy value, think that the ds_map is like a Java HashMap NOT a HashSet
    // (Which is funny, because in Java HashSets are backed by HashMaps lol)
    bool value;
} FrameSetEntry;

typedef struct {
    const char* dataWinPath;
    const char* screenshotPattern;
    FrameSetEntry* screenshotFrames;
    FrameSetEntry* dumpFrames;
    FrameSetEntry* dumpJsonFrames;
    const char* dumpJsonFilePattern;
    StringBooleanEntry* varReadsToBeTraced;
    StringBooleanEntry* varWritesToBeTraced;
    StringBooleanEntry* functionCallsToBeTraced;
    StringBooleanEntry* alarmsToBeTraced;
    StringBooleanEntry* instanceLifecyclesToBeTraced;
    StringBooleanEntry* eventsToBeTraced;
    StringBooleanEntry* opcodesToBeTraced;
    StringBooleanEntry* stackToBeTraced;
    StringBooleanEntry* disassemble;
    StringBooleanEntry* tilesToBeTraced;
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
    const char* recordInputsPath;
    const char* playbackInputsPath;
} CommandLineArgs;

static void freeCommandLineArgs(CommandLineArgs* args) {
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
    if (pixels == nullptr) {
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

// ===[ KEYBOARD INPUT ]===

static int32_t glfwKeyToGml(int glfwKey) {
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

static InputRecording* globalInputRecording = nullptr;

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

static void LogToSD(const char* text) {
    // Open file in append mode on SD card
    FILE *f = fopen("sdmc:/log.txt", "a");
    if (f) {
        fprintf(f, "%s\n", text);
        fclose(f);
    }
}

void ShowErrorAndExit(const char* msg)
{
    // init
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);

    C2D_TextBuf buf = C2D_TextBufNew(4096);
    C2D_Text text;
    C2D_TextParse(&text, buf, msg);
    C2D_TextOptimize(&text);

    while (aptMainLoop())
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, C2D_Color32(0,0,0,255));
        C2D_SceneBegin(top);

        C2D_DrawText(&text,
            C2D_WithColor,
            8.0f, 8.0f,
            0.0f,
            0.5f, 0.5f,
            C2D_Color32(255,255,255,255)
        );

        C3D_FrameEnd(0);
    }

    // cleanup
    C2D_TextBufDelete(buf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();

    exit(0);
}

// ===[ MAIN ]===
int main(int argc, char* argv[]) {
    fsInit();

    // Flush to SD card every new line (\n)
    setvbuf(stdout, NULL, _IOLBF, 0);

    // send printf to sdcard
    freopen("full_log.txt", "w", stdout);

    LogToSD("Application Started");
    //args.dataWinPath = "sdmc:/cinnamon/data.win";
    //parseCommandLineArgs(&args, argc, argv);

    printf("Loading %s...\n", "sdmc:/cinnamon/data.win");

    LogToSD("Loading data.win...");

    FILE* f = fopen("sdmc:/cinnamon/data.win", "rb");
    if (f) {
        // exists

        fclose(f);
    } else {
        // doesn't exist

        fprintf(stderr, "Error: data.win not found at sdmc:/cinnamon/data.win\n");
        LogToSD("Error: data.win not found at sdmc:/cinnamon/data.win");

        ShowErrorAndExit("An error has occurred.\nPlease make sure data.win is located at: cinnamon/data.win\non your SD card!\nPress START to exit.");

        return 0;
    }

    DataWin* dataWin = DataWin_parse(
        "sdmc:/cinnamon/data.win",
        (DataWinParserOptions) {
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
            .skipLoadingPreciseMasksForNonPreciseSprites = true
        }
    );

    Gen8* gen8 = &dataWin->gen8;
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
    VMContext* vm = VM_create(dataWin);

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
    N3DSFileSystem* n3dsFileSystem = N3DSFileSystem_create("sdmc:/cinnamon/data.win");

    // Initialize the runner
    LogToSD("LOADING RUNNER");
    Runner* runner = Runner_create(dataWin, vm, (FileSystem*) n3dsFileSystem);
    //runner->debugMode = args.debug;
    LogToSD("RUNNER OK");

    // Set up input recording/playback (both can be active: playback then continue recording)
    /*
    if (args.playbackInputsPath != nullptr) {
        globalInputRecording = InputRecording_createPlayer(args.playbackInputsPath, args.recordInputsPath);
    } else if (args.recordInputsPath != nullptr) {
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

    // Initalize romfs access
    romfsInit();

    // initalize gfx
	gfxInitDefault();

    // initalize some Citro2d stuff
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();

    // set the console to display on the bottom screen
	consoleInit(GFX_BOTTOM, NULL);

    LogToSD("Initalized 3DS Libaries");

    // TODO: do something with (int) gen8->defaultWindowWidth, (int) gen8->defaultWindowHeight
    // To make it fit the screen correctly. REMEMBER TO PUT THIS IN A FUNCTION
    // FOR CALLING AFTER INITALIZATION
    C3D_RenderTarget* window = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);

    /*
    C3D_RenderTarget* window = glfwCreateWindow((int) gen8->defaultWindowWidth, (int) gen8->defaultWindowHeight, windowTitle, nullptr, nullptr);
    if (window == nullptr) {
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

    LogToSD("Initalizing first room...");
    // Initialize the first room and fire Game Start / Room Start events
    Runner_initFirstRoom(runner);
    LogToSD("First room loaded!");

    // Main loop
    bool debugPaused = false;
    double lastFrameTime = svcGetSystemTick();
    while (aptMainLoop()) {
        // Clear last frame's pressed/released state, then poll new input events
        RunnerKeyboard_beginFrame(runner->keyboard);
        hidScanInput(); // glfwPollEvents();

        u32 kDown = hidKeysDown(); // Keys that are newly pressed
        u32 kHeld = hidKeysHeld(); // Keys that are currently held down

        // Process input recording/playback (must happen after glfwPollEvents, before Runner_step)
        InputRecording_processFrame(globalInputRecording, runner->keyboard, runner->frameCount);

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

                if (args.dumpJsonFilePattern != nullptr) {
                    char filename[512];
                    snprintf(filename, sizeof(filename), args.dumpJsonFilePattern, runner->frameCount);
                    FILE* f = fopen(filename, "w");
                    if (f != nullptr) {
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
        if (runner->debugMode && debugPaused) {
            shouldStep = RunnerKeyboard_checkPressed(runner->keyboard, 'O');
            if (shouldStep) fprintf(stderr, "Debug: Frame advance (frame %d)\n", runner->frameCount);
        }

        double frameStartTime = 0;

        if (shouldStep) {
            /*
            if (args.traceFrames) {
                frameStartTime = svcGetSystemTick();
                fprintf(stderr, "Frame %d (Start)\n", runner->frameCount);
            }
            */

            // Run one game step (Begin Step, Keyboard, Alarms, Step, End Step, room transitions)
            Runner_step(runner);

            /*
            // Dump full runner state if this frame was requested
            if (hmget(args.dumpFrames, runner->frameCount)) {
                Runner_dumpState(runner);
            }

            // Dump runner state as JSON if this frame was requested
            if (hmget(args.dumpJsonFrames, runner->frameCount)) {
                char* json = Runner_dumpStateJson(runner);
                if (args.dumpJsonFilePattern != nullptr) {
                    char filename[512];
                    snprintf(filename, sizeof(filename), args.dumpJsonFilePattern, runner->frameCount);
                    FILE* f = fopen(filename, "w");
                    if (f != nullptr) {
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

        Room* activeRoom = runner->currentRoom;

        // Query actual framebuffer size (differs from window size on Wayland with fractional scaling)
        int fbWidth, fbHeight;
        // glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        fbWidth = (int) gen8->defaultWindowWidth;
        fbHeight = (int) gen8->defaultWindowHeight;

        // Clear the default framebuffer (window background) to black
        // TODO: Make sure this is correct (it should be)
        /*        
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        */
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(window, C2D_Color32f(0.0f, 0.0f, 0.0f, 1.0f));
        C2D_SceneBegin(window);

        int32_t gameW = (int32_t) gen8->defaultWindowWidth;
        int32_t gameH = (int32_t) gen8->defaultWindowHeight;

        // IMPORTANT TODO: Create the citro_renderer.c and citro_renderer.h
        // based on the gl_renderer.c and the gl_renderer.h
        // renderer->vtable->beginFrame(renderer, gameW, gameH, fbWidth, fbHeight);

        // Clear FBO with room background color
        if (runner->drawBackgroundColor) {
            int rInt = BGR_R(runner->backgroundColor);
            int gInt = BGR_G(runner->backgroundColor);
            int bInt = BGR_B(runner->backgroundColor);
            // glClearColor(rInt / 255.0f, gInt / 255.0f, bInt / 255.0f, 1.0f);
            C2D_TargetClear(window, C2D_Color32f(rInt / 255.0f, gInt / 255.0f, bInt / 255.0f, 1.0f));
        } else {
            // glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        }
        // glClear(GL_COLOR_BUFFER_BIT);

        // Render each enabled view (or a default full-screen view if views are disabled)
        bool viewsEnabled = (activeRoom->flags & 1) != 0;
        bool anyViewRendered = false;

        // TODO: Render one of the views to the bottom screen!
        if (viewsEnabled) {
            repeat(8, vi) {
                if (!activeRoom->views[vi].enabled) continue;

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
                // renderer->vtable->beginView(renderer, viewX, viewY, viewW, viewH, portX, portY, portW, portH, viewAngle);

                Runner_draw(runner);

                // renderer->vtable->endView(renderer);
                anyViewRendered = true;
            }
        }

        if (!anyViewRendered) {
            // No views enabled or views disabled: render with default full-screen view
            runner->viewCurrent = 0;
            // TODO: Add renderer, see first comment about  renderer
            // renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, 0, 0, gameW, gameH, 0.0f);
            Runner_draw(runner);
            // renderer->vtable->endView(renderer);
        }

        // Reset view_current to 0 so non-Draw events (Step, Alarm, Create) see view_current = 0
        runner->viewCurrent = 0;

        // TODO: Add renderer, see first comment about renderer
        // renderer->vtable->endFrame(renderer);

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

        C3D_FrameEnd(0); // Maybe it's this?

        // Limit frame rate to room speed (skip in headless mode for max speed!!)
        if (runner->currentRoom->speed > 0) {
            double targetFrameTime = 1.0 / (runner->currentRoom->speed);
            double nextFrameTime = lastFrameTime + targetFrameTime;
            // Sleep for most of the remaining time, then spin-wait for precision
            double remaining = nextFrameTime - svcGetSystemTick();
            if (remaining > 0.002) {
                struct timespec ts = {
                    .tv_sec = 0,
                    .tv_nsec = (long) ((remaining - 0.001) * 1e9)
                };
                nanosleep(&ts, nullptr);
            }
            while (svcGetSystemTick() < nextFrameTime) {
                // Spin-wait for the remaining sub-millisecond
            }
            lastFrameTime = nextFrameTime;
        } else {
            lastFrameTime = svcGetSystemTick();
        }
    }

    // Save input recording if active, then free
    if (globalInputRecording != nullptr) {
        if (globalInputRecording->isRecording) {
            InputRecording_save(globalInputRecording);
        }
        InputRecording_free(globalInputRecording);
        globalInputRecording = nullptr;
    }

    // Cleanup
    /*
    renderer->vtable->destroy(renderer);

    glfwDestroyWindow(window);
    glfwTerminate();
    */

    Runner_free(runner);
    // GlfwFileSystem_destroy(glfwFileSystem);
    VM_free(vm);
    DataWin_free(dataWin);

    // Deinit libs
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	romfsExit();

    //freeCommandLineArgs(&args);

    printf("Bye! :3\n");
    return 0;
}
