#pragma once

#include <gsKit.h>
#include <stdint.h>

#define GS_VRAM_SIZE (4 * 1024 * 1024)

// Clamp alpha to 0.0-1.0, then scale to PS2 GS range (0-128).
// Without clamping, values > 1.0 cause uint8_t overflow/wrapping, making fades repeat.
static inline uint8_t alphaToGS(float alpha) {
    if (alpha > 1.0f) alpha = 1.0f;
    else if (0.0f > alpha) alpha = 0.0f;
    return (uint8_t) (alpha * 128.0f);
}

typedef struct {
    char* key;
    bool usesISO9660;
} PS2DeviceKey;

extern PS2DeviceKey deviceKey;
extern bool deviceKeyLoaded;

void PS2Utils_extractDeviceKey(const char* path);
void PS2Utils_loadFSDrivers();
char* PS2Utils_createDevicePath(const char* path);
