#include "runner_keyboard.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

static bool isValidKey(int32_t key) {
    return key >= 0 && GML_KEY_COUNT > key;
}

RunnerKeyboardState* RunnerKeyboard_create(void) {
    RunnerKeyboardState* kb = safeCalloc(1, sizeof(RunnerKeyboardState));
#ifdef __WIIU__
    kb->currentKey = VK_NOKEY;
#endif
    kb->lastKey = VK_NOKEY;
    for (int32_t i = 0; i < GML_KEY_COUNT; i++) {
        kb->keyMap[i] = i;
    }
    return kb;
}

void RunnerKeyboard_free(RunnerKeyboardState* kb) {
    free(kb);
}

void RunnerKeyboard_beginFrame(RunnerKeyboardState* kb) {
    memset(kb->keyPressed, 0, sizeof(kb->keyPressed));
    memset(kb->keyReleased, 0, sizeof(kb->keyReleased));
}

void RunnerKeyboard_onKeyDown(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (!isValidKey(gmlKeyCode)) return;
    gmlKeyCode = kb->keyMap[gmlKeyCode];
    if (!isValidKey(gmlKeyCode)) return;
    kb->keyDown[gmlKeyCode] = true;
    kb->keyPressed[gmlKeyCode] = true;
#ifdef __WIIU__
    kb->currentKey = gmlKeyCode;
#endif
    kb->lastKey = gmlKeyCode;
}

void RunnerKeyboard_onKeyUp(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (!isValidKey(gmlKeyCode)) return;
    gmlKeyCode = kb->keyMap[gmlKeyCode];
    if (!isValidKey(gmlKeyCode)) return;
    kb->keyDown[gmlKeyCode] = false;
    kb->keyReleased[gmlKeyCode] = true;
#ifdef __WIIU__
    if (kb->currentKey == gmlKeyCode) {
        kb->currentKey = VK_NOKEY;
    }
#endif
}

bool RunnerKeyboard_check(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (gmlKeyCode == VK_ANYKEY) {
        for (int32_t i = 2; GML_KEY_COUNT > i; i++) {
            if (kb->keyDown[i]) return true;
        }
        return false;
    }
    if (gmlKeyCode == VK_NOKEY) {
        for (int32_t i = 2; GML_KEY_COUNT > i; i++) {
            if (kb->keyDown[i]) return false;
        }
        return true;
    }
    if (!isValidKey(gmlKeyCode)) return false;
    return kb->keyDown[gmlKeyCode];
}

bool RunnerKeyboard_checkPressed(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (gmlKeyCode == VK_ANYKEY) {
        for (int32_t i = 2; GML_KEY_COUNT > i; i++) {
            if (kb->keyPressed[i]) return true;
        }
        return false;
    }
    if (gmlKeyCode == VK_NOKEY) {
        for (int32_t i = 2; GML_KEY_COUNT > i; i++) {
            if (kb->keyPressed[i]) return false;
        }
        return true;
    }
    if (!isValidKey(gmlKeyCode)) return false;
    return kb->keyPressed[gmlKeyCode];
}

bool RunnerKeyboard_checkReleased(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (gmlKeyCode == VK_ANYKEY) {
        for (int32_t i = 2; GML_KEY_COUNT > i; i++) {
            if (kb->keyReleased[i]) return true;
        }
        return false;
    }
    if (gmlKeyCode == VK_NOKEY) {
        for (int32_t i = 2; GML_KEY_COUNT > i; i++) {
            if (kb->keyReleased[i]) return false;
        }
        return true;
    }
    if (!isValidKey(gmlKeyCode)) return false;
    return kb->keyReleased[gmlKeyCode];
}

void RunnerKeyboard_simulatePress(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (!isValidKey(gmlKeyCode)) return;
    kb->keyDown[gmlKeyCode] = true;
    kb->keyPressed[gmlKeyCode] = true;
#ifdef __WIIU__
    kb->currentKey = gmlKeyCode;
#endif
    kb->lastKey = gmlKeyCode;
}

void RunnerKeyboard_simulateRelease(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (!isValidKey(gmlKeyCode)) return;
    kb->keyDown[gmlKeyCode] = false;
    kb->keyReleased[gmlKeyCode] = true;
#ifdef __WIIU__
    if (kb->currentKey == gmlKeyCode) {
        kb->currentKey = VK_NOKEY;
    }
#endif
}

void RunnerKeyboard_clear(RunnerKeyboardState* kb, int32_t gmlKeyCode) {
    if (gmlKeyCode == VK_ANYKEY) {
        memset(kb->keyDown, 0, sizeof(kb->keyDown));
        memset(kb->keyPressed, 0, sizeof(kb->keyPressed));
        memset(kb->keyReleased, 0, sizeof(kb->keyReleased));
#ifdef __WIIU__
        kb->currentKey = VK_NOKEY;
#endif
        kb->lastKey = VK_NOKEY;
        return;
    }
    if (!isValidKey(gmlKeyCode)) return;
    kb->keyDown[gmlKeyCode] = false;
    kb->keyPressed[gmlKeyCode] = false;
    kb->keyReleased[gmlKeyCode] = false;
#ifdef __WIIU__
    if (kb->currentKey == gmlKeyCode) {
        kb->currentKey = VK_NOKEY;
    }
#endif
}

void RunnerKeyboard_setMap(RunnerKeyboardState* kb, int32_t fromKey, int32_t toKey) {
    if (!isValidKey(fromKey) || !isValidKey(toKey)) return;
    kb->keyMap[fromKey] = toKey;
}
