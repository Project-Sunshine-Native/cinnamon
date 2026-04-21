#pragma once

#include "common.h"
#include <stdint.h>
#include <stdbool.h>

// GML uses key codes 0-255 (vk_nokey=0, vk_anykey=1, ASCII codes, etc.)
#define GML_KEY_COUNT 256

// GML Virtual Key Constants (match Windows VK codes)
#define VK_NOKEY     0
#define VK_ANYKEY    1
#define VK_BACKSPACE 8
#define VK_TAB       9
#define VK_ENTER    13
#define VK_SHIFT    16
#define VK_CONTROL  17
#define VK_ALT      18
#define VK_ESCAPE   27
#define VK_SPACE    32
#define VK_PAGEUP   33
#define VK_PAGEDOWN 34
#define VK_END      35
#define VK_HOME     36
#define VK_LEFT     37
#define VK_UP       38
#define VK_RIGHT    39
#define VK_DOWN     40
#define VK_INSERT   45
#define VK_DELETE   46

// Letter keys (ASCII values)
#define VK_A 65
#define VK_B 66
#define VK_C 67
#define VK_D 68
#define VK_E 69
#define VK_F 70
#define VK_G 71
#define VK_H 72
#define VK_I 73
#define VK_J 74
#define VK_K 75
#define VK_L 76
#define VK_M 77
#define VK_N 78
#define VK_O 79
#define VK_P 80
#define VK_Q 81
#define VK_R 82
#define VK_S 83
#define VK_T 84
#define VK_U 85
#define VK_V 86
#define VK_W 87
#define VK_X 88
#define VK_Y 89
#define VK_Z 90
// 48-57 = '0'-'9', 65-90 = 'A'-'Z' (ASCII)
#define VK_F1      112
#define VK_F2      113
#define VK_F3      114
#define VK_F4      115
#define VK_F5      116
#define VK_F6      117
#define VK_F7      118
#define VK_F8      119
#define VK_F9      120
#define VK_F10     121
#define VK_F11     122
#define VK_F12     123

typedef struct RunnerKeyboardState {
    bool keyDown[GML_KEY_COUNT];     // Currently held
    bool keyPressed[GML_KEY_COUNT];  // Just pressed this frame
    bool keyReleased[GML_KEY_COUNT]; // Just released this frame
    int32_t lastKey;                 // Last key pressed (for keyboard_key variable)
    char lastChar[2];                // Last character pressed (for keyboard_char variable)
} RunnerKeyboardState;

// Lifecycle
RunnerKeyboardState* RunnerKeyboard_create(void);
void RunnerKeyboard_free(RunnerKeyboardState* kb);

// Called at the start of each frame to clear pressed/released arrays
void RunnerKeyboard_beginFrame(RunnerKeyboardState* kb);

// Called by platform layer when a key is pressed/released (gmlKeyCode = GML vk_ code)
void RunnerKeyboard_onKeyDown(RunnerKeyboardState* kb, int32_t gmlKeyCode);
void RunnerKeyboard_onKeyUp(RunnerKeyboardState* kb, int32_t gmlKeyCode);

// Called by platform layer when a character is typed
void RunnerKeyboard_onCharacter(RunnerKeyboardState* kb, unsigned int character);

// GML function queries
bool RunnerKeyboard_check(RunnerKeyboardState* kb, int32_t gmlKeyCode);
bool RunnerKeyboard_checkPressed(RunnerKeyboardState* kb, int32_t gmlKeyCode);
bool RunnerKeyboard_checkReleased(RunnerKeyboardState* kb, int32_t gmlKeyCode);

// Simulated press/release (used by keyboard_key_press/keyboard_key_release GML functions)
void RunnerKeyboard_simulatePress(RunnerKeyboardState* kb, int32_t gmlKeyCode);
void RunnerKeyboard_simulateRelease(RunnerKeyboardState* kb, int32_t gmlKeyCode);

// Clear a specific key's state
void RunnerKeyboard_clear(RunnerKeyboardState* kb, int32_t gmlKeyCode);
