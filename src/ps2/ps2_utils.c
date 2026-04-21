#include <stdio.h>
#include <string.h>
#include <loadfile.h>
#include <libcdvd.h>
#include "../utils.h"
#include "ps2_utils.h"

PS2DeviceKey deviceKey;
bool deviceKeyLoaded = false;

void PS2Utils_extractDeviceKey(const char* path) {
    require(!deviceKeyLoaded);

    char* pos = strchr(path, ':');
    requireNotNull(pos);

    size_t length = pos - path;
    char* result = safeMalloc((length + 1) * sizeof(char));
    strncpy(result, path, length);
    result[length] = '\0';

    // The "result" is the device key as a string (example: "mass" or "host")
    deviceKey = (PS2DeviceKey) {
        .key = result,
        .usesISO9660 = strncmp(result, "cdrom", strlen("cdrom")) == 0,
    };

    deviceKeyLoaded = true;
}

// Loads the required IOP drivers based on the current device key
// For cdrom devices, this loads the CDVD filesystem modules so we can read from the disc
void PS2Utils_loadFSDrivers() {
    require(deviceKeyLoaded);

    if (deviceKey.usesISO9660) {
        fprintf(stderr, "PS2Utils: Loading CDVD drivers for device key '%s'\n", deviceKey.key);

        int ret;
        ret = SifLoadModule("rom0:CDVDMAN", 0, nullptr);
        if (0 > ret) {
            fprintf(stderr, "PS2Utils: Failed to load CDVDMAN: %d\n", ret);
            abort();
        }

        ret = SifLoadModule("rom0:CDVDFSV", 0, nullptr);
        if (0 > ret) {
            fprintf(stderr, "PS2Utils: Failed to load CDVDFSV: %d\n", ret);
            abort();
        }

        sceCdInit(SCECdINIT);
        fprintf(stderr, "PS2Utils: CDVD initialized\n");
    }
}

// Creates a path with the device key + path for the loaded device key
// You need to free after using the path!
char* PS2Utils_createDevicePath(const char* path) {
    require(deviceKeyLoaded);

    if (deviceKey.usesISO9660) {
        size_t len = strlen(deviceKey.key) + 3 + strlen(path) + 2 + 1;
        char* devicePath = safeMalloc(len);
        snprintf(devicePath, len, "%s:\\%s;1", deviceKey.key, path);
        return devicePath;
    } else {
        size_t len = strlen(deviceKey.key) + 1 + strlen(path) + 1;
        char* devicePath = safeMalloc(len);
        snprintf(devicePath, len, "%s:%s", deviceKey.key, path);
        return devicePath;
    }
}
